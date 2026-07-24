#pragma once

#include <Arduino.h>

// Records a short brew for offline use; exports Flowlog-compatible JSON.
// Samples at ~10 Hz; capacity ~4 minutes.

struct ShotSampleRec {
  uint16_t t_cs;    // centiseconds since shot start
  int16_t p_mbar;   // pressure millibar (INT16_MIN = none)
  int16_t w_dg;     // weight deci-grams (0.1 g), INT16_MIN = none
};

class ShotRecorder {
 public:
  static constexpr size_t kMaxSamples = 2400;  // 240 s @ 10 Hz
  static constexpr int16_t kNone = INT16_MIN;

  void begin();
  void start();
  void stop();
  bool isRecording() const { return recording_; }
  bool hasShot() const {
    return (!recording_ && sample_count_ > 0) || flash_has_shot_;
  }
  size_t sampleCount() const { return sample_count_; }

  void add(float pressure_bar, bool has_p, float weight_g, bool has_w);

  // Flowlog Shot JSON (subset).
  String toJson() const;
  String readFlashJson() const;  // last saved file (may outlive RAM buffer)
  bool saveToFlash();
  bool loadFromFlash();
  void clear();

  float lastWeightG() const;
  float lastPressureBar() const;

 private:
  bool recording_ = false;
  bool flash_has_shot_ = false;
  uint32_t start_ms_ = 0;
  uint32_t started_epoch_s_ = 0;  // approx wall clock if available
  size_t sample_count_ = 0;
  ShotSampleRec samples_[kMaxSamples];
};
