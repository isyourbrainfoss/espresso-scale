#include "wifi_ota.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

namespace {
Preferences wifi_prefs;
WebServer server(80);
bool server_routes_ok = false;

constexpr const char* kNvsNs = "wifi";
constexpr const char* kNvsSsid = "ssid";
constexpr const char* kNvsPass = "pass";

WifiOta* g_wifi = nullptr;
WifiOta::WeightFn g_weight;

String htmlEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '&')
      o += F("&amp;");
    else if (c == '<')
      o += F("&lt;");
    else if (c == '>')
      o += F("&gt;");
    else if (c == '"')
      o += F("&quot;");
    else
      o += c;
  }
  return o;
}

String pageShell(const String& title, const String& body) {
  String h;
  h.reserve(body.length() + 400);
  h += F("<!DOCTYPE html><html><head><meta charset=utf-8>"
         "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
         "<title>");
  h += title;
  h += F("</title><style>"
         "body{font-family:system-ui,sans-serif;max-width:36rem;margin:1.5rem auto;padding:0 1rem;}"
         "h1{font-size:1.25rem}label{display:block;margin-top:.75rem}"
         "input,button{font-size:1rem;padding:.4rem .6rem;width:100%;box-sizing:border-box}"
         "button{margin-top:1rem;cursor:pointer}"
         ".meta{color:#555;font-size:.9rem}code{background:#f0f0f0;padding:0 .25rem}"
         "a{color:#06c}</style></head><body>");
  h += body;
  h += F("</body></html>");
  return h;
}

void handleRoot() {
  String ip = WiFi.getMode() == WIFI_AP ? WiFi.softAPIP().toString()
                                        : WiFi.localIP().toString();
  String mode = (WiFi.getMode() == WIFI_AP) ? "Access Point (setup)"
               : (WiFi.status() == WL_CONNECTED) ? "Station" : "—";
  float w = g_weight ? g_weight() : 0.f;

  String body;
  body += F("<h1>Half Decent Scale</h1>");
  body += F("<p class=meta>Firmware ");
  body += kFirmwareVersion;
  body += F(" · hostname <code>");
  body += kHostname;
  body += F(".local</code></p>");
  body += F("<p><b>Mode:</b> ");
  body += mode;
  body += F("<br><b>IP:</b> ");
  body += ip;
  body += F("<br><b>SSID:</b> ");
  body += htmlEscape(WiFi.SSID().length() ? WiFi.SSID() : String(kApSsid));
  body += F("<br><b>Weight:</b> ");
  body += String(w, 1);
  body += F(" g</p>");
  body += F("<p><a href=/wifi>Wi‑Fi setup</a> · <a href=/update>OTA update</a></p>");
  body += F("<p class=meta>PlatformIO wireless upload uses ArduinoOTA on this host "
            "(password in config). Example:<br>"
            "<code>pio run -t upload -e ota --upload-port ");
  body += ip;
  body += F("</code></p>");
  server.send(200, "text/html", pageShell("Half Decent Scale", body));
}

void handleWifiGet() {
  String body = F("<h1>Wi‑Fi setup</h1>"
                  "<form method=POST action=/wifi>"
                  "<label>SSID<input name=ssid required autocomplete=username></label>"
                  "<label>Password<input name=pass type=password autocomplete=current-password></label>"
                  "<button type=submit>Save &amp; connect</button></form>"
                  "<p><a href=/>Back</a></p>");
  server.send(200, "text/html", pageShell("Wi‑Fi setup", body));
}

void handleWifiPost() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "missing ssid");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  ssid.trim();
  if (ssid.isEmpty()) {
    server.send(400, "text/plain", "empty ssid");
    return;
  }
  if (g_wifi) g_wifi->saveCredentials(ssid, pass);
  String body = F("<h1>Saved</h1><p>Connecting to <b>");
  body += htmlEscape(ssid);
  body += F("</b>… Rebooting in 2s.</p>");
  server.send(200, "text/html", pageShell("Saved", body));
  delay(500);
  ESP.restart();
}

