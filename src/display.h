#pragma once

#include <Arduino.h>

struct DisplayState {
  float weight_g = 0;
  float flow_g_s = 0;
  uint32_t timer_ms = 0;
  bool timer_running = false;
  bool ble_connected = false;
  bool ble_advertising = true;
  bool app_mode = false;  // LED-on / app connected stream active
  const char* status = nullptr;  // optional one-line status (cal, error)
  bool scale_ok = true;
};

class Display {
 public:
  bool begin();
  void showSplash();
  void render(const DisplayState& s);

 private:
  bool ready_ = false;
};
