#include "shot_recorder.h"

#include <Preferences.h>
#include <SPIFFS.h>
#include <cstdio>
#include <ctime>

namespace {
constexpr const char* kNvsNs = "shots";
constexpr const char* kNvsHead = "head";
constexpr const char* kNvsCount = "count";
constexpr const char* kLegacyPath = "/last_shot.json";
}  // namespace

void ShotRecorder::slotPath(size_t physical, char* out, size_t out_len) {
  snprintf(out, out_len, "/s%u.json", static_cast<unsigned>(physical % kMaxStoredShots));
}

size_t ShotRecorder::physicalIndex(size_t age) const {
  if (stored_count_ == 0 || age >= stored_count_) return 0;
  // Newest is the last written slot: (head_ + kMax - 1) % kMax
  return (head_ + kMaxStoredShots - 1 - age) % kMaxStoredShots;
}

void ShotRecorder::begin() {
  if (!SPIFFS.begin(true)) {
    Serial.println("[shot] SPIFFS mount failed");
    return;
  }
  loadIndex();
  migrateLegacyLastShot();
  for (size_t i = 0; i < stored_count_; i++) {
    refreshMetaFromFile(physicalIndex(i));
  }
  Serial.printf("[shot] ring: %u stored (head=%u)\n",
                static_cast<unsigned>(stored_count_),
                static_cast<unsigned>(head_));
}

void ShotRecorder::migrateLegacyLastShot() {
  if (!SPIFFS.exists(kLegacyPath)) return;
  if (stored_count_ > 0) {
    // Already have ring data — drop legacy to free space.
    SPIFFS.remove(kLegacyPath);
    return;
  }
  File f = SPIFFS.open(kLegacyPath, FILE_READ);
  if (!f) return;
  String json = f.readString();
  f.close();
  if (json.length() < 20) {
    SPIFFS.remove(kLegacyPath);
    return;
  }
  char path[16];
  slotPath(0, path, sizeof(path));
  File out = SPIFFS.open(path, FILE_WRITE);
  if (out) {
    out.print(json);
    out.close();
    head_ = 1;
    stored_count_ = 1;
    persistIndex();
    meta_[0] = parseMetaFromJson(json, json.length());
    Serial.printf("[shot] migrated legacy last_shot.json → %s\n", path);
  }
  SPIFFS.remove(kLegacyPath);
}

void ShotRecorder::persistIndex() {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return;
  p.putUChar(kNvsHead, head_);
  p.putUChar(kNvsCount, stored_count_);
  p.end();
}

void ShotRecorder::loadIndex() {
  Preferences p;
  if (!p.begin(kNvsNs, true)) {
    head_ = 0;
    stored_count_ = 0;
    return;
  }
  head_ = p.getUChar(kNvsHead, 0);
  stored_count_ = p.getUChar(kNvsCount, 0);
  p.end();
  if (head_ >= kMaxStoredShots) head_ = 0;
  if (stored_count_ > kMaxStoredShots) stored_count_ = kMaxStoredShots;

  // Verify files still exist.
  uint8_t present = 0;
  for (uint8_t age = 0; age < stored_count_; age++) {
    char path[16];
    slotPath(physicalIndex(age), path, sizeof(path));
    if (SPIFFS.exists(path)) present++;
  }
  if (present == 0) {
    stored_count_ = 0;
    head_ = 0;
  }
}

void ShotRecorder::start() {
  sample_count_ = 0;
  recording_ = true;
  start_ms_ = millis();
  time_t now = time(nullptr);
  started_epoch_s_ = (now > 100000) ? static_cast<uint32_t>(now) : 0;
  Serial.println("[shot] recording start");
}

void ShotRecorder::stop() {
  if (!recording_) return;
  recording_ = false;
  Serial.printf("[shot] stop — %u samples\n",
                static_cast<unsigned>(sample_count_));
  saveToFlash();
}

void ShotRecorder::abort() {
  if (!recording_ && sample_count_ == 0) return;
  recording_ = false;
  sample_count_ = 0;
  Serial.println("[shot] abort — not saved");
}

void ShotRecorder::clear() {
  recording_ = false;
  sample_count_ = 0;
  for (size_t i = 0; i < kMaxStoredShots; i++) {
    char path[16];
    slotPath(i, path, sizeof(path));
    SPIFFS.remove(path);
    meta_[i] = ShotSlotMeta{};
  }
  SPIFFS.remove(kLegacyPath);
  head_ = 0;
  stored_count_ = 0;
  persistIndex();
}

