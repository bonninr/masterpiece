// SPDX-License-Identifier: GPL-3.0-only
// Where a release starts, so that it meets the attack it follows in phase.
//
// At a note-off the attack keeps sounding through its loop while the release
// fades in over the set's crossfade. A release always started at its first
// frame meets the loop at whatever point of its cycle the key was let go: in
// phase the two add, out of phase they cancel, and the level through the
// crossfade then depends on the timing of the key, by as much as 9 dB on one
// pipe of a Sonus Paradisi set (#525). Sets ask for the alignment on their
// releases with PhaseAlignAutomatically; GrandOrgue aligns every release.
//
// The match is made on the first three harmonics of the pipe, at its recorded
// pitch: their phases in two periods of the attack from the note-off point,
// against their phases in two periods of the release from its start. That
// scores the same as a full cross-correlation over the same windows (measured
// on the set's own files) and costs a few hundred operations a voice. The
// start moves forward by less than one period.
//
// Left alone, as recorded: an attack too quiet to hold a phase (a silent loop
// standing in for an attack, as some sets build key noises), a signal whose
// pitch does not show at the stated frequency, and a release whose head is
// not in memory.
#pragma once
#include "VoiceEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mp {

struct ReleaseAlignment {
  double offset = 0.0; // release frames to move the start forward by
  bool aligned = false;
};

namespace releasealign {

// The sum of a buffer's channels at a fractional frame, linearly interpolated.
// `read(frame, channel)` answers as SampleBuffer::sample() does.
template <class Read>
inline double summed(int channels, Read& read, double pos) {
  const auto i = static_cast<int64_t>(pos);
  const double t = pos - static_cast<double>(i);
  double a = 0.0, c = 0.0;
  for (int ch = 0; ch < channels; ++ch) {
    a += read(i, ch);
    c += read(i + 1, ch);
  }
  return a + (c - a) * t;
}

constexpr int kHarmonics = 3;
// At most this many reads a period; a low pipe is read with a stride.
constexpr int kPerPeriod = 64;
// The share of a window's energy the three harmonics must hold for the pitch
// to be trusted. A trumpet holds 21-51 %, flue pipes over 70 %.
constexpr double kMinHarmonicShare = 0.05;
// An attack this far below the release (in energy) holds no phase to meet.
constexpr double kSilentAttack = 1e-6;

}  // namespace releasealign

