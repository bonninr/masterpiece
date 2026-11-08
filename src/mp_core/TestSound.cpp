#include "TestSound.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace mp {
namespace {

namespace fs = std::filesystem;

constexpr double kPi = 3.14159265358979323846;
constexpr double kLoop = 5.0;     // seconds; the sustain repeats exactly over this
constexpr double kSettle = 0.4;   // every onset has settled by here: the loop starts here
constexpr double kRelease = 0.7;  // the release, after the cue point
constexpr int kCtrl = 200;        // control rate for the slow modulations, Hz
constexpr const char* kStem = kTestSoundStem;

struct Rank {
  const char* name;
  double ratio, level, roll;
  bool odd, reed;
};

const Rank kRanks[] = {
    {"Principal 32'", 0.25, 0.45, 0.60, false, false},
    {"Bourdon 16'", 0.5, 0.55, 0.50, true, false},
    {"Principal 16'", 0.5, 0.50, 0.66, false, false},
    {"Bombarde 16'", 0.5, 0.22, 0.86, false, true},
    {"Principal 8'", 1.0, 0.70, 0.70, false, false},
    {"Second Principal 8'", 1.0, 0.45, 0.68, false, false},
    {"Gedackt 8'", 1.0, 0.25, 0.45, true, false},
    {"Octave 4'", 2.0, 0.55, 0.66, false, false},
    {"Quint 2 2/3'", 3.0, 0.30, 0.60, false, false},
    {"Superoctave 2'", 4.0, 0.42, 0.62, false, false},
    {"Mixture 1 1/3'", 6.0, 0.30, 0.58, false, false},
    {"Mixture 1'", 8.0, 0.28, 0.58, false, false},
    {"Mixture 2/3'", 12.0, 0.24, 0.55, false, false},
    {"Mixture 1/2'", 16.0, 0.20, 0.55, false, false},
    {"Trumpet 8'", 1.0, 0.12, 0.85, false, true},
};

using Rng = std::mt19937_64;

double normal(Rng& r, double mean = 0.0, double sd = 1.0) {
  return std::normal_distribution<double>(mean, sd)(r);
}
double uniform(Rng& r, double lo, double hi) {
  return std::uniform_real_distribution<double>(lo, hi)(r);
}

// Band-limited noise on the control grid that repeats every kLoop: k cycles in
// kLoop for k = 1..kmax, amplitudes falling as k^-shape, scaled to `rms`.
// With `integral`, its time integral instead (seconds), which turns a relative
// frequency deviation into a phase deviation. Each component turns by a fixed
// angle per point, so it is rotated there.
std::vector<double> loopNoise(Rng& r, int points, int kmax, double rms, bool integral = false,
                              double shape = 0.5) {
  std::vector<double> amp(static_cast<size_t>(kmax)), ph(static_cast<size_t>(kmax));
  double sum2 = 0.0;
  for (int k = 1; k <= kmax; ++k) {
    amp[static_cast<size_t>(k - 1)] = normal(r) * std::pow(k, -shape);
    ph[static_cast<size_t>(k - 1)] = uniform(r, 0.0, 2 * kPi);
    sum2 += amp[static_cast<size_t>(k - 1)] * amp[static_cast<size_t>(k - 1)];
  }
  const double scale = rms / std::sqrt(0.5 * sum2);
  std::vector<double> y(static_cast<size_t>(points), 0.0);
  for (int k = 1; k <= kmax; ++k) {
    const double w = 2 * kPi * k / kLoop;
    const double a = amp[static_cast<size_t>(k - 1)] * scale / (integral ? w : 1.0);
    double c = std::cos(ph[static_cast<size_t>(k - 1)]), s = std::sin(ph[static_cast<size_t>(k - 1)]);
    const double dc = std::cos(w / kCtrl), ds = std::sin(w / kCtrl);
    for (int i = 0; i < points; ++i) {
      y[static_cast<size_t>(i)] += integral ? a * s : a * c;
      const double c2 = c * dc - s * ds;
      s = s * dc + c * ds;
      c = c2;
    }
  }
  return y;
}

inline double atCtrl(const std::vector<double>& c, double t) {
  const double x = t * kCtrl;
  const int i = static_cast<int>(x);
  const double f = x - i;
  return c[static_cast<size_t>(i)] + (c[static_cast<size_t>(i) + 1] - c[static_cast<size_t>(i)]) * f;
}

// A second-order section, RBJ's cookbook.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  static Biquad make(bool high, double f, double rate) {
    const double w = 2 * kPi * f / rate, c = std::cos(w), s = std::sin(w);
    const double alpha = s / (2 * std::sqrt(0.5));
    Biquad q;
    const double a0 = 1 + alpha;
    if (high) {
      q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = (1 + c) / 2 / a0;
    } else {
      q.b0 = (1 - c) / 2 / a0; q.b1 = (1 - c) / a0; q.b2 = (1 - c) / 2 / a0;
    }
    q.a1 = -2 * c / a0;
    q.a2 = (1 - alpha) / a0;
    return q;
  }
  // A band an octave wide around f, its peak at 0 dB.
  static Biquad band(double f, double rate) {
    const double w = 2 * kPi * f / rate, c = std::cos(w), s = std::sin(w);
    const double alpha = s * std::sinh(std::log(2.0) / 2 * 1.0 * w / s);
    Biquad q;
    const double a0 = 1 + alpha;
    q.b0 = alpha / a0; q.b1 = 0; q.b2 = -alpha / a0;
    q.a1 = -2 * c / a0;
    q.a2 = (1 - alpha) / a0;
    return q;
  }
  double run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

