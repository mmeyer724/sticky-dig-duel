#pragma once

#include "esp_err.h"

using TapHandler = void (*)(int x, int y);

// Powers the GT911 touch controller and starts a polling task that reports
// taps in screen coordinates. Touches that begin during an e-paper refresh
// are ignored, so a tap always acts on what was on screen.
esp_err_t touch_init(TapHandler on_tap);

// Stops polling before the controller loses power for deep sleep.
void touch_stop();
