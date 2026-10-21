#include "OutputLimiter.h"

#include <algorithm>
#include <cmath>

namespace mp {
namespace {

// How much a peak of this level has to come down, in dB. The knee is a
// parabola in dB: nothing at all below its floor, the whole amount above its
// ceiling, and a smooth curve in between. A level just under the ceiling is
// therefore barely touched and one just over it is barely held down.
float reductionFor(float peak, float ceilingDb) {
  const float levelDb = 20.0f * std::log10(peak);
  const float knee = OutputLimiter::kKneeDb;
  if (levelDb >= ceilingDb + knee * 0.5f) return levelDb - ceilingDb;
  const float over = levelDb - ceilingDb + knee * 0.5f;
  return over * over / (2.0f * knee);
}

}  // namespace

void OutputLimiter::setCeilingDb(float db) {
  ceilingDb_.store(std::clamp(db, -12.0f, -0.1f), std::memory_order_relaxed);
}

void OutputLimiter::setReleaseMs(float ms) {
  releaseMs_.store(std::clamp(ms, 50.0f, 1000.0f), std::memory_order_relaxed);
}

void OutputLimiter::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  reset();
}

void OutputLimiter::reset() {
  for (auto& pair : pairs_) {
    pair.reductionDb = 0.0f;
    pair.gain = 1.0f;
  }
  lastReductionDb_.store(0.0f, std::memory_order_relaxed);
}

void OutputLimiter::processPair(Pair& pair, float* left, float* right,
                                 int numFrames, float ceilingDb, float kneeStart,
                                 float decay) {
  for (int i = 0; i < numFrames; ++i) {
    // The two sides are linked: the louder of them sets the gain.
    float peak = std::fabs(left[i]);
    if (right != nullptr) peak = std::max(peak, std::fabs(right[i]));

    if (peak > kneeStart) {
      const float wanted = reductionFor(peak, ceilingDb);
      if (wanted > pair.reductionDb) {
        // The attack is instant: the gain a sample asks for is the gain it
        // gets, so nothing ever comes out above the ceiling.
        pair.reductionDb = wanted;
      } else {
        // The gain comes back at the release rate, but never below what the
        // sample in hand asks for.
        pair.reductionDb *= decay;
        if (pair.reductionDb < wanted) pair.reductionDb = wanted;
      }
    } else if (pair.reductionDb > 0.0f) {
      // Below the knee nothing is asked for, and the gain comes back.
      pair.reductionDb *= decay;
    }

    if (pair.reductionDb > 1.0e-4f) {
      pair.gain = std::pow(10.0f, -pair.reductionDb / 20.0f);
    } else {
      // Nothing left to hold down: the gain is exactly one and the samples
      // come out as they went in.
      pair.reductionDb = 0.0f;
      pair.gain = 1.0f;
    }

    left[i] *= pair.gain;
    if (right != nullptr) right[i] *= pair.gain;
  }
}

void OutputLimiter::process(float* const* channels, int numChannels, int numFrames) {
  if (!enabled() || channels == nullptr || numChannels <= 0 || numFrames <= 0) return;

  // The limiter was switched on: the gain starts at unity, with no
  // reduction left over from the last time it was in use. The reset is done
  // here, on the thread that owns the state.
  if (resetRequested_.exchange(false, std::memory_order_relaxed)) reset();

  const float ceilingDb = ceilingDb_.load(std::memory_order_relaxed);
  const float releaseMs = releaseMs_.load(std::memory_order_relaxed);
  // The level at which the knee begins, and the share the reduction loses in
  // one sample. Both are worked out once a block, not once a sample.
  const float kneeStart = std::pow(10.0f, (ceilingDb - kKneeDb * 0.5f) / 20.0f);
  const double tau = std::max(1.0, static_cast<double>(releaseMs)) / 1000.0;
  const float decay = static_cast<float>(std::exp(-1.0 / (tau * sampleRate_)));

  float worst = 0.0f;
  int index = 0;
  for (int ch = 0; ch < numChannels && index < kMaxPairs; ch += 2) {
    float* left = channels[ch];
    float* right = ch + 1 < numChannels ? channels[ch + 1] : nullptr;
    processPair(pairs_[static_cast<size_t>(index)], left, right, numFrames,
                ceilingDb, kneeStart, decay);
    worst = std::max(worst, pairs_[static_cast<size_t>(index)].reductionDb);
    ++index;
  }
  lastReductionDb_.store(worst, std::memory_order_relaxed);
  blocksProcessed_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace mp
