// Ambient-light dimming factor shared by the clock and the LED ring: log-shaped (dark room ~0.02,
// 10 lx ~0.4, >= 500 lx full), low-pass filtered, with hysteresis so that sensor noise around a
// step boundary does not make the LEDs flicker between two levels.
#pragma once

#include <algorithm>
#include <cmath>

namespace cs {

class AmbientDimmer {
 public:
  // Feed a lux reading (polled every ~2 s); returns true if factor() changed.
  bool update(float lux) {
    float a = std::log10(1.f + std::max(0.f, lux)) / std::log10(501.f);
    float target = 0.02f + 0.98f * std::min(1.f, a);
    smooth_ = primed_ ? smooth_ + kAlpha * (target - smooth_) : target;
    if (primed_ && std::fabs(smooth_ - out_) <= kHysteresis) return false;
    primed_ = true;
    out_ = smooth_;
    return true;
  }
  float factor() const { return out_; }  // 1 until the first reading

 private:
  static constexpr float kAlpha = 0.3f;       // ~6 s time constant at one reading per 2 s
  static constexpr float kHysteresis = 0.03f;
  float smooth_ = 1.f, out_ = 1.f;
  bool primed_ = false;
};

}  // namespace cs
