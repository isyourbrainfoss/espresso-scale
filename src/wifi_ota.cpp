#include "wifi_ota.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>

#include "config.h"

namespace {
Preferences wifi_prefs;
WebServer server(80);
bool server_routes_ok = false;

constexpr const char* kNvsNs = "wifi";
constexpr const char* kNvsSsid = "ssid";
constexpr const char* kNvsPass = "pass";
constexpr const char* kNvsSecNs = "sec";
constexpr const char* kNvsDevPw = "devpw";

// 32 symbols (no 0/o/1/l) so each char is exactly 5 random bits.
constexpr char kPwAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
static_assert(sizeof(kPwAlphabet) - 1 == 32, "alphabet must have 32 symbols");

WifiOta* g_wifi = nullptr;
WifiOta::WeightFn g_weight;

// ---------------------------------------------------------------- helpers

String randomHex16() {  // 128 bits from the hardware RNG
  char buf[33];
  for (int i = 0; i < 4; ++i) {
    snprintf(buf + i * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  }
  return String(buf);
}

bool constTimeEq(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

String md5Hex(const String& s) {
  MD5Builder md5;
  md5.begin();
  md5.add(s);
  md5.calculate();
  return md5.toString();
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

// ------------------------------------------------------- HTTP Digest auth
// WebServer::authenticate() in arduino-esp32 2.x also accepts Basic auth
// (password in clear over HTTP), keeps a single nonce, ignores nc (so a
// sniffed Authorization header can be replayed) and does not check that the
// digest uri matches the request. This is a stricter RFC 7616 (MD5, qop=auth)
// check: Digest only, a few server nonces with expiry, strictly increasing
// nc per nonce, uri must match the request path, plus a failure lockout.

struct NonceSlot {
  String nonce;
  uint32_t issued_ms = 0;
  uint32_t last_nc = 0;
};
constexpr int kNonceSlots = 4;
constexpr uint32_t kNonceLifetimeMs = 30UL * 60UL * 1000UL;
NonceSlot nonces[kNonceSlots];
String opaque;  // per boot

constexpr uint32_t kAuthFailWindowMs = 5UL * 60UL * 1000UL;
constexpr int kAuthMaxFails = 10;
constexpr uint32_t kAuthLockoutMs = 60UL * 1000UL;
uint32_t auth_fail_window_start = 0;
int auth_fail_count = 0;
uint32_t auth_locked_until = 0;
bool auth_locked = false;

enum class AuthResult : uint8_t { Ok, Missing, Stale, Bad, Locked };

String issueNonce() {
  int victim = 0;
  const uint32_t now = millis();
  for (int i = 0; i < kNonceSlots; ++i) {
    if (nonces[i].nonce.isEmpty()) {
      victim = i;
      break;
    }
    if (now - nonces[i].issued_ms > now - nonces[victim].issued_ms) victim = i;
  }
  nonces[victim].nonce = randomHex16();
  nonces[victim].issued_ms = now;
  nonces[victim].last_nc = 0;
  return nonces[victim].nonce;
}

// Parse `key=value` / `key="value"` from a Digest Authorization header.
String digestParam(const String& h, const char* key) {
  const size_t klen = strlen(key);
  int i = 0;
  const int n = h.length();
  while (i < n) {
    while (i < n && (h[i] == ' ' || h[i] == ',')) ++i;
    int eq = h.indexOf('=', i);
    if (eq < 0) break;
    String k = h.substring(i, eq);
    k.trim();
    int v0 = eq + 1;
    String val;
    int next;
    if (v0 < n && h[v0] == '"') {
      int v1 = h.indexOf('"', v0 + 1);
      if (v1 < 0) return String();
      val = h.substring(v0 + 1, v1);
      next = v1 + 1;
    } else {
      int v1 = h.indexOf(',', v0);
      if (v1 < 0) v1 = n;
      val = h.substring(v0, v1);
      val.trim();
      next = v1;
    }
    if (k.length() == klen && k.equalsIgnoreCase(key)) return val;
    i = next;
  }
  return String();
}

const char* methodName(HTTPMethod m) {
  switch (m) {
    case HTTP_POST:
      return "POST";
    case HTTP_PUT:
      return "PUT";
    case HTTP_DELETE:
      return "DELETE";
    default:
      return "GET";
  }
}

void noteAuthFailure() {
  const uint32_t now = millis();
  if (now - auth_fail_window_start > kAuthFailWindowMs) {
    auth_fail_window_start = now;
    auth_fail_count = 0;
  }
  if (++auth_fail_count >= kAuthMaxFails) {
    auth_locked = true;
    auth_locked_until = now + kAuthLockoutMs;
    auth_fail_count = 0;
    Serial.println("[web] too many failed logins — locked for 60 s");
  }
}

AuthResult checkAuth(const String& password) {
  const uint32_t now = millis();
  if (auth_locked) {
    if (static_cast<int32_t>(now - auth_locked_until) < 0) return AuthResult::Locked;
    auth_locked = false;
  }
  if (password.isEmpty()) return AuthResult::Bad;
  if (!server.hasHeader("Authorization")) return AuthResult::Missing;
  String h = server.header("Authorization");
  if (!h.startsWith("Digest ")) return AuthResult::Missing;  // no Basic
  h = h.substring(7);

  const String user = digestParam(h, "username");
  const String realm = digestParam(h, "realm");
  const String nonce = digestParam(h, "nonce");
  const String uri = digestParam(h, "uri");
  const String response = digestParam(h, "response");
  const String qop = digestParam(h, "qop");
  const String nc_s = digestParam(h, "nc");
  const String cnonce = digestParam(h, "cnonce");
  const String algo = digestParam(h, "algorithm");
  const String op = digestParam(h, "opaque");

  if (user.isEmpty() || nonce.isEmpty() || uri.isEmpty() ||
      response.length() != 32 || qop != "auth" || nc_s.length() != 8 ||
      cnonce.isEmpty() || (!algo.isEmpty() && algo != "MD5")) {
    return AuthResult::Missing;
  }
  if (realm != kWebRealm || op != opaque) return AuthResult::Missing;

  // uri must be the requested path (query string, if any, may follow).
  String uri_path = uri;
  int q = uri_path.indexOf('?');
  if (q >= 0) uri_path = uri_path.substring(0, q);
  if (uri_path != server.uri()) return AuthResult::Missing;

  NonceSlot* slot = nullptr;
  for (auto& s : nonces) {
    if (!s.nonce.isEmpty() && constTimeEq(s.nonce, nonce)) {
      slot = &s;
      break;
    }
  }
  if (slot == nullptr || now - slot->issued_ms > kNonceLifetimeMs) {
    return AuthResult::Stale;  // unknown/expired nonce: re-challenge quietly
  }

  const String ha1 = md5Hex(String(kWebUser) + ':' + kWebRealm + ':' + password);
  const String ha2 = md5Hex(String(methodName(server.method())) + ':' + uri);
  const String expect =
      md5Hex(ha1 + ':' + nonce + ':' + nc_s + ':' + cnonce + ":auth:" + ha2);
  String resp_lc = response;
  resp_lc.toLowerCase();
  if (!constTimeEq(user, String(kWebUser)) | !constTimeEq(resp_lc, expect)) {
    noteAuthFailure();
    return AuthResult::Bad;
  }
  // Correct password: enforce replay protection via nc.
  const uint32_t nc = strtoul(nc_s.c_str(), nullptr, 16);
  if (nc <= slot->last_nc) return AuthResult::Stale;
  slot->last_nc = nc;
  return AuthResult::Ok;
}

void sendAuthChallenge(AuthResult r) {
  if (r == AuthResult::Locked) {
    server.send(429, "text/plain", "Too many failed logins; try again in a minute");
    return;
  }
  String h = F("Digest realm=\"");
  h += kWebRealm;
  h += F("\", qop=\"auth\", algorithm=MD5, nonce=\"");
  h += issueNonce();
  h += F("\", opaque=\"");
  h += opaque;
  h += '"';
  if (r == AuthResult::Stale) h += F(", stale=true");
  server.sendHeader("WWW-Authenticate", h);
  server.send(401, "text/plain",
              "Login required. User: scale, password: shown on the scale's OLED "
              "(hold Timer ~5 s).");
}

bool requireAuth() {
  if (g_wifi == nullptr) return false;
  AuthResult r = checkAuth(g_wifi->devicePassword());
  if (r == AuthResult::Ok) return true;
  sendAuthChallenge(r);
  return false;
}

// ------------------------------------------------------------------- CSRF
// Single-use tokens issued with each form (GET) and required on POST. A
// cross-site page cannot read them; the Origin/Referer check is a second,
// independent layer.

struct CsrfSlot {
  String token;
  uint32_t issued_ms = 0;
};
constexpr int kCsrfSlots = 4;
constexpr uint32_t kCsrfLifetimeMs = 15UL * 60UL * 1000UL;
CsrfSlot csrf_tokens[kCsrfSlots];

String issueCsrf() {
  int victim = 0;
  const uint32_t now = millis();
  for (int i = 0; i < kCsrfSlots; ++i) {
    if (csrf_tokens[i].token.isEmpty()) {
      victim = i;
      break;
    }
    if (now - csrf_tokens[i].issued_ms > now - csrf_tokens[victim].issued_ms) victim = i;
  }
  csrf_tokens[victim].token = randomHex16();
  csrf_tokens[victim].issued_ms = now;
  return csrf_tokens[victim].token;
}

bool consumeCsrf(const String& tok) {
  if (tok.length() != 32) return false;
  const uint32_t now = millis();
  for (auto& s : csrf_tokens) {
    if (s.token.isEmpty()) continue;
    if (now - s.issued_ms > kCsrfLifetimeMs) {
      s.token = "";
      continue;
    }
    if (constTimeEq(s.token, tok)) {
      s.token = "";  // single use
      return true;
    }
  }
  return false;
}

// Origin (or, failing that, Referer) must be this device.
bool sameOrigin() {
  const String host = server.hostHeader();
  if (host.isEmpty()) return false;
  const String self = String("http://") + host;
  if (server.hasHeader("Origin") && server.header("Origin").length()) {
    return server.header("Origin") == self;
  }
  if (server.hasHeader("Referer") && server.header("Referer").length()) {
    const String ref = server.header("Referer");
    return ref == self || ref.startsWith(self + "/");
  }
  return false;
}

// For POST handlers that run after the body is parsed (urlencoded forms).
bool checkPostGuards() {
  if (!requireAuth()) return false;
  if (!sameOrigin()) {
    server.send(403, "text/plain", "Cross-origin request rejected");
    return false;
  }
  if (!consumeCsrf(server.arg("csrf"))) {
    server.send(403, "text/plain",
                "Invalid or expired form token — reload the page and retry");
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ pages

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

void sendPage(int code, const String& title, const String& body) {
  // No caching of authenticated pages / tokens; no framing (clickjacking).
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Content-Security-Policy",
                    "default-src 'none'; style-src 'unsafe-inline'; "
                    "form-action 'self'; frame-ancestors 'none'");
  server.sendHeader("Referrer-Policy", "same-origin");
  server.send(code, "text/html", pageShell(title, body));
}

void handleRoot() {
  if (!requireAuth()) return;
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
  body += htmlEscape(WiFi.getMode() == WIFI_AP && g_wifi ? g_wifi->apSsid()
                                                         : WiFi.SSID());
  body += F("<br><b>Weight:</b> ");
  body += String(w, 1);
  body += F(" g</p>");
  body += F("<p><a href=/wifi>Wi‑Fi setup</a> · <a href=/update>OTA update</a></p>");
  body += F("<p class=meta>PlatformIO wireless upload uses ArduinoOTA with the "
            "device password (hold Timer ~5 s to show it on the OLED):<br>"
            "<code>SCALE_OTA_PASSWORD=… pio run -t upload -e ota --upload-port ");
  body += ip;
  body += F("</code></p>");
  sendPage(200, "Half Decent Scale", body);
}

void handleWifiGet() {
  if (!requireAuth()) return;
  String body = F("<h1>Wi‑Fi setup</h1>"
                  "<p class=meta>2.4 GHz networks only. Password: 8–63 "
                  "characters, or empty for an open network.</p>"
                  "<form method=POST action=/wifi>"
                  "<input type=hidden name=csrf value=\"");
  body += issueCsrf();
  body += F("\"><label>SSID<input name=ssid required maxlength=32 "
            "autocomplete=off></label>"
            "<label>Password<input name=pass type=password maxlength=63 "
            "autocomplete=new-password></label>"
            "<button type=submit>Save &amp; connect</button></form>"
            "<p><a href=/>Back</a></p>");
  sendPage(200, "Wi‑Fi setup", body);
}

void handleWifiPost() {
  if (!checkPostGuards()) return;
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "missing ssid");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  const char* why = nullptr;
  if (!WifiOta::validateCredentials(ssid, pass, &why)) {
    server.send(400, "text/plain", why ? why : "invalid credentials");
    return;
  }
  if (!g_wifi || !g_wifi->saveCredentials(ssid, pass)) {
    server.send(500, "text/plain", "could not save credentials");
    return;
  }
  String body = F("<h1>Saved</h1><p>Connecting to <b>");
  body += htmlEscape(ssid);
  body += F("</b>… Rebooting in 2s.</p>");
  sendPage(200, "Saved", body);
  delay(500);
  ESP.restart();
}

void handleUpdateGet() {
  if (!requireAuth()) return;
  // Token goes in the action URL: query args are parsed before the multipart
  // body, so the upload callback can check it before touching flash.
  String body = F("<h1>Firmware OTA</h1>"
                  "<p class=meta>Upload a <code>firmware.bin</code> from "
                  "<code>.pio/build/.../firmware.bin</code></p>"
                  "<form method=POST action=\"/update?csrf=");
  body += issueCsrf();
  body += F("\" enctype=multipart/form-data>"
            "<label>File<input type=file name=firmware accept=.bin required></label>"
            "<button type=submit>Flash &amp; reboot</button></form>"
            "<p><a href=/>Back</a></p>");
  sendPage(200, "OTA update", body);
}

// Upload state for the current /update request. The upload callback runs
// (with headers and query args available) before the final handler.
enum class UploadGate : uint8_t { None, Ok, NeedAuth, Forbidden, BadToken };
UploadGate upload_gate = UploadGate::None;
AuthResult upload_auth = AuthResult::Missing;
bool update_started = false;
bool update_ok = false;

void handleUpdatePost() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    update_started = false;
    update_ok = false;
    upload_auth = checkAuth(g_wifi ? g_wifi->devicePassword() : String());
    if (upload_auth != AuthResult::Ok) {
      upload_gate = UploadGate::NeedAuth;
    } else if (!sameOrigin()) {
      upload_gate = UploadGate::Forbidden;
    } else if (!consumeCsrf(server.arg("csrf"))) {
      upload_gate = UploadGate::BadToken;
    } else {
      upload_gate = UploadGate::Ok;
    }
    if (upload_gate != UploadGate::Ok) {
      Serial.println("[ota] web upload rejected (auth/origin/token)");
      return;
    }
    Serial.printf("[ota] web start: %s\n", upload.filename.c_str());
    if (g_wifi) g_wifi->setOtaActive(true);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      if (g_wifi) g_wifi->setOtaActive(false);
      return;
    }
    update_started = true;
  } else if (upload_gate != UploadGate::Ok || !update_started) {
    return;  // rejected or failed to start: drain the body, write nothing
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      Update.abort();
      update_started = false;
      if (g_wifi) g_wifi->setOtaActive(false);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      update_ok = true;
      Serial.printf("[ota] web success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
      update_started = false;
      if (g_wifi) g_wifi->setOtaActive(false);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    update_started = false;
    upload_gate = UploadGate::None;
    if (g_wifi) g_wifi->setOtaActive(false);
    Serial.println("[ota] web upload aborted");
  }
}

void handleUpdatePostDone() {
  const UploadGate gate = upload_gate;
  const bool ok = update_started && update_ok;
  upload_gate = UploadGate::None;
  update_started = false;
  update_ok = false;
  switch (gate) {
    case UploadGate::None:
      // No file part at all: still require auth before saying anything.
      if (!requireAuth()) return;
      server.send(400, "text/plain", "no firmware file");
      return;
    case UploadGate::NeedAuth:
      sendAuthChallenge(upload_auth);
      return;
    case UploadGate::Forbidden:
      server.send(403, "text/plain", "Cross-origin request rejected");
      return;
    case UploadGate::BadToken:
      server.send(403, "text/plain",
                  "Invalid or expired form token — reload the page and retry");
      return;
    case UploadGate::Ok:
      break;
  }
  if (!ok) {
    if (g_wifi) g_wifi->setOtaActive(false);
    server.send(500, "text/plain", "Update failed — see serial log");
    return;
  }
  sendPage(200, "OTA", F("<h1>OK</h1><p>Rebooting…</p>"));
  delay(400);
  ESP.restart();
}

void setupRoutes() {
  if (server_routes_ok) return;
  static const char* kHeaders[] = {"Origin", "Referer"};
  server.collectHeaders(kHeaders, 2);  // Authorization is always collected
  if (opaque.isEmpty()) opaque = randomHex16();
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on(
      "/update", HTTP_POST,
      handleUpdatePostDone,
      handleUpdatePost);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.onNotFound([]() {
    // Captive portal: send unknown hosts/paths to the (authenticated) setup
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

// ------------------------------------------------------- device password

bool WifiOta::loadOrCreatePassword() {
  Preferences p;
  if (p.begin(kNvsSecNs, true)) {
    device_password_ = p.getString(kNvsDevPw, "");
    p.end();
  }
  if (device_password_.length() >= 8) return true;
  // First boot: the radio (BLE) is already up, so esp_random() is a true RNG.
  String pw;
  pw.reserve(kDevicePasswordLen);
  for (size_t i = 0; i < kDevicePasswordLen; ++i) {
    pw += kPwAlphabet[esp_random() & 31u];
  }
  if (!p.begin(kNvsSecNs, false)) {
    Serial.println("[sec] NVS open failed — Wi-Fi/OTA disabled");
    device_password_ = "";
    return false;
  }
  const bool ok = p.putString(kNvsDevPw, pw) == pw.length();
  p.end();
  if (!ok) {
    Serial.println("[sec] could not store device password — Wi-Fi/OTA disabled");
    device_password_ = "";
    return false;
  }
  device_password_ = pw;
  Serial.printf("[sec] generated device password %s (shown on OLED)\n",
                maskedPassword().c_str());
  return true;
}

bool WifiOta::regeneratePassword() {
  Preferences p;
  if (!p.begin(kNvsSecNs, false)) return false;
  p.remove(kNvsDevPw);
  p.end();
  device_password_ = "";
  return loadOrCreatePassword();
}

String WifiOta::maskedPassword() const {
  const size_t n = device_password_.length();
  if (n < 4) return String("(none)");
  String m = device_password_.substring(0, 2);
  for (size_t i = 2; i < n - 2; ++i) m += '*';
  m += device_password_.substring(n - 2);
  return m;
}

bool WifiOta::validateCredentials(const String& ssid, const String& pass,
                                  const char** why) {
  auto fail = [why](const char* msg) {
    if (why) *why = msg;
    return false;
  };
  if (ssid.length() == 0) return fail("SSID is empty");
  if (ssid.length() > 32) return fail("SSID longer than 32 bytes");
  for (size_t i = 0; i < ssid.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(ssid[i]);
    if (c < 0x20 || c == 0x7F) return fail("SSID contains control characters");
  }
  if (pass.length() == 0) return true;  // open network
  if (pass.length() < 8 || pass.length() > 63) {
    return fail("Password must be 8-63 characters (or empty for open Wi-Fi)");
  }
  for (size_t i = 0; i < pass.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(pass[i]);
    if (c < 0x20 || c > 0x7E) return fail("Password must be printable ASCII");
  }
  return true;
}

// ---------------------------------------------------------- credentials

bool WifiOta::loadCredentials(String& ssid, String& pass) {
  if (!wifi_prefs.begin(kNvsNs, true)) return false;
  ssid = wifi_prefs.getString(kNvsSsid, "");
  pass = wifi_prefs.getString(kNvsPass, "");
  wifi_prefs.end();
  return ssid.length() > 0;
}

bool WifiOta::saveCredentials(const String& ssid, const String& pass) {
  const char* why = nullptr;
  if (!validateCredentials(ssid, pass, &why)) {
    Serial.printf("[wifi] not saved: %s\n", why);
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
  WiFi.setAutoReconnect(true);
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
    Serial.printf("[wifi] STA connect failed (status=%d) — will keep retrying. "
                  "ESP32 only sees 2.4 GHz — is the SSID on 2.4 GHz?\n",
                  static_cast<int>(WiFi.status()));
    last_reconnect_ms_ = millis();
    return false;  // stays in Connecting; update() retries
  }
  mode_ = WifiMode::Station;
  Serial.printf("[wifi] STA OK %s  RSSI %d ch=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
  return true;
}

void WifiOta::scanNetworks() {
  Serial.println("[wifi] scanning 2.4 GHz…");
  if (mode_ == WifiMode::AccessPoint) {
    Serial.println("[wifi] setup AP active — scan skipped");
    return;
  }
  WiFi.mode(WIFI_STA);
  if (mode_ != WifiMode::Station) WiFi.disconnect(false);
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
  return false;  // keeps retrying STA in update(); no automatic AP
}

void WifiOta::startApPortal() {
  if (device_password_.length() < 8) {
    Serial.println("[wifi] no device password — setup AP disabled");
    return;
  }
  stopServices();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);
  // WPA2 with the per-device password (shown on the OLED).
  if (!WiFi.softAP(ap_ssid_.c_str(), device_password_.c_str())) {
    Serial.println("[wifi] softAP failed");
    mode_ = WifiMode::Off;
    return;
  }
  mode_ = WifiMode::AccessPoint;
  ssid_ = ap_ssid_;
  ap_last_activity_ms_ = millis();
  Serial.printf("[wifi] setup AP \"%s\"  IP %s  pass %s (on OLED), closes after "
                "%lu min idle\n",
                ap_ssid_.c_str(), WiFi.softAPIP().toString().c_str(),
                maskedPassword().c_str(),
                static_cast<unsigned long>(kSetupApTimeoutMs / 60000UL));
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
  if (device_password_.length() < 8) {
    Serial.println("[wifi] no device password — web UI / OTA not started");
    return;
  }
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
  Serial.println("[wifi] HTTP :80  (/  /wifi  /update) — Digest auth, user \"scale\"");

  ArduinoOTA.setHostname(kHostname);
  ArduinoOTA.setPassword(device_password_.c_str());
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
  Serial.printf("[ota] ArduinoOTA ready (password %s)\n", maskedPassword().c_str());

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

  uint8_t mac[6] = {};
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  ap_ssid_ = String(kApSsidPrefix) + suffix;

  if (!loadOrCreatePassword()) {
    WiFi.mode(WIFI_OFF);
    mode_ = WifiMode::Off;
    return false;
  }

  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    if (connectSta(ssid, pass)) startServices();
    // On failure: keep retrying STA (update()); the setup AP only opens on
    // request (Timer very-long press / serial "wifi ap").
    return true;
  }
  // First boot / after "wifi clear": no credentials -> setup AP (timed).
  Serial.println("[wifi] no saved credentials — starting setup AP");
  startApPortal();
  return true;
}

bool WifiOta::startSetupAp() {
  if (!g_wifi) g_wifi = this;
  startApPortal();
  return mode_ == WifiMode::AccessPoint;
}

void WifiOta::update() {
  const uint32_t now = millis();
  if ((mode_ == WifiMode::Station || mode_ == WifiMode::Connecting) &&
      WiFi.status() != WL_CONNECTED) {
    if (now - last_reconnect_ms_ > 10000) {
      last_reconnect_ms_ = now;
      Serial.println("[wifi] STA not connected — reconnecting");
      WiFi.reconnect();
    }
  }
  if (mode_ == WifiMode::Connecting && WiFi.status() == WL_CONNECTED) {
    mode_ = WifiMode::Station;
    Serial.printf("[wifi] STA OK %s\n", WiFi.localIP().toString().c_str());
    startServices();
  }
  if (mode_ == WifiMode::AccessPoint && !ota_active_) {
    if (WiFi.softAPgetStationNum() > 0) {
      ap_last_activity_ms_ = now;
    } else if (now - ap_last_activity_ms_ > kSetupApTimeoutMs) {
      Serial.println("[wifi] setup AP idle timeout — closing");
      stopServices();
      WiFi.softAPdisconnect(true);
      String ssid, pass;
      if (loadCredentials(ssid, pass)) {
        // Fall back to the saved network without blocking the loop.
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(kHostname);
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssid.c_str(), pass.c_str());
        ssid_ = ssid;
        mode_ = WifiMode::Connecting;
        last_reconnect_ms_ = now;
      } else {
        WiFi.mode(WIFI_OFF);
        mode_ = WifiMode::Off;
        ssid_ = "";
      }
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
    Serial.printf("[wifi] join AP \"%s\" (password on OLED) → http://%s/\n",
                  ap_ssid_.c_str(), WiFi.softAPIP().toString().c_str());
  }
  Serial.printf("[wifi] web user \"%s\", device password %s — hold Timer ~5 s "
                "or type 'wifi pass' to show it on the OLED\n",
                kWebUser, maskedPassword().c_str());
}
