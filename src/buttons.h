#pragma once

#include <Arduino.h>

enum class BtnEvent : uint8_t {
  None = 0,
  TareShort,
  TimerShort,
  TimerLong,
  BothHeldCal,
};

class Buttons {
 public:
  void begin();
  // Poll once per loop; returns at most one discrete event per call.
  BtnEvent poll();

 private:
  bool tare_down_ = false;
  bool timer_down_ = false;
  uint32_t tare_down_ms_ = 0;
  uint32_t timer_down_ms_ = 0;
  uint32_t both_down_ms_ = 0;
  bool both_fired_ = false;
  bool tare_long_consumed_ = false;
  bool timer_long_consumed_ = false;

  bool readTare() const;
  bool readTimer() const;
};
