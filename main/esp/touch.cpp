#include "touch.h"

#include <algorithm>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "epd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins.h"

namespace {

constexpr char kTag[] = "touch";
constexpr uint8_t kAddrLow = 0x5D;   // INT held low during reset
constexpr uint8_t kAddrHigh = 0x14;  // INT held high during reset
constexpr uint16_t kRegProductId = 0x8140;
constexpr uint16_t kRegResolution = 0x8146;
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoints = 0x8150;
constexpr int kTimeoutMs = 20;
constexpr TickType_t kPoll = pdMS_TO_TICKS(20);
constexpr int kIdlePollsForRelease = 10;  // no data for 200 ms counts as lifted
constexpr int kErrorsBeforeReset = 25;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_stop = false;
TapHandler s_on_tap = nullptr;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
TouchStatus s_status = {};

enum class Read { Error, NoData, Released, Touch };

bool read_regs(uint16_t reg, uint8_t *out, size_t len)
{
    const uint8_t addr[2] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
    return i2c_master_transmit_receive(s_dev, addr, 2, out, len, kTimeoutMs) == ESP_OK;
}

bool write_reg(uint16_t reg, uint8_t value)
{
    const uint8_t data[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg), value};
    return i2c_master_transmit(s_dev, data, 3, kTimeoutMs) == ESP_OK;
}

// The GT911 latches its I2C address from the INT pin on the rising edge of reset.
bool reset_at(uint8_t address)
{
    const gpio_num_t rst = static_cast<gpio_num_t>(PIN_TOUCH_RST);
    const gpio_num_t irq = static_cast<gpio_num_t>(PIN_TOUCH_INT);
    gpio_set_direction(irq, GPIO_MODE_OUTPUT);
    gpio_set_level(rst, 0);
    gpio_set_level(irq, address == kAddrHigh ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_direction(irq, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(80));

    if (s_dev != nullptr) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
    if (i2c_master_probe(s_bus, address, kTimeoutMs) != ESP_OK) {
        return false;
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = address;
    dev.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) {
        s_dev = nullptr;
        return false;
    }
    uint8_t id[4];
    return read_regs(kRegProductId, id, sizeof(id));
}

bool detect()
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (uint8_t address : {kAddrHigh, kAddrLow}) {
            if (!reset_at(address)) {
                continue;
            }
            uint16_t w = 0;
            uint16_t h = 0;
            uint8_t res[4];
            if (read_regs(kRegResolution, res, sizeof(res))) {
                w = static_cast<uint16_t>(res[0] | (res[1] << 8));
                h = static_cast<uint16_t>(res[2] | (res[3] << 8));
            }
            write_reg(kRegStatus, 0);
            taskENTER_CRITICAL(&s_lock);
            s_status.found = true;
            s_status.address = address;
            s_status.sensor_w = w != 0 ? w : 800;
            s_status.sensor_h = h != 0 ? h : 480;
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(kTag, "GT911 found at 0x%02X, resolution %ux%u", address, w, h);
            return true;
        }
    }
    return false;
}

uint16_t scale(uint32_t value, uint32_t in_max, uint32_t out_max)
{
    value = std::min(value, in_max);
    return static_cast<uint16_t>((value * out_max + in_max / 2) / in_max);
}

// Controller coordinates to screen coordinates. The sensor is mounted rotated
// relative to the panel; this is the mapping Seeed's Sticky demos use.
void to_screen(uint16_t raw_x, uint16_t raw_y, int &x, int &y)
{
    const uint32_t tx = static_cast<uint32_t>(raw_x) * 800 / s_status.sensor_w;
    const uint32_t ty = static_cast<uint32_t>(raw_y) * 480 / s_status.sensor_h;
    const uint16_t portrait_x = scale(tx, 800, 479);
    const uint16_t portrait_y = scale(480 - std::min<uint32_t>(ty, 480), 480, 799);
    x = portrait_y;
    y = 479 - portrait_x;
}

Read read_point(int &raw_x, int &raw_y)
{
    uint8_t status = 0;
    if (!read_regs(kRegStatus, &status, 1)) {
        return Read::Error;
    }
    if ((status & 0x80) == 0) {
        return Read::NoData;
    }
    const int count = status & 0x0F;
    Read result = Read::Released;
    if (count > 0 && count <= 5) {
        uint8_t p[8];
        if (!read_regs(kRegPoints, p, sizeof(p))) {
            return Read::Error;
        }
        raw_x = p[0] | (p[1] << 8);
        raw_y = p[2] | (p[3] << 8);
        result = Read::Touch;
    }
    write_reg(kRegStatus, 0);
    return result;
}

