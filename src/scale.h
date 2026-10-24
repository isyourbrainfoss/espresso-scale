#pragma once

#include <Arduino.h>

// HX711 scale: raw samples, tare, calibration factor, OLED-filtered grams.
//
// Zeroing is non-blocking. begin() only powers the HX711 up; update() lets it
// settle, then averages one fresh sample window (6 conversions) for the boot
// zero while loop() is already drawing the weight screen.
//
// The scale also remembers the raw reading of the *empty* platform in NVS
// ("empty zero"). At boot it tares as usual when the platform reads within
// kBootZeroLoadG of that reference (absorbs normal drift), but when it reads
// clearly heavier (a cup was left on the scale while it slept) it keeps the
// empty zero instead, so lifting the cup reads 0 g rather than -cup.

class Scale {
 public:
  enum class BootZero : uint8_t { Pending, First, Tared, KeptEmptyZero };

  bool begin();
  void update();  // call frequently; non-blocking sample grab + tare state

  // Latest raw grams (minimal filtering) for BLE.
  float rawGrams() const { return raw_g_; }
  // Smoothed grams for OLED.
  float displayGrams() const { return filtered_g_; }

  bool isReady() const { return ready_; }
  // True once the boot zero is in place and grams are live.
  bool bootZeroDone() const { return boot_zero_ != BootZero::Pending; }
  BootZero bootZero() const { return boot_zero_; }
  uint32_t bootZeroMs() const { return boot_zero_ms_; }
  // Platform load vs the stored empty zero at boot (grams), for the status page.
  float bootLoadG() const { return boot_load_g_; }
  float sps() const;
  bool hasNewSample() const { return new_sample_; }
  void clearNewSample() { new_sample_ = false; }

  // Non-blocking by default — BLE command handlers must not stall the stack.
  // Pass wait=true only for calibration / serial flows that need a settled zero.
  void tare(bool wait = false);
  bool tarePending() const { return phase_ != Phase::Running; }
  void powerDown();  // HX711 low-power before deep sleep
  float calFactor() const { return cal_factor_; }
  void setCalFactor(float factor, bool save = true);
  void clearEmptyZero();  // forget the stored empty-platform reference

  // Calibration helpers: average raw (untared) reading for known mass.
  bool startCalEmpty();   // tare empty platform (also stores the empty zero)
  bool finishCalKnownMass(float mass_g);  // capture with mass, compute factor

  float emptyRaw() const { return empty_raw_; }

 private:
  enum class Phase : uint8_t { Settling, Zeroing, Running };
  Phase phase_ = Phase::Settling;
  bool boot_pending_ = true;  // current zeroing pass is the boot zero
  BootZero boot_zero_ = BootZero::Pending;
  uint32_t power_up_ms_ = 0;
  uint32_t zero_start_ms_ = 0;
  uint32_t boot_zero_ms_ = 0;
  float boot_load_g_ = 0;
  uint8_t zero_n_ = 0;
  long zero_min_ = 0;
  long zero_max_ = 0;

  bool has_empty_zero_ = false;
  long empty_zero_ = 0;  // raw average (HX711 counts) of the empty platform

  bool ready_ = false;
  bool new_sample_ = false;
  float cal_factor_ = 0;
  float raw_g_ = 0;
  float filtered_g_ = 0;
  float empty_raw_ = 0;
  bool empty_captured_ = false;

  void startZeroing();
  void finishZeroing(bool timed_out);
  void storeEmptyZero(long avg, bool force);
  float countsToGrams(long counts) const;
  bool loadCalFromNvs();
  void saveCalToNvs() const;
};
