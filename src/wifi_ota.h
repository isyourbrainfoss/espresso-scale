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
  // Optional: last shot JSON for GET /shot.json (empty when none).
  using ShotJsonFn = std::function<String()>;
  // Optional: true when a standalone/phone shot is available for export.
  using HasShotFn = std::function<bool()>;

  bool begin(WeightFn weight_fn = nullptr, ShotJsonFn shot_json_fn = nullptr,
             HasShotFn has_shot_fn = nullptr);
  void end();  // WiFi off before deep sleep
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

  // Optional Nextcloud WebDAV: PUT last_shot.json after a brew.
  bool saveNextcloud(const String& base_url, const String& user,
                     const String& pass, const String& remote_path);
  void clearNextcloud();
  bool hasNextcloud() const;
  // Best-effort upload of current shot JSON. Returns true on HTTP 2xx.
  bool pushShotToNextcloud(const String& json);

 private:
  WifiMode mode_ = WifiMode::Off;
  bool ota_active_ = false;
  WeightFn weight_fn_;
  ShotJsonFn shot_json_fn_;
  HasShotFn has_shot_fn_;
  String ssid_;
  uint32_t last_reconnect_ms_ = 0;

  bool loadCredentials(String& ssid, String& pass);
  bool connectSta(const String& ssid, const String& pass);
  void startApPortal();
  void startServices();  // mDNS, web, ArduinoOTA (once per mode)
  void stopServices();
  bool services_started_ = false;
};