struct Note {
  std::vector<float> left, right;
  int loopStart = 0, loopFrames = 0;
};

// One key: fifteen ranks of harmonics, the chiffs and the wind.
Note makeNote(int key, int rate) {
  Rng rng(static_cast<uint64_t>(1000 + key));
  const int nl = static_cast<int>(std::lround(kLoop * rate));
  const int s0 = static_cast<int>(std::lround(kSettle * rate));
  const int n = s0 + nl + static_cast<int>(kRelease * rate);
  const int points = static_cast<int>(static_cast<double>(n) / rate * kCtrl) + 3;
  const double off = static_cast<double>(s0 + nl) / rate;  // key let go: the cue point
  const int offIndex = s0 + nl;

  const double error = normal(rng, 0.0, 2.0);
  const double f8 = 440.0 * std::pow(2.0, (key - 69) / 12.0) * std::pow(2.0, error / 1200.0);
  const double nyq = std::min(0.45 * rate, 16000.0);
  const int side = key % 2 == 0 ? -1 : 1;  // C side left

  std::vector<double> outL(static_cast<size_t>(n), 0.0), outR(static_cast<size_t>(n), 0.0),
      tone(static_cast<size_t>(n));
  const auto windPhase = loopNoise(rng, points, 8, 0.0002, true);
  auto windLevel = loopNoise(rng, points, 10, 0.008);
  for (auto& v : windLevel) v += 1.0;

  // Per harmonic, what the sample loop reads.
  std::vector<std::vector<double>> drifts;
  std::vector<double> gains, invRise, relStep, cosPh, sinPh, rel, gc, gs, level, levelStep, pr, pi, qr, qi;

  double totalAmp = 0.0;
  int rankIndex = 0;
  for (const Rank& rk : kRanks) {
    Rng voice(static_cast<uint64_t>(77 + rankIndex++));  // the rank's own voice, the same on every key
    double f = f8 * rk.ratio;
    while (f > 4000 && rk.ratio >= 6) f /= 2;  // mixture ranks break back
    f *= std::pow(2.0, normal(rng, 0.0, 1.5) / 1200.0);
    f = std::max(1.0, std::round(f * kLoop)) / kLoop;  // whole cycles in kLoop
    const bool bass = rk.ratio <= 0.5;

    // Every harmonic under the top, or the odd ones: a run from the first,
    // one or two apart.
    const int stepH = rk.odd ? 2 : 1;
    std::vector<int> hs;
    for (int h = 1; h <= 40; h += stepH)
      if (h * f < nyq) hs.push_back(h);
    double voiceDb[40];
    for (double& d : voiceDb) d = normal(voice, 0.0, 2.5);
    if (hs.empty()) continue;
    const double aboveC4 = std::max(0.0, std::log2(f / 262.0));
    const double rEff = std::pow(rk.roll, 1 + (rk.reed ? 0.25 : 0.5) * aboveC4);
    const double fc = rk.reed ? 9000.0 : 6500.0;
    const size_t H = hs.size();
    std::vector<double> g(H);
    double g2 = 0.0;
    for (size_t j = 0; j < H; ++j) {
      const int h = hs[j];
      const double hz = h * f;
      g[j] = std::pow(rEff, h - 1) / std::sqrt(1 + std::pow(hz / fc, 4)) *
             std::pow(10.0, (voiceDb[h - 1] + normal(rng, 0.0, 1.0)) / 20.0);
      g2 += g[j] * g[j];
    }
    for (double& v : g) v *= rk.level / std::sqrt(g2);
    totalAmp += rk.level;

    // Pitch: the rank's own drift and the wind, as phase.
    auto dev = loopNoise(rng, points, 25, 0.0003, true);
    for (int i = 0; i < points; ++i) dev[static_cast<size_t>(i)] += windPhase[static_cast<size_t>(i)];

    const double onset = uniform(rng, 0.0, 0.03);
    const double rise = bass ? 0.02 : uniform(rng, 0.03, 0.09);
    const double tau =
        rk.reed ? 0.02 : std::clamp(0.025 + 0.05 * std::sqrt(262.0 / f), 0.02, 0.15);
    const int onsetIndex = static_cast<int>(onset * rate);
    const double punchFloor = std::exp(-0.3 / 0.08);

    drifts.assign(H, {});
    gains.assign(H, 0.0);
    invRise.assign(H, 0.0);
    relStep.assign(H, 0.0);
    cosPh.assign(H, 0.0);
    sinPh.assign(H, 0.0);
    rel.assign(H, 1.0);
    gc.assign(H, 0.0);
    gs.assign(H, 0.0);
    level.assign(H, 0.0);
    levelStep.assign(H, 0.0);
    pr.assign(H, 0.0);
    pi.assign(H, 0.0);
    qr.assign(H, 0.0);
    qi.assign(H, 0.0);
    for (size_t j = 0; j < H; ++j) {
      const int h = hs[j];
      // Level: slow drift, a few per cent, times the wind.
      auto drift = loopNoise(rng, points, 15, h == 1 ? 0.015 : 0.03);
      for (int i = 0; i < points; ++i)
        drift[static_cast<size_t>(i)] = (1.0 + drift[static_cast<size_t>(i)]) * windLevel[static_cast<size_t>(i)];
      double riseH = rise;
      if (rk.reed) riseH *= std::max(0.5, 1 - 0.04 * (h - 1));  // the reed blooms bright
      else if (h == 1) riseH *= 1.5;                              // the octave speaks first
      const double tauH = tau / (1 + (rk.reed ? 0.0 : 0.05) * (h - 1));
      const double ph0 = uniform(rng, 0.0, 2 * kPi);
      drifts[j] = std::move(drift);
      gains[j] = g[j];
      invRise[j] = 1.0 / riseH;
      relStep[j] = std::exp(-1.0 / (tauH * rate));
      cosPh[j] = std::cos(ph0);
      sinPh[j] = std::sin(ph0);
    }

    // The phase and every harmonic's level are linear inside each control
    // interval, so there the phase turns by a fixed angle per sample and the
    // levels move by fixed steps; each harmonic is the fundamental's turn
    // raised to its number. Between the last harmonic's rise and the cue
    // point the envelope is flat.
    std::fill(tone.begin(), tone.end(), 0.0);
    for (size_t j = 0; j < H; ++j) {
      gc[j] = gains[j] * cosPh[j];
      gs[j] = gains[j] * sinPh[j];
    }
    double latestRise = 0.0;
    for (const double ir : invRise) latestRise = std::max(latestRise, 1.0 / ir);
    const double steadyFrom = onset + std::max(latestRise, bass ? 0.3 : 0.0);
    int seg = -1;
    for (int i = onsetIndex; i < n; ++i) {
      const double t = static_cast<double>(i) / rate;
      const double x = t * kCtrl;
      const int ci = static_cast<int>(x);
      if (ci != seg) {
        // Exact at the start of each interval: the fundamental's phase and
        // its turn per sample, raised to each harmonic.
        seg = ci;
        const double phi = 2 * kPi * f * (t + atCtrl(dev, t));
        const double dphi =
            2 * kPi * f / rate *
            (1.0 + (dev[static_cast<size_t>(ci) + 1] - dev[static_cast<size_t>(ci)]) * kCtrl);
        const double zr = std::cos(phi), zi = std::sin(phi), sr = std::cos(dphi), si = std::sin(dphi);
        // The step from one harmonic to the next, in phase and in turn.
        const double hzr = stepH == 2 ? zr * zr - zi * zi : zr, hzi = stepH == 2 ? 2 * zr * zi : zi;
        const double hsr = stepH == 2 ? sr * sr - si * si : sr, hsi = stepH == 2 ? 2 * sr * si : si;
        double wr = zr, wi = zi, vr = sr, vi = si;
        const double cf = x - ci;
        for (size_t j = 0; j < H; ++j) {
          if (j > 0) {
            const double w2 = wr * hzr - wi * hzi;
            wi = wr * hzi + wi * hzr;
            wr = w2;
            const double v2 = vr * hsr - vi * hsi;
            vi = vr * hsi + vi * hsr;
            vr = v2;
          }
          pr[j] = wr;
          pi[j] = wi;
          qr[j] = vr;
          qi[j] = vi;
          const auto& d = drifts[j];
          const double slope = d[static_cast<size_t>(ci) + 1] - d[static_cast<size_t>(ci)];
          level[j] = d[static_cast<size_t>(ci)] + slope * cf;
          levelStep[j] = slope * kCtrl / rate;
        }
      } else {
        for (size_t j = 0; j < H; ++j) {
          const double r2 = pr[j] * qr[j] - pi[j] * qi[j];
          pi[j] = pr[j] * qi[j] + pi[j] * qr[j];
          pr[j] = r2;
          level[j] += levelStep[j];
        }
      }
      const double tt = t - onset;
      const bool released = i >= offIndex;
      double acc = 0.0;
      if (t >= steadyFrom && !released) {
        for (size_t j = 0; j < H; ++j) acc += level[j] * (pi[j] * gc[j] + pr[j] * gs[j]);
      } else {
        double punch = 1.0;
        if (bass && tt < 0.3) punch = 1 + 0.5 * std::max(0.0, std::exp(-tt / 0.08) - punchFloor);
        for (size_t j = 0; j < H; ++j) {
          double env = std::min(1.0, tt * invRise[j]);
          env *= env * punch;
          if (released) {
            env *= rel[j];
            rel[j] *= relStep[j];
          }
          acc += env * level[j] * (pi[j] * gc[j] + pr[j] * gs[j]);
        }
      }
      tone[static_cast<size_t>(i)] = acc;
    }

    if (!rk.reed && rk.ratio <= 4) {
      // Chiff: a breath of noise around the 3rd to 8th harmonic as the pipe speaks.
      const int m = static_cast<int>(0.15 * rate);
      const double lo = std::min(3 * f, nyq * 0.8), hi = std::min(8 * f, nyq * 0.95);
      if (hi > lo * 1.1) {
        auto hp = Biquad::make(true, lo, rate), lp = Biquad::make(false, hi, rate);
        std::vector<double> burst(static_cast<size_t>(m));
        double e2 = 0.0;
        for (int i = 0; i < m; ++i) {
          const double tb = static_cast<double>(i) / rate;
          burst[static_cast<size_t>(i)] =
              lp.run(hp.run(normal(rng))) * (1 - std::exp(-tb / 0.003)) * std::exp(-tb / 0.02);
          e2 += burst[static_cast<size_t>(i)] * burst[static_cast<size_t>(i)];
        }
        const double scale = 0.08 * rk.level / (std::sqrt(e2 / m) + 1e-12) *
                             std::sqrt(262.0 / std::max(f, 262.0));
        for (int i = 0; i < m && onsetIndex + i < n; ++i)
          tone[static_cast<size_t>(onsetIndex + i)] += burst[static_cast<size_t>(i)] * scale;
      }
    }

    const double spread = bass ? 0.06 : uniform(voice, 0.1, 0.35);
    const double pan = 0.5 + side * spread;
    const double gl = std::cos(pan * kPi / 2), gr = std::sin(pan * kPi / 2);
    for (int i = 0; i < n; ++i) {
      outL[static_cast<size_t>(i)] += tone[static_cast<size_t>(i)] * gl;
      outR[static_cast<size_t>(i)] += tone[static_cast<size_t>(i)] * gr;
    }
  }

  // Wind noise, repeating with the loop: pinkish, 150 Hz to 6 kHz. Filtered
  // over two periods and the second kept, so it joins itself.
  {
    std::vector<double> white(static_cast<size_t>(nl)), hiss(static_cast<size_t>(nl));
    for (double& v : white) v = normal(rng);
    auto hp = Biquad::make(true, 150.0, rate), lp = Biquad::make(false, 6000.0, rate);
    double b0 = 0, b1 = 0, b2 = 0;  // Paul Kellet's economy pinking filter
    double e2 = 0.0;
    for (int pass = 0; pass < 2; ++pass)
      for (int i = 0; i < nl; ++i) {
        const double w = white[static_cast<size_t>(i)];
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        const double y = lp.run(hp.run(b0 + b1 + b2 + w * 0.1848));
        if (pass == 1) {
          hiss[static_cast<size_t>(i)] = y;
          e2 += y * y;
        }
      }
    const double scale = 0.003 * totalAmp / std::sqrt(e2 / nl);
    for (int i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / rate;
      double env = std::min(1.0, t / 0.05);
      if (i >= offIndex) env *= std::exp(-(t - off) / 0.05);
      const double v = hiss[static_cast<size_t>(i % nl)] * scale * env;
      outL[static_cast<size_t>(i)] += 0.9 * v;
      outR[static_cast<size_t>(i)] += 1.1 * v;
    }
  }

  // The end of the release, faded to nothing.
  const int fade = static_cast<int>(0.1 * rate);
  for (int i = 0; i < fade; ++i) {
    const double g = 1.0 - static_cast<double>(i) / (fade - 1);
    outL[static_cast<size_t>(n - fade + i)] *= g * g;
    outR[static_cast<size_t>(n - fade + i)] *= g * g;
  }

  // Every key at the same loudness in its sustain, -20 dBFS.
  double e2 = 0.0, peak = 0.0;
  for (int i = s0; i < s0 + nl; ++i)
    e2 += outL[static_cast<size_t>(i)] * outL[static_cast<size_t>(i)] +
          outR[static_cast<size_t>(i)] * outR[static_cast<size_t>(i)];
  double gain = std::pow(10.0, -20.0 / 20.0) / std::sqrt(e2 / (2.0 * nl));
  for (int i = 0; i < n; ++i)
    peak = std::max({peak, std::abs(outL[static_cast<size_t>(i)] * gain), std::abs(outR[static_cast<size_t>(i)] * gain)});
  if (peak > 0.97) gain *= 0.97 / peak;

  Note note;
  note.left.resize(static_cast<size_t>(n));
  note.right.resize(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    note.left[static_cast<size_t>(i)] = static_cast<float>(outL[static_cast<size_t>(i)] * gain);
    note.right[static_cast<size_t>(i)] = static_cast<float>(outR[static_cast<size_t>(i)] * gain);
  }
  note.loopStart = s0;
  note.loopFrames = nl;
  return note;
}