void ShotRecorder::add(float pressure_bar, bool has_p, float weight_g,
                       bool has_w) {
  if (!recording_ || sample_count_ >= kMaxSamples) return;
  const uint32_t elapsed = millis() - start_ms_;
  ShotSampleRec& s = samples_[sample_count_++];
  s.t_cs = static_cast<uint16_t>((elapsed / 10) > 65535 ? 65535 : elapsed / 10);
  if (has_p) {
    long mbar = lroundf(pressure_bar * 1000.0f);
    if (mbar > 32767) mbar = 32767;
    if (mbar < -32768) mbar = -32768;
    s.p_mbar = static_cast<int16_t>(mbar);
  } else {
    s.p_mbar = kNone;
  }
  if (has_w) {
    long dg = lroundf(weight_g * 10.0f);
    if (dg > 32767) dg = 32767;
    if (dg < -32768) dg = -32768;
    s.w_dg = static_cast<int16_t>(dg);
  } else {
    s.w_dg = kNone;
  }
}

float ShotRecorder::lastWeightG() const {
  if (sample_count_ == 0) return 0;
  const int16_t w = samples_[sample_count_ - 1].w_dg;
  if (w == kNone) return 0;
  return w / 10.0f;
}

float ShotRecorder::lastPressureBar() const {
  if (sample_count_ == 0) return 0;
  const int16_t p = samples_[sample_count_ - 1].p_mbar;
  if (p == kNone) return 0;
  return p / 1000.0f;
}

String ShotRecorder::toJson() const {
  if (sample_count_ == 0) return String();

  char id[40];
  char started[40];
  char ended[40];
  uint32_t duration_ms = samples_[sample_count_ - 1].t_cs * 10UL;
  if (started_epoch_s_ > 0) {
    time_t t0 = static_cast<time_t>(started_epoch_s_);
    time_t t1 = t0 + duration_ms / 1000;
    strftime(started, sizeof(started), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t0));
    strftime(ended, sizeof(ended), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t1));
    snprintf(id, sizeof(id), "shot-scale-%lu",
             static_cast<unsigned long>(started_epoch_s_));
  } else {
    snprintf(id, sizeof(id), "shot-scale-%lu",
             static_cast<unsigned long>(millis()));
    snprintf(started, sizeof(started), "1970-01-01T00:00:00.000Z");
    snprintf(ended, sizeof(ended), "1970-01-01T00:00:%02lu.000Z",
             static_cast<unsigned long>((duration_ms / 1000) % 60));
  }

  float yield_g = lastWeightG();
  String json;
  json.reserve(sample_count_ * 48 + 256);
  json += F("{\"id\":\"");
  json += id;
  json += F("\",\"startedAt\":\"");
  json += started;
  json += F("\",\"endedAt\":\"");
  json += ended;
  json += F("\",\"scale\":\"Flowlog Scale\"");
  if (yield_g > 0.5f) {
    json += F(",\"yieldG\":");
    json += String(yield_g, 1);
  }
  json += F(",\"notes\":\"Recorded on Flowlog Scale (standalone)\",\"samples\":[");

  for (size_t i = 0; i < sample_count_; i++) {
    if (i) json += ',';
    const ShotSampleRec& s = samples_[i];
    json += F("{\"elapsedMs\":");
    json += String(static_cast<unsigned>(s.t_cs) * 10U);
    if (s.p_mbar != kNone) {
      json += F(",\"pressureBar\":");
      json += String(s.p_mbar / 1000.0f, 3);
    }
    if (s.w_dg != kNone) {
      json += F(",\"weightG\":");
      json += String(s.w_dg / 10.0f, 2);
    }
    if (i > 0 && s.w_dg != kNone && samples_[i - 1].w_dg != kNone) {
      const float dt =
          (static_cast<float>(s.t_cs) - static_cast<float>(samples_[i - 1].t_cs)) /
          100.0f;
      if (dt > 0.01f) {
        const float dw = (s.w_dg - samples_[i - 1].w_dg) / 10.0f;
        json += F(",\"flowGs\":");
        json += String(dw / dt, 3);
      }
    }
    json += '}';
  }
  json += F("]}");
  return json;
}

