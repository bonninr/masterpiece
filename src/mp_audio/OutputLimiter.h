// A peak limiter on the final output.
//
// It sits after the master fader and before the device, so it holds the
// level that leaves without touching the balance of the organ. The two
// channels of a pair are linked: the louder side sets the gain and both get
// it, so a loud left channel does not leave the right one alone.
//
// No look-ahead. The gain that holds a sample down is the one that sample
// itself asks for, so nothing is delayed and what comes out is the input
// turned down. A soft knee rounds the corner where the limiting begins, so
// a level just under the ceiling is barely touched.
//
// Off by default. While it is off the processor does not call it at all, and
// the output is bit for bit what the engine made.
#pragma once

#include <array>
#include <atomic>

namespace mp {

class OutputLimiter {
public:
  // The most pairs the limiter handles: sixteen, which is as many output
  // channels as the bus can have.
  static constexpr int kMaxPairs = 16;

  // The width of the soft knee, in dB. The limiting begins this far below
  // the ceiling and grows out of nothing to the full amount over it.
  static constexpr float kKneeDb = 3.0f;

  // The ceiling, in dBFS: -0.1 to -12.
  void setCeilingDb(float db);
  float ceilingDb() const { return ceilingDb_.load(std::memory_order_relaxed); }

  // How long the gain takes to come back after a peak, in ms: 50 to 1000.
  void setReleaseMs(float ms);
  float releaseMs() const { return releaseMs_.load(std::memory_order_relaxed); }

  // Whether the limiter is in use. The audio thread reads this and skips
  // the call altogether while it is off. Switching it on starts the gain
  // from unity, so no reduction is left over from the last time it was on.
  void setEnabled(bool on) {
    if (enabled_.exchange(on, std::memory_order_relaxed) == on) return;
    if (on) resetRequested_.store(true, std::memory_order_relaxed);
  }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // The rate the release is worked out at.
  void prepare(double sampleRate);
  void reset();

  // Limits a block in place. The channels are handled in pairs: 0 and 1,
  // 2 and 3, and so on; a lone channel at the end is handled on its own.
  void process(float* const* channels, int numChannels, int numFrames);

  // The reduction the last block ended on, in dB, for a readout.
  float gainReductionDb() const {
    return lastReductionDb_.load(std::memory_order_relaxed);
  }

  // How many blocks the limiter has processed. It stays at zero while the
  // processor leaves it alone.
  unsigned long long blocksProcessed() const {
    return blocksProcessed_.load(std::memory_order_relaxed);
  }

private:
  struct Pair {
    float reductionDb = 0.0f;  // the gain reduction in force, in dB
    float gain = 1.0f;          // the same thing in linear terms
  };

  // One pair of channels, over one block.
  static void processPair(Pair& pair, float* left, float* right, int numFrames,
                           float ceilingDb, float kneeStart, float decay);

  std::atomic<float> ceilingDb_{-1.0f};
  std::atomic<float> releaseMs_{100.0f};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> resetRequested_{false};
  std::atomic<float> lastReductionDb_{0.0f};
  std::atomic<unsigned long long> blocksProcessed_{0};
  std::array<Pair, kMaxPairs> pairs_{};
  double sampleRate_ = 48000.0;
};

} // namespace mp
