#pragma once

#include <cstdint>

#include "esp_err.h"

using TapHandler = void (*)(int x, int y);

// How the GT911 was brought up. The datasheet reset selects the I2C address
// with the INT pin; a power cycle leaves reset alone and lets the chip boot
// by itself.
enum class TouchStart : uint8_t { ResetAt5D, ResetAt14, PowerCycle };
constexpr int kTouchStarts = 3;
const char *touch_start_name(TouchStart start);

// Live counters for the on-screen touch test and the serial log.
struct TouchStatus {
    bool found;            // GT911 answered on I2C
    bool healthy;          // ...and reported a product id and resolution
    TouchStart start;
    uint8_t address;
    char product_id[5];    // "911" on a working GT911
    uint16_t firmware;
    uint8_t config_version;
    uint16_t sensor_w;     // controller's reported resolution (0 if blank)
    uint16_t sensor_h;
    uint32_t reads;        // status polls that succeeded
    uint32_t frames;       // ...of which carried a new report
    uint32_t errors;       // status polls that failed
    uint32_t resets;       // controller re-detections
    uint8_t last_status;   // last non-zero status byte
    uint32_t touches;      // finger-down events
    uint32_t taps;         // taps handed to the game
    uint32_t ignored;      // taps dropped because the panel was refreshing
    int raw_x;             // last touch, controller coordinates
    int raw_y;
    int x;                 // last touch, screen coordinates
    int y;
};

// Powers the GT911 touch controller and starts a task that brings it up
// (retrying until it answers) and reports a tap each time a finger lands. A
// touch that lands during an e-paper refresh is ignored so it can't act on a
// stale screen.
esp_err_t touch_init(TapHandler on_tap);

// Stops polling before the controller loses power for deep sleep.
void touch_stop();

TouchStatus touch_status();

// Restarts the controller using the next start method (touch test, Up button).
void touch_try_next_start();
