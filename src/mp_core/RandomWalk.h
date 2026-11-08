// A slow random drift, as the format states it for a tremulant's rate and
// depth and for each pipe's share of the tremulant (#806).
//
// Four numbers describe each one: a positive accelerating coefficient, a
// positive damping coefficient, the index of a probability function, and the
// largest fluctuation in percent. No public source documents how they
// combine. They are read here by their names, as a body pushed about at
// random inside a box:
//
// - The accelerating coefficient is the random push on the drift's speed.
// - The damping coefficient slows that speed, as a rate per second. The sets'
//   figures are rates (4.6052 = ln 100, 0.8047 = ln sqrt 5, 23.0259 = 10 ln 10).
//   The speed settles at a spread of accel / sqrt(2 x damping), in percent
//   per second.
// - The largest fluctuation is the box: the drift turns back at its walls.
//
// Every figure set the format's sets state then drifts smoothly over the
// whole of its range, spread evenly across it (0.58 x the maximum): about a
// second to move across a tremulant's range (44, 4.6, 15 %), a tenth to a
// third of a second for the wind's flow (200, 7, 5 %). Read as a pull back to
// the centre, the same figures would sit at the wall most of the time and
// jump across the range from one block to the next.
//
// The probability function's index seeds the generator, so two drifts with
// different indices move apart. Some sets give each pipe its own index.
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
  // A speed with no damping would grow without end, so a drift needs all three.
  bool active() const { return on && accel > 0.0 && damping > 0.0 && maxPct > 0.0; }
  // The settled spread of the drift's speed, in percent per second.
  double speedSpread() const { return damping > 0.0 ? accel / std::sqrt(2.0 * damping) : 0.0; }
};

class RandomWalkState {
public:
  // Start a drift where it would have settled: anywhere in its range, moving
  // at a settled speed, so a note struck now does not begin every time at the
  // centre. A walk that is not active stays at 1.
  void start(const RandomWalk& w, uint64_t seed) {
    w_ = w;
    on_ = w.active();
    x_ = 0.0;
    v_ = 0.0;
    lastDt_ = -1.0;
    s_ = mix(seed ^ (static_cast<uint64_t>(static_cast<uint32_t>(w.fn)) << 32));
    if (!on_) return;
    x_ = (2.0 * uniform() - 1.0) * w_.maxPct;
    v_ = w_.speedSpread() * normal();
  }
  void stop() { on_ = false; x_ = 0.0; v_ = 0.0; }
  bool on() const { return on_; }

  // Move the drift on by dt seconds; returns the new multiplier.
  double step(double dt) {
    if (!on_ || dt <= 0.0) return multiplier();
    if (dt != lastDt_) {
      lastDt_ = dt;
      keep_ = std::exp(-w_.damping * dt);
      kick_ = w_.speedSpread() * std::sqrt(std::max(0.0, 1.0 - keep_ * keep_));
    }
    v_ = v_ * keep_ + kick_ * normal();
    x_ += v_ * dt;
    // The walls turn it back. A step longer than the range bounces more than
    // once; the clamp is only for rounding.
    for (int i = 0; i < 4 && (x_ > w_.maxPct || x_ < -w_.maxPct); ++i) {
      x_ = (x_ > 0.0 ? 2.0 * w_.maxPct : -2.0 * w_.maxPct) - x_;
      v_ = -v_;
    }
    x_ = std::clamp(x_, -w_.maxPct, w_.maxPct);
    return multiplier();
  }
  // The drift's speed now, in percent per second.
  double speed() const { return v_; }
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
  double x_ = 0.0, v_ = 0.0;
  double lastDt_ = -1.0, keep_ = 1.0, kick_ = 0.0;
  uint64_t s_ = 1;
};

// A pipe's fixed tuning error (#808), as a fraction of its largest allowed
// error, from -1 to 1. The format states the probability that a pipe errs by
// less than the largest error; the draw is a normal one whose spread puts that
// share of pipes inside it, held inside it. The probability index and the pipe
// seed it, so every load tunes the organ the same way.
inline double randomTuningUnit(uint64_t pipeId, int fn, double probPct) {
  const double p = std::clamp(probPct, 1.0, 99.9) / 100.0;
  // The half-width, in spreads, that holds a share p of a normal draw.
  double lo = 0.0, hi = 10.0;
  for (int i = 0; i < 60; ++i) {
    const double mid = 0.5 * (lo + hi);
    (std::erf(mid / std::sqrt(2.0)) < p ? lo : hi) = mid;
  }
  uint64_t z = pipeId * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(static_cast<uint32_t>(fn)) << 40);
  const auto next = [&z]() {
    z += 0x9E3779B97F4A7C15ull;
    uint64_t x = z;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    x ^= x >> 31;
    return (static_cast<double>(x >> 11) + 0.5) * (1.0 / 9007199254740992.0);
  };
  const double u1 = next(), u2 = next();
  const double normal = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  return std::clamp(normal / (0.5 * (lo + hi)), -1.0, 1.0);
}

// That error in cents for a pipe sounding at targetHz: the unit times the
// smaller of the two stated limits, the hertz one taken at the pipe's own
// pitch. A limit of 0 states none.
inline double tuningErrorCents(double unit, double maxCents, double maxHz, double targetHz) {
  double limit = maxCents > 0.0 ? maxCents : -1.0;
  if (maxHz > 0.0 && targetHz > 0.0) {
    const double hzCents = 1200.0 * std::log2(1.0 + maxHz / targetHz);
    limit = limit < 0.0 ? hzCents : std::min(limit, hzCents);
  }
  return limit > 0.0 ? unit * limit : 0.0;
}

} // namespace mp
