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

  // Wi-Fi setup / device password screen (takes over the OLED when set).
  bool setup_screen = false;
  bool setup_ap = false;               // setup AP open (else STA info)
  const char* setup_ssid = nullptr;    // AP SSID, or joined network
  const char* setup_pass = nullptr;    // per-device password
  const char* setup_ip = nullptr;
  uint32_t setup_secs_left = 0;        // AP idle timeout countdown
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
