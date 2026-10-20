#pragma once

#include <Arduino.h>
#include <functional>

// WiFi STA (NVS credentials) + SoftAP setup portal + web UI + ArduinoOTA.
// Hostname / mDNS: half-decent.local (see config.h)
//
// Security model:
// - A random per-device password is generated on first boot and kept in NVS.
//   It is the WPA2 key of the setup AP, the HTTP Digest password (user
//   kWebUser) for every web page, and the ArduinoOTA password.
// - Every state-changing POST needs a single-use CSRF token issued with the
//   form, plus a same-origin Origin/Referer header.
// - The setup AP only starts on first boot without saved credentials, or on
//   request (Timer very-long press / serial "wifi ap"), and closes after
//   kSetupApTimeoutMs without a connected station. A failed STA join keeps
//   retrying STA instead of opening the AP.

enum class WifiMode : uint8_t {
  Off = 0,
  Connecting,
  Station,
  AccessPoint,
};

class WifiOta {
 public:
  // Optional: latest weight for status page (set each notify tick).
  using WeightFn = std::function<float()>;

  bool begin(WeightFn weight_fn = nullptr);
  void end();  // WiFi off before deep sleep
  void update();

  WifiMode mode() const { return mode_; }
  bool isStation() const { return mode_ == WifiMode::Station; }
  bool isAccessPoint() const { return mode_ == WifiMode::AccessPoint; }
  bool otaInProgress() const { return ota_active_; }

  // "192.168.x.x" or "AP 192.168.4.1" style short status; empty if off.
  String ipString() const;
  const char* modeLabel() const;
  const String& apSsid() const { return ap_ssid_; }

  // Per-device password (generated on first boot). Show it only on the
  // OLED; use maskedPassword() for logs.
  const String& devicePassword() const { return device_password_; }
  String maskedPassword() const;
  // Generate and store a new device password (takes effect after reboot).
  bool regeneratePassword();

  // SSID 1–32 bytes, no control chars; password empty (open network) or
  // 8–63 printable ASCII. On failure *why is set to a short reason.
  static bool validateCredentials(const String& ssid, const String& pass,
                                  const char** why = nullptr);

  // Serial / UI helpers
  bool saveCredentials(const String& ssid, const String& pass);
  void clearCredentials();
  void printStatus() const;
  bool startSetupAp();  // force SoftAP portal (with timeout)
  void setOtaActive(bool on) { ota_active_ = on; }
  void scanNetworks();  // print 2.4 GHz scan to serial
  bool tryConnectSaved();  // re-attempt STA with NVS creds

 private:
  WifiMode mode_ = WifiMode::Off;
  bool ota_active_ = false;
  WeightFn weight_fn_;
  String ssid_;
  String ap_ssid_;
  String device_password_;
  uint32_t last_reconnect_ms_ = 0;
  uint32_t ap_last_activity_ms_ = 0;

  bool loadCredentials(String& ssid, String& pass);
  bool loadOrCreatePassword();
  bool connectSta(const String& ssid, const String& pass);
  void startApPortal();
  void startServices();  // mDNS, web, ArduinoOTA (once per mode)
  void stopServices();
  bool services_started_ = false;
};
