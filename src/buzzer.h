#pragma once

#include <Arduino.h>

class Buzzer {
 public:
  void begin();
  void update();  // non-blocking; call from loop

  void beep();           // short confirm
  void doubleBeep();     // success
  void longBeep();       // error / reset
  void playTone(uint16_t hz, uint16_t ms);

 private:
  bool active_ = false;
  uint32_t end_ms_ = 0;
  int remaining_ = 0;
  uint16_t next_hz_ = 0;
  uint16_t next_ms_ = 0;
  uint32_t gap_until_ = 0;

  void startTone(uint16_t hz, uint16_t ms);
  void stopTone();
};
