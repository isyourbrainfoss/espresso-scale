#pragma once

#include <Arduino.h>

// HX711 scale: raw samples, tare, calibration factor, OLED-filtered grams.

class Scale {
 public:
  bool begin();
  void update();  // call frequently; non-blocking sample grab

  // Latest raw grams (minimal filtering) for BLE.
  float rawGrams() const { return raw_g_; }
  // Smoothed grams for OLED.
  float displayGrams() const { return filtered_g_; }

  bool isReady() const { return ready_; }
  bool hasNewSample() const { return new_sample_; }
  void clearNewSample() { new_sample_ = false; }

  void tare();
  float calFactor() const { return cal_factor_; }
  void setCalFactor(float factor, bool save = true);

  // Calibration helpers: average raw (untared) reading for known mass.
  bool startCalEmpty();   // capture empty platform raw average
  bool finishCalKnownMass(float mass_g);  // capture with mass, compute factor

  float emptyRaw() const { return empty_raw_; }

 private:
  bool ready_ = false;
  bool new_sample_ = false;
  float cal_factor_ = 0;
  long tare_offset_ = 0;
  float raw_g_ = 0;
  float filtered_g_ = 0;
  float empty_raw_ = 0;
  bool empty_captured_ = false;

  bool loadCalFromNvs();
  void saveCalToNvs() const;
  float rawToGrams(long raw) const;
};
