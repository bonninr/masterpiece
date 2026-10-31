#include "OutputCompressor.h"

#include <algorithm>
#include <cmath>

namespace mp {
namespace {

constexpr double kPi = 3.14159265358979323846;

// How far a level is held down, in dB, before the make-up gain. The knee is
// a parabola in dB, as in the limiter: nothing below its floor, the full
// slope above its ceiling, a smooth curve in between.
float reductionFor(float levelDb, float thresholdDb, float slope) {
  const float knee = OutputCompressor::kKneeDb;
  const float over = levelDb - thresholdDb;
  if (over <= -knee * 0.5f) return 0.0f;
  if (over >= knee * 0.5f) return over * slope;
  const float x = over + knee * 0.5f;
  return slope * x * x / (2.0f * knee);
}

// The share of the distance to a new level left after one sample.
float coefficient(float ms, double sampleRate) {
  const double tau = std::max(0.1, static_cast<double>(ms)) / 1000.0;
  return static_cast<float>(std::exp(-1.0 / (tau * sampleRate)));
}

}  // namespace

void OutputCompressor::setThresholdDb(float db) {
  thresholdDb_.store(std::clamp(db, -40.0f, 0.0f), std::memory_order_relaxed);
}

void OutputCompressor::setRatio(float ratio) {
  ratio_.store(std::clamp(ratio, 1.5f, 10.0f), std::memory_order_relaxed);
}

void OutputCompressor::setAttackMs(float ms) {
  attackMs_.store(std::clamp(ms, 1.0f, 100.0f), std::memory_order_relaxed);
}

void OutputCompressor::setReleaseMs(float ms) {
  releaseMs_.store(std::clamp(ms, 50.0f, 1000.0f), std::memory_order_relaxed);
}

void OutputCompressor::setMakeupDb(float db) {
  makeupDb_.store(std::clamp(db, 0.0f, 18.0f), std::memory_order_relaxed);
}

void OutputCompressor::setDetectorHighPassHz(int hz) {
  // The nearest of the choices, so a hand-edited settings file still lands
  // on one the window can show.
  int best = kHighPassChoices[0];
  for (int c : kHighPassChoices)
    if (std::abs(c - hz) < std::abs(best - hz)) best = c;
  highPassHz_.store(best, std::memory_order_relaxed);
}

void OutputCompressor::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  reset();
}

void OutputCompressor::reset() {
  for (auto& pair : pairs_) pair = Pair{};
  lastReductionDb_.store(0.0f, std::memory_order_relaxed);
}

float OutputCompressor::filter(HighPass& s, const Coefficients& c, float x) {
  const float y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
  s.x2 = s.x1;
  s.x1 = x;
  s.y2 = s.y1;
  s.y1 = y;
  return y;
}

void OutputCompressor::processPair(Pair& pair, float* left, float* right, int numFrames,
                                   const Block& b) {
  for (int i = 0; i < numFrames; ++i) {
    // What the detector hears: the louder side, filtered if asked.
    float l = left[i];
    float r = right != nullptr ? right[i] : 0.0f;
    if (b.filtered) {
      l = filter(pair.left, b.hp, l);
      if (right != nullptr) r = filter(pair.right, b.hp, r);
    }
    const float level = std::max(std::fabs(l), std::fabs(r));

    // A peak follower: it rises at the attack rate and falls at the release.
    const float k = level > pair.envelope ? b.attack : b.release;
    pair.envelope = level + k * (pair.envelope - level);

    float gain = b.makeup;
    if (pair.envelope > 1.0e-6f) {
      const float levelDb = 20.0f * std::log10(pair.envelope);
      pair.reductionDb = reductionFor(levelDb, b.thresholdDb, b.slope);
      if (pair.reductionDb > 0.0f) gain *= std::pow(10.0f, -pair.reductionDb / 20.0f);
    } else {
      pair.reductionDb = 0.0f;
    }

    left[i] *= gain;
    if (right != nullptr) right[i] *= gain;
  }
}

void OutputCompressor::process(float* const* channels, int numChannels, int numFrames) {
  if (!enabled() || channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
  if (resetRequested_.exchange(false, std::memory_order_relaxed)) reset();

  // Everything a sample needs, worked out once a block.
  Block b{};
  b.thresholdDb = thresholdDb_.load(std::memory_order_relaxed);
  b.slope = 1.0f - 1.0f / ratio_.load(std::memory_order_relaxed);
  b.attack = coefficient(attackMs_.load(std::memory_order_relaxed), sampleRate_);
  b.release = coefficient(releaseMs_.load(std::memory_order_relaxed), sampleRate_);
  b.makeup = std::pow(10.0f, makeupDb_.load(std::memory_order_relaxed) / 20.0f);
  const int hz = highPassHz_.load(std::memory_order_relaxed);
  b.filtered = hz > 0;
  if (b.filtered) {
    // A Butterworth high-pass, from the bilinear transform.
    const double w = 2.0 * kPi * hz / sampleRate_;
    const double cosw = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * std::sqrt(0.5));
    const double a0 = 1.0 + alpha;
    b.hp.b0 = static_cast<float>((1.0 + cosw) / 2.0 / a0);
    b.hp.b1 = static_cast<float>(-(1.0 + cosw) / a0);
    b.hp.b2 = b.hp.b0;
    b.hp.a1 = static_cast<float>(-2.0 * cosw / a0);
    b.hp.a2 = static_cast<float>((1.0 - alpha) / a0);
  }

  float worst = 0.0f;
  int index = 0;
  for (int ch = 0; ch < numChannels && index < kMaxPairs; ch += 2) {
    float* left = channels[ch];
    float* right = ch + 1 < numChannels ? channels[ch + 1] : nullptr;
    auto& pair = pairs_[static_cast<size_t>(index)];
    processPair(pair, left, right, numFrames, b);
    worst = std::max(worst, pair.reductionDb);
    ++index;
  }
  lastReductionDb_.store(worst, std::memory_order_relaxed);
  blocksProcessed_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace mp
