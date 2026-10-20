#include "wifi_ota.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "device_auth.h"

namespace {
Preferences wifi_prefs;
WebServer server(80);
bool server_routes_ok = false;

constexpr const char* kNvsNs = "wifi";
constexpr const char* kNvsSsid = "ssid";
constexpr const char* kNvsPass = "pass";

WifiOta* g_wifi = nullptr;
WifiOta::WeightFn g_weight;

// ---------------------------------------------------------------------------
// Session secrets (regenerated every time the web server starts)
// ---------------------------------------------------------------------------
String g_csrf;          // per-session CSRF token, required in every POST
String g_nonce;         // HTTP Digest server nonce
String g_opaque;        // HTTP Digest opaque
uint32_t g_nonce_ms = 0;
constexpr uint32_t kNonceLifetimeMs = 30UL * 60UL * 1000UL;

uint8_t g_auth_failures = 0;
uint32_t g_lockout_until_ms = 0;

// Web OTA upload state for the current request
struct UploadState {
  bool seen = false;         // upload callback ran for this request
  bool accepted = false;     // passed auth / CSRF / origin checks
  bool auth_failed = false;  // send a Digest challenge instead of 403
  bool begun = false;        // Update.begin() succeeded
  bool ok = false;           // Update.end() succeeded
  const char* reason = "";
  uint8_t auth = 0;          // AuthResult of the upload request
};
UploadState g_upload;

void rotateNonce() {
  g_nonce = device_auth::randomHex(16);
  g_opaque = device_auth::randomHex(16);
  g_nonce_ms = millis();
}

void newSession() {
  g_csrf = device_auth::randomHex(16);
  rotateNonce();
  g_auth_failures = 0;
  g_lockout_until_ms = 0;
}

String realm() { return String(kProductName) + " " + device_auth::apSsid(); }

String md5hex(const String& in) {
  MD5Builder md5;
  md5.begin();
  md5.add(in);
  md5.calculate();
  return md5.toString();
}

// Minimal RFC 7616 / 2617 Authorization: Digest parameter parser.
struct DigestParams {
  String username, realm, nonce, uri, response, opaque, qop, nc, cnonce,
      algorithm;
};

bool parseDigest(const String& header, DigestParams& p) {
  if (!header.startsWith("Digest ")) return false;
  const size_t n = header.length();
  size_t i = 7;
  while (i < n) {
    while (i < n && (header[i] == ' ' || header[i] == ',')) ++i;
    size_t eq = header.indexOf('=', i);
    if (eq == static_cast<size_t>(-1) || eq <= i) break;
    String key = header.substring(i, eq);
    key.trim();
    key.toLowerCase();
    i = eq + 1;
    String val;
    if (i < n && header[i] == '"') {
      size_t end = header.indexOf('"', i + 1);
      if (end == static_cast<size_t>(-1)) return false;
      val = header.substring(i + 1, end);
      i = end + 1;
    } else {
      size_t end = header.indexOf(',', i);
      if (end == static_cast<size_t>(-1)) end = n;
      val = header.substring(i, end);
      val.trim();
      i = end;
    }
    if (key == "username") p.username = val;
    else if (key == "realm") p.realm = val;
    else if (key == "nonce") p.nonce = val;
    else if (key == "uri") p.uri = val;
    else if (key == "response") p.response = val;
    else if (key == "opaque") p.opaque = val;
    else if (key == "qop") p.qop = val;
    else if (key == "nc") p.nc = val;
    else if (key == "cnonce") p.cnonce = val;
    else if (key == "algorithm") p.algorithm = val;
  }
  return p.username.length() && p.nonce.length() && p.uri.length() &&
         p.response.length();
}

const char* methodName() {
  switch (server.method()) {
    case HTTP_GET:
      return "GET";
    case HTTP_POST:
      return "POST";
    default:
      return "";
  }
}

void sendChallenge(bool stale) {
  if (stale || (millis() - g_nonce_ms) > kNonceLifetimeMs) rotateNonce();
  String h = F("Digest realm=\"");
  h += realm();
  h += F("\", qop=\"auth\", algorithm=MD5, nonce=\"");
  h += g_nonce;
  h += F("\", opaque=\"");
  h += g_opaque;
  h += '"';
  if (stale) h += F(", stale=true");
  server.sendHeader("WWW-Authenticate", h);
  server.send(401, "text/plain",
              "Authentication required. User: admin, password: the device "
              "password shown on the scale's OLED.");
}

bool lockedOut() {
  return g_lockout_until_ms != 0 &&
         static_cast<int32_t>(millis() - g_lockout_until_ms) < 0;
}

enum class AuthResult : uint8_t { Ok, Missing, Stale, Bad, Locked };

// HTTP Digest (qop=auth, MD5) against the per-device password. Basic auth
// is deliberately not accepted (it would send the password in clear text).
AuthResult checkDigest() {
  if (lockedOut()) return AuthResult::Locked;
  if (!server.hasHeader("Authorization")) return AuthResult::Missing;
  DigestParams p;
  if (!parseDigest(server.header("Authorization"), p)) return AuthResult::Bad;

  // The digest must be for *this* request path (the stock WebServer does
  // not check this, which lets a sniffed response be replayed elsewhere).
  String uri_path = p.uri;
  int q = uri_path.indexOf('?');
  if (q >= 0) uri_path = uri_path.substring(0, q);
  if (uri_path != server.uri()) return AuthResult::Bad;
  if (p.qop != "auth" || !p.nc.length() || !p.cnonce.length())
    return AuthResult::Bad;
  if (p.algorithm.length() && !p.algorithm.equalsIgnoreCase("MD5"))
    return AuthResult::Bad;
  if (p.username != device_auth::kWebUser) return AuthResult::Bad;

  const String& pw = device_auth::password();
  const String ha1 = md5hex(String(device_auth::kWebUser) + ':' + realm() +
                            ':' + pw);
  const String ha2 = md5hex(String(methodName()) + ':' + p.uri);
  const String expect = md5hex(ha1 + ':' + p.nonce + ':' + p.nc + ':' +
                               p.cnonce + ":auth:" + ha2);
  const bool hash_ok = device_auth::equalsConstantTime(expect, p.response);

  if (p.nonce != g_nonce || p.opaque != g_opaque ||
      (millis() - g_nonce_ms) > kNonceLifetimeMs) {
    // Old nonce: correct password → silent re-auth (stale=true);
    // wrong password with an old nonce still counts as a failure.
    return hash_ok ? AuthResult::Stale : AuthResult::Bad;
  }
  return hash_ok ? AuthResult::Ok : AuthResult::Bad;
}

void noteAuthResult(AuthResult r) {
  if (r == AuthResult::Ok) {
    g_auth_failures = 0;
    return;
  }
  if (r != AuthResult::Bad) return;
  if (++g_auth_failures >= kAuthMaxFailures) {
    g_auth_failures = 0;
    g_lockout_until_ms = millis() + kAuthLockoutMs;
    if (g_lockout_until_ms == 0) g_lockout_until_ms = 1;
    rotateNonce();
    Serial.printf("[web] too many failed logins — locked for %lus\n",
                  static_cast<unsigned long>(kAuthLockoutMs / 1000));
  }
}

void sendAuthFailure(AuthResult r) {
  if (r == AuthResult::Locked) {
    server.sendHeader("Retry-After", String(kAuthLockoutMs / 1000));
    server.send(429, "text/plain", "Too many failed logins. Try again later.");
  } else {
    sendChallenge(r == AuthResult::Stale);
  }
}

// Host header must name this device (blocks DNS-rebinding attacks).
bool hostOk() {
  String host = server.hostHeader();
  host.toLowerCase();
  if (host.endsWith(":80")) host.remove(host.length() - 3);
  if (!host.length()) return false;
  const String mdns = String(kHostname) + ".local";
  if (host == mdns || host == kHostname) return true;
  if (WiFi.getMode() & WIFI_MODE_AP) {
    if (host == WiFi.softAPIP().toString()) return true;
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (host == WiFi.localIP().toString()) return true;
  }
  return false;
}

// Same-origin check for POSTs: Origin (preferred) or Referer must match the
// Host we are being addressed as. Requests with neither are rejected.
bool sameOrigin() {
  const String expected = String("http://") + server.hostHeader();
  if (server.hasHeader("Origin") && server.header("Origin").length()) {
    return server.header("Origin").equalsIgnoreCase(expected);
  }
  if (server.hasHeader("Referer") && server.header("Referer").length()) {
    const String ref = server.header("Referer");
    return ref.equalsIgnoreCase(expected) ||
           ref.substring(0, expected.length() + 1)
               .equalsIgnoreCase(expected + "/");
  }
  return false;
}

bool csrfOk() {
  if (!g_csrf.length() || !server.hasArg("csrf")) return false;
  return device_auth::equalsConstantTime(server.arg("csrf"), g_csrf);
}

void redirectToDevice(const char* path) {
  String ip = (WiFi.getMode() & WIFI_MODE_AP) && WiFi.status() != WL_CONNECTED
                  ? WiFi.softAPIP().toString()
                  : WiFi.localIP().toString();
  server.sendHeader("Location", String("http://") + ip + path);
  server.send(302, "text/plain", "");
}

void securityHeaders() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "same-origin");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; style-src 'unsafe-inline'; "
                    "form-action 'self'; frame-ancestors 'none'");
}

