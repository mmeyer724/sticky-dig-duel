#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// Latches board power, enables battery charging and creates the sensor I2C bus.
esp_err_t board_init();

i2c_master_bus_handle_t board_sensor_bus();

// Powers peripherals down and sleeps until the AI button is pressed. The
// e-paper image stays visible. Waking restarts the firmware.
[[noreturn]] void board_deep_sleep();
