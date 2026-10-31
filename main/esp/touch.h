#pragma once

#include <cstdint>

#include "esp_err.h"

using TapHandler = void (*)(int x, int y);

// Live counters for the on-screen touch test and the serial log.
struct TouchStatus {
    bool found;         // GT911 answered on I2C
    uint8_t address;
    uint16_t sensor_w;  // controller's reported resolution
    uint16_t sensor_h;
    uint32_t reads;     // status polls that succeeded
    uint32_t errors;    // status polls that failed
    uint32_t resets;    // controller re-detections
    uint32_t touches;   // finger-down events
    uint32_t taps;      // taps handed to the game
    uint32_t ignored;   // taps dropped because the panel was refreshing
    int raw_x;          // last touch, controller coordinates
    int raw_y;
    int x;              // last touch, screen coordinates
    int y;
};

// Powers the GT911 touch controller and starts a task that finds it (retrying
// until it answers) and reports a tap each time a finger lands. A touch that
// lands during an e-paper refresh is ignored so it can't act on a stale screen.
esp_err_t touch_init(TapHandler on_tap);

// Stops polling before the controller loses power for deep sleep.
void touch_stop();

TouchStatus touch_status();
