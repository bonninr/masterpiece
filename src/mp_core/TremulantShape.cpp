#include "TremulantShape.h"

#include "OrganModel.h"

#include <algorithm>

namespace mp {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// The strongest of harmonics 1..8 of a mean-removed series, as a cycle count.
// Read at about 4096 points of the loop: eight harmonics need far fewer, and
// a loop of several seconds would otherwise cost seconds over a whole organ.
int strongestHarmonic(const std::vector<double>& x) {
  const size_t n = x.size();
  const size_t stride = std::max<size_t>(1, n / 4096);
  int best = 1;
  double bestPower = -1.0;
  for (int k = 1; k <= 8; ++k) {
    if (static_cast<size_t>(k) * 64 > n) break;
    double re = 0.0, im = 0.0;
    for (size_t j = 0; j < n; j += stride) {
      const double w = kTwoPi * static_cast<double>(k) * static_cast<double>(j) / static_cast<double>(n);
      re += x[j] * std::cos(w);
      im -= x[j] * std::sin(w);
    }
    const double power = re * re + im * im;
    if (power > bestPower) {
      bestPower = power;
      best = k;
    }
  }
  return best;
}

double peakOf(const std::array<float, TremulantShape::kPoints + 1>& t) {
  double p = 0.0;
  for (int i = 0; i < TremulantShape::kPoints; ++i)
    p = std::max(p, static_cast<double>(std::fabs(t[static_cast<size_t>(i)])));
  return p;
}

}  // namespace

bool buildTremulantShape(const float* pitch, const float* level, size_t frames,
                         size_t loopStart, size_t loopEnd, TremulantShape& out) {
  out = TremulantShape{};
  if (pitch == nullptr || loopEnd > frames || loopEnd <= loopStart || loopEnd - loopStart < 64)
    return false;
  const size_t n = loopEnd - loopStart;

  std::vector<double> cents(n), lvl(n, 0.0);
  double centsMean = 0.0, lvlMean = 0.0;
  for (size_t j = 0; j < n; ++j) {
    const double f = 1.0 + static_cast<double>(pitch[loopStart + j]);
    cents[j] = 1200.0 * std::log2(std::max(f, 1.0e-6));
    centsMean += cents[j];
    if (level != nullptr) {
      lvl[j] = static_cast<double>(level[loopStart + j]);
      lvlMean += lvl[j];
    }
  }
  centsMean /= static_cast<double>(n);
  lvlMean /= static_cast<double>(n);
  double centsSpread = 0.0, lvlSpread = 0.0;
  for (size_t j = 0; j < n; ++j) {
    cents[j] -= centsMean;
    lvl[j] -= lvlMean;
    centsSpread = std::max(centsSpread, std::fabs(cents[j]));
    lvlSpread = std::max(lvlSpread, std::fabs(lvl[j]));
  }
  if (centsSpread < 1.0e-4 && lvlSpread < 1.0e-7) return false;

  // The cycles in the loop, from the channel that moves.
  const int cycles = strongestHarmonic(centsSpread >= 1.0e-4 ? cents : lvl);
  const double period = static_cast<double>(n) / static_cast<double>(cycles);
  if (period < TremulantShape::kPoints / 4) return false;

  // Every frame of the loop into the point of the cycle it falls on, counted
  // from the file's start, then averaged: the cycles together, and the frames
  // of one point together.
  constexpr int kP = TremulantShape::kPoints;
  std::array<double, kP> sumC{}, sumL{};
  std::array<int, kP> count{};
  for (size_t j = 0; j < n; ++j) {
    const double phase = static_cast<double>(loopStart + j) / period;
    const double frac = phase - std::floor(phase);
    const int point = static_cast<int>(std::floor(frac * kP + 0.5)) % kP;
    sumC[static_cast<size_t>(point)] += cents[j];
    sumL[static_cast<size_t>(point)] += lvl[j];
    ++count[static_cast<size_t>(point)];
  }
  std::array<double, kP> c{}, l{};
  double cMean = 0.0, lMean = 0.0;
  for (int i = 0; i < kP; ++i) {
    const auto u = static_cast<size_t>(i);
    if (count[u] > 0) {
      c[u] = sumC[u] / count[u];
      l[u] = sumL[u] / count[u];
    } else if (i > 0) {
      c[u] = c[u - 1];
      l[u] = l[u - 1];
    }
    cMean += c[u];
    lMean += l[u];
  }
  cMean /= kP;
  lMean /= kP;

  double peakC = 0.0;
  for (int i = 0; i < kP; ++i) {
    const auto u = static_cast<size_t>(i);
    c[u] -= cMean;
    l[u] -= lMean;
    peakC = std::max(peakC, std::fabs(c[u]));
  }
  out.pitchPeakCents = peakC;
  out.cyclesInLoop = cycles;
  out.pitchCapped = peakC > TremulantShape::kMaxPitchCents;
  const double toUnits = out.pitchCapped ? (peakC > 0.0 ? 1.0 / peakC : 0.0)
                                         : 1.0 / kTremulantWaveformPitchCents;
  for (int i = 0; i < kP; ++i) {
    const auto u = static_cast<size_t>(i);
    out.pitch[u] = static_cast<float>(c[u] * toUnits);
    out.amp[u] = static_cast<float>(l[u]);
  }
  out.pitch[kP] = out.pitch[0];
  out.amp[kP] = out.amp[0];
  out.ampPeak = peakOf(out.amp);
  return true;
}

void normaliseTremulantShapes(std::vector<TremulantShape>& shapes) {
  std::vector<double> peaks;
  for (const TremulantShape& s : shapes)
    if (s.ampPeak > 1.0e-9) peaks.push_back(s.ampPeak);
  constexpr int kP = TremulantShape::kPoints;
  if (peaks.empty()) {
    for (TremulantShape& s : shapes) {
      const double p = peakOf(s.pitch);
      for (int i = 0; i <= kP; ++i) {
        const auto u = static_cast<size_t>(i);
        s.amp[u] = p > 0.0 ? static_cast<float>(s.pitch[u] / p) : 0.0f;
      }
    }
    return;
  }
  std::nth_element(peaks.begin(), peaks.begin() + static_cast<long>(peaks.size() / 2), peaks.end());
  const double median = peaks[peaks.size() / 2];
  for (TremulantShape& s : shapes)
    for (int i = 0; i <= kP; ++i) {
      const auto u = static_cast<size_t>(i);
      s.amp[u] = static_cast<float>(std::clamp(s.amp[u] / median, -4.0, 4.0));
    }
}

}  // namespace mp