// attackPos: where the attack is, in its own frames. attackStep: attack frames
// per release frame. periodFrames: the pipe's period, in release frames.
// readAttack and readRelease answer for each buffer as SampleBuffer::sample()
// does; a sample held coded is read through the voice's windows. Each side is
// read in a pass of its own, so a reader that decodes reads one sample at a
// time, in order.
template <class ReadAttack, class ReadRelease>
inline ReleaseAlignment alignReleaseWith(const SampleBuffer& attack, ReadAttack&& readAttack,
                                         double attackPos, int64_t loopStart, int64_t loopEnd,
                                         double attackStep, const SampleBuffer& release,
                                         ReadRelease&& readRelease, double releaseStart,
                                         double periodFrames) {
  using namespace releasealign;
  ReleaseAlignment none;
  const double P = periodFrames;
  if (!(P >= 4.0) || P > 16384.0 || !(attackStep > 0.0) || !std::isfinite(attackStep))
    return none;
  const int stride = std::max(1, static_cast<int>(P / kPerPeriod));
  const int n = static_cast<int>(2.0 * P / stride);
  if (n < 8) return none;
  // The release's two periods plus the one period the start may move by.
  if (releaseStart < 0.0 || releaseStart + 3.0 * P + 2.0 > static_cast<double>(release.numFrames))
    return none;
  const bool loops = loopStart >= 0 && loopEnd > loopStart + 1 && loopEnd <= attack.numFrames;
  const double loopLen = static_cast<double>(loopEnd - loopStart);
  if (!loops && attackPos + 2.0 * P * attackStep + 2.0 > static_cast<double>(attack.numFrames))
    return none;

  // One DFT bin per harmonic on each side, the phasors turned step by step.
  double aRe[kHarmonics] = {}, aIm[kHarmonics] = {};
  double rRe[kHarmonics] = {}, rIm[kHarmonics] = {};
  double aEnergy = 0.0, rEnergy = 0.0;
  const double step = 2.0 * 3.14159265358979323846 * stride / P;
  const double cs = std::cos(step), sn = std::sin(step);
  // One side's reads into its bins. The two sides' sums do not meet, so one
  // pass each gives the same numbers as one pass over both.
  const auto bins = [&](auto position, auto& read, int channels, double* re, double* im,
                        double& energy) {
    double c1 = 1.0, s1 = 0.0; // the fundamental's phasor at this read
    for (int k = 0; k < n; ++k) {
      const double x = summed(channels, read, position(k));
      energy += x * x;
      double ch = c1, sh = s1; // harmonic h's phasor, as a power of the fundamental's
      for (int h = 0; h < kHarmonics; ++h) {
        re[h] += x * ch;
        im[h] -= x * sh;
        const double nc = ch * c1 - sh * s1;
        sh = ch * s1 + sh * c1;
        ch = nc;
      }
      const double nc = c1 * cs - s1 * sn;
      s1 = c1 * sn + s1 * cs;
      c1 = nc;
    }
  };
  double pa = attackPos;
  const double paStep = attackStep * stride;
  // The attack's reads step on from where it is, round its loop.
  bins(
      [&](int k) {
        if (k > 0) pa += paStep;
        if (loops)
          while (pa >= static_cast<double>(loopEnd) - 1.0) pa -= loopLen;
        return pa;
      },
      readAttack, attack.numChannels, aRe, aIm, aEnergy);
  bins([&](int k) { return releaseStart + static_cast<double>(k) * stride; }, readRelease,
       release.numChannels, rRe, rIm, rEnergy);
  if (!(rEnergy > 0.0) || aEnergy < rEnergy * kSilentAttack) return none;
  double aHarm = 0.0, rHarm = 0.0;
  for (int h = 0; h < kHarmonics; ++h) {
    aHarm += aRe[h] * aRe[h] + aIm[h] * aIm[h];
    rHarm += rRe[h] * rRe[h] + rIm[h] * rIm[h];
  }
  // A pure tone at the bin puts all its energy there: |X|^2 = E * n / 2.
  const double half = static_cast<double>(n) / 2.0;
  if (aHarm < kMinHarmonicShare * aEnergy * half || rHarm < kMinHarmonicShare * rEnergy * half)
    return none;

  // Starting the release s frames later turns its harmonic h by h * w * s.
  // The match is the sum over the harmonics of |A||R| cos(arg A - arg R - h w s),
  // found on a grid over one period, then refined around the best point.
  double mag[kHarmonics], diff[kHarmonics];
  for (int h = 0; h < kHarmonics; ++h) {
    mag[h] = std::hypot(aRe[h], aIm[h]) * std::hypot(rRe[h], rIm[h]);
    diff[h] = std::atan2(aIm[h], aRe[h]) - std::atan2(rIm[h], rRe[h]);
  }
  const double w = 2.0 * 3.14159265358979323846 / P;
  auto score = [&](double s) {
    double v = 0.0;
    for (int h = 0; h < kHarmonics; ++h) v += mag[h] * std::cos(diff[h] - (h + 1) * w * s);
    return v;
  };
  constexpr int kGrid = 48;
  double best = -1e300, bestS = 0.0;
  for (int g = 0; g < kGrid; ++g) {
    const double s = P * g / kGrid;
    const double v = score(s);
    if (v > best) best = v, bestS = s;
  }
  const double cell = P / kGrid;
  for (int g = -8; g <= 8; ++g) {
    const double s = bestS + cell * g / 8.0;
    const double v = score(s);
    if (v > best) best = v, bestS = s;
  }
  bestS = std::fmod(bestS + P, P);
  return {bestS, true};
}

