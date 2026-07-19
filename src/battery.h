#pragma once

#include <Arduino.h>

// Battery / power status for OLED + Decent BLE LED-ack byte.
// Breadboard is USB-powered with no divider by default → reports USB / 100%.

class Battery {
 public:
  void begin();
  void update();  // call occasionally (e.g. 1 Hz)

  // 0–100, or 100 when USB with no ADC.
  int percent() const { return percent_; }
  bool isUsb() const { return usb_; }
  // Decent Scale LED-ack battery byte: 0xFF = USB, else 3–100.
  uint8_t decentBatteryByte() const;

  // Short label for OLED, e.g. "USB", "87%"
  const char* label() const { return label_; }

 private:
  int percent_ = 100;
  bool usb_ = true;
  char label_[8] = "USB";
  uint32_t last_ms_ = 0;

  void refresh();
};
