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

private:
  juce::dsp::Convolution convolution_;
  juce::AudioBuffer<float> dry_;
  bool enabled_ = false;
  bool loaded_ = false;
  bool prepared_ = false;
  float mix_ = 0.25f;
  juce::String irName_;
};

} // namespace mp
