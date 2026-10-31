#pragma once

// reTerminal Sticky pin map, from Seeed's hardware overview
// (https://www.seeedstudio.com/sticky/docs/en/device-guide/hardware-overview)
// and the Sticky ESP-IDF demo.

// Power latch: the board stays on from battery only while these are high.
#define PIN_POWER_HOLD 45
#define PIN_POWER_LOCK 46
#define PIN_CHARGE_EN_N 39  // BQ25616 charge enable, active low

// Buttons, active low. The AI button also wakes the board from deep sleep.
#define PIN_BTN_AI 4
#define PIN_BTN_UP 5
#define PIN_BTN_DOWN 6

#define PIN_BUZZER 48

// Sensor I2C bus (BQ27220 fuel gauge, RTC, SHT40, IMU)
#define PIN_SENSOR_SCL 0
#define PIN_SENSOR_SDA 1
#define BQ27220_ADDR 0x55

// GT911 touch controller on its own I2C bus
#define PIN_TOUCH_SCL 2
#define PIN_TOUCH_SDA 3
#define PIN_TOUCH_INT 21
#define PIN_TOUCH_RST 41
#define PIN_TOUCH_EN 42

// SSD1677 e-paper panel on SPI2
#define PIN_EPD_MOSI 14
#define PIN_EPD_CLK 13
#define PIN_EPD_CS 15
#define PIN_EPD_DC 16
#define PIN_EPD_RST 17
#define PIN_EPD_BUSY 18
#define PIN_EPD_EN 47

// MicroSD shares SPI2; it is unused here but must stay deselected.
#define PIN_SD_CS 8
#define PIN_SD_EN 10

// PDM microphone power, unused here.
#define PIN_MIC_EN 38
