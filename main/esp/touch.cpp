#include "touch.h"

#include <algorithm>
#include <cstdlib>

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
constexpr int kMaxTapTravel = 40;  // pixels; longer movements are not taps

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_stop = false;
TapHandler s_on_tap = nullptr;
uint16_t s_max_x = 2048;
uint16_t s_max_y = 2048;

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
    gpio_set_direction(rst, GPIO_MODE_OUTPUT);
    gpio_set_direction(irq, GPIO_MODE_OUTPUT);
    gpio_set_level(rst, 0);
    gpio_set_level(irq, address == kAddrHigh ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_direction(irq, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(80));

    if (i2c_master_probe(s_bus, address, kTimeoutMs) != ESP_OK) {
        return false;
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = address;
    dev.scl_speed_hz = 100000;
    if (s_dev != nullptr) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
    }
    if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) {
        return false;
    }
    uint8_t id[4];
    return read_regs(kRegProductId, id, sizeof(id));
}

uint16_t scale(uint32_t value, uint32_t in_max, uint32_t out_max)
{
    value = std::min(value, in_max);
    return static_cast<uint16_t>((value * out_max + in_max / 2) / in_max);
}

// Raw GT911 coordinates to screen coordinates. The sensor is mounted rotated
// relative to the panel; this is the mapping Seeed verified for the Sticky.
void to_screen(uint16_t raw_x, uint16_t raw_y, int &x, int &y)
{
    const uint32_t tx = static_cast<uint32_t>(raw_x) * 800 / s_max_x;
    const uint32_t ty = static_cast<uint32_t>(raw_y) * 480 / s_max_y;
    const uint16_t portrait_x = scale(tx, 800, 479);
    const uint16_t portrait_y = scale(480 - std::min<uint32_t>(ty, 480), 480, 799);
    x = portrait_y;
    y = 479 - portrait_x;
}

// Returns 1 with a point, 0 when nothing is touched, -1 on a bus error.
int read_point(int &x, int &y)
{
    uint8_t status = 0;
    if (!read_regs(kRegStatus, &status, 1)) {
        return -1;
    }
    if ((status & 0x80) == 0) {
        return 0;
    }
    const int count = status & 0x0F;
    int result = 0;
    if (count > 0 && count <= 5) {
        uint8_t p[8];
        if (read_regs(kRegPoints, p, sizeof(p))) {
            to_screen(static_cast<uint16_t>(p[0] | (p[1] << 8)),
                      static_cast<uint16_t>(p[2] | (p[3] << 8)), x, y);
            result = 1;
        }
    }
    write_reg(kRegStatus, 0);
    return result;
}

void touch_task(void *)
{
    bool down = false;
    bool ignore = false;
    int start_x = 0;
    int start_y = 0;
    int last_x = 0;
    int last_y = 0;
    int idle_polls = 0;
    TickType_t wake = xTaskGetTickCount();
    while (!s_stop) {
        int x = 0;
        int y = 0;
        const int n = read_point(x, y);
        if (n > 0) {
            idle_polls = 0;
            if (!down) {
                down = true;
                ignore = epd_refreshing();
                start_x = x;
                start_y = y;
            }
            last_x = x;
            last_y = y;
        } else if (n == 0 && down) {
            // GT911 can report a missing frame mid-touch; require two in a row.
            if (++idle_polls >= 2) {
                down = false;
                const int travel = std::abs(last_x - start_x) + std::abs(last_y - start_y);
                if (!ignore && !epd_refreshing() && travel <= kMaxTapTravel && s_on_tap) {
                    s_on_tap(start_x, start_y);
                }
            }
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t touch_init(TapHandler on_tap)
{
    s_on_tap = on_tap;
    gpio_config_t power = {};
    power.pin_bit_mask = 1ULL << PIN_TOUCH_EN;
    power.mode = GPIO_MODE_OUTPUT;
    gpio_config(&power);
    gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_EN), 1);
    vTaskDelay(pdMS_TO_TICKS(250));

    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SDA);
    bus.scl_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SCL);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = 1;
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        return err;
    }

    bool ready = false;
    for (int attempt = 0; attempt < 3 && !ready; ++attempt) {
        ready = reset_at(kAddrHigh) || reset_at(kAddrLow);
    }
    if (!ready) {
        ESP_LOGE(kTag, "GT911 not found");
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t res[4];
    if (read_regs(kRegResolution, res, sizeof(res))) {
        const uint16_t mx = static_cast<uint16_t>(res[0] | (res[1] << 8));
        const uint16_t my = static_cast<uint16_t>(res[2] | (res[3] << 8));
        s_max_x = mx != 0 ? mx : s_max_x;
        s_max_y = my != 0 ? my : s_max_y;
    }
    write_reg(kRegStatus, 0);
    ESP_LOGI(kTag, "GT911 ready, sensor %ux%u", s_max_x, s_max_y);

    s_stop = false;
    return xTaskCreate(touch_task, "touch", 4096, nullptr, 5, &s_task) == pdPASS ? ESP_OK
                                                                                : ESP_ERR_NO_MEM;
}

void touch_stop()
{
    s_stop = true;
    for (int i = 0; i < 20 && s_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