// Gate for every protected route. Returns true if the handler may proceed.
bool guard(bool is_post) {
  if (g_wifi) g_wifi->noteActivity();
  if (!hostOk()) {
    // Captive-portal probes etc. arrive with foreign Host headers.
    redirectToDevice("/");
    return false;
  }
  AuthResult r = checkDigest();
  noteAuthResult(r);
  if (r != AuthResult::Ok) {
    sendAuthFailure(r);
    return false;
  }
  if (is_post && (!sameOrigin() || !csrfOk())) {
    Serial.println("[web] POST rejected (CSRF / origin check)");
    server.send(403, "text/plain", "Forbidden (CSRF / origin check failed)");
    return false;
  }
  securityHeaders();
  return true;
}

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
    else if (c == '\'')
      o += F("&#39;");
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
  if (!guard(false)) return;
  const bool ap = WiFi.getMode() & WIFI_MODE_AP;
  String ip = ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String mode = ap ? "Access Point (setup)"
               : (WiFi.status() == WL_CONNECTED) ? "Station" : "-";
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
  body += htmlEscape(ap ? device_auth::apSsid() : WiFi.SSID());
  body += F("<br><b>Weight:</b> ");
  body += String(w, 1);
  body += F(" g</p>");
  body += F("<p><a href=/wifi>Wi‑Fi setup</a> · <a href=/update>OTA update</a></p>");
  body += F("<p class=meta>PlatformIO wireless upload (ArduinoOTA) uses the same "
            "device password as this page. Example:<br>"
            "<code>HALF_DECENT_OTA_PASSWORD=&lt;device password&gt; "
            "pio run -t upload -e ota --upload-port ");
  body += ip;
  body += F("</code></p>");
  server.send(200, "text/html", pageShell("Half Decent Scale", body));
}