// ---- the room ---------------------------------------------------------------
// A stone church, 60 x 22 x 24 m, worked out from the room alone: image-source
// echoes to the fourth order heard by a cardioid pair 35 m down the nave, and
// a diffuse tail decaying at each octave's reverberation time (8.5 s in the
// deep bass to 1.2 s at 16 kHz), as alike in the two channels as a diffuse
// field makes two microphones 0.6 m apart. Time zero is the direct sound.

struct V3 {
  double x, y, z;
  double operator[](int a) const { return a == 0 ? x : a == 1 ? y : z; }
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double norm(V3 a) { return std::sqrt(dot(a, a)); }
V3 scaled(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }

constexpr double kSound = 343.0;
constexpr V3 kRoomSize{60.0, 22.0, 24.0};
constexpr V3 kSource{3.0, 11.0, 15.0};
constexpr V3 kListener{38.0, 8.5, 1.7};  // in the pews, left of the aisle
constexpr double kSpacing = 0.6;
constexpr double kPair = 0.17;
constexpr double kWallX = 0.93, kWallY = 0.92, kFloor = 0.80, kVault = 0.90;
constexpr double kRt[][2] = {{31, 8.5},   {63, 8.2},   {125, 7.6},  {250, 6.8},
                             {500, 6.0},  {1000, 5.2}, {2000, 4.3}, {4000, 3.2},
                             {8000, 2.2}, {16000, 1.2}, {24000, 0.8}};
constexpr double kDirectToReverbDb = -12.0;
constexpr double kWarmDb = 4.0, kWarmFrom = 150.0, kWarmTo = 600.0;

double rtAt(double f) {
  const double lf = std::log(std::max(f, 20.0));
  const size_t count = sizeof kRt / sizeof kRt[0];
  if (lf <= std::log(kRt[0][0])) return kRt[0][1];
  for (size_t i = 1; i < count; ++i)
    if (lf <= std::log(kRt[i][0])) {
      const double a = std::log(kRt[i - 1][0]), b = std::log(kRt[i][0]);
      return kRt[i - 1][1] + (kRt[i][1] - kRt[i - 1][1]) * (lf - a) / (b - a);
    }
  return kRt[count - 1][1];
}

// Interleaved stereo, `seconds` long.
std::vector<float> makeRoom(int rate, double seconds) {
  const int n = static_cast<int>(seconds * rate);
  std::vector<double> l(static_cast<size_t>(n), 0.0), r(static_cast<size_t>(n), 0.0);

  // Early reflections. A cardioid pair facing the organ (down -x), each
  // turned 55 degrees out: an echo off one side wall lands on that side.
  const double half = 55.0 * kPi / 180.0;
  const V3 aims[2] = {{-std::cos(half), -std::sin(half), 0.0}, {-std::cos(half), std::sin(half), 0.0}};
  const V3 mics[2] = {{kListener.x, kListener.y - kPair / 2, kListener.z},
                      {kListener.x, kListener.y + kPair / 2, kListener.z}};
  const double r0 = norm(kListener - kSource);
  double front[2];
  for (int ch = 0; ch < 2; ++ch) {
    const V3 d = kSource - mics[ch];
    front[ch] = 0.5 * (1 + dot(aims[ch], scaled(d, 1.0 / norm(d))));
  }
  for (int nx = -2; nx <= 2; ++nx)
    for (int ny = -3; ny <= 3; ++ny)
      for (int nz = -2; nz <= 2; ++nz)
        for (int px = 0; px < 2; ++px)
          for (int py = 0; py < 2; ++py)
            for (int pz = 0; pz < 2; ++pz) {
              const int nn[3] = {nx, ny, nz}, pp[3] = {px, py, pz};
              double img[3];
              int lo[3], hi[3], order = 0;
              for (int a = 0; a < 3; ++a) {
                img[a] = 2 * nn[a] * kRoomSize[a] + (1 - 2 * pp[a]) * kSource[a];
                lo[a] = std::abs(nn[a] - pp[a]);
                hi[a] = std::abs(nn[a]);
                order += lo[a] + hi[a];
              }
              if (order == 0 || order > 4) continue;
              const double g = std::pow(kWallX, lo[0] + hi[0]) * std::pow(kWallY, lo[1] + hi[1]) *
                               std::pow(kFloor, lo[2]) * std::pow(kVault, hi[2]);
              const V3 im{img[0], img[1], img[2]};
              for (int ch = 0; ch < 2; ++ch) {
                const V3 d = im - mics[ch];
                const double dist = norm(d);
                const int i = static_cast<int>(std::lround((dist - r0) / kSound * rate));
                const double pick = 0.5 * (1 + dot(aims[ch], scaled(d, 1.0 / dist))) / front[ch];
                if (i > 0 && i < n) (ch == 0 ? l : r)[static_cast<size_t>(i)] += g * r0 / dist * pick;
              }
            }
  // The more bounces, the duller: a first-order lowpass at 7 kHz.
  {
    const double k = std::tan(kPi * 7000.0 / rate);
    const double b = k / (1 + k), a = (k - 1) / (k + 1);
    for (auto* ch : {&l, &r}) {
      double x1 = 0, y1 = 0;
      for (double& v : *ch) {
        const double y = b * v + b * x1 - a * y1;
        x1 = v;
        y1 = y;
        v = y;
      }
    }
  }
  double eEarly = 0.0;
  for (int i = 0; i < n; ++i)
    eEarly += 0.5 * (l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)] + r[static_cast<size_t>(i)] * r[static_cast<size_t>(i)]);

  // The diffuse tail: two noises split into octaves, each decaying at its
  // own time and building up over the first 80 ms.
  std::vector<double> dl(static_cast<size_t>(n), 0.0), dr(static_cast<size_t>(n), 0.0);
  {
    Rng rng(11);
    std::vector<double> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
    for (double& v : a) v = normal(rng);
    for (double& v : b) v = normal(rng);
    for (double fc = 31.25; fc < 0.45 * rate; fc *= 2.0) {
      auto qa = Biquad::band(fc, rate), qb = Biquad::band(fc, rate);
      const double rt = rtAt(fc);
      const double lf = std::log(fc);
      double liftDb = kWarmDb;
      if (lf >= std::log(kWarmTo)) liftDb = 0.0;
      else if (lf > std::log(kWarmFrom))
        liftDb = kWarmDb * (1.0 - (lf - std::log(kWarmFrom)) / (std::log(kWarmTo) - std::log(kWarmFrom)));
      const double lift = std::pow(10.0, liftDb / 20.0);
      const double kd = 2 * kPi * fc / kSound * kSpacing;
      const double coh = std::clamp(std::sin(kd) / kd, 0.0, 1.0);
      const double other = std::sqrt(1.0 - coh * coh);
      const double fall = std::exp(-6.91 / (rt * rate));
      double decay = 1.0;
      for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double build = std::min(1.0, t / 0.08);
        const double shape = decay * build * build * lift;
        decay *= fall;
        const double ya = qa.run(a[static_cast<size_t>(i)]), yb = qb.run(b[static_cast<size_t>(i)]);
        dl[static_cast<size_t>(i)] += ya * shape;
        dr[static_cast<size_t>(i)] += (coh * ya + other * yb) * shape;
      }
    }
    // The diffuse field begins after the first reflections.
    const int start = static_cast<int>(0.012 * rate);
    for (int i = 0; i < start && i < n; ++i) dl[static_cast<size_t>(i)] = dr[static_cast<size_t>(i)] = 0.0;
  }
  double eDiffuse = 0.0;
  for (int i = 0; i < n; ++i)
    eDiffuse += 0.5 * (dl[static_cast<size_t>(i)] * dl[static_cast<size_t>(i)] + dr[static_cast<size_t>(i)] * dr[static_cast<size_t>(i)]);

  // The direct sound at time zero, and the room's energy set against it.
  const double want = 1.0 / std::pow(10.0, kDirectToReverbDb / 10.0);
  const double gain = std::sqrt(std::max(want - eEarly, 1e-12) / std::max(eDiffuse, 1e-30));
  for (int i = 0; i < n; ++i) {
    l[static_cast<size_t>(i)] += gain * dl[static_cast<size_t>(i)];
    r[static_cast<size_t>(i)] += gain * dr[static_cast<size_t>(i)];
  }
  l[0] += 1.0;
  r[0] += 1.0;
  // The last second faded, so the file never ends on a step.
  const int fade = std::min(n, rate);
  for (int i = 0; i < fade; ++i) {
    const double g = 0.5 + 0.5 * std::cos(kPi * i / std::max(1, fade - 1));
    l[static_cast<size_t>(n - fade + i)] *= g;
    r[static_cast<size_t>(n - fade + i)] *= g;
  }
  double peak = 1e-12, e = 0.0;
  for (int i = 0; i < n; ++i) peak = std::max({peak, std::abs(l[static_cast<size_t>(i)]), std::abs(r[static_cast<size_t>(i)])});
  for (int i = 0; i < n; ++i) {
    l[static_cast<size_t>(i)] *= 0.5 / peak;
    r[static_cast<size_t>(i)] *= 0.5 / peak;
    e += l[static_cast<size_t>(i)] * l[static_cast<size_t>(i)];
  }
  // The convolver scales a room by its power and trims what lies under
  // -80 dBFS from the end, which on a long room would cut the tail about
  // 45 dB down. One quiet sample at the very end, 70 dB under the room,
  // keeps the whole of it.
  const double marker = 3e-4 * std::sqrt(e);
  l[static_cast<size_t>(n - 1)] = r[static_cast<size_t>(n - 1)] = marker;

  std::vector<float> out(static_cast<size_t>(n) * 2);
  for (int i = 0; i < n; ++i) {
    out[static_cast<size_t>(2 * i)] = static_cast<float>(l[static_cast<size_t>(i)]);
    out[static_cast<size_t>(2 * i + 1)] = static_cast<float>(r[static_cast<size_t>(i)]);
  }
  return out;
}

