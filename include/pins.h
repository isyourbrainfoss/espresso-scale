#pragma once

// Breadboard pin map — ESP32-S3 Super Mini

// SSD1306 OLED (I2C)
static constexpr int PIN_OLED_SDA = 8;
static constexpr int PIN_OLED_SCL = 9;

// HX711 load cell amplifier
static constexpr int PIN_HX711_DOUT = 6;
static constexpr int PIN_HX711_SCK = 7;

// TTP223 capacitive touch buttons (active high)
static constexpr int PIN_BTN_TARE = 4;
static constexpr int PIN_BTN_TIMER = 5;

// Audio buzzer
static constexpr int PIN_BUZZER = 10;

// ESP32-S3 Super Mini onboard addressable LED (WS2812 / “RGB”) on many clones.
// Driving a single 0 bit / digital LOW before sleep reduces glow. A hardwired
// red “power” LED (if present) may still light whenever 3.3 V is up.
static constexpr int PIN_BOARD_RGB = 48;

// Optional battery voltage ADC (-1 = USB-only breadboard, no sense wire)
static constexpr int PIN_BAT_ADC = -1;
