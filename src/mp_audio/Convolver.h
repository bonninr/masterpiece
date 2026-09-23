// Impulse-response reverb (ADR-012 / M4.1).
//
// A dry organ recorded close-up needs a room. Hauptwerk sets often ship
// several perspectives (close / far / rear) which ARE the room, but a set that
// only offers a dry perspective — or a player using headphones — wants
// convolution, and it is the one effect where writing our own would be
// obviously worse than JUCE's: juce::dsp::Convolution is a partitioned
// uniform/non-uniform engine that hand-rolling would take weeks to match.
//
// Loading is deferred to a background thread by JUCE itself, so calling
// loadImpulseResponse() does not stall the audio thread.
#pragma once
#include <juce_dsp/juce_dsp.h>

#include <juce_core/juce_core.h>

namespace mp {

class Convolver {
public:
  void prepare(const juce::dsp::ProcessSpec& spec);
  void reset();

  // Load an IR from a WAV/AIFF file. Returns false only when the file cannot
  // be seen at all; JUCE reports decode problems asynchronously, so a
  // successful return means "accepted", not "already in effect".
  bool loadImpulseResponse(const juce::File& irFile);
  void clear();

  bool hasImpulseResponse() const { return loaded_; }
  juce::String impulseResponseName() const { return irName_; }

  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }
  // Wet/dry as a fraction. Organs want far less than a typical reverb plugin:
  // the samples usually carry their own room already.
  void setMix(float wet01) { mix_ = juce::jlimit(0.0f, 1.0f, wet01); }
  float mix() const { return mix_; }

  // Wet/dry mixed in place. Does nothing when disabled or with no IR loaded,
  // so it is safe to call unconditionally from processBlock.
  void process(juce::AudioBuffer<float>& buffer);

  // A Hauptwerk impulse-response package ships the same room once per
  // sample rate, as "<name>-44100Hz.wav", "-48000Hz.wav" and so on. Given any
  // one of them, the sibling recorded at `rate`, or the file itself if there
  // is none. Resampling a room is audible as a change of its size.
  static juce::File fileForRate(const juce::File& irFile, double rate);

private:
  // Non-uniform partitioning: a short head at the audio block size keeps the
  // reverb at zero latency, and the long tail is done in large partitions.
  // Uniform partitioning at an ASIO-sized block (32 or 64 samples) spent
  // most of a core on a two-second room, and live that is a crackle --
  // reported as "just scratching sound" in #29.
  static constexpr int kHeadSize = 256;
  // Two engines, for a true-stereo IR: one carries the left input to both
  // outputs, the other the right. A two-channel IR uses only the first.
  juce::dsp::Convolution fromLeft_{juce::dsp::Convolution::NonUniform{kHeadSize}};
  juce::dsp::Convolution fromRight_{juce::dsp::Convolution::NonUniform{kHeadSize}};
  bool trueStereo_ = false;
  double sampleRate_ = 0.0;
  juce::AudioBuffer<float> right_;
  juce::AudioBuffer<float> dry_;
  bool enabled_ = false;
  bool loaded_ = false;
  bool prepared_ = false;
  float mix_ = 0.25f;
  juce::String irName_;
};

} // namespace mp