// ---- files ------------------------------------------------------------------

void put16(std::string& s, uint16_t v) {
  s.push_back(static_cast<char>(v & 0xff));
  s.push_back(static_cast<char>(v >> 8));
}
void put32(std::string& s, uint32_t v) {
  for (int b = 0; b < 4; ++b) s.push_back(static_cast<char>((v >> (8 * b)) & 0xff));
}

bool writeFile(const fs::path& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

// 16-bit PCM with TPDF dither, a smpl chunk with the loop (end inclusive) and
// a cue chunk marking where the release begins.
bool writeNote(const fs::path& path, int rate, const Note& note, int key) {
  Rng r(static_cast<uint64_t>(key));
  std::uniform_real_distribution<double> u(-0.5, 0.5);
  const int n = static_cast<int>(note.left.size());
  std::string c;
  c.reserve(static_cast<size_t>(n) * 4 + 200);
  c += "fmt ";
  put32(c, 16);
  put16(c, 1); put16(c, 2); put32(c, static_cast<uint32_t>(rate)); put32(c, static_cast<uint32_t>(rate * 4));
  put16(c, 4); put16(c, 16);
  c += "smpl";
  put32(c, 60);
  for (uint32_t v : {0u, 0u, static_cast<uint32_t>(std::lround(1e9 / rate)), static_cast<uint32_t>(key),
                     0u, 0u, 0u, 1u, 0u, 0u, 0u, static_cast<uint32_t>(note.loopStart),
                     static_cast<uint32_t>(note.loopStart + note.loopFrames - 1), 0u, 0u})
    put32(c, v);
  const uint32_t cue = static_cast<uint32_t>(note.loopStart + note.loopFrames);
  c += "cue ";
  put32(c, 28);
  put32(c, 1);
  put32(c, 1); put32(c, cue); c += "data"; put32(c, 0); put32(c, 0); put32(c, cue);
  c += "data";
  put32(c, static_cast<uint32_t>(n) * 4);
  for (int i = 0; i < n; ++i)
    for (float v : {note.left[static_cast<size_t>(i)], note.right[static_cast<size_t>(i)]}) {
      const double q = std::round(v * 32767.0 + u(r) + u(r));
      put16(c, static_cast<uint16_t>(static_cast<int16_t>(std::clamp(q, -32768.0, 32767.0))));
    }
  std::string file = "RIFF";
  put32(file, static_cast<uint32_t>(4 + c.size()));
  file += "WAVE";
  file += c;
  return writeFile(path, file);
}

// 32-bit float, stereo.
bool writeRoom(const fs::path& path, int rate, const std::vector<float>& frames) {
  std::string c = "fmt ";
  put32(c, 16);
  put16(c, 3); put16(c, 2); put32(c, static_cast<uint32_t>(rate)); put32(c, static_cast<uint32_t>(rate * 8));
  put16(c, 8); put16(c, 32);
  c += "data";
  put32(c, static_cast<uint32_t>(frames.size() * 4));
  for (float v : frames) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put32(c, bits);
  }
  std::string file = "RIFF";
  put32(file, static_cast<uint32_t>(4 + c.size()));
  file += "WAVE";
  file += c;
  return writeFile(path, file);
}

