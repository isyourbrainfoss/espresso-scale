#pragma once

#include <Arduino.h>
#include <functional>

// WiFi STA (NVS credentials) + SoftAP setup portal + web UI + ArduinoOTA.
// Hostname / mDNS: half-decent.local (see config.h)
//
// Security model (see README "Wi-Fi security"):
//   * One per-device random password (device_auth) protects the setup AP
//     (WPA2), the web UI (HTTP Digest, user "admin") and ArduinoOTA.
//   * POST forms require a per-session CSRF token and a same-origin
//     Origin/Referer; the Host header must be this device (anti DNS-rebind).
//   * The setup AP only opens on first boot with no saved network, on the
//     Timer-hold gesture, or via the USB serial `wifi ap` command, and it
//     closes itself after kSetupApIdleTimeoutMs of inactivity.

enum class WifiMode : uint8_t {
  Off = 0,
  Connecting,
  Station,
  AccessPoint,
  Retrying,  // saved network unreachable; retrying STA in background
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

  // "192.168.x.x" style address; empty if off.
  String ipString() const;
  const char* modeLabel() const;

  // Serial / UI helpers
  bool saveCredentials(const String& ssid, const String& pass);
  void clearCredentials();
  void printStatus() const;
  bool startSetupAp();   // open SoftAP portal (explicit user action only)
  void closeSetupAp();   // close portal; resume STA if creds saved
  void setOtaActive(bool on) { ota_active_ = on; }
  void scanNetworks();     // print 2.4 GHz scan to serial
  bool tryConnectSaved();  // re-attempt STA with NVS creds (no AP fallback)

  // OLED "setup / password" screen: shown for kSetupScreenMs after it is
  // requested (first boot, Timer hold, AP open) and while a phone is joined
  // to the setup AP.
  void requestSetupScreen();
  bool setupScreenActive() const;
  // Seconds until the setup AP auto-closes (0 if not in AP mode).
  uint32_t apSecondsLeft() const;
  // Called by the web server on every request (keeps the AP alive).
  void noteActivity();

  // SSID 1..32 bytes; passphrase empty (open network) or 8..63 printable
  // ASCII (WPA2-PSK). Returns false and fills *err on failure.
  static bool validateCredentials(const String& ssid, const String& pass,
                                  String* err);

 private:
  WifiMode mode_ = WifiMode::Off;
  bool ota_active_ = false;
  WeightFn weight_fn_;
  String ssid_;
  uint32_t last_reconnect_ms_ = 0;
  uint32_t ap_last_activity_ms_ = 0;
  uint32_t setup_screen_until_ms_ = 0;

  bool loadCredentials(String& ssid, String& pass);
  bool connectSta(const String& ssid, const String& pass);
  void startStaBackground(const String& ssid, const String& pass);
  void startApPortal();
  void startServices();  // mDNS, web, ArduinoOTA (once per mode)
  void stopServices();
  bool services_started_ = false;
};
