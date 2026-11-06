// SPDX-License-Identifier: GPL-3.0-only
#include "SpectrumTap.h"

#include <cmath>
#include <complex>

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
  const int end = write_.load(std::memory_order_acquire);
  int r = (end - kFft + kRing) & (kRing - 1);
  for (int i = 0; i < kFft; ++i) {
    block[static_cast<size_t>(i)] = ring_[static_cast<size_t>(r)].load(std::memory_order_relaxed);
    r = (r + 1) & (kRing - 1);
  }
  return analyse(block.data());
}

}  // namespace mp