void handleUpdateGet() {
  String body = F("<h1>Firmware OTA</h1>"
                  "<p class=meta>Upload a <code>firmware.bin</code> from "
                  "<code>.pio/build/.../firmware.bin</code></p>"
                  "<form method=POST action=/update enctype=multipart/form-data>"
                  "<label>File<input type=file name=firmware accept=.bin required></label>"
                  "<button type=submit>Flash &amp; reboot</button></form>"
                  "<p><a href=/>Back</a></p>");
  server.send(200, "text/html", pageShell("OTA update", body));
}

void handleUpdatePost() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[ota] web start: %s\n", upload.filename.c_str());
    if (g_wifi) g_wifi->setOtaActive(true);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      if (g_wifi) g_wifi->setOtaActive(false);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[ota] web success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
      if (g_wifi) g_wifi->setOtaActive(false);
    }
  }
}

void handleUpdatePostDone() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update failed — see serial log");
  } else {
    server.send(200, "text/html",
                pageShell("OTA", F("<h1>OK</h1><p>Rebooting…</p>")));
    delay(400);
    ESP.restart();
  }
}

void setupRoutes() {
  if (server_routes_ok) return;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on(
      "/update", HTTP_POST,
      handleUpdatePostDone,
      handleUpdatePost);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.onNotFound([]() {
    // Captive portal: redirect unknown hosts to setup
    if (WiFi.getMode() == WIFI_AP) {
      server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() +
                                        "/wifi");
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "not found");
    }
  });
  server_routes_ok = true;
}
}  // namespace

bool WifiOta::loadCredentials(String& ssid, String& pass) {
  if (!wifi_prefs.begin(kNvsNs, true)) return false;
  ssid = wifi_prefs.getString(kNvsSsid, "");
  pass = wifi_prefs.getString(kNvsPass, "");
  wifi_prefs.end();
  return ssid.length() > 0;
}

bool WifiOta::saveCredentials(const String& ssid, const String& pass) {
  if (!wifi_prefs.begin(kNvsNs, false)) return false;
  wifi_prefs.putString(kNvsSsid, ssid);
  wifi_prefs.putString(kNvsPass, pass);
  wifi_prefs.end();
  ssid_ = ssid;
  Serial.printf("[wifi] saved SSID \"%s\"\n", ssid.c_str());
  return true;
}

void WifiOta::clearCredentials() {
  if (wifi_prefs.begin(kNvsNs, false)) {
    wifi_prefs.clear();
    wifi_prefs.end();
  }
  ssid_ = "";
  Serial.println("[wifi] credentials cleared");
}

bool WifiOta::connectSta(const String& ssid, const String& pass) {
  mode_ = WifiMode::Connecting;
  ssid_ = ssid;

  // ESP32-S3 is 2.4 GHz only — 5 GHz APs with the same SSID are invisible.
  WiFi.persistent(false);
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 2
  // Prefer full 2.4 GHz scan / best signal among 2.4 GHz BSS.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] connecting to \"%s\" (2.4 GHz only)…\n", ssid.c_str());

  const uint32_t t0 = millis();
  wl_status_t last = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED &&
         millis() - t0 < kWifiConnectTimeoutMs) {
    wl_status_t st = WiFi.status();
    if (st != last) {
      last = st;
      Serial.printf("[wifi] status=%d\n", static_cast<int>(st));
    }
    delay(100);
    yield();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[wifi] STA connect failed (status=%d). "
                  "ESP32 only sees 2.4 GHz — is the SSID on 2.4 GHz?\n",
                  static_cast<int>(WiFi.status()));
    return false;
  }
  mode_ = WifiMode::Station;
  Serial.printf("[wifi] STA OK %s  RSSI %d ch=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  return true;
}

void WifiOta::scanNetworks() {
  Serial.println("[wifi] scanning 2.4 GHz…");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  delay(50);
  const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n <= 0) {
    Serial.println("[wifi] no networks found");
    return;
  }
  Serial.printf("[wifi] %d network(s):\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("  %2d  ch=%2d  RSSI=%4d  %s  \"%s\"\n", i + 1,
                  WiFi.channel(i), WiFi.RSSI(i),
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "psk ",
                  WiFi.SSID(i).c_str());
  }
  WiFi.scanDelete();
}

bool WifiOta::tryConnectSaved() {
  String ssid, pass;
  if (!loadCredentials(ssid, pass)) {
    Serial.println("[wifi] no saved credentials");
    return false;
  }
  stopServices();
  if (connectSta(ssid, pass)) {
    startServices();
    return true;
  }
  startApPortal();
  return false;
}

