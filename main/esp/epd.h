#pragma once

#include <cstdint>

#include "esp_err.h"

// SSD1677 800x480 black-and-white e-paper panel.
//
// Frames are 1 bit per pixel, MSB first, 100 bytes per row, 1 = white, in the
// physical landscape orientation used by Canvas.

esp_err_t epd_init();

// Full refresh flashes the panel and clears ghosting (about 2 s). Partial
// refresh only redraws changed pixels and is faster, but leaves faint
// ghosting over time. The first refresh after boot is always full.
esp_err_t epd_show(const uint8_t *frame, bool full);

// Puts the controller into deep sleep and removes panel power. The image
// stays on screen.
esp_err_t epd_sleep();

// True while a refresh is running. Read from the touch task.
bool epd_refreshing();