void handleWifiGet() {
  if (!guard(false)) return;
  String body = F("<h1>Wi‑Fi setup</h1>"
                  "<p class=meta>2.4 GHz only. SSID up to 32 bytes; password "
                  "8–63 characters (leave empty for an open network).</p>"
                  "<form method=POST action=/wifi>"
                  "<input type=hidden name=csrf value=\"");
  body += g_csrf;
  body += F("\"><label>SSID<input name=ssid required maxlength=32 "
            "autocomplete=off></label>"
            "<label>Password<input name=pass type=password maxlength=63 "
            "autocomplete=new-password></label>"
            "<button type=submit>Save &amp; connect</button></form>"
            "<p><a href=/>Back</a></p>");
  server.send(200, "text/html", pageShell("Wi‑Fi setup", body));
}

void handleWifiPost() {
  if (!guard(true)) return;
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "missing ssid");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  ssid.trim();
  String err;
  if (!WifiOta::validateCredentials(ssid, pass, &err)) {
    server.send(400, "text/plain", err);
    return;
  }
  if (!g_wifi || !g_wifi->saveCredentials(ssid, pass)) {
    server.send(500, "text/plain", "could not save credentials");
    return;
  }
  String body = F("<h1>Saved</h1><p>Connecting to <b>");
  body += htmlEscape(ssid);
  body += F("</b>… Rebooting in 2s.</p>");
  server.send(200, "text/html", pageShell("Saved", body));
  delay(500);
  ESP.restart();
}