void WifiOta::startApPortal() {
  stopServices();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid, kApPassword);
  mode_ = WifiMode::AccessPoint;
  ssid_ = kApSsid;
  Serial.printf("[wifi] AP \"%s\"  IP %s  pass \"%s\"\n", kApSsid,
                WiFi.softAPIP().toString().c_str(), kApPassword);
  startServices();
}

void WifiOta::stopServices() {
  if (!services_started_) return;
  server.stop();
  ArduinoOTA.end();
  MDNS.end();
  services_started_ = false;
}

void WifiOta::startServices() {
  if (services_started_) return;
  g_wifi = this;
  g_weight = weight_fn_;

  if (!MDNS.begin(kHostname)) {
    Serial.println("[wifi] mDNS failed");
  } else {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("arduino", "tcp", 3232);
    Serial.printf("[wifi] mDNS %s.local\n", kHostname);
  }

  setupRoutes();
  server.begin();
  Serial.println("[wifi] HTTP :80  (/  /wifi  /update)");

  ArduinoOTA.setHostname(kHostname);
  if (kOtaPassword && kOtaPassword[0] != '\0') {
    ArduinoOTA.setPassword(kOtaPassword);
  }
  ArduinoOTA.onStart([this]() {
    ota_active_ = true;
    Serial.println("[ota] ArduinoOTA start");
  });
  ArduinoOTA.onEnd([this]() {
    ota_active_ = false;
    Serial.println("\n[ota] ArduinoOTA end");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int last = -1;
    int pct = total ? (int)(progress * 100 / total) : 0;
    if (pct != last && pct % 10 == 0) {
      last = pct;
      Serial.printf("[ota] %u%%\n", pct);
    }
  });
  ArduinoOTA.onError([this](ota_error_t err) {
    ota_active_ = false;
    Serial.printf("[ota] error %u\n", (unsigned)err);
  });
  ArduinoOTA.begin();
  Serial.printf("[ota] ArduinoOTA ready (pass \"%s\")\n",
                (kOtaPassword && kOtaPassword[0]) ? kOtaPassword : "(none)");

  services_started_ = true;
}

bool WifiOta::begin(WeightFn weight_fn) {
  weight_fn_ = std::move(weight_fn);
  g_wifi = this;
  g_weight = weight_fn_;
  WiFi.persistent(false);

  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    if (connectSta(ssid, pass)) {
      startServices();
      return true;
    }
  } else {
    Serial.println("[wifi] no saved credentials");
  }
  startApPortal();
  return true;
}

bool WifiOta::startSetupAp() {
  startApPortal();
  return true;
}

void WifiOta::update() {
  if (mode_ == WifiMode::Station && WiFi.status() != WL_CONNECTED) {
    const uint32_t now = millis();
    if (now - last_reconnect_ms_ > 10000) {
      last_reconnect_ms_ = now;
      Serial.println("[wifi] STA lost — reconnecting");
      WiFi.reconnect();
    }
  }
  if (services_started_) {
    ArduinoOTA.handle();
    server.handleClient();
  }
}

String WifiOta::ipString() const {
  if (mode_ == WifiMode::Station && WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  if (mode_ == WifiMode::AccessPoint) {
    return WiFi.softAPIP().toString();
  }
  return String();
}

const char* WifiOta::modeLabel() const {
  switch (mode_) {
    case WifiMode::Station:
      return "WiFi";
    case WifiMode::AccessPoint:
      return "AP";
    case WifiMode::Connecting:
      return "…";
    default:
      return "";
  }
}

void WifiOta::printStatus() const {
  Serial.printf("[wifi] mode=%s ssid=\"%s\" ip=%s\n", modeLabel(),
                ssid_.c_str(), ipString().c_str());
  if (mode_ == WifiMode::Station) {
    Serial.printf("[wifi] RSSI=%d hostname=%s.local\n", WiFi.RSSI(), kHostname);
  }
  if (mode_ == WifiMode::AccessPoint) {
    Serial.printf("[wifi] join AP \"%s\" / \"%s\" → http://%s/\n", kApSsid,
                  kApPassword, WiFi.softAPIP().toString().c_str());
  }
}
