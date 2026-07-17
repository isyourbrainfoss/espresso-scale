#pragma once

#include <Arduino.h>

class ShotTimer {
 public:
  void start();
  void stop();
  void reset();
  void toggle();

  bool running() const { return running_; }
  uint32_t elapsedMs() const;
  // Minutes, seconds, deciseconds for Decent 10-byte weight packet.
  void decentFields(uint8_t& minutes, uint8_t& seconds, uint8_t& deciseconds) const;

 private:
  bool running_ = false;
  uint32_t start_ms_ = 0;
  uint32_t accumulated_ms_ = 0;
};
