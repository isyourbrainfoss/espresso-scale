#pragma once

#include <Arduino.h>

// Per-device secret used for:
//   * the setup access point (WPA2 passphrase)
//   * the web UI (HTTP Digest auth, user "admin")
//   * ArduinoOTA (espota --auth=...)
//
// Generated from the hardware RNG on first boot and stored in NVS
// (Preferences namespace "auth"). It is never compiled into the firmware.
namespace device_auth {

// Load the password from NVS, or generate + persist a new one.
// Call after the radio (BLE or Wi-Fi) is up so esp_random() is backed by
// RF noise (true RNG). Safe to call more than once.
void begin();

// Current per-device password, e.g. "k7mq-29xh-tp4c". Empty before begin().
const String& password();

// True only on the boot where the password was freshly generated
// (first boot, after `auth reset`, or after flash erase).
bool generatedThisBoot();

// Unique per-device setup SSID, e.g. "HalfDecent-3FA2" (from the MAC).
const String& apSsid();

// Web UI username for HTTP Digest auth.
constexpr const char* kWebUser = "admin";

// Erase the stored password; a new one is generated on next boot.
void reset();

// Constant-time string compare (for tokens / secrets).
bool equalsConstantTime(const String& a, const String& b);

// Random lowercase-hex string of 2*bytes characters (hardware RNG).
String randomHex(size_t bytes);

}  // namespace device_auth