void handleUpdateGet() {
  if (!guard(false)) return;
  String body = F("<h1>Firmware OTA</h1>"
                  "<p class=meta>Upload a <code>firmware.bin</code> from "
                  "<code>.pio/build/.../firmware.bin</code></p>"
                  "<form method=POST enctype=multipart/form-data "
                  "action=\"/update?csrf=");
  // CSRF token rides in the query string: multipart text fields are not
  // available yet when the file-upload callback starts.
  body += g_csrf;
  body += F("\"><label>File<input type=file name=firmware accept=.bin required>"
            "</label><button type=submit>Flash &amp; reboot</button></form>"
            "<p><a href=/>Back</a></p>");
  server.send(200, "text/html", pageShell("OTA update", body));
}

// Upload callback: runs before handleUpdatePostDone. Authorisation is decided
// once at UPLOAD_FILE_START; rejected uploads are drained without touching
// flash.
void handleUpdatePost() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    g_upload = UploadState{};
    g_upload.seen = true;
    if (g_wifi) g_wifi->noteActivity();
    if (!hostOk()) {
      g_upload.reason = "bad host";
      return;
    }
    AuthResult r = checkDigest();
    noteAuthResult(r);
    g_upload.auth = static_cast<uint8_t>(r);
    if (r != AuthResult::Ok) {
      g_upload.auth_failed = true;
      g_upload.reason = "auth";
      return;
    }
    if (!sameOrigin() || !csrfOk()) {
      g_upload.reason = "csrf/origin";
      return;
    }
    g_upload.accepted = true;
    Serial.printf("[ota] web start: %s\n", upload.filename.c_str());
    if (g_wifi) g_wifi->setOtaActive(true);
    // NOTE: no signature verification yet — see README "Signed OTA images".
    // Update itself checks the ESP image magic byte and, at end(), the
    // image SHA-256 appended by esptool.
    if (Update.begin(UPDATE_SIZE_UNKNOWN)) {
      g_upload.begun = true;
    } else {
      Update.printError(Serial);
      if (g_wifi) g_wifi->setOtaActive(false);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!g_upload.begun) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!g_upload.begun) return;
    if (Update.end(true)) {
      g_upload.ok = true;
      Serial.printf("[ota] web success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
      if (g_wifi) g_wifi->setOtaActive(false);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (g_upload.begun) Update.abort();
    g_upload.begun = false;
    if (g_wifi) g_wifi->setOtaActive(false);
    Serial.println("[ota] web upload aborted");
  }
}

void handleUpdatePostDone() {
  if (!g_upload.seen) {
    // No file part at all: still run the normal gate first.
    if (!guard(true)) return;
    server.send(400, "text/plain", "no firmware file received");
    return;
  }
  g_upload.seen = false;
  if (!g_upload.accepted) {
    Serial.printf("[ota] web upload rejected (%s)\n", g_upload.reason);
    if (!hostOk()) {
      redirectToDevice("/");
    } else if (g_upload.auth_failed) {
      sendAuthFailure(static_cast<AuthResult>(g_upload.auth));
    } else {
      server.send(403, "text/plain", "Forbidden (CSRF / origin check failed)");
    }
    return;
  }
  securityHeaders();
  if (!g_upload.ok || Update.hasError()) {
    if (g_wifi) g_wifi->setOtaActive(false);
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
  static const char* kHeaders[] = {"Origin", "Referer"};
  server.collectHeaders(kHeaders, 2);  // Authorization is always collected
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on(
      "/update", HTTP_POST,
      handleUpdatePostDone,
      handleUpdatePost);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.onNotFound([]() {
    if (g_wifi) g_wifi->noteActivity();
    // Captive portal: redirect unknown hosts/paths to the (auth-protected)
    // status page.
    if (WiFi.getMode() & WIFI_MODE_AP) {
      redirectToDevice("/");
    } else {
      server.send(404, "text/plain", "not found");
    }
  });
  server_routes_ok = true;
}
}  // namespace

