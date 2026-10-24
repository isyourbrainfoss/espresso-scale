#include "wifi_ota.h"

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
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

constexpr const char* kNcNs = "nextcloud";
constexpr const char* kNcUrl = "url";
constexpr const char* kNcUser = "user";
constexpr const char* kNcPass = "pass";
constexpr const char* kNcPath = "path";
constexpr const char* kNcDefaultPath = "Flowlog/scale-last-shot.json";

WifiOta* g_wifi = nullptr;
WifiOta::WeightFn g_weight;
WifiOta::ShotJsonFn g_shot_json;
WifiOta::HasShotFn g_has_shot;
WifiOta::ShotsListFn g_shots_list;
WifiOta::ShotAtFn g_shot_at;
WifiOta::BootInfoFn g_boot_info;
WifiOta::TareFn g_tare;

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
  if (g_tare) {
    body += F("<form method=POST action=/tare><input type=hidden name=csrf "
              "value=\"");
    body += g_csrf;
    body += F("\"><button type=submit>Tare</button></form>");
  }
  if (g_boot_info) {
    body += F("<p class=meta>");
    body += htmlEscape(g_boot_info());
    body += F("</p>");
  }
  const bool has_shot = g_has_shot ? g_has_shot() : false;
  body += F("<p><b>Stored shots:</b> ");
  body += has_shot ? F("yes (up to 3)") : F("none");
  body += F(" · <a href=/shots.json>List</a>"
            " · <a href=/shot.json>Newest shot.json</a>"
            " · <a href=/shot/0.json>0</a>"
            " · <a href=/shot/1.json>1</a>"
            " · <a href=/shot/2.json>2</a></p>");
  body += F("<p><a href=/wifi>Wi‑Fi setup</a> · <a href=/update>OTA update</a>"
            " · <a href=/nextcloud>Nextcloud</a></p>");
  body += F("<p class=meta>PlatformIO wireless upload (ArduinoOTA) uses the same "
            "device password as this page. Example:<br>"
            "<code>HALF_DECENT_OTA_PASSWORD=&lt;device password&gt; "
            "pio run -t upload -e ota --upload-port ");
  body += ip;
  body += F("</code></p>");
  body += F("<p class=meta><b>Import into Flowlog:</b> History → Import from scale. "
            "Phones often cannot resolve <code>half-decent.local</code> — use this "
            "IP: <code>");
  body += ip;
  body += F("</code><br>"
            "Or import over Bluetooth when the scale is paired in Flowlog.</p>");
  server.send(200, "text/html", pageShell("Half Decent Scale", body));
}

void handleTarePost() {
  if (!guard(true)) return;
  if (g_tare) g_tare();
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "tared");
}

