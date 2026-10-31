#include "inputs.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pins.h"

// ---------------------------------------------------------------- buttons

namespace {

constexpr int kLongPressMs = 2000;
constexpr int kDebounceMs = 30;

struct ButtonState {
    int pin;
    Button button;
    bool pressed;
    bool ignore_until_release;
    bool long_sent;
    int64_t changed_us;
};

ButtonState s_buttons[] = {
    {PIN_BTN_AI, Button::Ai, false, false, false, 0},
    {PIN_BTN_UP, Button::Up, false, false, false, 0},
    {PIN_BTN_DOWN, Button::Down, false, false, false, 0},
};
ButtonHandler s_on_button = nullptr;

void buttons_task(void *)
{
    while (true) {
        const int64_t now = esp_timer_get_time();
        for (ButtonState &b : s_buttons) {
            const bool level_pressed = gpio_get_level(static_cast<gpio_num_t>(b.pin)) == 0;
            const int64_t held_ms = (now - b.changed_us) / 1000;
            if (level_pressed != b.pressed && held_ms >= kDebounceMs) {
                b.pressed = level_pressed;
                b.changed_us = now;
                if (!b.pressed) {
                    if (!b.ignore_until_release && !b.long_sent) {
                        s_on_button(b.button, false);
                    }
                    b.ignore_until_release = false;
                    b.long_sent = false;
                }
            } else if (b.pressed && !b.long_sent && !b.ignore_until_release &&
                       held_ms >= kLongPressMs) {
                b.long_sent = true;
                s_on_button(b.button, true);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

esp_err_t buttons_init(ButtonHandler on_button)
{
    s_on_button = on_button;
    gpio_config_t config = {};
    config.pin_bit_mask = (1ULL << PIN_BTN_AI) | (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN);
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t now = esp_timer_get_time();
    for (ButtonState &b : s_buttons) {
        b.pressed = gpio_get_level(static_cast<gpio_num_t>(b.pin)) == 0;
        b.ignore_until_release = b.pressed;
        b.changed_us = now;
    }
    return xTaskCreate(buttons_task, "buttons", 3072, nullptr, 5, nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

// ---------------------------------------------------------------- buzzer

namespace {

struct Note {
    uint16_t hz;  // 0 = rest
    uint16_t ms;
};

struct Tune {
    const Note *notes;
    int count;
};

constexpr Note kTap[] = {{2400, 25}};
constexpr Note kError[] = {{320, 120}};
constexpr Note kPaired[] = {{1319, 80}, {1760, 80}, {2637, 140}};
constexpr Note kYourTurn[] = {{1760, 90}, {0, 50}, {2349, 150}};
constexpr Note kBone[] = {{2093, 60}, {2637, 100}};
constexpr Note kFossil[] = {{1568, 70}, {2093, 70}, {2637, 70}, {3136, 160}};
constexpr Note kDirt[] = {{640, 70}};
constexpr Note kWin[] = {{1568, 110}, {2093, 110}, {2637, 110}, {0, 40}, {3136, 300}};
constexpr Note kLose[] = {{784, 160}, {659, 160}, {523, 320}};

#define TUNE(notes) Tune{notes, sizeof(notes) / sizeof(notes[0])}

Tune tune_for(Sound sound)
{
    switch (sound) {
    case Sound::Tap:
        return TUNE(kTap);
    case Sound::Error:
        return TUNE(kError);
    case Sound::Paired:
        return TUNE(kPaired);
    case Sound::YourTurn:
        return TUNE(kYourTurn);
    case Sound::Bone:
        return TUNE(kBone);
    case Sound::Fossil:
        return TUNE(kFossil);
    case Sound::Dirt:
        return TUNE(kDirt);
    case Sound::Win:
        return TUNE(kWin);
    case Sound::Lose:
        return TUNE(kLose);
    }
    return TUNE(kTap);
}

constexpr ledc_mode_t kSpeed = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
constexpr uint32_t kDuty = 256;  // 25% of 10-bit, as Seeed's demo uses
QueueHandle_t s_sounds = nullptr;

void buzzer_task(void *)
{
    Sound sound;
    while (xQueueReceive(s_sounds, &sound, portMAX_DELAY) == pdTRUE) {
        const Tune tune = tune_for(sound);
        for (int i = 0; i < tune.count; ++i) {
            const Note &n = tune.notes[i];
            if (n.hz != 0) {
                ledc_set_freq(kSpeed, kTimer, n.hz);
                ledc_set_duty(kSpeed, kChannel, kDuty);
                ledc_update_duty(kSpeed, kChannel);
            }
            vTaskDelay(pdMS_TO_TICKS(n.ms));
            ledc_set_duty(kSpeed, kChannel, 0);
            ledc_update_duty(kSpeed, kChannel);
        }
    }
}

}  // namespace

esp_err_t buzzer_init()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeed;
    timer.timer_num = kTimer;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.freq_hz = 2400;
    timer.clk_cfg = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t channel = {};
    channel.gpio_num = PIN_BUZZER;
    channel.speed_mode = kSpeed;
    channel.channel = kChannel;
    channel.timer_sel = kTimer;
    channel.duty = 0;
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }
    s_sounds = xQueueCreate(4, sizeof(Sound));
    if (s_sounds == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(buzzer_task, "buzzer", 2048, nullptr, 4, nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void buzzer_play(Sound sound)
{
    if (s_sounds != nullptr) {
        xQueueSend(s_sounds, &sound, 0);
    }
}

// ---------------------------------------------------------------- battery

namespace {

constexpr uint8_t kStateOfCharge = 0x2C;  // BQ27220 standard command, percent
i2c_master_dev_handle_t s_gauge = nullptr;

}  // namespace

esp_err_t battery_init()
{
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = BQ27220_ADDR;
    dev.scl_speed_hz = 100000;
    return i2c_master_bus_add_device(board_sensor_bus(), &dev, &s_gauge);
}

int battery_percent()
{
    if (s_gauge == nullptr) {
        return -1;
    }
    const uint8_t cmd = kStateOfCharge;
    uint8_t data[2];
    if (i2c_master_transmit_receive(s_gauge, &cmd, 1, data, 2, 50) != ESP_OK) {
        return -1;
    }
    const int soc = data[0] | (data[1] << 8);
    return soc <= 100 ? soc : -1;
}
