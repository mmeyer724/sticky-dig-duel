#pragma once

#include "esp_err.h"
#include "game.h"

using ButtonHandler = void (*)(Button button, bool long_press);

// Polls the three active-low buttons. Short presses report on release; a
// long press (2 s) reports while still held. A button already held at boot
// (the AI press that woke the board) is ignored until released.
esp_err_t buttons_init(ButtonHandler on_button);

esp_err_t buzzer_init();
// Queues a sound; returns immediately.
void buzzer_play(Sound sound);

// Battery state of charge from the BQ27220 fuel gauge, or -1.
esp_err_t battery_init();
int battery_percent();