bool WifiOta::validateCredentials(const String& ssid, const String& pass,
                                  String* err) {
  auto fail = [err](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (ssid.isEmpty()) return fail("SSID must not be empty");
  if (ssid.length() > 32) return fail("SSID must be at most 32 bytes");
  if (pass.length() == 0) return true;  // open network
  if (pass.length() < 8 || pass.length() > 63)
    return fail("password must be 8-63 characters (or empty for open network)");
  for (size_t i = 0; i < pass.length(); ++i) {
    if (pass[i] < 0x20 || pass[i] > 0x7e)
      return fail("password must be printable ASCII");
  }
  return true;
}

bool WifiOta::loadCredentials(String& ssid, String& pass) {
  if (!wifi_prefs.begin(kNvsNs, true)) return false;
  ssid = wifi_prefs.getString(kNvsSsid, "");
  pass = wifi_prefs.getString(kNvsPass, "");
  wifi_prefs.end();
  return ssid.length() > 0;
}

bool WifiOta::saveCredentials(const String& ssid, const String& pass) {
  String err;
  if (!validateCredentials(ssid, pass, &err)) {
    Serial.printf("[wifi] not saved: %s\n", err.c_str());
    return false;
  }
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

// Non-blocking STA attempt; update() promotes to Station once connected.
void WifiOta::startStaBackground(const String& ssid, const String& pass) {
  ssid_ = ssid;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  mode_ = WifiMode::Retrying;
  last_reconnect_ms_ = millis();
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
  // Do NOT fall back to the setup AP; keep retrying in the background.
  startStaBackground(ssid, pass);
  return false;
}

void WifiOta::startApPortal() {
  stopServices();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);
  const String& ssid = device_auth::apSsid();
  if (!WiFi.softAP(ssid.c_str(), device_auth::password().c_str())) {
    Serial.println("[wifi] softAP start failed");
  }
  mode_ = WifiMode::AccessPoint;
  ssid_ = ssid;
  ap_last_activity_ms_ = millis();
  requestSetupScreen();
  Serial.printf("[wifi] setup AP \"%s\"  IP %s  (password on OLED; closes "
                "after %lu min idle)\n",
                ssid.c_str(), WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned long>(kSetupApIdleTimeoutMs / 60000));
  startServices();
}

void WifiOta::closeSetupAp() {
  if (mode_ != WifiMode::AccessPoint) return;
  stopServices();
  WiFi.softAPdisconnect(true);
  setup_screen_until_ms_ = 0;
  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    Serial.println("[wifi] setup AP closed — reconnecting to saved network");
    startStaBackground(ssid, pass);
  } else {
    WiFi.mode(WIFI_OFF);
    mode_ = WifiMode::Off;
    ssid_ = "";
    Serial.println("[wifi] setup AP closed — Wi-Fi off (hold Timer 5 s to "
                   "reopen)");
  }
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
  newSession();

  if (!MDNS.begin(kHostname)) {
    Serial.println("[wifi] mDNS failed");
  } else {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("arduino", "tcp", 3232);
    Serial.printf("[wifi] mDNS %s.local\n", kHostname);
  }

  setupRoutes();
  server.begin();
  Serial.println("[wifi] HTTP :80  (/  /wifi  /update)  digest auth, user "
                 "\"admin\"");

  ArduinoOTA.setHostname(kHostname);
  // Same per-device password as the web UI / setup AP. Never empty.
  ArduinoOTA.setPassword(device_auth::password().c_str());
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
  Serial.println("[ota] ArduinoOTA ready (device password)");

  services_started_ = true;
}

void WifiOta::end() {
  stopServices();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  mode_ = WifiMode::Off;
  g_wifi = nullptr;
  Serial.println("[wifi] off");
}

