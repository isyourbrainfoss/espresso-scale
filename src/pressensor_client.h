#pragma once

#include <Arduino.h>
#include <cstdint>

// BLE central client for Pressensor PRS (same UUIDs as Flowlog).
// Dual-role: runs alongside Decent Scale peripheral on NimBLE.

class PressensorClient {
 public:
  bool begin();
  void end();
  void update();  // reconnect / scan retry

  // Request connect to first advertising PRS* device.
  void requestConnect();
  void disconnect();

  bool isConnected() const { return connected_; }
  bool isScanning() const { return scanning_; }
  bool hasReading() const { return has_reading_; }
  float pressureBar() const { return pressure_bar_; }
  float tempC() const { return temp_c_; }
  bool hasTemp() const { return has_temp_; }

  // Called from NimBLE callbacks (internal).
  void onNotify(const uint8_t* data, size_t len);
  void onLinkLost();
  void setScanning(bool on) { scanning_ = on; }
  void setConnected(bool on) { connected_ = on; }

 private:
  bool connected_ = false;
  bool scanning_ = false;
  bool connect_requested_ = false;
  bool has_reading_ = false;
  bool has_temp_ = false;
  float pressure_bar_ = 0;
  float temp_c_ = 0;
  uint32_t last_connect_try_ms_ = 0;

  bool startScan();
  void stopScan();
};
