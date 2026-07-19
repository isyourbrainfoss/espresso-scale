#include "buttons.h"

#include "config.h"
#include "pins.h"

void Buttons::begin() {
  pinMode(PIN_BTN_TARE, INPUT);
  pinMode(PIN_BTN_TIMER, INPUT);
  Serial.println("[buttons] ready");
}

bool Buttons::readTare() const { return digitalRead(PIN_BTN_TARE) == HIGH; }
bool Buttons::readTimer() const { return digitalRead(PIN_BTN_TIMER) == HIGH; }

BtnEvent Buttons::poll() {
  const uint32_t now = millis();
  const bool tare = readTare();
  const bool timer = readTimer();

  // Both held → calibration entry
  if (tare && timer) {
    if (both_down_ms_ == 0) both_down_ms_ = now;
    if (!both_fired_ && (now - both_down_ms_ >= kBtnBothHoldCalMs)) {
      both_fired_ = true;
      tare_long_consumed_ = true;
      timer_long_consumed_ = true;
      return BtnEvent::BothHeldCal;
    }
  } else {
    both_down_ms_ = 0;
    if (!tare && !timer) both_fired_ = false;
  }

  BtnEvent ev = BtnEvent::None;

  // Tare: short on release, long while held (standby)
  if (tare && !tare_down_) {
    tare_down_ = true;
    tare_down_ms_ = now;
    tare_long_consumed_ = false;
  } else if (tare && tare_down_ && !tare_long_consumed_ && !both_fired_) {
    if (now - tare_down_ms_ >= kBtnLongPressMs) {
      tare_long_consumed_ = true;
      return BtnEvent::TareLong;
    }
  } else if (!tare && tare_down_) {
    const uint32_t held = now - tare_down_ms_;
    tare_down_ = false;
    if (!tare_long_consumed_ && !both_fired_ && held >= kBtnDebounceMs &&
        held < kBtnLongPressMs) {
      ev = BtnEvent::TareShort;
    }
  }

  // Timer edge handling
  if (timer && !timer_down_) {
    timer_down_ = true;
    timer_down_ms_ = now;
    timer_long_consumed_ = false;
  } else if (timer && timer_down_ && !timer_long_consumed_ && !both_fired_) {
    if (now - timer_down_ms_ >= kBtnLongPressMs) {
      timer_long_consumed_ = true;
      return BtnEvent::TimerLong;
    }
  } else if (!timer && timer_down_) {
    const uint32_t held = now - timer_down_ms_;
    timer_down_ = false;
    if (!timer_long_consumed_ && !both_fired_ && held >= kBtnDebounceMs &&
        held < kBtnLongPressMs) {
      if (ev == BtnEvent::None) ev = BtnEvent::TimerShort;
    }
  }

  return ev;
}
