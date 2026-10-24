#include "scale.h"

#include <HX711_ADC.h>
#include <Preferences.h>

#include "config.h"
#include "pins.h"

namespace {

// Exposes the protected sample window of HX711_ADC so a tare can finish as
// soon as the moving-average window holds fresh samples. The stock tare waits
// for DATA_SET (= 16 + 2) conversions even with setSamplesInUse(4), although
// its result only averages the newest 4 + 2 — ~1.9 s at 10 SPS for nothing.
class Hx711 : public HX711_ADC {
 public:
  using HX711_ADC::HX711_ADC;
  long average() { return smoothedData(); }
  long lastSample() const { return dataSampleSet[readIndex]; }
  int windowSize() const {
    return samplesInUse + IGN_HIGH_SAMPLE + IGN_LOW_SAMPLE;
  }
};

Hx711 loadcell(PIN_HX711_DOUT, PIN_HX711_SCK);
Preferences prefs;
constexpr const char* kNvsNs = "scale";
constexpr const char* kNvsFactor = "cal_factor";
constexpr const char* kNvsEmptyZero = "empty_zero";
}  // namespace

bool Scale::begin() {
  loadcell.begin();  // powers the HX711 up; it settles while setup() goes on
  loadcell.setSamplesInUse(4);
  power_up_ms_ = millis();

  if (!loadCalFromNvs()) {
    cal_factor_ = kDefaultCalFactor;
  }
  loadcell.setCalFactor(cal_factor_);

  {
    Preferences p;
    if (p.begin(kNvsNs, true)) {
      has_empty_zero_ = p.isKey(kNvsEmptyZero);
      if (has_empty_zero_) empty_zero_ = p.getLong(kNvsEmptyZero, 0);
      p.end();
    }
  }

  phase_ = Phase::Settling;
  boot_pending_ = true;
  boot_zero_ = BootZero::Pending;
  ready_ = true;
  raw_g_ = 0;
  filtered_g_ = 0;
  Serial.printf("[scale] HX711 up, cal_factor=%.4f, empty zero %s\n",
                cal_factor_, has_empty_zero_ ? "stored" : "not stored");
  return true;
}

float Scale::countsToGrams(long counts) const {
  return cal_factor_ != 0.0f ? static_cast<float>(counts) / cal_factor_ : 0.0f;
}

float Scale::sps() const { return loadcell.getSPS(); }

void Scale::startZeroing() {
  phase_ = Phase::Zeroing;
  zero_start_ms_ = millis();
  zero_n_ = 0;
  // Optimistic zero so BLE/UI don't keep streaming pre-tare grams.
  raw_g_ = 0;
  filtered_g_ = 0;
}

void Scale::storeEmptyZero(long avg, bool force) {
  // Only rewrite NVS when the reference actually moved (flash wear).
  if (!force && has_empty_zero_ &&
      fabsf(countsToGrams(avg - empty_zero_)) < kEmptyZeroRewriteG) {
    return;
  }
  Preferences p;
  if (!p.begin(kNvsNs, false)) return;
  p.putLong(kNvsEmptyZero, avg);
  p.end();
  empty_zero_ = avg;
  has_empty_zero_ = true;
}

void Scale::finishZeroing(bool timed_out) {
  const long avg = loadcell.average();
  long offset = avg;
  const float load_g = has_empty_zero_ ? countsToGrams(avg - empty_zero_) : 0;

  if (boot_pending_) {
    boot_pending_ = false;
    boot_load_g_ = load_g;
    if (!has_empty_zero_) {
      boot_zero_ = BootZero::First;
      storeEmptyZero(avg, true);
    } else if (load_g < kBootZeroLoadG) {
      // Empty platform (anything lighter than a cup counts as drift).
      boot_zero_ = BootZero::Tared;
      storeEmptyZero(avg, false);
    } else {
      // Something heavy sat on the platform through power-on. Keep the
      // empty zero so it reads its real weight and lifting it reads 0 g.
      boot_zero_ = BootZero::KeptEmptyZero;
      offset = empty_zero_;
    }
    boot_zero_ms_ = millis();
    Serial.printf("[scale] boot zero %s at %lu ms (%.1f g vs empty zero%s, "
                  "%.1f SPS)\n",
                  boot_zero_ == BootZero::KeptEmptyZero ? "kept stored" : "tared",
                  static_cast<unsigned long>(boot_zero_ms_),
                  static_cast<double>(load_g), timed_out ? ", unsettled" : "",
                  static_cast<double>(sps()));
  } else {
    // User / app tare. A light platform means it is (nearly) empty, so the
    // new zero also refreshes the empty reference; a cup tare does not.
    if (has_empty_zero_ && load_g < kBootZeroLoadG) storeEmptyZero(avg, false);
    Serial.println(timed_out ? "[scale] tared (unsettled)" : "[scale] tared");
  }

  loadcell.setTareOffset(offset);
  phase_ = Phase::Running;
  const float g = loadcell.getData();
  raw_g_ = (offset == avg) ? 0.0f : g;
  filtered_g_ = raw_g_;
  new_sample_ = true;
}

