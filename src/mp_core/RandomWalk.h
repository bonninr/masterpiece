// A slow random drift, as the format states it for a tremulant's rate and
// depth and for each pipe's share of the tremulant (#806).
//
// Four numbers describe each one: a positive accelerating coefficient, a
// positive damping coefficient, the index of a probability function, and the
// largest fluctuation in percent. No public source documents how they
// combine. They are read here as a mean-reverting walk (an Ornstein-Uhlenbeck
// process): the damping pulls the drift back to zero at that rate per second,
// and the accelerating coefficient is the strength of the random push. Its
// spread settles at accel / sqrt(2 x damping), which lands on the stated
// maximum for the sets that ship these fields: 44 and 4.6 settle at 14.5 %
// against a 15 % maximum, 2.6293 and 0.8047 at 2.07 % against 2 %. The
// damping figures are themselves rates (4.6052 = ln 100, 0.8047 = ln sqrt 5).
// The drift is held inside the maximum. The probability function's index
// seeds the generator, so two drifts with different indices move apart.
//
// JUCE-free: the tremulant LFO and the voice engine both step one.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mp {

struct RandomWalk {
  bool on = false;
  double accel = 0.0;   // PositiveAcceleratingCoeff
  double damping = 0.0; // PositiveDampingCoeff, per second
  int fn = 0;           // IndexGeneratingProbabilityFn
  double maxPct = 0.0;  // the largest fluctuation, in percent
  bool active() const { return on && accel > 0.0 && maxPct > 0.0; }
};

class RandomWalkState {
public:
  // Start a drift from its settled spread, so a note struck now does not
  // begin every time at the centre. A walk that is not active stays at 1.
  void start(const RandomWalk& w, uint64_t seed) {
    w_ = w;
    on_ = w.active();
    x_ = 0.0;
    lastDt_ = -1.0;
    s_ = mix(seed ^ (static_cast<uint64_t>(static_cast<uint32_t>(w.fn)) << 32));
    if (!on_) return;
    if (w_.damping > 0.0)
      x_ = std::clamp(w_.accel / std::sqrt(2.0 * w_.damping) * normal(), -w_.maxPct, w_.maxPct);
  }
  void stop() { on_ = false; x_ = 0.0; }
  bool on() const { return on_; }

  // Move the drift on by dt seconds; returns the new multiplier.
  double step(double dt) {
    if (!on_ || dt <= 0.0) return multiplier();
    if (dt != lastDt_) {
      lastDt_ = dt;
      if (w_.damping > 0.0) {
        keep_ = std::exp(-w_.damping * dt);
        kick_ = w_.accel / std::sqrt(2.0 * w_.damping) * std::sqrt(std::max(0.0, 1.0 - keep_ * keep_));
      } else {
        keep_ = 1.0;
        kick_ = w_.accel * std::sqrt(dt);
      }
    }
    x_ = std::clamp(x_ * keep_ + kick_ * normal(), -w_.maxPct, w_.maxPct);
    return multiplier();
  }
  // 1 + the drift as a fraction: what the rate or depth is multiplied by.
  double multiplier() const { return on_ ? 1.0 + x_ / 100.0 : 1.0; }
  double percent() const { return x_; }

private:
  static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return z != 0 ? z : 0x2545F4914F6CDD1Dull;
  }
  double uniform() {
    s_ ^= s_ >> 12;
    s_ ^= s_ << 25;
    s_ ^= s_ >> 27;
    return static_cast<double>((s_ * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
  }
  // Near enough to a unit normal for a drift: four uniforms, centred and
  // scaled to unit variance.
  double normal() {
    return (uniform() + uniform() + uniform() + uniform() - 2.0) * 1.7320508075688772;
  }

  RandomWalk w_;
  bool on_ = false;
  double x_ = 0.0;
  double lastDt_ = -1.0, keep_ = 1.0, kick_ = 0.0;
  uint64_t s_ = 1;
};

} // namespace mp
