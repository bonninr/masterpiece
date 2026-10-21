// Bass management for one output pair.
//
// A crossover splits each channel of the pair: the highs stay on the pair,
// and the lows of both channels, summed, go to a subwoofer channel. The
// crossover is Linkwitz-Riley of the 4th order, whose two halves add up to
// an allpass: the pair and the subwoofer together are flat in level at
// every frequency, the crossover's included.
//
// Off by default. While it is off the processor does not call it, and the
// pair is what the engine made.
#pragma once

#include <juce_dsp/juce_dsp.h>

namespace mp {

class BassManager {
public:
  static constexpr float kMinHz = 40.0f;
  static constexpr float kMaxHz = 200.0f;

  // Message thread, before the audio runs: the filter's state is made here.
  void prepare(double sampleRate, int maxBlock);
  // The crossover frequency, clamped to 40-200 Hz; 0 turns it off. The
  // filter's state starts again from rest when the frequency changes.
  void setCrossoverHz(float hz);
  float crossoverHz() const { return hz_; }
  bool active() const { return hz_ > 0.0f; }

  // Audio thread. `left` and `right` keep their highs, in place; the lows of
  // both are added into `sub`, which may be the same buffer for several
  // pairs. A null `right` is a mono pair.
  void process(float* left, float* right, float* sub, int numFrames);

private:
  juce::dsp::LinkwitzRileyFilter<float> filter_;
  double rate_ = 48000.0;
  float hz_ = 0.0f;
};

}  // namespace mp
