#pragma once

#include <Arduino.h>

struct DisplayState {
  float weight_g = 0;
  float flow_g_s = 0;
  uint32_t timer_ms = 0;
  bool timer_running = false;
  bool ble_connected = false;
  bool ble_advertising = true;
  bool app_mode = false;
  const char* status = nullptr;
  bool scale_ok = true;
  const char* wifi_label = nullptr;
  bool ota_active = false;
  const char* battery_label = nullptr;  // "USB" or "87%"
  bool standby = false;
};

class Display {
 public:
  bool begin();
  void showSplash();
  void render(const DisplayState& s);
  void setPowerSave(bool on);  // OLED off for standby
  bool powerSave() const { return power_save_; }

 private:
  bool ready_ = false;
  bool power_save_ = false;
};