ShotSlotMeta ShotRecorder::parseMetaFromJson(const String& json, size_t bytes) {
  ShotSlotMeta m;
  m.present = json.length() > 20;
  m.bytes = bytes;
  // Lightweight field scrape (avoid full JSON parser on MCU).
  auto extract = [&](const char* key) -> String {
    String pat = String("\"") + key + "\":\"";
    int i = json.indexOf(pat);
    if (i < 0) return String();
    i += pat.length();
    int j = json.indexOf('"', i);
    if (j < 0) return String();
    return json.substring(i, j);
  };
  m.id = extract("id");
  m.startedAt = extract("startedAt");
  int yi = json.indexOf("\"yieldG\":");
  if (yi >= 0) {
    m.yieldG = json.substring(yi + 9).toFloat();
  }
  // Count samples via "elapsedMs" occurrences.
  size_t count = 0;
  int pos = 0;
  while (true) {
    int n = json.indexOf("\"elapsedMs\"", pos);
    if (n < 0) break;
    count++;
    pos = n + 11;
  }
  m.samples = count;
  return m;
}

void ShotRecorder::refreshMetaFromFile(size_t physical) {
  char path[16];
  slotPath(physical, path, sizeof(path));
  if (!SPIFFS.exists(path)) {
    meta_[physical] = ShotSlotMeta{};
    return;
  }
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) {
    meta_[physical] = ShotSlotMeta{};
    return;
  }
  size_t sz = f.size();
  // Only need the header for meta; still read all for sample count accuracy
  // on small files — cap read for huge ones.
  String json;
  if (sz <= 4096) {
    json = f.readString();
  } else {
    // Read first 512 for id/startedAt/yield; estimate samples from size.
    char buf[513];
    size_t n = f.readBytes(buf, 512);
    buf[n] = 0;
    json = String(buf);
    f.close();
    ShotSlotMeta m = parseMetaFromJson(json, sz);
    m.bytes = sz;
    if (m.samples == 0) {
      m.samples = sz / 48;  // rough
    }
    meta_[physical] = m;
    return;
  }
  f.close();
  meta_[physical] = parseMetaFromJson(json, sz);
}

bool ShotRecorder::saveToFlash() {
  String json = toJson();
  if (json.isEmpty()) return false;

  char path[16];
  slotPath(head_, path, sizeof(path));
  File f = SPIFFS.open(path, FILE_WRITE);
  if (!f) {
    Serial.println("[shot] write open failed");
    return false;
  }
  size_t n = f.print(json);
  f.close();
  if (n < 20) {
    Serial.println("[shot] write too short");
    return false;
  }

  meta_[head_] = parseMetaFromJson(json, n);
  head_ = static_cast<uint8_t>((head_ + 1) % kMaxStoredShots);
  if (stored_count_ < kMaxStoredShots) {
    stored_count_++;
  }
  persistIndex();
  Serial.printf("[shot] saved %u bytes → %s (stored=%u)\n",
                static_cast<unsigned>(n), path,
                static_cast<unsigned>(stored_count_));
  return true;
}

bool ShotRecorder::loadFromFlash() {
  loadIndex();
  for (size_t i = 0; i < kMaxStoredShots; i++) {
    refreshMetaFromFile(i);
  }
  return stored_count_ > 0;
}

String ShotRecorder::readSlotJson(size_t age) const {
  if (age >= stored_count_) return String();
  char path[16];
  slotPath(physicalIndex(age), path, sizeof(path));
  if (!SPIFFS.exists(path)) return String();
  File f = SPIFFS.open(path, FILE_READ);
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

ShotSlotMeta ShotRecorder::slotMeta(size_t age) const {
  if (age >= stored_count_) return ShotSlotMeta{};
  return meta_[physicalIndex(age)];
}

String ShotRecorder::listJson() const {
  String json;
  json.reserve(256 + stored_count_ * 120);
  json += F("{\"count\":");
  json += String(stored_count_);
  json += F(",\"max\":");
  json += String(kMaxStoredShots);
  json += F(",\"shots\":[");
  for (size_t age = 0; age < stored_count_; age++) {
    if (age) json += ',';
    ShotSlotMeta m = slotMeta(age);
    json += F("{\"index\":");
    json += String(age);
    json += F(",\"id\":\"");
    json += m.id;
    json += F("\",\"startedAt\":\"");
    json += m.startedAt;
    json += F("\",\"samples\":");
    json += String(m.samples);
    json += F(",\"bytes\":");
    json += String(m.bytes);
    json += F(",\"yieldG\":");
    json += String(m.yieldG, 1);
    json += F(",\"path\":\"/shot/");
    json += String(age);
    json += F(".json\"}");
  }
  json += F("]}");
  return json;
}
