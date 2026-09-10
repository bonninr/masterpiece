#include "Convolver.h"

namespace mp {

void Convolver::prepare(const juce::dsp::ProcessSpec& spec) {
  convolution_.prepare(spec);
  // The dry path has to be kept whole so the mix is a real crossfade rather
  // than "wet plus whatever survived".
  dry_.setSize(static_cast<int>(spec.numChannels),
               static_cast<int>(spec.maximumBlockSize), false, true, true);
  prepared_ = true;
}

void Convolver::reset() { convolution_.reset(); }

bool Convolver::loadImpulseResponse(const juce::File& irFile) {
  if (!irFile.existsAsFile()) return false;

  convolution_.loadImpulseResponse(
      irFile,
      // Keep the IR's own sample rate handling to JUCE; resampling a room
      // impulse badly is audible as a change of room size.
      juce::dsp::Convolution::Stereo::yes,
      juce::dsp::Convolution::Trim::yes,
      0, // 0 = use the whole file
      juce::dsp::Convolution::Normalise::yes);

  irName_ = irFile.getFileNameWithoutExtension();
  loaded_ = true;
  return true;
}

void Convolver::clear() {
  convolution_.reset();
  loaded_ = false;
  irName_ = {};
}

void Convolver::process(juce::AudioBuffer<float>& buffer) {
  if (!enabled_ || !loaded_ || !prepared_) return;

  const int numCh = buffer.getNumChannels();
  const int numSamples = buffer.getNumSamples();
  if (numCh <= 0 || numSamples <= 0) return;
  if (mix_ <= 0.0f) return;

  // Hold the dry signal aside. dry_ was sized at prepare(); a host handing us
  // a larger block than it promised falls back to bypass rather than
  // allocating on the audio thread.
  if (dry_.getNumChannels() < numCh || dry_.getNumSamples() < numSamples)
    return;
  for (int ch = 0; ch < numCh; ++ch)
    dry_.copyFrom(ch, 0, buffer, ch, 0, numSamples);

  juce::dsp::AudioBlock<float> block(buffer);
  juce::dsp::ProcessContextReplacing<float> ctx(block);
  convolution_.process(ctx);

  // Equal-gain crossfade. The wet signal is the same material through a room,
  // so it correlates with the dry: equal-power would push the level up.
  const float wet = mix_;
  const float dryGain = 1.0f - mix_;
  for (int ch = 0; ch < numCh; ++ch) {
    float* out = buffer.getWritePointer(ch);
    const float* d = dry_.getReadPointer(ch);
    for (int i = 0; i < numSamples; ++i)
      out[i] = out[i] * wet + d[i] * dryGain;
  }
}

} // namespace mp
