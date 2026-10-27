// The bumpometer: how smoothly a pipe goes from its held sound into its
// release, measured on what came out.
//
// A key let go joins two recordings, the sustain and the release. Where they
// meet in phase the level passes from one to the other along a smooth line;
// where they meet out of phase the crossfade cancels or adds, and the player
// hears a dip or a bump whose size depends on the moment the key let go.
//
// Two figures for one release:
// - the level bump: the largest gap, in dB, between the short-window level
//   through the joint and a straight line (in dB) from the held level to the
//   release's level just after the crossfade;
// - the click: the high-frequency energy at the joint (the second difference
//   of the signal) over that of the held sound, in dB;
// - the jump: the most the level rises above the held level through the
//   joint, in dB: a release recorded louder than the note it follows.
//
// No JUCE and no engine: it reads a rendered signal, so the tests and the
// render tool measure alike.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mp::bump {

struct Joint {
  double bumpDb = 0.0;   // largest gap from the smooth line, either way
  double clickDb = 0.0;  // HF energy at the joint over the held sound's
  double jumpDb = 0.0;   // the most the level rises above the held level
  double heldDb = 0.0;
  double tailDb = 0.0;
};

struct Summary {
  double worstBumpDb = 0.0;
  double medianBumpDb = 0.0;
  double spreadDb = 0.0;  // worst less best: how much the key's timing matters
  double worstClickDb = 0.0;
  double worstJumpDb = 0.0;
  double medianJumpDb = 0.0;
  int count = 0;
};

inline double rmsOf(const std::vector<float>& y, int64_t from, int64_t to) {
  from = std::max<int64_t>(0, from);
  to = std::min<int64_t>(static_cast<int64_t>(y.size()), to);
  if (to <= from) return 0.0;
  double acc = 0.0;
  for (int64_t i = from; i < to; ++i) acc += static_cast<double>(y[static_cast<size_t>(i)]) * y[static_cast<size_t>(i)];
  return std::sqrt(acc / static_cast<double>(to - from));
}

inline double dbOf(double level) { return 20.0 * std::log10(std::max(level, 1e-6)); }

// `y` is the pipe alone, mono; the key let go at frame `at`. The window is
// 5 ms, or one period of the pipe when that is longer, so a bass pipe's own
// waveform is not read as a bump.
inline Joint measure(const std::vector<float>& y, double rate, int64_t at, int64_t crossfadeFrames,
                     double periodFrames) {
  Joint j;
  const int64_t w = std::max<int64_t>(static_cast<int64_t>(std::lround(0.005 * rate)),
                                      static_cast<int64_t>(std::ceil(periodFrames)));
  const int64_t heldSpan = std::max<int64_t>(static_cast<int64_t>(0.030 * rate), 4 * w);
  const int64_t span = crossfadeFrames + 2 * w + static_cast<int64_t>(0.010 * rate);
  j.heldDb = dbOf(rmsOf(y, at - heldSpan, at));
  j.tailDb = dbOf(rmsOf(y, at + span, at + span + w));

  const int64_t step = std::max<int64_t>(1, w / 8);
  for (int64_t c = at; c <= at + span; c += step) {
    const double line = j.heldDb + (j.tailDb - j.heldDb) * static_cast<double>(c - at) / static_cast<double>(span);
    const double level = dbOf(rmsOf(y, c - w / 2, c - w / 2 + w));
    j.bumpDb = std::max(j.bumpDb, std::fabs(level - line));
    j.jumpDb = std::max(j.jumpDb, level - j.heldDb);
  }

  // The second difference: a sine's grows with its frequency squared, so a
  // step or a corner at the joint stands far above the pipe's own sound.
  auto d2 = [&](int64_t from, int64_t to) {
    from = std::max<int64_t>(2, from);
    to = std::min<int64_t>(static_cast<int64_t>(y.size()), to);
    if (to <= from) return 0.0;
    double acc = 0.0;
    for (int64_t i = from; i < to; ++i) {
      const double d = static_cast<double>(y[static_cast<size_t>(i)]) - 2.0 * y[static_cast<size_t>(i - 1)] +
                       y[static_cast<size_t>(i - 2)];
      acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(to - from));
  };
  const double held = d2(at - heldSpan, at);
  const int64_t ms = std::max<int64_t>(8, static_cast<int64_t>(0.001 * rate));
  double worst = 0.0;
  for (int64_t c = at - ms; c < at + span; c += ms / 2) worst = std::max(worst, d2(c, c + ms));
  j.clickDb = dbOf(worst) - dbOf(held);
  return j;
}

inline Summary summarise(std::vector<Joint> joints) {
  Summary s;
  s.count = static_cast<int>(joints.size());
  if (joints.empty()) return s;
  std::vector<double> bumps, jumps;
  for (const auto& j : joints) {
    bumps.push_back(j.bumpDb);
    jumps.push_back(j.jumpDb);
    s.worstClickDb = std::max(s.worstClickDb, j.clickDb);
  }
  std::sort(jumps.begin(), jumps.end());
  s.worstJumpDb = jumps.back();
  s.medianJumpDb = jumps[jumps.size() / 2];
  std::sort(bumps.begin(), bumps.end());
  s.worstBumpDb = bumps.back();
  s.medianBumpDb = bumps[bumps.size() / 2];
  s.spreadDb = bumps.back() - bumps.front();
  return s;
}

}  // namespace mp::bump
