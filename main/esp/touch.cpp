#include "touch.h"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "epd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins.h"

namespace {

constexpr char kTag[] = "touch";
constexpr uint8_t kAddr5D = 0x5D;  // INT low while reset is released
constexpr uint8_t kAddr14 = 0x14;  // INT high while reset is released
constexpr uint16_t kRegCommand = 0x8040;
constexpr uint16_t kRegConfigVersion = 0x8047;
constexpr uint16_t kRegProductId = 0x8140;  // id[4], firmware[2], x res[2], y res[2]
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoints = 0x8150;
constexpr int kTimeoutMs = 20;
constexpr TickType_t kPoll = pdMS_TO_TICKS(20);
constexpr int kIdlePollsForRelease = 10;  // no data for 200 ms counts as lifted
constexpr int kErrorsBeforeReset = 25;

const gpio_num_t kRst = static_cast<gpio_num_t>(PIN_TOUCH_RST);
const gpio_num_t kInt = static_cast<gpio_num_t>(PIN_TOUCH_INT);
const gpio_num_t kEn = static_cast<gpio_num_t>(PIN_TOUCH_EN);

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_stop = false;
volatile int s_requested_start = -1;  // set by touch_try_next_start()
TapHandler s_on_tap = nullptr;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
TouchStatus s_status = {};

enum class Read { Error, NoData, Released, Touch };

struct Identity {
    char product_id[5];
    uint16_t firmware;
    uint8_t config_version;
    uint16_t w;
    uint16_t h;
};

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

// GT911 datasheet power-on timing (also what the Linux goodix driver does):
// reset low, INT selects the address, release reset, then hold INT low for
// 50 ms ("INT sync") before letting the chip drive it.
void datasheet_reset(uint8_t address)
{
    gpio_set_pull_mode(kRst, GPIO_FLOATING);
    gpio_set_direction(kRst, GPIO_MODE_OUTPUT);
    gpio_set_direction(kInt, GPIO_MODE_OUTPUT);
    gpio_set_level(kRst, 0);
    gpio_set_level(kInt, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(kInt, address == kAddr14 ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(kRst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(kInt, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_direction(kInt, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(60));
}

// Cut the controller's power and let it boot on its own, with reset released
// and INT left to the board.
void power_cycle()
{
    gpio_set_direction(kInt, GPIO_MODE_INPUT);
    gpio_set_direction(kRst, GPIO_MODE_INPUT);
    gpio_set_pull_mode(kRst, GPIO_PULLUP_ONLY);
    gpio_set_level(kEn, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(kEn, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
}

bool attach(uint8_t address)
{
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
    return true;
}

bool identify(Identity &id)
{
    uint8_t info[10];
    if (!read_regs(kRegProductId, info, sizeof(info))) {
        return false;
    }
    std::memcpy(id.product_id, info, 4);
    id.product_id[4] = '\0';
    for (char &ch : id.product_id) {
        if (ch != '\0' && (ch < 0x20 || ch > 0x7E)) {
            ch = '?';
        }
    }
    id.firmware = static_cast<uint16_t>(info[4] | (info[5] << 8));
    id.w = static_cast<uint16_t>(info[6] | (info[7] << 8));
    id.h = static_cast<uint16_t>(info[8] | (info[9] << 8));
    id.config_version = 0;
    read_regs(kRegConfigVersion, &id.config_version, 1);
    return true;
}

bool healthy(const Identity &id)
{
    return id.product_id[0] == '9' && id.w != 0 && id.h != 0;
}

// Brings the controller up one way; returns the address it answered on.
bool bring_up(TouchStart how, uint8_t &address, Identity &id)
{
    uint8_t candidates[2] = {kAddr5D, kAddr14};
    int count = 2;
    switch (how) {
    case TouchStart::ResetAt5D:
        datasheet_reset(kAddr5D);
        count = 1;
        break;
    case TouchStart::ResetAt14:
        datasheet_reset(kAddr14);
        candidates[0] = kAddr14;
        count = 1;
        break;
    case TouchStart::PowerCycle:
        power_cycle();
        break;
    }
    for (int i = 0; i < count; ++i) {
        const uint8_t candidate = candidates[i];
        if (attach(candidate) && identify(id)) {
            address = candidate;
            ESP_LOGI(kTag, "%s: answered at 0x%02X, id '%s', firmware 0x%04X, config 0x%02X, "
                     "resolution %ux%u", touch_start_name(how), candidate, id.product_id,
                     id.firmware, id.config_version, id.w, id.h);
            return true;
        }
    }
    ESP_LOGW(kTag, "%s: no answer", touch_start_name(how));
    return false;
}

void adopt(TouchStart how, uint8_t address, const Identity &id)
{
    write_reg(kRegCommand, 0x00);  // coordinate reporting mode
    write_reg(kRegStatus, 0);
    taskENTER_CRITICAL(&s_lock);
    s_status.found = true;
    s_status.healthy = healthy(id);
    s_status.start = how;
    s_status.address = address;
    std::memcpy(s_status.product_id, id.product_id, sizeof(id.product_id));
    s_status.firmware = id.firmware;
    s_status.config_version = id.config_version;
    s_status.sensor_w = id.w;
    s_status.sensor_h = id.h;
    taskEXIT_CRITICAL(&s_lock);
}

// Tries each start method, beginning with `first`, and keeps the first one
// after which the chip looks healthy. If none does, falls back to the first
// that answered at all. With `only_first`, tries just that one method.
bool detect(int first, bool only_first)
{
    int fallback = -1;
    for (int k = 0; k < (only_first ? 1 : kTouchStarts); ++k) {
        const TouchStart how = static_cast<TouchStart>((first + k) % kTouchStarts);
        uint8_t address = 0;
        Identity id = {};
        if (!bring_up(how, address, id)) {
            continue;
        }
        if (healthy(id) || only_first) {
            adopt(how, address, id);
            return true;
        }
        if (fallback < 0) {
            fallback = static_cast<int>(how);
        }
    }
    if (fallback >= 0) {
        const TouchStart how = static_cast<TouchStart>(fallback);
        uint8_t address = 0;
        Identity id = {};
        if (bring_up(how, address, id)) {
            ESP_LOGW(kTag, "no start method looked healthy; using %s", touch_start_name(how));
            adopt(how, address, id);
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
    const uint32_t w = s_status.sensor_w != 0 ? s_status.sensor_w : 800;
    const uint32_t h = s_status.sensor_h != 0 ? s_status.sensor_h : 480;
    const uint32_t tx = static_cast<uint32_t>(raw_x) * 800 / w;
    const uint32_t ty = static_cast<uint32_t>(raw_y) * 480 / h;
    const uint16_t portrait_x = scale(tx, 800, 479);
    const uint16_t portrait_y = scale(480 - std::min<uint32_t>(ty, 480), 480, 799);
    x = portrait_y;
    y = 479 - portrait_x;
}

Read read_point(int &raw_x, int &raw_y, uint8_t &status)
{
    status = 0;
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
        const int requested = s_requested_start;
        if (requested >= 0) {
            s_requested_start = -1;
            taskENTER_CRITICAL(&s_lock);
            s_status.found = false;
            ++s_status.resets;
            taskEXIT_CRITICAL(&s_lock);
            detect(requested, true);
            down = false;
            wake = xTaskGetTickCount();
        }
        if (!s_status.found) {
            if (!detect(0, false)) {
                ESP_LOGW(kTag, "GT911 not answering; retrying");
                vTaskDelay(pdMS_TO_TICKS(2000));
                wake = xTaskGetTickCount();
                continue;
            }
        }

        int raw_x = 0;
        int raw_y = 0;
        uint8_t status = 0;
        const Read r = read_point(raw_x, raw_y, status);
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
            if (r != Read::NoData) {
                ++s_status.frames;
            }
            if (status != 0) {
                s_status.last_status = status;
            }
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

const char *touch_start_name(TouchStart start)
{
    switch (start) {
    case TouchStart::ResetAt5D:
        return "reset at 0x5D";
    case TouchStart::ResetAt14:
        return "reset at 0x14";
    case TouchStart::PowerCycle:
        return "power cycle";
    }
    return "?";
}

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
    // Power up held in reset; the touch task then starts the controller.
    gpio_set_level(kRst, 0);
    gpio_set_level(kEn, 1);
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

void touch_try_next_start()
{
    const TouchStatus now = touch_status();
    s_requested_start = (static_cast<int>(now.start) + 1) % kTouchStarts;
}
