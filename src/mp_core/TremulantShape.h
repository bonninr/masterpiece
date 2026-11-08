#pragma once
// One cycle of a tremulant's recorded waveform, as two tables over the
// tremulant's phase (#805).
//
// A set that records its tremulants gives each pipe, or each group of pipes,
// a file: channel 0 is the pitch's deviation as a fraction of its frequency,
// channel 1 the fundamental's level, looped over whole cycles. The tremulant's
// own rate still sets the speed; the file sets the shape of each cycle, how
// far the pitch swings, and how deep one pipe's level swings beside another's.
//
// JUCE-free: the file is read by mp_audio, the shape is made here.

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mp {

struct TremulantShape {
  static constexpr int kPoints = 256;
  // The pitch's swing, in units of kTremulantWaveformPitchCents, mean zero:
  // a pipe's pitch depth at 100 % (tremulantPitchSwingSemitones) gives the
  // file's own cents.
  std::array<float, kPoints + 1> pitch{};
  // The level's swing, mean zero. As read it is the file's own fraction;
  // normaliseTremulantShapes() scales the organ's shapes together.
  std::array<float, kPoints + 1> amp{};
  // The largest swing the file holds, before any capping or scaling.
  double pitchPeakCents = 0.0;
  double ampPeak = 0.0;
  int cyclesInLoop = 0;
  // A pitch swing past kMaxPitchCents is not a tremulant's; such a file is
  // played with its shape at the standard depth.
  bool pitchCapped = false;
  static constexpr double kMaxPitchCents = 50.0;

  // phase01: the tremulant's phase, one cycle per unit. Linear between points.
  float pitchAt(double phase01) const { return at(pitch, phase01); }
  float ampAt(double phase01) const { return at(amp, phase01); }

 private:
  static float at(const std::array<float, kPoints + 1>& t, double phase01) {
    double x = (phase01 - std::floor(phase01)) * kPoints;
    if (!(x >= 0.0)) x = 0.0;
    int i = static_cast<int>(x);
    if (i >= kPoints) i = kPoints - 1;
    const float f = static_cast<float>(x - static_cast<double>(i));
    return t[static_cast<size_t>(i)] + (t[static_cast<size_t>(i) + 1] - t[static_cast<size_t>(i)]) * f;
  }
};

// The shape of the loop [loopStart, loopEnd) of a waveform file. `pitch` is
// channel 0, `level` channel 1 (null when the file has one channel). The
// cycles in the loop are counted from the strongest of its first eight
// harmonics and averaged into one; the table's phase is counted from the
// file's start, so files that start together stay together. False when the
// loop is too short to hold a cycle or nothing in it moves.
bool buildTremulantShape(const float* pitch, const float* level, size_t frames,
                         size_t loopStart, size_t loopEnd, TremulantShape& out);

// Scale the level of every shape of one organ so that the median of their
// peaks is 1: the organ's typical waveform then swings by the calibrated
// depth (kTremulantWaveformAmpDepth), and every other waveform by its own
// depth beside it. A level peak is capped at 4. Where no file moves in level,
// each shape's level follows its pitch, as the sine does.
void normaliseTremulantShapes(std::vector<TremulantShape>& shapes);

}  // namespace mp
