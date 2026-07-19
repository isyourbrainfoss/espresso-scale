#pragma once

#include <Arduino.h>

// Non-blocking multi-note buzzer (soft duty cycle, musical pitches).

class Buzzer {
 public:
  void begin();
  void update();

  void beep();         // soft confirm (tare / tick)
  void doubleBeep();   // success / ready
  void longBeep();     // reset / attention
  void playTone(uint16_t hz, uint16_t ms);

  // Higher-level cues
  void bootChime();     // power-on
  void tareChime();     // tare done
  void timerStartChime();
  void timerStopChime();
  void timerResetChime();
  void successChime();  // cal OK
  void errorChime();
  void sleepChime();    // enter standby
  void wakeChime();     // leave standby

 private:
  static constexpr int kMaxNotes = 8;
  struct Note {
    uint16_t hz;
    uint16_t ms;
    uint16_t gap_ms;
  };

  Note queue_[kMaxNotes]{};
  int queue_len_ = 0;
  int queue_idx_ = 0;
  bool active_ = false;
  bool in_gap_ = false;
  uint32_t phase_end_ms_ = 0;

  void clearQueue();
  void enqueue(uint16_t hz, uint16_t ms, uint16_t gap_ms = 30);
  void startCurrent();
  void stopTone();
};
