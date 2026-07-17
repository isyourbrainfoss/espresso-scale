#include "buzzer.h"

#include "pins.h"

namespace {
constexpr int kLedcChannel = 0;
constexpr int kLedcResolution = 8;
}  // namespace

void Buzzer::begin() {
  // Arduino-ESP32 2.x LEDC API (platformio espressif32 7.x)
  ledcSetup(kLedcChannel, 2000, kLedcResolution);
  ledcAttachPin(PIN_BUZZER, kLedcChannel);
  ledcWrite(kLedcChannel, 0);
  Serial.println("[buzzer] ready");
}

void Buzzer::startTone(uint16_t hz, uint16_t ms) {
  ledcSetup(kLedcChannel, hz, kLedcResolution);
  ledcWrite(kLedcChannel, 128);
  active_ = true;
  end_ms_ = millis() + ms;
}

void Buzzer::stopTone() {
  ledcWrite(kLedcChannel, 0);
  active_ = false;
}

void Buzzer::beep() {
  remaining_ = 0;
  gap_until_ = 0;
  startTone(2400, 40);
}

void Buzzer::doubleBeep() {
  remaining_ = 1;
  next_hz_ = 2400;
  next_ms_ = 40;
  gap_until_ = 0;
  startTone(2400, 40);
}

void Buzzer::longBeep() {
  remaining_ = 0;
  gap_until_ = 0;
  startTone(1200, 180);
}

void Buzzer::playTone(uint16_t hz, uint16_t ms) {
  remaining_ = 0;
  gap_until_ = 0;
  startTone(hz, ms);
}

void Buzzer::update() {
  const uint32_t now = millis();
  if (active_ && now >= end_ms_) {
    stopTone();
    if (remaining_ > 0) {
      remaining_--;
      gap_until_ = now + 60;
    }
  }
  if (!active_ && remaining_ >= 0 && gap_until_ != 0 && now >= gap_until_) {
    gap_until_ = 0;
    if (next_ms_ > 0) {
      startTone(next_hz_, next_ms_);
      remaining_ = -1;
    }
  }
}