void handleShotJson() {
  if (!g_shot_json) {
    server.send(503, "application/json", "{\"error\":\"no shot provider\"}");
    return;
  }
  String json = g_shot_json();
  if (json.isEmpty()) {
    server.send(404, "application/json", "{\"error\":\"no shot recorded\"}");
    return;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleShotsList() {
  if (!g_shots_list) {
    server.send(503, "application/json", "{\"error\":\"no shot list\"}");
    return;
  }
  String json = g_shots_list();
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleShotAt() {
  // URI: /shot/0.json /shot/1.json /shot/2.json
  String uri = server.uri();
  int age = -1;
  if (uri.startsWith("/shot/") && uri.endsWith(".json")) {
    String mid = uri.substring(6, uri.length() - 5);
    age = mid.toInt();
  }
  if (age < 0 || age > 2 || !g_shot_at) {
    server.send(404, "application/json", "{\"error\":\"invalid shot index\"}");
    return;
  }
  String json = g_shot_at(static_cast<size_t>(age));
  if (json.isEmpty()) {
    server.send(404, "application/json", "{\"error\":\"no shot at index\"}");
    return;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleNextcloudGet() {
  if (!guard(false)) return;
  const String csrf_field =
      String(F("<input type=hidden name=csrf value=\"")) + g_csrf + "\">";
  String body = F("<h1>Nextcloud (optional)</h1>"
                  "<p class=meta>After a phone-free brew the scale can PUT "
                  "<code>last_shot.json</code> to your Nextcloud via WebDAV. "
                  "Leave blank to disable. Prefer app import when possible.</p>"
                  "<form method=POST action=/nextcloud>");
  body += csrf_field;
  body += F("<label>Server base URL"
                  "<input name=url placeholder=\"https://cloud.example.com\" "
                  "autocomplete=url></label>"
                  "<label>Username<input name=user autocomplete=username></label>"
                  "<label>App password<input name=pass type=password "
                  "autocomplete=current-password></label>"
                  "<label>Remote path"
                  "<input name=path value=\"Flowlog/scale-last-shot.json\"></label>"
                  "<button type=submit>Save</button></form>"
                  "<form method=POST action=/nextcloud-clear style=\"margin-top:1rem\">");
  body += csrf_field;
  body += F("<button type=submit>Clear credentials</button></form>"
            "<p><a href=/>Back</a></p>");
  server.send(200, "text/html", pageShell("Nextcloud", body));
}

void handleNextcloudPost() {
  if (!guard(true)) return;
  String url = server.hasArg("url") ? server.arg("url") : "";
  String user = server.hasArg("user") ? server.arg("user") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  String path = server.hasArg("path") ? server.arg("path") : kNcDefaultPath;
  url.trim();
  user.trim();
  path.trim();
  if (url.isEmpty() || user.isEmpty()) {
    server.send(400, "text/plain", "url and user required");
    return;
  }
  if (g_wifi) g_wifi->saveNextcloud(url, user, pass, path);
  server.send(200, "text/html",
              pageShell("Nextcloud",
                        F("<h1>Saved</h1><p>Nextcloud credentials stored.</p>"
                          "<p><a href=/>Back</a></p>")));
}

void handleNextcloudClear() {
  if (!guard(true)) return;
  if (g_wifi) g_wifi->clearNextcloud();
  server.send(200, "text/html",
              pageShell("Nextcloud",
                        F("<h1>Cleared</h1><p><a href=/>Back</a></p>")));
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

// Upload gate: auth, Host, Origin and CSRF are decided once per upload.
bool authorizeUpload() {
  if (g_wifi) g_wifi->noteActivity();
  if (!hostOk()) {
    g_upload.reason = "bad host";
    return false;
  }
  AuthResult r = checkDigest();
  noteAuthResult(r);
  g_upload.auth = static_cast<uint8_t>(r);
  if (r != AuthResult::Ok) {
    g_upload.auth_failed = true;
    g_upload.reason = "auth";
    return false;
  }
  if (!sameOrigin() || !csrfOk()) {
    g_upload.reason = "csrf/origin";
    return false;
  }
  return true;
}

// Upload callback: runs before handleUpdatePostDone. Authorisation is decided
// once at UPLOAD_FILE_START; rejected uploads are drained without touching
// flash.
void handleUpdatePost(HTTPUpload& upload) {
  if (upload.status == UPLOAD_FILE_START) {
    g_upload = UploadState{};
    g_upload.seen = true;
    if (!authorizeUpload()) return;
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

// POST /update. Not registered with server.on(uri, method, fn, ufn): in
// arduino-esp32 2.0.x such a handler also accepts *non-multipart* bodies via
// RequestHandler::raw(), which calls the upload callback while
// server.upload() is a null pointer -> LoadProhibited panic. curl --digest
// sends exactly that (an empty probe POST before the multipart one), and any
// unauthenticated client on the LAN could use it to reboot the scale.
// This handler refuses the raw path, so plain POSTs reach
// handleUpdatePostDone() and get a normal 401/400.
class UpdatePostHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override {
    return method == HTTP_POST && uri == "/update";
  }
  bool canUpload(String uri) override { return uri == "/update"; }
  bool canRaw(String) override { return false; }
  bool handle(WebServer&, HTTPMethod, String) override {
    handleUpdatePostDone();
    return true;
  }
  void upload(WebServer&, String, HTTPUpload& upload) override {
    handleUpdatePost(upload);
  }
};

void setupRoutes() {
  if (server_routes_ok) return;
  static const char* kHeaders[] = {"Origin", "Referer"};
  server.collectHeaders(kHeaders, 2);  // Authorization is always collected
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_GET, handleWifiGet);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on("/tare", HTTP_POST, handleTarePost);
  server.on("/shot.json", HTTP_GET, handleShotJson);
  server.on("/shots.json", HTTP_GET, handleShotsList);
  server.on("/shot/0.json", HTTP_GET, handleShotAt);
  server.on("/shot/1.json", HTTP_GET, handleShotAt);
  server.on("/shot/2.json", HTTP_GET, handleShotAt);
  server.on("/nextcloud", HTTP_GET, handleNextcloudGet);
  server.on("/nextcloud", HTTP_POST, handleNextcloudPost);
  server.on("/nextcloud-clear", HTTP_POST, handleNextcloudClear);
  server.addHandler(new UpdatePostHandler());
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

// Load (or on first use generate) the device password once the radio is on,
// so esp_random() is RF-seeded. Announces a freshly generated password once.
static void ensureDeviceAuth() {
  static bool announced = false;
  device_auth::begin();
  if (device_auth::generatedThisBoot() && !announced) {
    announced = true;
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
    if (g_wifi) g_wifi->requestSetupScreen();
  }
}

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

void WifiOta::radioOff() {
  boot_ = BootStep::Done;
  stopServices();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  mode_ = WifiMode::Off;
  sta_lost_since_ms_ = 0;
  Serial.println("[wifi] radio off — Bluetooth only");
}

bool WifiOta::savedSsidVisible(const String& ssid) {
  // Blocking directed scan (serial `wifi connect`, AP idle close). If the
  // home SSID is absent this returns in about a second.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  delay(40);
  const int n = WiFi.scanNetworks(
      /*async=*/false, /*show_hidden=*/true, /*passive=*/false,
      kWifiScanMsPerChan, /*channel=*/0, ssid.c_str());
  bool found = false;
  if (n > 0) {
    for (int i = 0; i < n; ++i) {
      if (ssid == WiFi.SSID(i)) {
        found = true;
        Serial.printf("[wifi] saw \"%s\" ch=%d RSSI=%d\n",
                      WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
        break;
      }
    }
  }
  WiFi.scanDelete();
  if (!found) {
    Serial.printf("[wifi] saved \"%s\" not in range (scan=%d)\n", ssid.c_str(),
                  n);
  }
  return found;
}

void WifiOta::beginJoin(const String& ssid, const String& pass,
                        int32_t channel, const uint8_t* bssid) {
  mode_ = WifiMode::Connecting;
  ssid_ = ssid;

  // ESP32-S3 is 2.4 GHz only — 5 GHz APs with the same SSID are invisible.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 2
  // Prefer full 2.4 GHz scan / best signal among 2.4 GHz BSS.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
#endif
  // With channel + BSSID from the directed scan the driver joins directly
  // instead of scanning all channels a second time.
  WiFi.begin(ssid.c_str(), pass.c_str(), channel, bssid);
  join_start_ms_ = millis();
  Serial.printf("[wifi] connecting to \"%s\" (2.4 GHz only, ch=%d)…\n",
                ssid.c_str(), static_cast<int>(channel));
}

bool WifiOta::connectSta(const String& ssid, const String& pass) {
  WiFi.disconnect(true, true);
  delay(100);
  beginJoin(ssid, pass, 0, nullptr);

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
    radioOff();
    return false;
  }
  if (boot_ == BootStep::Scanning) WiFi.scanDelete();
  boot_ = BootStep::Done;
  stopServices();
  if (savedSsidVisible(ssid) && connectSta(ssid, pass)) {
    startServices();
    return true;
  }
  radioOff();
  return false;
}

void WifiOta::startApPortal() {
  if (boot_ == BootStep::Scanning) WiFi.scanDelete();
  boot_ = BootStep::Done;
  stopServices();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_AP);
  ensureDeviceAuth();
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
  WiFi.mode(WIFI_OFF);
  mode_ = WifiMode::Off;
  ssid_ = "";
  String ssid, pass;
  if (loadCredentials(ssid, pass)) {
    // Same one-shot directed scan + join as serial `wifi connect`; radio
    // goes off again if the saved network is not around (v1.6.3 policy).
    Serial.println("[wifi] setup AP closed — trying saved network once");
    tryConnectSaved();
  } else {
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
  ensureDeviceAuth();
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
  Serial.println(
      "[wifi] HTTP :80  (/  /wifi  /nextcloud  /update: digest auth, user "
      "\"admin\"; /shot.json  /shots.json  /shot/N.json: open, read-only)");

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
  if (boot_ == BootStep::Scanning) WiFi.scanDelete();
  boot_ = BootStep::Done;
  boot_pass_ = "";
  stopServices();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  mode_ = WifiMode::Off;
  g_wifi = nullptr;
  Serial.println("[wifi] off");
}

bool WifiOta::begin(WeightFn weight_fn, ShotJsonFn shot_json_fn,
                    HasShotFn has_shot_fn, ShotsListFn shots_list_fn,
                    ShotAtFn shot_at_fn, bool force_setup_ap) {
  weight_fn_ = std::move(weight_fn);
  shot_json_fn_ = std::move(shot_json_fn);
  has_shot_fn_ = std::move(has_shot_fn);
  shots_list_fn_ = std::move(shots_list_fn);
  shot_at_fn_ = std::move(shot_at_fn);
  g_wifi = this;
  g_weight = weight_fn_;
  g_shot_json = shot_json_fn_;
  g_has_shot = has_shot_fn_;
  g_shots_list = shots_list_fn_;
  g_shot_at = shot_at_fn_;
  WiFi.persistent(false);

  boot_ = BootStep::Done;
  if (force_setup_ap) {
    Serial.println("[wifi] setup hotspot requested");
    startApPortal();
    return true;
  }

  String ssid, pass;
  if (!loadCredentials(ssid, pass)) {
    Serial.println("[wifi] no saved credentials — radio off. Hold Timer 5 s "
                   "(or serial `wifi ap`) to open the setup AP.");
    radioOff();
    return true;
  }
  // Directed scan in the background, before BLE is up. If the home SSID is
  // absent it ends in about a second and the radio goes off — no join
  // attempt, no setup AP (that combination with BLE rebooted the scale away
  // from home).
  WiFi.setHostname(kHostname);  // applied when the STA interface starts
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  const int16_t r = WiFi.scanNetworks(
      /*async=*/true, /*show_hidden=*/true, /*passive=*/false,
      kWifiScanMsPerChan, /*channel=*/0, ssid.c_str());
  if (r == WIFI_SCAN_FAILED) {
    Serial.println("[wifi] scan failed to start — radio off");
    radioOff();
    return true;
  }
  ssid_ = ssid;
  boot_pass_ = pass;
  mode_ = WifiMode::Connecting;
  boot_ = BootStep::Scanning;
  join_start_ms_ = millis();  // scan start until the join begins
  return true;
}

void WifiOta::setBootInfo(BootInfoFn fn) { g_boot_info = std::move(fn); }
void WifiOta::setTareHandler(TareFn fn) { g_tare = std::move(fn); }

void WifiOta::bootStep() {
  if (boot_ == BootStep::Scanning) {
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    // scanComplete() reports FAILED once max_ms_per_chan * 20 has passed,
    // but the active scan dwells >= 100 ms per channel, so a 13-channel
    // scan can outlast that. The real result still arrives (scanComplete()
    // then returns the count); only give up after a hard deadline.
    if (n == WIFI_SCAN_FAILED && millis() - join_start_ms_ < kWifiScanDeadlineMs) {
      return;
    }
    int32_t channel = 0;
    uint8_t bssid[6] = {};
    bool found = false;
    for (int i = 0; i < n; ++i) {  // n < 0 (failed): loop skipped
      if (ssid_ == WiFi.SSID(i)) {
        found = true;
        channel = WiFi.channel(i);
        memcpy(bssid, WiFi.BSSID(i), sizeof(bssid));
        Serial.printf("[wifi] saw \"%s\" ch=%d RSSI=%d\n", ssid_.c_str(),
                      static_cast<int>(channel), WiFi.RSSI(i));
        break;
      }
    }
    WiFi.scanDelete();
    if (!found) {
      Serial.printf("[wifi] saved \"%s\" not in range (scan=%d)\n",
                    ssid_.c_str(), n);
      boot_pass_ = "";
      boot_ = BootStep::Done;
      radioOff();
      return;
    }
    beginJoin(ssid_, boot_pass_, channel, bssid);
    boot_pass_ = "";
    boot_ = BootStep::Joining;
    return;
  }
  if (boot_ == BootStep::Joining) {
    if (WiFi.status() == WL_CONNECTED) {
      mode_ = WifiMode::Station;
      boot_ = BootStep::Done;
      Serial.printf("[wifi] STA OK %s  RSSI %d ch=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                    WiFi.channel());
      startServices();
    } else if (millis() - join_start_ms_ >= kWifiConnectTimeoutMs) {
      Serial.printf("[wifi] STA connect failed (status=%d)\n",
                    static_cast<int>(WiFi.status()));
      boot_ = BootStep::Done;
      radioOff();
    }
  }
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
  if (boot_ == BootStep::Scanning || boot_ == BootStep::Joining) {
    bootStep();
    return;
  }
  const uint32_t now = millis();
  if (mode_ == WifiMode::Station && WiFi.status() != WL_CONNECTED) {
    if (sta_lost_since_ms_ == 0) {
      sta_lost_since_ms_ = now;
      Serial.println("[wifi] STA lost — radio off if it stays down");
    } else if (now - sta_lost_since_ms_ >= kWifiStaGiveUpMs) {
      Serial.println("[wifi] STA still down — radio off until next boot");
      radioOff();
      return;
    }
  } else if (mode_ == WifiMode::Station) {
    sta_lost_since_ms_ = 0;
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
    default:
      return "";
  }
}

void WifiOta::printStatus() const {
  Serial.printf("[wifi] mode=%s ssid=\"%s\" ip=%s\n", modeLabel(),
                ssid_.c_str(), ipString().c_str());
  if (mode_ == WifiMode::Station) {
    Serial.printf("[wifi] RSSI=%d hostname=%s.local\n", WiFi.RSSI(), kHostname);
    Serial.printf("[wifi] shot export: http://%s/shot.json\n",
                  ipString().c_str());
  }
  if (mode_ == WifiMode::AccessPoint) {
    Serial.printf("[wifi] join AP \"%s\" (password on OLED) → http://%s/  "
                  "closes in %lus if idle\n",
                  device_auth::apSsid().c_str(),
                  WiFi.softAPIP().toString().c_str(),
                  static_cast<unsigned long>(apSecondsLeft()));
  }
  Serial.printf("[wifi] nextcloud=%s\n", hasNextcloud() ? "configured" : "off");
}

bool WifiOta::saveNextcloud(const String& base_url, const String& user,
                            const String& pass, const String& remote_path) {
  if (!wifi_prefs.begin(kNcNs, false)) return false;
  wifi_prefs.putString(kNcUrl, base_url);
  wifi_prefs.putString(kNcUser, user);
  wifi_prefs.putString(kNcPass, pass);
  wifi_prefs.putString(kNcPath, remote_path.length() ? remote_path
                                                     : kNcDefaultPath);
  wifi_prefs.end();
  Serial.printf("[nc] saved url=%s user=%s path=%s\n", base_url.c_str(),
                user.c_str(),
                remote_path.length() ? remote_path.c_str() : kNcDefaultPath);
  return true;
}

void WifiOta::clearNextcloud() {
  if (wifi_prefs.begin(kNcNs, false)) {
    wifi_prefs.clear();
    wifi_prefs.end();
  }
  Serial.println("[nc] credentials cleared");
}

bool WifiOta::hasNextcloud() const {
  if (!wifi_prefs.begin(kNcNs, true)) return false;
  String url = wifi_prefs.getString(kNcUrl, "");
  wifi_prefs.end();
  return url.length() > 0;
}

bool WifiOta::pushShotToNextcloud(const String& json) {
  if (json.isEmpty()) return false;
  if (mode_ != WifiMode::Station || WiFi.status() != WL_CONNECTED) {
    Serial.println("[nc] skip push — not on WiFi STA");
    return false;
  }
  if (!wifi_prefs.begin(kNcNs, true)) return false;
  String base = wifi_prefs.getString(kNcUrl, "");
  String user = wifi_prefs.getString(kNcUser, "");
  String pass = wifi_prefs.getString(kNcPass, "");
  String path = wifi_prefs.getString(kNcPath, kNcDefaultPath);
  wifi_prefs.end();
  if (base.isEmpty() || user.isEmpty()) return false;

  // Build WebDAV URL: {base}/remote.php/dav/files/{user}/{path}
  while (base.endsWith("/")) base.remove(base.length() - 1);
  if (path.startsWith("/")) path = path.substring(1);
  String url = base + "/remote.php/dav/files/" + user + "/" + path;

  HTTPClient http;
  http.setTimeout(15000);
  http.begin(url);
  http.setAuthorization(user.c_str(), pass.c_str());
  http.addHeader("Content-Type", "application/json");
  Serial.printf("[nc] PUT %s (%u bytes)\n", url.c_str(),
                static_cast<unsigned>(json.length()));
  int code = http.PUT(json);
  http.end();
  if (code >= 200 && code < 300) {
    Serial.printf("[nc] push OK (%d)\n", code);
    return true;
  }
  Serial.printf("[nc] push failed HTTP %d\n", code);
  return false;
}
