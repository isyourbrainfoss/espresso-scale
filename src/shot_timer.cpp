#include "shot_timer.h"

void ShotTimer::start() {
  if (running_) return;
  start_ms_ = millis();
  running_ = true;
}

void ShotTimer::stop() {
  if (!running_) return;
  accumulated_ms_ += millis() - start_ms_;
  running_ = false;
}

void ShotTimer::reset() {
  running_ = false;
  start_ms_ = 0;
  accumulated_ms_ = 0;
}

void ShotTimer::toggle() {
  if (running_)
    stop();
  else
    start();
}

uint32_t ShotTimer::elapsedMs() const {
  if (running_) return accumulated_ms_ + (millis() - start_ms_);
  return accumulated_ms_;
}

void ShotTimer::decentFields(uint8_t& minutes, uint8_t& seconds,
                             uint8_t& deciseconds) const {
  const uint32_t ms = elapsedMs();
  const uint32_t total_ds = ms / 100;
  deciseconds = static_cast<uint8_t>(total_ds % 10);
  const uint32_t total_s = total_ds / 10;
  seconds = static_cast<uint8_t>(total_s % 60);
  minutes = static_cast<uint8_t>((total_s / 60) > 255 ? 255 : (total_s / 60));
}
