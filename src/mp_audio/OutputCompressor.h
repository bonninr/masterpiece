// A compressor on the final output, ahead of the limiter.
//
// It brings the loud passages down towards the quiet ones, and its make-up
// gain lifts the whole back up, so a small loudspeaker can be played louder
// without a full organ running into the limiter. The two channels of a pair
// are linked, as in the limiter: the louder side sets the gain and both get
// it, so the stereo image stays where it was.
//
// The level it listens to can be high-passed. The pedal carries most of an
// organ's energy, and a compressor that heard it would turn the manuals down
// every time a 32' spoke; with the detector filtered, the bass still sounds
// in full and only what the filter lets through sets the gain.
//
// Off by default. While it is off the processor does not call it at all, and
// the output is bit for bit what the engine made. The program runs it; a
// plugin leaves the output to the host's own effects.
#pragma once

#include <array>
#include <atomic>

namespace mp {

class OutputCompressor {
public:
  // The most pairs it handles, as for the limiter.
  static constexpr int kMaxPairs = 16;

  // The width of the soft knee, in dB, centred on the threshold.
  static constexpr float kKneeDb = 6.0f;

  // The level the compression begins at, in dBFS: -40 to 0.
  void setThresholdDb(float db);
  float thresholdDb() const { return thresholdDb_.load(std::memory_order_relaxed); }

  // How much a level over the threshold is held down: 1.5:1 to 10:1.
  void setRatio(float ratio);
  float ratio() const { return ratio_.load(std::memory_order_relaxed); }

  // How long the gain takes to come down, in ms: 1 to 100.
  void setAttackMs(float ms);
  float attackMs() const { return attackMs_.load(std::memory_order_relaxed); }

  // How long the gain takes to come back, in ms: 50 to 1000.
  void setReleaseMs(float ms);
  float releaseMs() const { return releaseMs_.load(std::memory_order_relaxed); }

  // The gain added after the compression, in dB: 0 to +18.
  void setMakeupDb(float db);
  float makeupDb() const { return makeupDb_.load(std::memory_order_relaxed); }

  // The detector's high-pass, in Hz: 0 for none, otherwise 80, 120 or 200.
  // Only the level the compressor listens to is filtered; the sound is not.
  void setDetectorHighPassHz(int hz);
  int detectorHighPassHz() const { return highPassHz_.load(std::memory_order_relaxed); }
  static constexpr std::array<int, 4> kHighPassChoices{0, 80, 120, 200};

  // Whether it is in use. The audio thread skips the call altogether while
  // it is off. Switching it on starts from no reduction and an empty filter.
  void setEnabled(bool on) {
    if (enabled_.exchange(on, std::memory_order_relaxed) == on) return;
    if (on) resetRequested_.store(true, std::memory_order_relaxed);
  }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  void prepare(double sampleRate);
  void reset();

  // Compresses a block in place, in pairs of channels: 0 and 1, 2 and 3, and
  // so on; a lone channel at the end is handled on its own.
  void process(float* const* channels, int numChannels, int numFrames);

  // The reduction the last block ended on, in dB, for a readout. The
  // make-up gain is not counted.
  float gainReductionDb() const {
    return lastReductionDb_.load(std::memory_order_relaxed);
  }

  // How many blocks it has processed. It stays at zero while the processor
  // leaves it alone.
  unsigned long long blocksProcessed() const {
    return blocksProcessed_.load(std::memory_order_relaxed);
  }

private:
  // A second-order high-pass, one per channel of a pair.
  struct HighPass {
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
  };
  struct Coefficients {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  };
  struct Pair {
    float envelope = 0.0f;  // the detector's level, linear
    float reductionDb = 0.0f;
    HighPass left, right;
  };
  struct Block {
    float thresholdDb, slope, attack, release, makeup;
    bool filtered;
    Coefficients hp;
  };

  static float filter(HighPass& s, const Coefficients& c, float x);
  static void processPair(Pair& pair, float* left, float* right, int numFrames,
                          const Block& b);

  std::atomic<float> thresholdDb_{-18.0f};
  std::atomic<float> ratio_{3.0f};
  std::atomic<float> attackMs_{10.0f};
  std::atomic<float> releaseMs_{200.0f};
  std::atomic<float> makeupDb_{0.0f};
  std::atomic<int> highPassHz_{120};
  std::atomic<bool> enabled_{false};
  std::atomic<bool> resetRequested_{false};
  std::atomic<float> lastReductionDb_{0.0f};
  std::atomic<unsigned long long> blocksProcessed_{0};
  std::array<Pair, kMaxPairs> pairs_{};
  double sampleRate_ = 48000.0;
};

} // namespace mp
