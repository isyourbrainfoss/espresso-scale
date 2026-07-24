#pragma once

#include <Arduino.h>

// OLED / brew cues for the DIY scale. Defaults match Flowlog brew defaults;
// persisted in NVS and updated live via BLE type 0xF3 from the app.

struct ScaleDisplaySettings {
  float target_yield_g = 36.0f;
  float warn_at_g = 34.0f;
  float pressure_min_bar = 5.0f;
  float pressure_max_bar = 10.0f;
};

class ScaleSettings {
 public:
  void begin();  // load NVS (or defaults)
  const ScaleDisplaySettings& get() const { return s_; }

  // Apply and save. Clamps to safe ranges.
  bool apply(float target_g, float warn_g, float p_min, float p_max);
  bool applyFromBytes(uint8_t target_g, uint8_t warn_g, uint8_t p_min,
                      uint8_t p_max);

 private:
  ScaleDisplaySettings s_;
  void save();
  void load();
  void clamp();
};