// GrandOrgue's way (GOSoundReleaseAlignTable): the attack's last two frames
// give a level and a slope, each into a bin, 32 levels across the pipe's
// range and 2 slopes, rising or not; the release starts after the first frame
// of its first 50 ms that falls in the same bins, or the nearest bin found.
// The range here is the larger of the two sides' peaks over that window.
template <class ReadAttack, class ReadRelease>
inline ReleaseAlignment alignReleaseTableWith(const SampleBuffer& attack, ReadAttack&& readAttack,
                                              double attackPos, int64_t loopStart, int64_t loopEnd,
                                              double attackStep, const SampleBuffer& release,
                                              ReadRelease&& readRelease, double releaseStart) {
  using namespace releasealign;
  ReleaseAlignment none;
  constexpr int kLevels = 32;
  const int len = static_cast<int>(release.sampleRate / 20.0);
  if (len < 2 * kLevels * 2 || !(attackStep > 0.0) || !std::isfinite(attackStep)) return none;
  if (releaseStart < 0.0 || releaseStart + len + 2.0 > static_cast<double>(release.numFrames)) return none;
  const bool loops = loopStart >= 0 && loopEnd > loopStart + 1 && loopEnd <= attack.numFrames;
  const double loopLen = static_cast<double>(loopEnd - loopStart);
  if (attackPos < 2.0 * attackStep + 1.0) return none;
  if (!loops && attackPos + len * attackStep + 2.0 > static_cast<double>(attack.numFrames)) return none;
  const double f = summed(attack.numChannels, readAttack, attackPos - attackStep);
  const double fPrev = summed(attack.numChannels, readAttack, attackPos - 2.0 * attackStep);
  // The range: both sides' peaks over the window, the attack read on round
  // its loop as it goes on sounding.
  double peak = 0.0;
  double pa = attackPos;
  for (int i = 0; i < len; ++i) {
    peak = std::max(peak, std::fabs(summed(release.numChannels, readRelease, releaseStart + i)));
    if (loops)
      while (pa >= static_cast<double>(loopEnd) - 1.0) pa -= loopLen;
    peak = std::max(peak, std::fabs(summed(attack.numChannels, readAttack, pa)));
    pa += attackStep;
  }
  if (!(peak > 0.0)) return none;
  const auto level = [&](double x) {
    return std::clamp(static_cast<int>(std::floor((x + peak) * kLevels / (2.0 * peak))), 0, kLevels - 1);
  };
  const int wantLevel = level(f);
  const bool wantRising = f - fPrev > 0.0;
  int bestI = -1, bestGap = 1 << 30;
  double prev = summed(release.numChannels, readRelease, releaseStart);
  for (int i = 1; i < len; ++i) {
    const double x = summed(release.numChannels, readRelease, releaseStart + i);
    const bool rising = x - prev > 0.0;
    prev = x;
    const int gap = std::abs(level(x) - wantLevel) * 2 + (rising == wantRising ? 0 : 1);
    if (gap < bestGap) {
      bestGap = gap;
      bestI = i;
      if (gap == 0) break;
    }
  }
  if (bestI < 0) return none;
  return {static_cast<double>(bestI + 1), true};
}

// Both samples read as they are held, through SampleBuffer::sample().
inline ReleaseAlignment alignRelease(const SampleBuffer& attack, double attackPos,
                                     int64_t loopStart, int64_t loopEnd, double attackStep,
                                     const SampleBuffer& release, double releaseStart,
                                     double periodFrames) {
  return alignReleaseWith(
      attack, [&](int64_t i, int ch) { return attack.sample(i, ch); }, attackPos, loopStart,
      loopEnd, attackStep, release, [&](int64_t i, int ch) { return release.sample(i, ch); },
      releaseStart, periodFrames);
}

}  // namespace mp