std::vector<int> generatedKeys(const TestSoundOptions& o) {
  std::vector<int> keys;
  const int step = std::max(1, o.keyStep);
  for (int k = o.firstKey; k <= o.lastKey; k += step) keys.push_back(k);
  if (keys.empty() || keys.back() != o.lastKey) keys.push_back(o.lastKey);
  return keys;
}

// GrandOrgue's format: one manual, one stop, the swell pedal.
std::string organText(const TestSoundOptions& o) {
  const int count = o.lastKey - o.firstKey + 1;
  std::vector<std::string> l = {
      "[Organ]", std::string("ChurchName=") + kTestSoundName, "ChurchAddress=", "OrganBuilder=Masterpiece",
      "OrganBuildDate=", "OrganComments=Generated by Masterpiece to test the audio and MIDI setup",
      "RecordingDetails=", "HasPedals=N", "NumberOfManuals=1", "NumberOfRanks=1",
      "NumberOfWindchestGroups=1", "NumberOfEnclosures=1", "NumberOfTremulants=0", "NumberOfGenerals=0",
      "NumberOfDivisionalCouplers=0", "NumberOfReversiblePistons=0", "NumberOfPanels=0",
      "DivisionalsStoreIntermanualCouplers=Y", "DivisionalsStoreIntramanualCouplers=Y",
      "DivisionalsStoreTremulants=Y", "GeneralsStoreDivisionalCouplers=Y",
      "CombinationsStoreNonDisplayedDrawstops=N", "AmplitudeLevel=100", "",
      "[Enclosure001]", "Name=Swell", "Displayed=Y", "AmpMinimumLevel=20", "MIDIInputNumber=1", "",
      "[WindchestGroup001]", "Name=Main", "NumberOfEnclosures=1", "Enclosure001=1", "NumberOfTremulants=0", "",
      "[Manual001]", "Name=Manual", "NumberOfLogicalKeys=" + std::to_string(count),
      "FirstAccessibleKeyLogicalKeyNumber=1", "FirstAccessibleKeyMIDINoteNumber=" + std::to_string(o.firstKey),
      "NumberOfAccessibleKeys=" + std::to_string(count), "MIDIInputNumber=1", "Displayed=Y",
      "NumberOfStops=1", "NumberOfCouplers=0", "NumberOfDivisionals=0", "NumberOfTremulants=0",
      "Stop001=1", "",
      "[Rank001]", std::string("Name=") + kTestSoundName, "FirstMidiNoteNumber=" + std::to_string(o.firstKey),
      "NumberOfLogicalPipes=" + std::to_string(count), "WindchestGroup=1", "Percussive=N",
      // The samples sit at -20 dBFS each for their 16 bits; the rank plays
      // them 10 dB lower, so both hands full stay clear of full scale.
      "HarmonicNumber=8", "AmplitudeLevel=100", "Gain=-10"};
  char buf[96];
  for (int k = o.firstKey; k <= o.lastKey; ++k) {
    const int i = k - o.firstKey + 1;
    const int src = testSoundSourceKey(k, o);
    std::snprintf(buf, sizeof buf, "Pipe%03d=%s\\%03d.wav", i, kStem, src);
    l.push_back(buf);
    if (src != k) {
      std::snprintf(buf, sizeof buf, "Pipe%03dPitchTuning=%d", i, 100 * (k - src));
      l.push_back(buf);
    }
  }
  for (const std::string& s :
       {std::string(), std::string("[Stop001]"), std::string("Name=") + kTestSoundName,
        std::string("NumberOfRanks=1"), std::string("Rank001=1"),
        std::string("FirstAccessiblePipeLogicalKeyNumber=1"), std::string("FirstAccessiblePipeLogicalPipeNumber=1"),
        "NumberOfAccessiblePipes=" + std::to_string(count), std::string("DefaultToEngaged=Y"),
        std::string("Displayed=Y"), std::string("DisplayInInvertedState=N"), std::string("Function=Input"),
        std::string()})
    l.push_back(s);
  std::string out;
  for (size_t i = 0; i < l.size(); ++i) out += l[i] + (i + 1 < l.size() ? "\r\n" : "");
  return out;
}

}  // namespace

