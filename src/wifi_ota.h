#pragma once

#include <Arduino.h>
#include <functional>

// WiFi STA (NVS credentials) + SoftAP setup portal + web UI + ArduinoOTA.
// Hostname / mDNS: half-decent.local (see config.h)

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
  void update();

  WifiMode mode() const { return mode_; }
  bool isStation() const { return mode_ == WifiMode::Station; }
  bool isAccessPoint() const { return mode_ == WifiMode::AccessPoint; }
  bool otaInProgress() const { return ota_active_; }

  // "192.168.x.x" or "AP 192.168.4.1" style short status; empty if off.
  String ipString() const;
  const char* modeLabel() const;

  // Serial / UI helpers
  bool saveCredentials(const String& ssid, const String& pass);
  void clearCredentials();
  void printStatus() const;
  bool startSetupAp();  // force SoftAP portal
  void setOtaActive(bool on) { ota_active_ = on; }
  void scanNetworks();  // print 2.4 GHz scan to serial
  bool tryConnectSaved();  // re-attempt STA with NVS creds

 private:
  WifiMode mode_ = WifiMode::Off;
  bool ota_active_ = false;
  WeightFn weight_fn_;
  String ssid_;
  uint32_t last_reconnect_ms_ = 0;

  bool loadCredentials(String& ssid, String& pass);
  bool connectSta(const String& ssid, const String& pass);
  void startApPortal();
  void startServices();  // mDNS, web, ArduinoOTA (once per mode)
  void stopServices();
  bool services_started_ = false;
};