void touch_task(void *)
{
    bool down = false;
    int idle = 0;
    int errors_in_row = 0;
    TickType_t wake = xTaskGetTickCount();
    while (!s_stop) {
        if (!s_status.found) {
            if (!detect()) {
                ESP_LOGW(kTag, "GT911 not answering; retrying");
                vTaskDelay(pdMS_TO_TICKS(2000));
                wake = xTaskGetTickCount();
                continue;
            }
        }

        int raw_x = 0;
        int raw_y = 0;
        const Read r = read_point(raw_x, raw_y);
        if (r == Read::Error) {
            taskENTER_CRITICAL(&s_lock);
            ++s_status.errors;
            taskEXIT_CRITICAL(&s_lock);
            if (++errors_in_row >= kErrorsBeforeReset) {
                ESP_LOGW(kTag, "GT911 stopped answering; re-detecting");
                taskENTER_CRITICAL(&s_lock);
                s_status.found = false;
                ++s_status.resets;
                taskEXIT_CRITICAL(&s_lock);
                errors_in_row = 0;
                down = false;
            }
        } else {
            errors_in_row = 0;
            taskENTER_CRITICAL(&s_lock);
            ++s_status.reads;
            taskEXIT_CRITICAL(&s_lock);
        }

        if (r == Read::Touch) {
            idle = 0;
            if (!down) {
                // Act as soon as the finger lands; no release or travel
                // check that a noisy frame could defeat.
                down = true;
                int x = 0;
                int y = 0;
                to_screen(static_cast<uint16_t>(raw_x), static_cast<uint16_t>(raw_y), x, y);
                const bool busy = epd_refreshing();
                taskENTER_CRITICAL(&s_lock);
                ++s_status.touches;
                s_status.raw_x = raw_x;
                s_status.raw_y = raw_y;
                s_status.x = x;
                s_status.y = y;
                if (busy) {
                    ++s_status.ignored;
                } else {
                    ++s_status.taps;
                }
                taskEXIT_CRITICAL(&s_lock);
                ESP_LOGI(kTag, "touch raw %d,%d -> screen %d,%d%s", raw_x, raw_y, x, y,
                         busy ? " (ignored: refreshing)" : "");
                if (!busy && s_on_tap != nullptr) {
                    s_on_tap(x, y);
                }
            }
        } else if (r == Read::Released) {
            down = false;
        } else if (r == Read::NoData && down && ++idle >= kIdlePollsForRelease) {
            down = false;
        }
        vTaskDelayUntil(&wake, kPoll);
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t touch_init(TapHandler on_tap)
{
    s_on_tap = on_tap;
    // gpio_config also routes each pad to the GPIO matrix. GPIO41 (reset) and
    // GPIO42 (power) default to JTAG functions on the ESP32-S3.
    gpio_config_t outputs = {};
    outputs.pin_bit_mask = (1ULL << PIN_TOUCH_EN) | (1ULL << PIN_TOUCH_RST);
    outputs.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&outputs);
    if (err != ESP_OK) {
        return err;
    }
    gpio_config_t irq = {};
    irq.pin_bit_mask = 1ULL << PIN_TOUCH_INT;
    irq.mode = GPIO_MODE_INPUT;
    err = gpio_config(&irq);
    if (err != ESP_OK) {
        return err;
    }
    // Power up held in reset; detect() then releases it with the address set.
    gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_RST), 0);
    gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_EN), 1);
    vTaskDelay(pdMS_TO_TICKS(250));

    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SDA);
    bus.scl_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SCL);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = 1;
    err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    s_stop = false;
    return xTaskCreate(touch_task, "touch", 4096, nullptr, 5, &s_task) == pdPASS ? ESP_OK
                                                                                : ESP_ERR_NO_MEM;
}

void touch_stop()
{
    s_stop = true;
    for (int i = 0; i < 50 && s_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

TouchStatus touch_status()
{
    taskENTER_CRITICAL(&s_lock);
    const TouchStatus copy = s_status;
    taskEXIT_CRITICAL(&s_lock);
    return copy;
}
