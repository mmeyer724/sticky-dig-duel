#include "epd.h"

#include <atomic>
#include <cstring>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins.h"

namespace {

constexpr char kTag[] = "epd";
constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr int kStride = kWidth / 8;
constexpr size_t kFrameBytes = kStride * kHeight;
constexpr int kRowsPerChunk = 40;
constexpr size_t kChunkBytes = kStride * kRowsPerChunk;
constexpr int64_t kBusyTimeoutUs = 10 * 1000 * 1000;

enum class Mode { None, Full, Partial };

spi_device_handle_t s_spi = nullptr;
uint8_t *s_dma = nullptr;    // DMA-capable staging buffer
uint8_t *s_shown = nullptr;  // what the panel currently displays
Mode s_mode = Mode::None;
bool s_shown_valid = false;
std::atomic<bool> s_refreshing{false};

esp_err_t transmit(const uint8_t *data, size_t len, int dc)
{
    gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_DC), dc);
    while (len > 0) {
        const size_t n = len < kChunkBytes ? len : kChunkBytes;
        std::memcpy(s_dma, data, n);
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = s_dma;
        ESP_RETURN_ON_ERROR(spi_device_transmit(s_spi, &t), kTag, "spi");
        data += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t command(uint8_t cmd)
{
    return transmit(&cmd, 1, 0);
}

esp_err_t command(uint8_t cmd, std::initializer_list<uint8_t> data)
{
    ESP_RETURN_ON_ERROR(command(cmd), kTag, "cmd 0x%02X", cmd);
    uint8_t bytes[8];
    size_t n = 0;
    for (uint8_t b : data) {
        bytes[n++] = b;
    }
    return transmit(bytes, n, 1);
}

esp_err_t wait_ready()
{
    const int64_t start = esp_timer_get_time();
    while (gpio_get_level(static_cast<gpio_num_t>(PIN_EPD_BUSY)) == 1) {
        if (esp_timer_get_time() - start > kBusyTimeoutUs) {
            ESP_LOGE(kTag, "panel busy timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

esp_err_t set_full_window()
{
    constexpr uint8_t xe0 = (kWidth - 1) & 0xFF, xe1 = (kWidth - 1) >> 8;
    constexpr uint8_t ye0 = (kHeight - 1) & 0xFF, ye1 = (kHeight - 1) >> 8;
    ESP_RETURN_ON_ERROR(command(0x44, {0x00, 0x00, xe0, xe1}), kTag, "ram x");
    ESP_RETURN_ON_ERROR(command(0x45, {0x00, 0x00, ye0, ye1}), kTag, "ram y");
    ESP_RETURN_ON_ERROR(command(0x4E, {0x00, 0x00}), kTag, "cursor x");
    return command(0x4F, {0x00, 0x00});
}

// Controller setup for a refresh mode, following Seeed's verified SSD1677
// sequence for this panel.
esp_err_t init_mode(Mode mode)
{
    gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_RST), 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(wait_ready(), kTag, "reset");
    if (mode == Mode::Full) {
        ESP_RETURN_ON_ERROR(command(0x12), kTag, "soft reset");
        ESP_RETURN_ON_ERROR(wait_ready(), kTag, "soft reset");
    }
    ESP_RETURN_ON_ERROR(command(0x18, {0x80}), kTag, "temp sensor");
    ESP_RETURN_ON_ERROR(command(0x3C, {static_cast<uint8_t>(mode == Mode::Full ? 0x01 : 0x80)}),
                        kTag, "border");
    ESP_RETURN_ON_ERROR(command(0x0C, {0xAE, 0xC7, 0xC3, 0xC0, 0x80}), kTag, "soft start");
    ESP_RETURN_ON_ERROR(command(0x01, {(kHeight - 1) & 0xFF, (kHeight - 1) >> 8, 0x02}), kTag,
                        "driver output");
    ESP_RETURN_ON_ERROR(command(0x11, {0x03}), kTag, "data entry");
    ESP_RETURN_ON_ERROR(set_full_window(), kTag, "window");
    ESP_RETURN_ON_ERROR(wait_ready(), kTag, "init");
    s_mode = mode;
    return ESP_OK;
}

// The panel is mounted upside down relative to its RAM rows: RAM row r shows
// screen row (479 - r), with columns in their natural order.
esp_err_t write_plane(uint8_t ram_command, const uint8_t *frame)
{
    ESP_RETURN_ON_ERROR(set_full_window(), kTag, "window");
    ESP_RETURN_ON_ERROR(command(ram_command), kTag, "ram write");
    gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_DC), 1);
    for (int r = 0; r < kHeight; r += kRowsPerChunk) {
        for (int i = 0; i < kRowsPerChunk; ++i) {
            const int screen_row = kHeight - 1 - (r + i);
            std::memcpy(s_dma + i * kStride, frame + screen_row * kStride, kStride);
        }
        spi_transaction_t t = {};
        t.length = kChunkBytes * 8;
        t.tx_buffer = s_dma;
        ESP_RETURN_ON_ERROR(spi_device_transmit(s_spi, &t), kTag, "spi");
    }
    return ESP_OK;
}

esp_err_t refresh(const uint8_t *frame, Mode mode)
{
    if (s_mode != mode) {
        ESP_RETURN_ON_ERROR(init_mode(mode), kTag, "init");
    }
    if (mode == Mode::Full) {
        ESP_RETURN_ON_ERROR(write_plane(0x26, frame), kTag, "previous plane");
    } else {
        ESP_RETURN_ON_ERROR(write_plane(0x26, s_shown), kTag, "previous plane");
    }
    ESP_RETURN_ON_ERROR(write_plane(0x24, frame), kTag, "current plane");
    ESP_RETURN_ON_ERROR(command(0x22, {static_cast<uint8_t>(mode == Mode::Full ? 0xF7 : 0xFF)}),
                        kTag, "update control");
    ESP_RETURN_ON_ERROR(command(0x20), kTag, "activate");
    return wait_ready();
}

}  // namespace

esp_err_t epd_init()
{
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << PIN_EPD_EN) | (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_RST);
    out.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&out), kTag, "outputs");
    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << PIN_EPD_BUSY;
    in.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&in), kTag, "busy");
    gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_RST), 1);
    gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_EN), 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_EPD_MOSI;
    bus.sclk_io_num = PIN_EPD_CLK;
    // Unused pins must be -1: a zero would claim GPIO0, the sensor I2C clock.
    bus.miso_io_num = -1;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.max_transfer_sz = kChunkBytes;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), kTag, "bus");

    spi_device_interface_config_t dev = {};
    dev.clock_speed_hz = 10 * 1000 * 1000;
    dev.mode = 0;
    dev.spics_io_num = PIN_EPD_CS;
    dev.queue_size = 1;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), kTag, "device");

    s_dma = static_cast<uint8_t *>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_DMA));
    s_shown = static_cast<uint8_t *>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (s_dma == nullptr || s_shown == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    std::memset(s_shown, 0xFF, kFrameBytes);
    return ESP_OK;
}

esp_err_t epd_show(const uint8_t *frame, bool full)
{
    const Mode mode = (full || !s_shown_valid) ? Mode::Full : Mode::Partial;
    s_refreshing = true;
    const int64_t start = esp_timer_get_time();
    esp_err_t err = refresh(frame, mode);
    if (err != ESP_OK) {
        // Recover with a fresh controller init and a full refresh next time.
        s_mode = Mode::None;
        s_shown_valid = false;
    } else {
        std::memcpy(s_shown, frame, kFrameBytes);
        s_shown_valid = true;
    }
    s_refreshing = false;
    ESP_LOGI(kTag, "%s refresh %s in %lld ms", mode == Mode::Full ? "full" : "partial",
             esp_err_to_name(err), (esp_timer_get_time() - start) / 1000);
    return err;
}

esp_err_t epd_sleep()
{
    if (s_spi != nullptr) {
        command(0x10, {0x03});
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_mode = Mode::None;
    return gpio_set_level(static_cast<gpio_num_t>(PIN_EPD_EN), 0);
}

bool epd_refreshing()
{
    return s_refreshing;
}
