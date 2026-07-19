#include "battery.h"

#include "config.h"
#include "pins.h"

void Battery::begin() {
  if (PIN_BAT_ADC >= 0) {
    analogReadResolution(12);
    pinMode(PIN_BAT_ADC, INPUT);
  }
  refresh();
  Serial.printf("[battery] %s (%d%%)\n", label_, percent_);
}

void Battery::update() {
  const uint32_t now = millis();
  if (now - last_ms_ < 1000) return;
  last_ms_ = now;
  refresh();
}

void Battery::refresh() {
  if (PIN_BAT_ADC < 0) {
    // No sense line on this breadboard — always USB powered.
    usb_ = true;
    percent_ = 100;
    snprintf(label_, sizeof(label_), "USB");
    return;
  }

  // Optional divider: ADC → battery volts (configure in pins.h / config.h)
  const int raw = analogRead(PIN_BAT_ADC);
  const float v_adc = (raw / 4095.0f) * 3.3f;
  const float v_bat = v_adc * kBatVoltageDivider;
  // LiPo approx 3.3–4.2 V
  float pct = (v_bat - 3.3f) / (4.2f - 3.3f) * 100.0f;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  percent_ = static_cast<int>(pct + 0.5f);

  // Treat high voltage as USB/charging rail
  usb_ = (v_bat > kBatUsbThresholdV);
  if (usb_) {
    snprintf(label_, sizeof(label_), "USB");
    if (percent_ < 100) percent_ = 100;
  } else {
    snprintf(label_, sizeof(label_), "%d%%", percent_);
  }
}

uint8_t Battery::decentBatteryByte() const {
  if (usb_) return 0xFF;
  if (percent_ < 3) return 3;
  if (percent_ > 100) return 100;
  return static_cast<uint8_t>(percent_);
}
