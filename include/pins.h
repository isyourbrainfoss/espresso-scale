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

// Passive buzzer
static constexpr int PIN_BUZZER = 10;
