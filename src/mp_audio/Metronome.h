// Practice metronome.
//
// Deliberately part of the engine rather than a separate app: it has to come
// out of the same audio callback as the organ, or it drifts against what the
// player hears. Synthesised rather than sampled so it costs nothing to ship
// and stays audible against a full organ.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

namespace mp {

class Metronome {
public:
  void prepare(double sampleRate);
  void reset();

  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  void setTempo(double bpm);
  double tempo() const { return bpm_; }
  // Beats per bar; the first beat of each bar is accented. 0 disables the
  // accent, which is what a player counting in free time wants.
  void setBeatsPerBar(int beats) { beatsPerBar_ = beats < 0 ? 0 : beats; }
  int beatsPerBar() const { return beatsPerBar_; }
  void setLevel(float linear) { level_ = juce::jlimit(0.0f, 1.0f, linear); }
  float level() const { return level_; }

  // Which beat of the bar is next, for a UI readout. 1-based; 0 when off.
  int currentBeat() const { return enabled_ ? beat_ + 1 : 0; }

  // Mix clicks into the block. Additive, like every other voice: the organ is
  // already in the buffer.
  void process(juce::AudioBuffer<float>& buffer);

private:
  double sampleRate_ = 48000.0;
  double bpm_ = 90.0;
  double samplesPerBeat_ = 32000.0;
  double counter_ = 0.0;   // samples until the next click
  int beatsPerBar_ = 4;
  int beat_ = 0;
  bool enabled_ = false;
  float level_ = 0.5f;

  // Click envelope state. A short decaying sine: a click that is too short
  // vanishes under a full organ, and one that rings competes with it.
  double clickPhase_ = 0.0;
  double clickInc_ = 0.0;
  float clickEnv_ = 0.0f;
  float clickDecay_ = 0.0f;
};

} // namespace mp
