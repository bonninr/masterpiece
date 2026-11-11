// A ten-band equaliser for voicing a whole organ to a room or a pair of
// speakers. Off by default and per organ: the organ as recorded is the
// reference, and this is the player's own correction on top of it.
//
// Ten bands, +-12 dB: peaks, and shelves at the two ends, where a peak would
// only add a bump. Each band's frequency and width are the player's, from 20 Hz
// to 20 kHz and Q 0.3 to 8; as they come, the bands stand an octave apart from
// 31 Hz to 16 kHz, each Q 1.4, so that neighbours blend. On a shelf the width
// sets the slope: Q 1.4 the steepest without a bump, less a gentler one. Then
// an output gain to make up for what the bands add or take.
//
// The coefficients are worked out on the message thread and handed to the
// audio thread with a try-lock: when the lock is busy for a block, that block
// keeps the previous filter. A band's frequency and width change its
// coefficients only, so the cost per sample is the same wherever they stand.
// Bands at 0 dB are skipped; off, it does nothing.
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
  static constexpr double kMinHz = 20.0, kMaxHz = 20000.0;
  static constexpr double kDefaultQ = 1.4, kMinQ = 0.3, kMaxQ = 8.0;

  bool on = false;
  std::array<double, kBands> bandsDb{};
  double gainDb = 0.0;
  std::array<double, kBands> hz = kFrequencies;
  std::array<double, kBands> q = filled(kDefaultQ);

  bool flat() const {
    for (double b : bandsDb)
      if (b != 0.0) return false;
    return gainDb == 0.0;
  }
  // Every band at its own frequency and width as they come.
  bool shapeAsDefault() const { return sameShape(VoicingEqSettings{}); }
  // The same frequencies and widths, to a tenth of a hertz and a hundredth of Q.
  bool sameShape(const VoicingEqSettings& o) const {
    for (size_t i = 0; i < static_cast<size_t>(kBands); ++i)
      if (std::fabs(hz[i] - o.hz[i]) > 0.1 || std::fabs(q[i] - o.q[i]) > 0.005) return false;
    return true;
  }
  void resetShape() {
    hz = kFrequencies;
    q = filled(kDefaultQ);
  }

  // "<on> <gain> <band1> ... <band10>", the settings file's `eq` line, then
  // "<hz1> ... <hz10> <q1> ... <q10>" when a band has moved. A line without
  // them reads as the bands as they come.
  std::string toLine() const {
    std::ostringstream o;
    o << (on ? 1 : 0) << " " << gainDb;
    for (double b : bandsDb) o << " " << b;
    if (!shapeAsDefault()) o << " " << shapeLine();
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
    s.readShape(in);
    *this = s;
    return true;
  }

  // "<hz1> ... <hz10> <q1> ... <q10>".
  std::string shapeLine() const {
    std::ostringstream o;
    for (size_t i = 0; i < hz.size(); ++i) o << (i ? " " : "") << hz[i];
    for (double v : q) o << " " << v;
    return o.str();
  }
  // Reads the frequencies and widths; when they are not all there, the bands
  // keep their shape as they come. Values out of range are brought in.
  bool readShape(std::istream& in) {
    std::array<double, kBands> h{}, w{};
    for (double& v : h)
      if (!(in >> v)) return false;
    for (double& v : w)
      if (!(in >> v)) return false;
    for (size_t i = 0; i < h.size(); ++i) {
      hz[i] = std::clamp(h[i], kMinHz, kMaxHz);
      q[i] = std::clamp(w[i], kMinQ, kMaxQ);
    }
    return true;
  }

private:
  static constexpr std::array<double, kBands> filled(double v) {
    std::array<double, kBands> a{};
    for (auto& x : a) x = v;
    return a;
  }
};

class VoicingEq {
public:
  static constexpr int kMaxChannels = 48;  // output channels, each its own filter memory
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  struct Coefficients {
    bool on = false;
    std::array<Biquad, VoicingEqSettings::kBands> bands{};
    std::array<bool, VoicingEqSettings::kBands> active{};
    float gain = 1.0f;
  };

  // The filter for a band: RBJ audio-EQ cookbook peak at `hz` with `q`, or a
  // shelf at the two ends, its slope S = q / 1.4 up to 1.
  static Biquad design(int band, double db, double sampleRate, double hz, double q) {
    const double nyquistSafe = std::min(hz, sampleRate * 0.45);
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * nyquistSafe / sampleRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    double b0, b1, b2, a0, a1, a2;
    if (band == 0 || band == VoicingEqSettings::kBands - 1) {
      const double slope = std::min(1.0, q / VoicingEqSettings::kDefaultQ);
      // S = 1 gives sqrt(2), the steepest shelf without a bump.
      const double alpha =
          slope >= 1.0 ? sw / 2.0 * std::sqrt(2.0) : sw / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
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
      const double alpha = sw / (2.0 * q);
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
        c.bands[static_cast<size_t>(i)] = design(i, s.bandsDb[static_cast<size_t>(i)], sampleRate,
                                                 s.hz[static_cast<size_t>(i)], s.q[static_cast<size_t>(i)]);
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
  std::mutex mutex_;
  std::atomic<bool> dirty_{false};
  Coefficients pending_, live_;
  std::array<std::array<std::array<double, 2>, VoicingEqSettings::kBands>, kMaxChannels> state_{};
};

}  // namespace mp
