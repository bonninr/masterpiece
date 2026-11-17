// A high-pass filter for one output (#1188): what a loudspeaker management
// processor puts in front of a speaker that cannot take the lowest notes, so
// the 16' and 32' stops do not drive it past its excursion.
//
// Butterworth, of the 2nd order (12 dB an octave) or the 4th (24 dB an
// octave, two sections): flat above the corner, -3 dB at it. 20 to 300 Hz.
// With bass management on the same output, the crossover already takes the
// lows to the subwoofer; this then guards the pair below it.
//
// Off by default. While it is off the processor does not call it, and the
// output is what the engine made. Built on the message thread, under the
// audio lock, as the bass management is; the audio thread only runs it.
//
// JUCE-free: the tests drive it without audio.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace mp {

struct BusHighPass {
  static constexpr float kMinHz = 20.0f;
  static constexpr float kMaxHz = 300.0f;
  float hz = 0.0f;  // 20..300, or 0 for off
  int slope = 12;   // dB an octave: 12 or 24
  bool active() const { return hz > 0.0f; }
  // A frequency brought into range, 0 kept as off; a slope of 12 or 24.
  static float clampHz(float v) { return v <= 0.0f ? 0.0f : std::clamp(v, kMinHz, kMaxHz); }
  static int clampSlope(int s) { return s >= 24 ? 24 : 12; }
};

class OutputHighPass {
public:
  static constexpr int kMaxChannels = 2;  // a pair
  static constexpr int kMaxSections = 2;

  void prepare(double sampleRate) {
    rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    design();
    reset();
  }
  void set(const BusHighPass& s) {
    hz_ = BusHighPass::clampHz(s.hz);
    sections_ = BusHighPass::clampSlope(s.slope) == 24 ? 2 : 1;
    design();
  }
  bool active() const { return hz_ > 0.0f; }
  void reset() { state_ = {}; }

  // Audio thread: `channels` filtered in place, at most a pair.
  void process(float* const* channels, int numChannels, int numFrames) {
    if (hz_ <= 0.0f) return;
    const int chans = std::min(numChannels, kMaxChannels);
    for (int ch = 0; ch < chans; ++ch) {
      float* x = channels[ch];
      if (x == nullptr) continue;
      for (int s = 0; s < sections_; ++s) {
        const Biquad& q = filter_[static_cast<size_t>(s)];
        auto& z = state_[static_cast<size_t>(ch)][static_cast<size_t>(s)];
        double z1 = z[0], z2 = z[1];
        for (int i = 0; i < numFrames; ++i) {
          const double in = x[i];
          const double out = q.b0 * in + z1;
          z1 = q.b1 * in - q.a1 * out + z2;
          z2 = q.b2 * in - q.a2 * out;
          x[i] = static_cast<float>(out);
        }
        z[0] = z1;
        z[1] = z2;
      }
    }
  }

  // The filter's level at `hz`, in dB, for drawing it and for the tests: a
  // Butterworth high-pass of order n is -10 log10(1 + (fc/f)^2n).
  static double responseDb(const BusHighPass& s, double hz) {
    if (!s.active() || hz <= 0.0) return s.active() ? -300.0 : 0.0;
    const int n = BusHighPass::clampSlope(s.slope) / 6;
    return -10.0 * std::log10(1.0 + std::pow(BusHighPass::clampHz(s.hz) / hz, 2.0 * n));
  }

private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  // The cookbook's high-pass for each section; the 4th order's two Qs are
  // Butterworth's, 1 / (2 cos(pi/8)) and 1 / (2 cos(3pi/8)).
  void design() {
    if (hz_ <= 0.0f) return;
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * std::min(static_cast<double>(hz_), rate_ * 0.45) / rate_;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const std::array<double, kMaxSections> qs =
        sections_ == 1 ? std::array<double, kMaxSections>{1.0 / std::sqrt(2.0), 0.0}
                       : std::array<double, kMaxSections>{0.54119610014619698, 1.3065629648763766};
    for (int s = 0; s < sections_; ++s) {
      const double alpha = sw / (2.0 * qs[static_cast<size_t>(s)]);
      const double a0 = 1.0 + alpha;
      Biquad& f = filter_[static_cast<size_t>(s)];
      f.b0 = (1.0 + cw) / 2.0 / a0;
      f.b1 = -(1.0 + cw) / a0;
      f.b2 = (1.0 + cw) / 2.0 / a0;
      f.a1 = -2.0 * cw / a0;
      f.a2 = (1.0 - alpha) / a0;
    }
  }

  double rate_ = 48000.0;
  float hz_ = 0.0f;
  int sections_ = 1;
  std::array<Biquad, kMaxSections> filter_{};
  std::array<std::array<std::array<double, 2>, kMaxSections>, kMaxChannels> state_{};
};

}  // namespace mp