int testSoundSourceKey(int key, const TestSoundOptions& options) {
  int best = options.firstKey;
  for (const int k : generatedKeys(options))
    if (std::abs(k - key) < std::abs(best - key)) best = k;
  return best;
}

TestSoundResult writeTestSound(const fs::path& folder, const TestSoundOptions& options,
                                 const std::function<bool(double)>& progress) {
  TestSoundResult result;
  TestSoundOptions o = options;
  o.firstKey = std::clamp(o.firstKey, 0, 127);
  o.lastKey = std::clamp(o.lastKey, o.firstKey, 127);
  o.sampleRate = std::clamp(o.sampleRate, 8000, 192000);
  const fs::path dir = folder / kStem;
  const fs::path organ = folder / (std::string(kStem) + ".organ");
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    result.error = "cannot create " + dir.string() + ": " + ec.message();
    return result;
  }

  // The room is job 0, the keys the rest, the work shared by every core.
  const std::vector<int> keys = generatedKeys(o);
  const int jobs = static_cast<int>(keys.size()) + 1;
  int threads = o.threads > 0 ? o.threads : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  threads = std::clamp(threads, 1, jobs);
  std::atomic<int> next{0}, done{0};
  std::atomic<bool> stop{false}, failed{false};
  std::mutex reporting;
  std::string firstError;
  auto work = [&] {
    for (int job; !stop.load() && (job = next.fetch_add(1)) < jobs;) {
      bool ok;
      fs::path path;
      if (job == 0) {
        path = dir / "room.wav";
        ok = writeRoom(path, o.sampleRate, makeRoom(o.sampleRate, o.roomSeconds));
      } else {
        const int key = keys[static_cast<size_t>(job - 1)];
        char name[16];
        std::snprintf(name, sizeof name, "%03d.wav", key);
        path = dir / name;
        ok = writeNote(path, o.sampleRate, makeNote(key, o.sampleRate), key);
      }
      std::lock_guard<std::mutex> lock(reporting);
      if (!ok) {
        if (!failed.exchange(true)) firstError = "cannot write " + path.string();
        stop = true;
        return;
      }
      const int finished = ++done;
      if (progress && !progress(static_cast<double>(finished) / (jobs + 1))) stop = true;
    }
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (auto& th : pool) th.join();

  // The definition last, so an organ list never shows a half-made organ.
  if (!stop.load()) {
    const fs::path partial = organ.string() + ".part";
    if (writeFile(partial, organText(o))) {
      fs::rename(partial, organ, ec);
      if (ec) firstError = "cannot write " + organ.string() + ": " + ec.message();
    } else {
      firstError = "cannot write " + organ.string();
    }
    fs::remove(partial, ec);
  }
  if (stop.load() || !firstError.empty()) {
    fs::remove_all(dir, ec);
    fs::remove(organ, ec);
    result.error = firstError.empty() ? "stopped" : firstError;
    return result;
  }
  if (progress) progress(1.0);
  result.ok = true;
  result.organFile = organ;
  result.roomFile = dir / "room.wav";
  result.samples = static_cast<int>(keys.size());
  return result;
}

}  // namespace mp