bool WifiOta::begin(WeightFn weight_fn) {
  weight_fn_ = std::move(weight_fn);
  g_wifi = this;
  g_weight = weight_fn_;
  WiFi.persistent(false);

  // BLE is already up at this point, so esp_random() is RF-seeded.
  device_auth::begin();
  if (device_auth::generatedThisBoot()) {
    // Deliberate one-time display: the only time the password is written to
    // the serial log. Afterwards it is only shown on the OLED.
    Serial.println();
    Serial.println("[auth] ===== NEW DEVICE PASSWORD (shown once) =====");
    Serial.printf("[auth]   %s\n", device_auth::password().c_str());
    Serial.printf("[auth]   setup AP \"%s\", web user \"%s\", ArduinoOTA\n",
                  device_auth::apSsid().c_str(), device_auth::kWebUser);
    Serial.println("[auth]   Later: hold Timer 5 s or type `auth show` to see "
                   "it on the OLED.");
    Serial.println("[auth] =============================================");
    requestSetupScreen();
  }

  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    if (connectSta(ssid, pass)) {
      startServices();
      return true;
    }
    // Saved network unreachable: keep retrying; do NOT open the setup AP.
    Serial.println("[wifi] will keep retrying in the background. Hold Timer "
                   "5 s (or serial `wifi ap`) to open the setup AP.");
    startStaBackground(ssid, pass);
    return true;
  }
  // First boot / no saved network: open the setup AP (with idle timeout).
  Serial.println("[wifi] no saved credentials — opening setup AP");
  startApPortal();
  return true;
}

bool WifiOta::startSetupAp() {
  startApPortal();
  return true;
}

void WifiOta::noteActivity() { ap_last_activity_ms_ = millis(); }

void WifiOta::requestSetupScreen() {
  setup_screen_until_ms_ = millis() + kSetupScreenMs;
  if (setup_screen_until_ms_ == 0) setup_screen_until_ms_ = 1;
}

bool WifiOta::setupScreenActive() const {
  if (setup_screen_until_ms_ != 0 &&
      static_cast<int32_t>(millis() - setup_screen_until_ms_) < 0) {
    return true;
  }
  return mode_ == WifiMode::AccessPoint && WiFi.softAPgetStationNum() > 0;
}

uint32_t WifiOta::apSecondsLeft() const {
  if (mode_ != WifiMode::AccessPoint) return 0;
  const uint32_t idle = millis() - ap_last_activity_ms_;
  if (idle >= kSetupApIdleTimeoutMs) return 0;
  return (kSetupApIdleTimeoutMs - idle) / 1000;
}

void WifiOta::update() {
  const uint32_t now = millis();
  if (mode_ == WifiMode::Station && WiFi.status() != WL_CONNECTED) {
    if (now - last_reconnect_ms_ > 10000) {
      last_reconnect_ms_ = now;
      Serial.println("[wifi] STA lost — reconnecting");
      WiFi.reconnect();
    }
  } else if (mode_ == WifiMode::Retrying) {
    if (WiFi.status() == WL_CONNECTED) {
      mode_ = WifiMode::Station;
      Serial.printf("[wifi] STA OK %s (background retry)\n",
                    WiFi.localIP().toString().c_str());
      startServices();
    } else if (now - last_reconnect_ms_ > kWifiRetryIntervalMs) {
      last_reconnect_ms_ = now;
      WiFi.reconnect();
    }
  } else if (mode_ == WifiMode::AccessPoint) {
    if (WiFi.softAPgetStationNum() > 0) ap_last_activity_ms_ = now;
    if (now - ap_last_activity_ms_ > kSetupApIdleTimeoutMs && !ota_active_) {
      Serial.println("[wifi] setup AP idle timeout");
      closeSetupAp();
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
      return "...";
    case WifiMode::Retrying:
      return "WiFi?";
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
    Serial.printf("[wifi] join AP \"%s\" (password on OLED) → http://%s/  "
                  "closes in %lus if idle\n",
                  device_auth::apSsid().c_str(),
                  WiFi.softAPIP().toString().c_str(),
                  static_cast<unsigned long>(apSecondsLeft()));
  }
  if (mode_ == WifiMode::Retrying) {
    Serial.println("[wifi] saved network unreachable — retrying; hold Timer "
                   "5 s or `wifi ap` for setup AP");
  }
}
