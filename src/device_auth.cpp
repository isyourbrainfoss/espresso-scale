#include "device_auth.h"

#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>

#include "config.h"

namespace device_auth {
namespace {

constexpr const char* kNvsNs = "auth";
constexpr const char* kNvsPw = "pw";

// 31 symbols, no 0/O, 1/l/I to keep the OLED readout unambiguous.
constexpr char kAlphabet[] = "23456789abcdefghjkmnpqrstuvwxyz";
constexpr size_t kAlphabetLen = sizeof(kAlphabet) - 1;  // 31
constexpr size_t kGroups = 3;
constexpr size_t kGroupLen = 4;  // 12 symbols ≈ 59 bits of entropy

String g_password;
String g_ap_ssid;
bool g_generated = false;
bool g_ready = false;

char randomSymbol() {
  // Rejection sampling: 248 = 8 * 31, so (b % 31) is unbiased for b < 248.
  for (;;) {
    uint8_t b = 0;
    esp_fill_random(&b, 1);
    if (b < 248) return kAlphabet[b % kAlphabetLen];
  }
}

String generatePassword() {
  String pw;
  pw.reserve(kGroups * (kGroupLen + 1));
  for (size_t g = 0; g < kGroups; ++g) {
    if (g) pw += '-';
    for (size_t i = 0; i < kGroupLen; ++i) pw += randomSymbol();
  }
  return pw;
}

bool looksValid(const String& pw) {
  // Must be usable as a WPA2 passphrase (8..63 printable ASCII).
  if (pw.length() < 8 || pw.length() > 63) return false;
  for (size_t i = 0; i < pw.length(); ++i) {
    if (pw[i] < 0x21 || pw[i] > 0x7e) return false;
  }
  return true;
}

}  // namespace

void begin() {
  if (g_ready) return;

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char ssid[33];
  snprintf(ssid, sizeof(ssid), "%s-%02X%02X", kApSsidPrefix, mac[4], mac[5]);
  g_ap_ssid = ssid;

  Preferences prefs;
  if (prefs.begin(kNvsNs, true)) {
    g_password = prefs.getString(kNvsPw, "");
    prefs.end();
  }
  if (!looksValid(g_password)) {
    g_password = generatePassword();
    g_generated = true;
    if (prefs.begin(kNvsNs, false)) {
      prefs.putString(kNvsPw, g_password);
      prefs.end();
    } else {
      Serial.println("[auth] WARNING: could not persist device password to NVS");
    }
  }
  g_ready = true;
}

const String& password() { return g_password; }
bool generatedThisBoot() { return g_generated; }
const String& apSsid() { return g_ap_ssid; }

void reset() {
  Preferences prefs;
  if (prefs.begin(kNvsNs, false)) {
    prefs.clear();
    prefs.end();
  }
}

bool equalsConstantTime(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) {
    diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
  }
  return diff == 0;
}

String randomHex(size_t bytes) {
  static const char hex[] = "0123456789abcdef";
  String out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    uint8_t b = 0;
    esp_fill_random(&b, 1);
    out += hex[b >> 4];
    out += hex[b & 0x0f];
  }
  return out;
}

}  // namespace device_auth
