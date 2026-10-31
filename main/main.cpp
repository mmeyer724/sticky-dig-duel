// Dig Duel firmware for the Seeed reTerminal Sticky.
//
// One event loop owns the game. Touch, buttons and the radio run in their own
// tasks and post events here; the loop applies them, ticks the game and
// refreshes the e-paper panel when the game asks for it.

#include <cstdio>
#include <cstring>

#include "board.h"
#include "canvas.h"
#include "epd.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "game.h"
#include "inputs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "radio.h"
#include "touch.h"

namespace {

constexpr char kTag[] = "dig_duel";
constexpr int kPartialsBeforeFull = 20;  // periodic full refresh clears ghosting
constexpr uint32_t kBatteryCacheMs = 30 * 1000;

struct Event {
    enum class Type : uint8_t { Tap, Button, Frame } type;
    int16_t x;
    int16_t y;
    Button button;
    bool long_press;
    Mac from;
    int8_t rssi;
    uint8_t len;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
};

QueueHandle_t s_events = nullptr;

void post(const Event &e)
{
    xQueueSend(s_events, &e, 0);  // drop when full; the link retransmits
}

void on_tap(int x, int y)
{
    Event e = {};
    e.type = Event::Type::Tap;
    e.x = static_cast<int16_t>(x);
    e.y = static_cast<int16_t>(y);
    post(e);
}

void on_button(Button button, bool long_press)
{
    Event e = {};
    e.type = Event::Type::Button;
    e.button = button;
    e.long_press = long_press;
    post(e);
}

void on_frame(const Mac &from, const uint8_t *data, size_t len, int rssi)
{
    if (len > sizeof(Event::data)) {
        return;
    }
    Event e;
    e.type = Event::Type::Frame;
    e.from = from;
    e.rssi = static_cast<int8_t>(rssi);
    e.len = static_cast<uint8_t>(len);
    std::memcpy(e.data, data, len);
    post(e);
}

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

class HardwareRandom : public dig::Random {
public:
    uint32_t next() override { return esp_random(); }
};

class Hardware : public Platform {
public:
    void play(Sound sound) override { buzzer_play(sound); }

    void save_stats(const Stats &stats) override
    {
        nvs_handle_t nvs;
        if (nvs_open("digduel", NVS_READWRITE, &nvs) != ESP_OK) {
            return;
        }
        nvs_set_u16(nvs, "wins", stats.wins);
        nvs_set_u16(nvs, "losses", stats.losses);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    int battery_percent() override
    {
        const uint32_t now = now_ms();
        if (battery_ < 0 || now - battery_read_ > kBatteryCacheMs) {
            battery_ = ::battery_percent();
            battery_read_ = now;
        }
        return battery_;
    }

    static Stats load_stats()
    {
        Stats stats = {};
        nvs_handle_t nvs;
        if (nvs_open("digduel", NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u16(nvs, "wins", &stats.wins);
            nvs_get_u16(nvs, "losses", &stats.losses);
            nvs_close(nvs);
        }
        return stats;
    }

private:
    int battery_ = -1;
    uint32_t battery_read_ = 0;
};

void check(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "%s failed: %s", what, esp_err_to_name(err));
    }
}

// Diagnostic screen toggled with the Down button: what the touch controller
// reports, with a crosshair where the last touch landed.
void draw_touch_test(Canvas &c, const TouchStatus &t)
{
    constexpr Color B = Color::Black;
    c.clear(Color::White);
    c.text(64, 14, "TOUCH TEST", font_large(), B);
    char line[96];
    if (t.found) {
        std::snprintf(line, sizeof(line), "Controller: GT911 at 0x%02X, %ux%u", t.address,
                      t.sensor_w, t.sensor_h);
    } else {
        std::snprintf(line, sizeof(line), "Controller: not found, retrying");
    }
    c.text(64, 82, line, font_small(), B);
    std::snprintf(line, sizeof(line), "Reads %lu, errors %lu, resets %lu",
                  static_cast<unsigned long>(t.reads), static_cast<unsigned long>(t.errors),
                  static_cast<unsigned long>(t.resets));
    c.text(64, 110, line, font_small(), B);
    std::snprintf(line, sizeof(line), "Touches %lu, taps %lu, ignored %lu",
                  static_cast<unsigned long>(t.touches), static_cast<unsigned long>(t.taps),
                  static_cast<unsigned long>(t.ignored));
    c.text(64, 138, line, font_small(), B);
    if (t.touches > 0) {
        std::snprintf(line, sizeof(line), "Last: raw %d,%d  screen %d,%d", t.raw_x, t.raw_y, t.x,
                      t.y);
    } else {
        std::snprintf(line, sizeof(line), "Last: no touch yet");
    }
    c.text(64, 166, line, font_small(), B);
    c.text(64, 206, "Tap each square. The cross should land on it.", font_small(), B);
    c.text(64, 234, "Press DOWN to return to the game.", font_small(), B);

    const Rect targets[] = {
        {0, 0, 44, 44}, {756, 0, 44, 44}, {0, 436, 44, 44}, {756, 436, 44, 44}, {378, 310, 44, 44},
    };
    for (const Rect &r : targets) {
        c.frame(r, 3, B);
    }
    if (t.touches > 0) {
        c.line(t.x - 34, t.y, t.x + 34, t.y, 3, B);
        c.line(t.x, t.y - 34, t.x, t.y + 34, 3, B);
        c.ring(t.x, t.y, 14, 3, B);
    }
}

}  // namespace

