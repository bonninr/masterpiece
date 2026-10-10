// A ten-band equaliser for voicing a whole organ to a room or a pair of
// speakers. Off by default and per organ: the organ as recorded is the
// reference, and this is the player's own correction on top of it.
//
// Ten bands an octave apart, 31 Hz to 16 kHz, +-12 dB: peaks with Q 1.4 so
// that neighbours blend, and shelves at the two ends, where a peak would only
// add a bump. Then an output gain to make up for what the bands add or take.
//
// The coefficients are worked out on the message thread and handed to the
// audio thread with a try-lock: when the lock is busy for a block, that block
// keeps the previous filter. Bands at 0 dB are skipped; off, it does nothing.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string>

namespace mp {

struct VoicingEqSettings {
  static constexpr int kBands = 10;
  static constexpr std::array<double, kBands> kFrequencies{31.5, 63.0, 125.0, 250.0, 500.0,
                                                          1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
  static constexpr double kMaxDb = 12.0;

  bool on = false;
  std::array<double, kBands> bandsDb{};
  double gainDb = 0.0;

  bool flat() const {
    for (double b : bandsDb)
      if (b != 0.0) return false;
    return gainDb == 0.0;
  }
  // "<on> <gain> <band1> ... <band10>", the settings file's `eq` line.
  std::string toLine() const {
    std::ostringstream o;
    o << (on ? 1 : 0) << " " << gainDb;
    for (double b : bandsDb) o << " " << b;
    return o.str();
  }
  bool fromLine(const std::string& line) {
    std::istringstream in(line);
    int o = 0;
    VoicingEqSettings s;
    if (!(in >> o >> s.gainDb)) return false;
    s.on = o != 0;
    for (double& b : s.bandsDb)
      if (!(in >> b)) return false;
    auto clamp = [](double v) { return v < -kMaxDb ? -kMaxDb : v > kMaxDb ? kMaxDb : v; };
    s.gainDb = clamp(s.gainDb);
    for (double& b : s.bandsDb) b = clamp(b);
    *this = s;
    return true;
  }
};

class VoicingEq {
public:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  struct Coefficients {
    bool on = false;
    std::array<Biquad, VoicingEqSettings::kBands> bands{};
    std::array<bool, VoicingEqSettings::kBands> active{};
    float gain = 1.0f;
  };

  // The filter for a band: RBJ audio-EQ cookbook peak, or a shelf (S = 1) at
  // the two ends.
  static Biquad design(int band, double db, double sampleRate) {
    const double f = VoicingEqSettings::kFrequencies[static_cast<size_t>(band)];
    const double nyquistSafe = std::min(f, sampleRate * 0.45);
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * nyquistSafe / sampleRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    double b0, b1, b2, a0, a1, a2;
    if (band == 0 || band == VoicingEqSettings::kBands - 1) {
      const double alpha = sw / 2.0 * std::sqrt(2.0);  // S = 1
      const double sq = 2.0 * std::sqrt(A) * alpha;
      if (band == 0) {  // low shelf
        b0 = A * ((A + 1) - (A - 1) * cw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sq;
      } else {  // high shelf
        b0 = A * ((A + 1) + (A - 1) * cw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sq;
      }
    } else {
      const double alpha = sw / (2.0 * 1.4);
      b0 = 1 + alpha * A;
      b1 = -2 * cw;
      b2 = 1 - alpha * A;
      a0 = 1 + alpha / A;
      a1 = -2 * cw;
      a2 = 1 - alpha / A;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
  }

  static Coefficients designAll(const VoicingEqSettings& s, double sampleRate) {
    Coefficients c;
    c.on = s.on;
    c.gain = static_cast<float>(std::pow(10.0, s.gainDb / 20.0));
    for (int i = 0; i < VoicingEqSettings::kBands; ++i) {
      c.active[static_cast<size_t>(i)] = s.bandsDb[static_cast<size_t>(i)] != 0.0;
      if (c.active[static_cast<size_t>(i)])
        c.bands[static_cast<size_t>(i)] = design(i, s.bandsDb[static_cast<size_t>(i)], sampleRate);
    }
    return c;
  }

  // The whole curve's level at `hz`, in dB, for drawing it.
  static double responseDb(const VoicingEqSettings& s, double hz, double sampleRate = 48000.0) {
    const auto c = designAll(s, sampleRate);
    const double w = 2.0 * 3.14159265358979323846 * hz / sampleRate;
    double db = s.gainDb;
    for (int i = 0; i < VoicingEqSettings::kBands; ++i) {
      if (!c.active[static_cast<size_t>(i)]) continue;
      const auto& q = c.bands[static_cast<size_t>(i)];
      // |H(e^jw)|^2 from the coefficients.
      const double c1 = std::cos(w), c2 = std::cos(2 * w), s1 = std::sin(w), s2 = std::sin(2 * w);
      const double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
      const double dr = 1 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
      db += 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return db;
  }

  // Message thread.
  void configure(const VoicingEqSettings& s, double sampleRate) {
    const auto c = designAll(s, sampleRate);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = c;
    dirty_.store(true, std::memory_order_release);
  }

  // Audio thread. `channels` interleaving-free channel pointers.
  void process(float* const* channels, int numChannels, int numFrames) {
    if (dirty_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
      if (lock.owns_lock()) {
        const bool wasOn = live_.on;
        live_ = pending_;
        dirty_.store(false, std::memory_order_relaxed);
        if (live_.on && !wasOn) state_ = {};
      }
    }
    if (!live_.on) return;
    const int chans = numChannels < kMaxChannels ? numChannels : kMaxChannels;
    for (int ch = 0; ch < chans; ++ch) {
      float* x = channels[ch];
      for (int b = 0; b < VoicingEqSettings::kBands; ++b) {
        if (!live_.active[static_cast<size_t>(b)]) continue;
        const auto& q = live_.bands[static_cast<size_t>(b)];
        auto& z = state_[static_cast<size_t>(ch)][static_cast<size_t>(b)];
        for (int i = 0; i < numFrames; ++i) {
          const double in = x[i];
          const double out = q.b0 * in + z[0];
          z[0] = q.b1 * in - q.a1 * out + z[1];
          z[1] = q.b2 * in - q.a2 * out;
          x[i] = static_cast<float>(out);
        }
      }
      if (live_.gain != 1.0f)
        for (int i = 0; i < numFrames; ++i) x[i] *= live_.gain;
    }
  }

  void reset() { state_ = {}; }

private:
  static constexpr int kMaxChannels = 32;
  std::mutex mutex_;
  std::atomic<bool> dirty_{false};
  Coefficients pending_, live_;
  std::array<std::array<std::array<double, 2>, VoicingEqSettings::kBands>, kMaxChannels> state_{};
};

}  // namespace mp