void Scale::update() {
  if (!ready_) return;
  if (!loadcell.update()) return;  // no new conversion

  switch (phase_) {
    case Phase::Settling:
      // HX711 output settles ~400 ms after power-up (datasheet, 10 SPS).
      if (millis() - power_up_ms_ >= kHx711SettleMs) startZeroing();
      return;

    case Phase::Zeroing: {
      // Track the spread of the fresh samples; the window must be still.
      const long s = loadcell.lastSample();
      if (zero_n_ == 0) {
        zero_min_ = zero_max_ = s;
      } else {
        if (s < zero_min_) zero_min_ = s;
        if (s > zero_max_) zero_max_ = s;
      }
      ++zero_n_;
      new_sample_ = true;
      if (zero_n_ < loadcell.windowSize()) return;

      const bool timed_out = millis() - zero_start_ms_ >= kTareMaxWaitMs;
      const float spread_g = fabsf(countsToGrams(zero_max_ - zero_min_));
      if (spread_g <= kTareStableSpreadG || timed_out) {
        finishZeroing(timed_out);
      } else {
        zero_n_ = 0;  // still moving: average a fresh window
      }
      return;
    }

    case Phase::Running:
      break;
  }

  float g = loadcell.getData();
  raw_g_ = g;
  filtered_g_ = (kOledFilterAlpha * g) + ((1.0f - kOledFilterAlpha) * filtered_g_);
  new_sample_ = true;
}

void Scale::tare(bool wait) {
  if (!ready_) return;
  if (phase_ == Phase::Settling) return;  // boot zero still to come
  startZeroing();
  if (!wait) return;
  const uint32_t t0 = millis();
  while (phase_ != Phase::Running && millis() - t0 < kTareMaxWaitMs + 1000) {
    update();
    delay(1);
  }
}

void Scale::powerDown() {
  if (!ready_) return;
  loadcell.powerDown();
  Serial.println("[scale] HX711 power down");
}

void Scale::setCalFactor(float factor, bool save) {
  if (factor == 0.0f || isnan(factor) || isinf(factor)) return;
  cal_factor_ = factor;
  loadcell.setCalFactor(cal_factor_);
  if (save) saveCalToNvs();
  Serial.printf("[scale] cal_factor set to %.6f\n", cal_factor_);
}

void Scale::clearEmptyZero() {
  Preferences p;
  if (p.begin(kNvsNs, false)) {
    p.remove(kNvsEmptyZero);
    p.end();
  }
  has_empty_zero_ = false;
  Serial.println("[scale] empty zero cleared (next boot tare stores a new one)");
}

bool Scale::startCalEmpty() {
  if (!ready_) return false;
  // Tare the empty platform; with the factor applied afterwards,
  // getData() = (raw - tareOffset) / calFactor.
  tare(/*wait=*/true);
  // Calibration starts from an explicitly empty platform: that is the
  // empty-zero reference by definition.
  storeEmptyZero(loadcell.getTareOffset(), true);
  empty_raw_ = 0;
  empty_captured_ = true;
  Serial.println("[scale] cal empty: tared (place known mass, then finishCal)");
  return true;
}

bool Scale::finishCalKnownMass(float mass_g) {
  if (!ready_ || mass_g <= 0.0f) return false;

  // measured_g = raw_delta / cal_factor, and we want raw_delta / new = mass_g,
  // so new_factor = cal_factor * measured_g / mass_g.
  const int n = 20;
  float sum = 0;
  int got = 0;
  unsigned long t0 = millis();
  while (got < n && millis() - t0 < 3000) {
    if (loadcell.update()) {
      sum += loadcell.getData();
      got++;
    }
    delay(1);
  }
  if (got == 0) return false;

  float measured = sum / got;
  if (fabsf(measured) < 0.01f) {
    Serial.println("[scale] cal fail: measured ~0 (is mass on platform?)");
    return false;
  }

  float new_factor = cal_factor_ * (measured / mass_g);
  setCalFactor(new_factor, true);
  raw_g_ = mass_g;
  filtered_g_ = mass_g;
  Serial.printf("[scale] calibrated: mass=%.1fg measured=%.2fg factor=%.6f\n",
                mass_g, measured, new_factor);
  return true;
}

bool Scale::loadCalFromNvs() {
  // RW open so the namespace is created on first boot (avoids Preferences ERROR log).
  if (!prefs.begin(kNvsNs, false)) return false;
  float f = prefs.getFloat(kNvsFactor, 0);
  prefs.end();
  if (f == 0.0f || isnan(f)) return false;
  cal_factor_ = f;
  return true;
}

void Scale::saveCalToNvs() const {
  Preferences w;
  if (!w.begin(kNvsNs, false)) return;
  w.putFloat(kNvsFactor, cal_factor_);
  w.end();
  Serial.println("[scale] cal saved to NVS");
}
