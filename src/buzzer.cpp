#include "buzzer.h"

#include "pins.h"

namespace {
constexpr int kLedcChannel = 0;
constexpr int kLedcResolution = 8;
// Soft square-ish volume (passive buzzer); lower = gentler
constexpr int kDuty = 64;

// Pleasant mid-range notes (Hz)
constexpr uint16_t NOTE_G4 = 392;
constexpr uint16_t NOTE_A4 = 440;
constexpr uint16_t NOTE_B4 = 494;
constexpr uint16_t NOTE_C5 = 523;
constexpr uint16_t NOTE_D5 = 587;
constexpr uint16_t NOTE_E5 = 659;
constexpr uint16_t NOTE_G5 = 784;
constexpr uint16_t NOTE_A5 = 880;
}  // namespace

void Buzzer::begin() {
  ledcSetup(kLedcChannel, NOTE_A4, kLedcResolution);
  ledcAttachPin(PIN_BUZZER, kLedcChannel);
  ledcWrite(kLedcChannel, 0);
  Serial.println("[buzzer] ready");
}

void Buzzer::clearQueue() {
  queue_len_ = 0;
  queue_idx_ = 0;
  in_gap_ = false;
  stopTone();
}

void Buzzer::enqueue(uint16_t hz, uint16_t ms, uint16_t gap_ms) {
  if (queue_len_ >= kMaxNotes) return;
  queue_[queue_len_++] = {hz, ms, gap_ms};
}

void Buzzer::stopTone() {
  ledcWrite(kLedcChannel, 0);
  active_ = false;
}

void Buzzer::startCurrent() {
  if (queue_idx_ >= queue_len_) {
    clearQueue();
    return;
  }
  const Note& n = queue_[queue_idx_];
  if (n.hz == 0) {
    // Rest
    stopTone();
    active_ = false;
    in_gap_ = true;
    phase_end_ms_ = millis() + n.ms;
    return;
  }
  ledcSetup(kLedcChannel, n.hz, kLedcResolution);
  ledcWrite(kLedcChannel, kDuty);
  active_ = true;
  in_gap_ = false;
  phase_end_ms_ = millis() + n.ms;
}

void Buzzer::playTone(uint16_t hz, uint16_t ms) {
  clearQueue();
  enqueue(hz, ms, 0);
  startCurrent();
}

void Buzzer::beep() { tareChime(); }

void Buzzer::doubleBeep() { successChime(); }

void Buzzer::longBeep() { errorChime(); }

void Buzzer::bootChime() {
  clearQueue();
  enqueue(NOTE_C5, 70, 25);
  enqueue(NOTE_E5, 70, 25);
  enqueue(NOTE_G5, 110, 0);
  startCurrent();
}

void Buzzer::tareChime() {
  clearQueue();
  enqueue(NOTE_E5, 45, 20);
  enqueue(NOTE_G5, 70, 0);
  startCurrent();
}

void Buzzer::timerStartChime() {
  clearQueue();
  enqueue(NOTE_A4, 50, 20);
  enqueue(NOTE_C5, 55, 20);
  enqueue(NOTE_E5, 90, 0);
  startCurrent();
}

void Buzzer::timerStopChime() {
  clearQueue();
  enqueue(NOTE_E5, 55, 20);
  enqueue(NOTE_C5, 70, 0);
  startCurrent();
}

void Buzzer::timerResetChime() {
  clearQueue();
  enqueue(NOTE_G5, 40, 15);
  enqueue(NOTE_E5, 40, 15);
  enqueue(NOTE_C5, 40, 15);
  enqueue(NOTE_G4, 90, 0);
  startCurrent();
}

void Buzzer::successChime() {
  clearQueue();
  enqueue(NOTE_C5, 55, 20);
  enqueue(NOTE_E5, 55, 20);
  enqueue(NOTE_G5, 55, 20);
  enqueue(1047, 100, 0);  // C6
  startCurrent();
}

void Buzzer::errorChime() {
  clearQueue();
  enqueue(NOTE_A4, 90, 40);
  enqueue(NOTE_G4, 140, 0);
  startCurrent();
}

void Buzzer::sleepChime() {
  clearQueue();
  enqueue(NOTE_E5, 60, 25);
  enqueue(NOTE_C5, 60, 25);
  enqueue(NOTE_A4, 100, 0);
  startCurrent();
}

void Buzzer::wakeChime() {
  clearQueue();
  enqueue(NOTE_A4, 50, 20);
  enqueue(NOTE_C5, 50, 20);
  enqueue(NOTE_E5, 80, 0);
  startCurrent();
}

void Buzzer::update() {
  if (queue_len_ == 0) return;
  const uint32_t now = millis();
  if (now < phase_end_ms_) return;

  if (active_ && !in_gap_) {
    // Finished a tone → optional gap, then next note
    stopTone();
    const Note& n = queue_[queue_idx_];
    if (n.gap_ms > 0) {
      in_gap_ = true;
      phase_end_ms_ = now + n.gap_ms;
      return;
    }
    queue_idx_++;
    startCurrent();
    return;
  }

  if (in_gap_) {
    in_gap_ = false;
    queue_idx_++;
    startCurrent();
  }
}
