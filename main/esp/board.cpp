#include "board.h"

#include <cstdio>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins.h"

namespace {

constexpr char kTag[] = "board";
i2c_master_bus_handle_t s_sensor_bus = nullptr;

void output(int pin, int level)
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    gpio_config(&config);
    gpio_set_level(static_cast<gpio_num_t>(pin), level);
}

void hold(int pin, int level)
{
    const gpio_num_t gpio = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(gpio);
    gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio, level);
    gpio_hold_en(gpio);
}

}  // namespace

esp_err_t board_init()
{
    // Pins held through deep sleep keep their state until released. Drive the
    // power latch high before releasing it so the board never drops power.
    gpio_set_direction(static_cast<gpio_num_t>(PIN_POWER_HOLD), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(PIN_POWER_HOLD), 1);
    gpio_set_direction(static_cast<gpio_num_t>(PIN_POWER_LOCK), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(PIN_POWER_LOCK), 1);
    gpio_deep_sleep_hold_dis();
    const int held[] = {PIN_POWER_HOLD, PIN_POWER_LOCK, PIN_BTN_AI, PIN_EPD_EN, PIN_TOUCH_EN,
                        PIN_TOUCH_RST, PIN_MIC_EN, PIN_SD_EN, PIN_BUZZER, PIN_CHARGE_EN_N};
    for (int pin : held) {
        gpio_hold_dis(static_cast<gpio_num_t>(pin));
    }

    output(PIN_POWER_HOLD, 1);
    output(PIN_POWER_LOCK, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    output(PIN_CHARGE_EN_N, 0);  // allow charging over USB-C
    output(PIN_SD_CS, 1);        // keep the SD card off the shared SPI bus
    output(PIN_SD_EN, 1);

    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_1;
    bus.sda_io_num = static_cast<gpio_num_t>(PIN_SENSOR_SDA);
    bus.scl_io_num = static_cast<gpio_num_t>(PIN_SENSOR_SCL);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = 1;
    return i2c_new_master_bus(&bus, &s_sensor_bus);
}

i2c_master_bus_handle_t board_sensor_bus()
{
    return s_sensor_bus;
}

void board_deep_sleep()
{
    const gpio_num_t wake = static_cast<gpio_num_t>(PIN_BTN_AI);
    // Wait for the AI button to be released, or it would wake us at once.
    while (gpio_get_level(wake) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(60));

    hold(PIN_POWER_HOLD, 1);
    hold(PIN_POWER_LOCK, 1);
    hold(PIN_CHARGE_EN_N, 0);
    hold(PIN_EPD_EN, 0);
    hold(PIN_TOUCH_EN, 0);
    hold(PIN_TOUCH_RST, 0);
    hold(PIN_MIC_EN, 0);
    hold(PIN_SD_EN, 0);
    hold(PIN_BUZZER, 0);

    gpio_hold_dis(wake);
    gpio_set_direction(wake, GPIO_MODE_INPUT);
    gpio_pullup_en(wake);
    gpio_pulldown_dis(wake);
    gpio_hold_en(wake);
    esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BTN_AI, ESP_EXT1_WAKEUP_ANY_LOW);
    gpio_deep_sleep_hold_en();

    ESP_LOGI(kTag, "Entering deep sleep; press AI to wake");
    std::fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}
