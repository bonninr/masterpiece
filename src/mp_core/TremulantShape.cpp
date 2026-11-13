#include "TremulantShape.h"

#include "OrganModel.h"

#include <algorithm>

namespace mp {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// Each harmonic 1..top+1 of a mean-removed series, by its power, added to
// `sum` as a share of the series' power over 1..top. Read at about 4096
// points of the loop with a rotating phasor, so a loop of several seconds
// costs a few milliseconds.
void addSpectrum(const std::vector<double>& x, int top, std::vector<double>& sum) {
  const size_t n = x.size();
  const size_t stride = std::max<size_t>(1, n / 4096);
  std::vector<double> power(static_cast<size_t>(top) + 2, 0.0);
  double total = 0.0;
  for (int k = 1; k <= top + 1; ++k) {
    const double w = kTwoPi * static_cast<double>(k) * static_cast<double>(stride) / static_cast<double>(n);
    const double cw = std::cos(w), sw = std::sin(w);
    double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
    for (size_t j = 0; j < n; j += stride) {
      re += x[j] * c;
      im -= x[j] * s;
      const double nc = c * cw - s * sw;
      s = s * cw + c * sw;
      c = nc;
    }
    power[static_cast<size_t>(k)] = re * re + im * im;
    if (k <= top) total += power[static_cast<size_t>(k)];
  }
  if (total <= 0.0) return;
  for (int k = 1; k <= top; ++k) sum[static_cast<size_t>(k)] += power[static_cast<size_t>(k)] / total;
}

// The cycles in a loop, from the spectra of the channels that move.
//
// A loop may hold one cycle or many: a file cut from a recording loops over
// seconds, dozens of cycles, and not always a whole number of them. A count
// k is the loop's when the harmonics at its multiples hold most of both
// channels' power together; the largest such count is the cycle. One channel
// alone misleads: a pitch that hardly moves is mostly noise, and its strongest
// line can be the third harmonic of a cycle its level shows plainly (#1074).
//
// The candidates are the spectrum's peaks, each placed between its line and
// the stronger neighbour by their amplitudes, and their divisors. Below four
// cycles a count is whole: a loop that short is cut to whole cycles.
double cyclesOf(const std::vector<double>& cents, const std::vector<double>* lvl) {
  constexpr int kMaxCycles = 512;
  const size_t n = cents.size();
  const int top = static_cast<int>(std::min<size_t>(kMaxCycles, n / 64));
  if (top < 2) return 1.0;
  std::vector<double> p(static_cast<size_t>(top) + 2, 0.0);
  addSpectrum(cents, top, p);
  if (lvl != nullptr) addSpectrum(*lvl, top, p);
  double total = 0.0;
  for (int k = 1; k <= top; ++k) total += p[static_cast<size_t>(k)];
  if (total <= 0.0) return 1.0;

  // The power at the multiples of a count: the line each falls on, or the
  // two it falls between.
  const auto held = [&](double kf) {
    double e = 0.0;
    for (int m = 1; m * kf <= top + 0.5; ++m) {
      const int lo = static_cast<int>(std::floor(m * kf));
      const int hi = static_cast<int>(std::ceil(m * kf));
      if (lo >= 1 && lo <= top) e += p[static_cast<size_t>(lo)];
      if (hi != lo && hi >= 1 && hi <= top) e += p[static_cast<size_t>(hi)];
    }
    return e;
  };

  std::vector<double> candidates;
  for (int h = 2; h <= top; ++h) {
    const auto u = static_cast<size_t>(h);
    if (p[u] < p[u - 1] || p[u] < p[u + 1] || p[u] <= 0.02 * total) continue;
    const double a = std::sqrt(p[u]), lo = std::sqrt(p[u - 1]), hi = std::sqrt(p[u + 1]);
    const double side = std::max(lo, hi);
    const double kf = static_cast<double>(h) + (hi >= lo ? 1.0 : -1.0) * (side / (a + side));
    for (int d = 1; kf / d >= 1.0; ++d) {
      const double c = kf / d;
      candidates.push_back(c >= 4.0 ? c : std::round(c));
    }
  }
  double best = 1.0;
  for (const double c : candidates)
    if (c > best && held(c) >= 0.6 * total) best = c;
  // One cycle holds most of its power in its first few lines. A loop whose
  // power is spread wide repeats nothing: squeezed into one tremulant cycle it
  // would warble, so it plays the tremulant's sine. Recorded one-cycle loops
  // hold 94 % or more there.
  if (best == 1.0) {
    double low = 0.0;
    for (int k = 1; k <= std::min(top, 8); ++k) low += p[static_cast<size_t>(k)];
    if (low < 0.6 * total) return 0.0;
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
  const double cycles = cyclesOf(centsSpread >= 1.0e-4 ? cents : lvl,
                                 centsSpread >= 1.0e-4 && level != nullptr && lvlSpread >= 1.0e-7 ? &lvl : nullptr);
  if (!(cycles > 0.0)) return false;
  const double period = static_cast<double>(n) / cycles;
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
  out.cyclesInLoop = static_cast<int>(std::lround(cycles));
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