extern "C" void app_main()
{
    ESP_LOGI(kTag, "Dig Duel %s", esp_app_get_description()->version);

    // Latch board power first: on battery the board only stays on while the
    // power button is held until this runs.
    ESP_ERROR_CHECK(board_init());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    check(err, "nvs");
    ESP_ERROR_CHECK(epd_init());

    s_events = xQueueCreate(32, sizeof(Event));
    check(buzzer_init(), "buzzer");
    check(battery_init(), "battery");
    check(buttons_init(on_button), "buttons");
    check(touch_init(on_tap), "touch");

    static EspNowRadio radio;
    ESP_ERROR_CHECK(radio.init(on_frame));

    static HardwareRandom rng;
    static Hardware hardware;
    uint8_t *pixels =
        static_cast<uint8_t *>(heap_caps_malloc(Canvas::kBytes, MALLOC_CAP_SPIRAM));
    ESP_ERROR_CHECK(pixels != nullptr ? ESP_OK : ESP_ERR_NO_MEM);
    Canvas canvas(pixels);

    Game *game = new Game(radio, rng, hardware, radio.self(), esp_random() | 1U,
                          Hardware::load_stats());
    game->start(now_ms());

    static const char *const kPhaseNames[] = {"lobby", "bury", "play", "over"};
    Game::Phase logged_phase = game->phase();
    int partials = 0;
    bool touch_test = false;
    bool test_needs_full = false;
    TouchStatus test_shown = {};
    uint32_t test_drawn_at = 0;
    while (true) {
        Event e;
        if (xQueueReceive(s_events, &e, pdMS_TO_TICKS(50)) == pdTRUE) {
            do {
                const uint32_t now = now_ms();
                switch (e.type) {
                case Event::Type::Tap:
                    ESP_LOGI(kTag, "tap %d,%d", e.x, e.y);
                    if (!touch_test) {
                        game->tap(e.x, e.y, now);
                    }
                    break;
                case Event::Type::Button:
                    ESP_LOGI(kTag, "button %d%s", static_cast<int>(e.button),
                             e.long_press ? " (long)" : "");
                    if (e.button == Button::Down && !e.long_press) {
                        touch_test = !touch_test;
                        test_needs_full = touch_test;
                        if (!touch_test) {
                            game->redraw_all();
                        }
                    } else {
                        game->button(e.button, e.long_press, now);
                    }
                    break;
                case Event::Type::Frame:
                    game->receive(e.from, e.data, e.len, e.rssi, now);
                    break;
                }
            } while (xQueueReceive(s_events, &e, 0) == pdTRUE);
        }

        const uint32_t now = now_ms();
        game->tick(now);
        if (game->phase() != logged_phase) {
            logged_phase = game->phase();
            ESP_LOGI(kTag, "phase %s, game %lu: %s", kPhaseNames[static_cast<int>(logged_phase)],
                     static_cast<unsigned long>(game->game_no()), game->status());
        }

        if (game->wants_sleep(now)) {
            touch_stop();
            game->render_sleep(canvas);
            epd_show(pixels, true);
            epd_sleep();
            board_deep_sleep();
        }

        if (touch_test) {
            // The game keeps ticking and talking to its peer but doesn't draw.
            const TouchStatus t = touch_status();
            const bool moved = t.touches != test_shown.touches || t.found != test_shown.found ||
                               t.resets != test_shown.resets;
            const bool counters = t.reads != test_shown.reads || t.errors != test_shown.errors;
            const uint32_t since = now - test_drawn_at;
            if (test_needs_full || (moved && since >= 300) || (counters && since >= 5000)) {
                draw_touch_test(canvas, t);
                check(epd_show(pixels, test_needs_full), "display");
                test_shown = t;
                test_needs_full = false;
                test_drawn_at = now_ms();
            }
            continue;
        }

        const Redraw redraw = game->redraw_needed(now);
        if (redraw != Redraw::None) {
            game->render(canvas, now);
            const bool full = redraw == Redraw::Full || ++partials >= kPartialsBeforeFull;
            if (full) {
                partials = 0;
            }
            check(epd_show(pixels, full), "display");
        }
    }
}
