#pragma once

#include <Arduino.h>

// Records a short brew for offline use; exports Flowlog-compatible JSON.
// Samples at ~10 Hz; capacity ~4 minutes.
// Keeps the last kMaxStoredShots shots on SPIFFS (ring buffer).

struct ShotSampleRec {
  uint16_t t_cs;    // centiseconds since shot start
  int16_t p_mbar;   // pressure millibar (INT16_MIN = none)
  int16_t w_dg;     // weight deci-grams (0.1 g), INT16_MIN = none
};

struct ShotSlotMeta {
  bool present = false;
  size_t bytes = 0;
  size_t samples = 0;
  String id;
  String startedAt;
  float yieldG = 0;
};

class ShotRecorder {
 public:
  static constexpr size_t kMaxSamples = 2400;  // 240 s @ 10 Hz
  static constexpr size_t kMaxStoredShots = 3;
  static constexpr int16_t kNone = INT16_MIN;

  void begin();
  void start();
  void stop();  // end recording and push into the flash ring
  void abort(); // end recording without saving
  bool isRecording() const { return recording_; }
  bool hasShot() const { return stored_count_ > 0 || (!recording_ && sample_count_ > 0); }
  size_t sampleCount() const { return sample_count_; }
  size_t storedCount() const { return stored_count_; }

  void add(float pressure_bar, bool has_p, float weight_g, bool has_w);

  // Flowlog Shot JSON (current RAM buffer, if any).
  String toJson() const;

  // Ring access: age 0 = newest, 1 = previous, 2 = oldest.
  String readSlotJson(size_t age) const;
  String readFlashJson() const { return readSlotJson(0); }  // newest
  ShotSlotMeta slotMeta(size_t age) const;
  // Compact list for GET /shots.json (no sample arrays).
  String listJson() const;

  bool saveToFlash();
  bool loadFromFlash();
  void clear();  // wipe RAM + all stored slots

  float lastWeightG() const;
  float lastPressureBar() const;

 private:
  bool recording_ = false;
  uint32_t start_ms_ = 0;
  uint32_t started_epoch_s_ = 0;
  size_t sample_count_ = 0;
  ShotSampleRec samples_[kMaxSamples];

  // Ring: next free physical slot index; stored_count_ in 0..kMaxStoredShots.
  uint8_t head_ = 0;
  uint8_t stored_count_ = 0;
  ShotSlotMeta meta_[kMaxStoredShots];

  size_t physicalIndex(size_t age) const;
  static void slotPath(size_t physical, char* out, size_t out_len);
  void persistIndex();
  void loadIndex();
  void refreshMetaFromFile(size_t physical);
  static ShotSlotMeta parseMetaFromJson(const String& json, size_t bytes);
  void migrateLegacyLastShot();
};
