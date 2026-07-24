#include "shot_recorder.h"

#include <SPIFFS.h>
#include <cstdio>
#include <time.h>

namespace {
const char* kShotPath = "/last_shot.json";
}

void ShotRecorder::begin() {
  if (!SPIFFS.begin(true)) {
    Serial.println("[shot] SPIFFS mount failed");
  } else {
    loadFromFlash();
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

void ShotRecorder::clear() {
  recording_ = false;
  sample_count_ = 0;
  flash_has_shot_ = false;
  SPIFFS.remove(kShotPath);
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

  // startedAt: use epoch if set, else synthetic UTC from millis.
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
    // Flow from consecutive weight samples (g/s) — matches Flowlog live metrics.
    if (i > 0 && s.w_dg != kNone && samples_[i - 1].w_dg != kNone) {
      const float dt =
          (static_cast<float>(s.t_cs) - static_cast<float>(samples_[i - 1].t_cs)) /
          100.0f;
      if (dt > 0.01f) {
        const float dw =
            (s.w_dg - samples_[i - 1].w_dg) / 10.0f;
        json += F(",\"flowGs\":");
        json += String(dw / dt, 3);
      }
    }
    json += '}';
  }
  json += F("]}");
  return json;
}

bool ShotRecorder::saveToFlash() {
  String json = toJson();
  if (json.isEmpty()) return false;
  File f = SPIFFS.open(kShotPath, FILE_WRITE);
  if (!f) {
    Serial.println("[shot] write open failed");
    return false;
  }
  size_t n = f.print(json);
  f.close();
  flash_has_shot_ = n > 20;
  Serial.printf("[shot] saved %u bytes to %s\n", static_cast<unsigned>(n),
                kShotPath);
  return n > 0;
}

bool ShotRecorder::loadFromFlash() {
  if (!SPIFFS.exists(kShotPath)) {
    flash_has_shot_ = false;
    return false;
  }
  File f = SPIFFS.open(kShotPath, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  f.close();
  flash_has_shot_ = sz > 20;
  Serial.printf("[shot] flash has last_shot.json (%u bytes)\n",
                static_cast<unsigned>(sz));
  return flash_has_shot_;
}

String ShotRecorder::readFlashJson() const {
  if (!SPIFFS.exists(kShotPath)) return String();
  File f = SPIFFS.open(kShotPath, FILE_READ);
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}
