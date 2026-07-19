#include "scale.h"

#include <HX711_ADC.h>
#include <Preferences.h>

#include "config.h"
#include "pins.h"

namespace {
HX711_ADC loadcell(PIN_HX711_DOUT, PIN_HX711_SCK);
Preferences prefs;
constexpr const char* kNvsNs = "scale";
constexpr const char* kNvsFactor = "cal_factor";
}  // namespace

bool Scale::begin() {
  loadcell.begin();
  loadcell.setSamplesInUse(4);
  // true = tare after stabilize so empty platform boots near 0 g
  loadcell.start(1500, true);

  unsigned long t0 = millis();
  while (!loadcell.update() && millis() - t0 < 3000) {
    delay(1);
  }

  if (!loadCalFromNvs()) {
    cal_factor_ = kDefaultCalFactor;
  }
  loadcell.setCalFactor(cal_factor_);

  // Second tare after cal factor applied (clean zero with correct scale)
  loadcell.tareNoDelay();
  t0 = millis();
  while (loadcell.getTareStatus() == false && millis() - t0 < 1500) {
    loadcell.update();
    delay(1);
  }

  tare_offset_ = 0;
  ready_ = true;
  raw_g_ = 0;
  filtered_g_ = 0;
  Serial.printf("[scale] ready (auto-tare), cal_factor=%.4f\n", cal_factor_);
  return true;
}

void Scale::update() {
  if (!ready_) return;
  if (loadcell.update()) {
    float g = loadcell.getData();
    raw_g_ = g;
    filtered_g_ = (kOledFilterAlpha * g) + ((1.0f - kOledFilterAlpha) * filtered_g_);
    new_sample_ = true;
  }
}

void Scale::tare() {
  if (!ready_) return;
  loadcell.tareNoDelay();
  // Wait briefly for tare to complete so UI/BLE see zero soon.
  unsigned long t0 = millis();
  while (loadcell.getTareStatus() == false && millis() - t0 < 1500) {
    loadcell.update();
    delay(1);
  }
  raw_g_ = 0;
  filtered_g_ = 0;
  Serial.println("[scale] tared");
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

bool Scale::startCalEmpty() {
  if (!ready_) return false;
  // Average several raw readings with current factor inverted.
  // HX711_ADC: getData() returns (raw - tareOffset) / calFactor.
  // For empty, we tare first so empty is zero, then place mass.
  tare();
  empty_raw_ = 0;
  empty_captured_ = true;
  Serial.println("[scale] cal empty: tared (place known mass, then finishCal)");
  return true;
}

bool Scale::finishCalKnownMass(float mass_g) {
  if (!ready_ || mass_g <= 0.0f) return false;

  // Average filtered grams using current factor, then rescale.
  // With tare at empty, getData() ≈ (raw_delta) / calFactor.
  // We want calFactor' such that raw_delta / calFactor' = mass_g.
  // So calFactor' = calFactor * (measured / mass_g) wait — if measured is wrong:
  // measured_g = raw_delta / cal_factor
  // mass_g = raw_delta / new_factor  => new_factor = raw_delta / mass_g
  //        = cal_factor * measured_g / mass_g

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

float Scale::rawToGrams(long /*raw*/) const {
  return raw_g_;
}
