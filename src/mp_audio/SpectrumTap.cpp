// SPDX-License-Identifier: GPL-3.0-only
#include "SpectrumTap.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace mp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// In-place radix-2 FFT; the size is a power of two.
void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> step(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k];
        const auto v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= step;
      }
    }
  }
}

}  // namespace

std::vector<float> SpectrumTap::analyse(const float* samples) {
  std::vector<std::complex<double>> a(kFft);
  double windowSum = 0.0;
  for (int i = 0; i < kFft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (kFft - 1));
    windowSum += w;
    a[static_cast<size_t>(i)] = {samples[i] * w, 0.0};
  }
  fft(a);
  // A sine of amplitude A puts A * windowSum / 2 in its bin.
  const double scale = 2.0 / windowSum;
  std::vector<float> out(kFft / 2 + 1);
  for (size_t k = 0; k < out.size(); ++k) {
    const double mag = std::abs(a[k]) * scale;
    out[k] = static_cast<float>(20.0 * std::log10(mag + 1e-9));
  }
  return out;
}

std::vector<float> SpectrumTap::spectrumDb() const {
  std::vector<float> block(kFft);
  latest(block.data(), kFft);
  return analyse(block.data());
}

void SpectrumTap::latest(float* out, int n) const {
  n = std::clamp(n, 0, kPitchSpan);
  const int end = write_.load(std::memory_order_acquire);
  int r = (end - n + kRing) & (kRing - 1);
  for (int i = 0; i < n; ++i) {
    out[i] = ring_[static_cast<size_t>(r)].load(std::memory_order_relaxed);
    r = (r + 1) & (kRing - 1);
  }
}

namespace {

// The magnitude of the windowed transform of x at hz: one bin of a transform
// as fine as wanted, worked out by a rotating phasor.
double magnitudeAt(const float* x, const std::vector<double>& window, double hz, double rate) {
  const double step = 2.0 * kPi * hz / rate;
  const std::complex<double> turn(std::cos(step), -std::sin(step));
  std::complex<double> phase(1.0, 0.0);
  std::complex<double> sum(0.0, 0.0);
  for (size_t i = 0; i < window.size(); ++i) {
    sum += phase * (window[i] * static_cast<double>(x[i]));
    phase *= turn;
  }
  return std::abs(sum);
}

// Where the windowed transform peaks between lo and hi: the best of a scan,
// then a golden-section search around it.
double peakBetween(const float* x, const std::vector<double>& window, double rate, double lo, double hi,
                   int scan, int refine, double& magnitude) {
  const double d = (hi - lo) / scan;
  int bestI = 0;
  double best = -1.0;
  for (int i = 0; i <= scan; ++i) {
    const double m = magnitudeAt(x, window, lo + i * d, rate);
    if (m > best) {
      best = m;
      bestI = i;
    }
  }
  double a = lo + std::max(0, bestI - 1) * d;
  double b = lo + std::min(scan, bestI + 1) * d;
  const double g = 0.5 * (std::sqrt(5.0) - 1.0);
  double c = b - g * (b - a);
  double e = a + g * (b - a);
  double mc = magnitudeAt(x, window, c, rate);
  double me = magnitudeAt(x, window, e, rate);
  for (int i = 0; i < refine; ++i) {
    if (mc > me) {
      b = e;
      e = c;
      me = mc;
      c = b - g * (b - a);
      mc = magnitudeAt(x, window, c, rate);
    } else {
      a = c;
      c = e;
      mc = me;
      e = a + g * (b - a);
      me = magnitudeAt(x, window, e, rate);
    }
  }
  const double hz = 0.5 * (a + b);
  magnitude = magnitudeAt(x, window, hz, rate);
  if (best > magnitude) {
    magnitude = best;
    return lo + bestI * d;
  }
  return hz;
}

}  // namespace

SpectrumTap::Pitch SpectrumTap::measurePitch(const float* samples, int n, double sampleRate, double lowestHz) {
  Pitch out;
  if (samples == nullptr || n < kFft || sampleRate <= 0.0) return out;
  // The bins of the last kFft samples find the note; the whole span reads it.
  const auto db = analyse(samples + (n - kFft));
  const double binHz = sampleRate / kFft;
  const size_t first = static_cast<size_t>(std::ceil(std::max(25.0, lowestHz) / binHz));
  const size_t last = std::min(db.size() - 2, static_cast<size_t>(5000.0 / binHz));
  if (first >= last) return out;
  const float loudest = *std::max_element(db.begin() + static_cast<std::ptrdiff_t>(first),
                                          db.begin() + static_cast<std::ptrdiff_t>(last) + 1);
  // Below this the window shows nothing: a blower or a reverb tail.
  if (loudest < -80.0f) return out;
  size_t k = first;
  for (; k <= last; ++k)
    if (db[k] >= loudest - 25.0f && db[k] >= db[k - 1] && db[k] >= db[k + 1]) break;
  if (k > last) return out;

  std::vector<double> window(static_cast<size_t>(n));
  double windowSum = 0.0;
  for (int i = 0; i < n; ++i) {
    window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
    windowSum += window[static_cast<size_t>(i)];
  }
  double fundamental = 0.0;
  out.hz = peakBetween(samples, window, sampleRate, (static_cast<double>(k) - 1.0) * binHz,
                       (static_cast<double>(k) + 1.0) * binHz, 16, 30, fundamental);
  if (fundamental <= 0.0) return out;
  out.found = true;
  out.levelDb = static_cast<float>(20.0 * std::log10(fundamental * 2.0 / windowSum + 1e-9));
  const double fineHz = sampleRate / n;
  for (int h = 1; h <= Pitch::kHarmonics; ++h) {
    auto& level = out.harmonicsDb[static_cast<size_t>(h - 1)];
    if (h == 1) {
      level = 0.0f;
      continue;
    }
    const double centre = h * out.hz;
    if (centre + fineHz >= 0.5 * sampleRate) {
      level = -120.0f;
      continue;
    }
    double m = 0.0;
    peakBetween(samples, window, sampleRate, centre - 0.5 * fineHz, centre + 0.5 * fineHz, 6, 12, m);
    level = static_cast<float>(std::max(-120.0, 20.0 * std::log10(m / fundamental + 1e-12)));
  }
  return out;
}

}  // namespace mp
