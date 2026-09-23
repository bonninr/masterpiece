#include "Convolver.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace mp {

void Convolver::prepare(const juce::dsp::ProcessSpec& spec) {
  fromLeft_.prepare(spec);
  fromRight_.prepare(spec);
  sampleRate_ = spec.sampleRate;
  // The dry path has to be kept whole so the mix is a real crossfade rather
  // than "wet plus whatever survived". The second buffer carries the right
  // input through its own engine for a true-stereo IR.
  dry_.setSize(static_cast<int>(spec.numChannels),
               static_cast<int>(spec.maximumBlockSize), false, true, true);
  right_.setSize(2, static_cast<int>(spec.maximumBlockSize), false, true, true);
  prepared_ = true;
}

void Convolver::reset() {
  fromLeft_.reset();
  fromRight_.reset();
}

juce::File Convolver::fileForRate(const juce::File& irFile, double rate) {
  if (rate <= 0.0) return irFile;
  const juce::String name = irFile.getFileNameWithoutExtension();
  // "<room>-48000Hz": replace the rate, keep everything before it.
  const int dash = name.lastIndexOfChar('-');
  if (dash < 0 || !name.endsWithIgnoreCase("Hz")) return irFile;
  const juce::String stem = name.substring(0, dash);
  const auto sibling = irFile.getSiblingFile(
      stem + "-" + juce::String(juce::roundToInt(rate)) + "Hz" +
      irFile.getFileExtension());
  return sibling.existsAsFile() ? sibling : irFile;
}

bool Convolver::loadImpulseResponse(const juce::File& chosen) {
  if (!chosen.existsAsFile()) return false;
  const juce::File irFile = fileForRate(chosen, sampleRate_);

  juce::AudioFormatManager formats;
  formats.registerBasicFormats();
  std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(irFile));
  if (reader == nullptr || reader->lengthInSamples <= 0) return false;

  const int channels = static_cast<int>(reader->numChannels);
  const int length = static_cast<int>(reader->lengthInSamples);
  juce::AudioBuffer<float> all(channels, length);
  reader->read(&all, 0, length, 0, true, true);

  // One gain for every path, so a true-stereo room keeps its own balance
  // between them; normalising each engine separately would not. By energy,
  // not by peak: a room's loudness is the whole of its tail, and a two-second
  // tail normalised to its peak came out more than 20 dB too hot. The scale
  // is the one JUCE's own normalisation uses, so a room sits where it did.
  double energy = 0.0;
  for (int ch = 0; ch < channels; ++ch) {
    const float* p = all.getReadPointer(ch);
    double e = 0.0;
    for (int i = 0; i < length; ++i) e += static_cast<double>(p[i]) * p[i];
    energy = juce::jmax(energy, e);
  }
  const float gain = energy > 0.0 ? static_cast<float>(0.125 / std::sqrt(energy)) : 1.0f;

  auto pair = [&](int a, int b) {
    juce::AudioBuffer<float> ir(2, length);
    ir.copyFrom(0, 0, all, juce::jmin(a, channels - 1), 0, length);
    ir.copyFrom(1, 0, all, juce::jmin(b, channels - 1), 0, length);
    ir.applyGain(gain);
    return ir;
  };

  // Four channels is true stereo, in Hauptwerk's order: left input to the
  // left and right outputs, then right input to the left and right outputs.
  // Two is an ordinary stereo IR; one is mono, used for both sides.
  trueStereo_ = channels >= 4;
  const double rate = reader->sampleRate;
  using C = juce::dsp::Convolution;
  if (trueStereo_) {
    fromLeft_.loadImpulseResponse(pair(0, 1), rate, C::Stereo::yes, C::Trim::yes,
                                  C::Normalise::no);
    fromRight_.loadImpulseResponse(pair(2, 3), rate, C::Stereo::yes, C::Trim::yes,
                                   C::Normalise::no);
  } else {
    fromLeft_.loadImpulseResponse(pair(0, channels > 1 ? 1 : 0), rate, C::Stereo::yes,
                                  C::Trim::yes, C::Normalise::no);
  }

  irName_ = chosen.getFileNameWithoutExtension();
  loaded_ = true;
  return true;
}

void Convolver::clear() {
  fromLeft_.reset();
  fromRight_.reset();
  loaded_ = false;
  trueStereo_ = false;
  irName_ = {};
}

void Convolver::process(juce::AudioBuffer<float>& buffer) {
  if (!enabled_ || !loaded_ || !prepared_) return;

  const int numCh = buffer.getNumChannels();
  const int numSamples = buffer.getNumSamples();
  if (numCh <= 0 || numSamples <= 0) return;
  if (mix_ <= 0.0f) return;

  // Hold the dry signal aside. The buffers were sized at prepare(); a host
  // handing us a larger block than it promised falls back to bypass rather
  // than allocating on the audio thread.
  if (dry_.getNumChannels() < numCh || dry_.getNumSamples() < numSamples ||
      right_.getNumSamples() < numSamples)
    return;
  for (int ch = 0; ch < numCh; ++ch)
    dry_.copyFrom(ch, 0, buffer, ch, 0, numSamples);

  if (trueStereo_ && numCh >= 2) {
    // The right input, doubled, through its own engine gives right-to-left
    // and right-to-right. The left input, doubled, through the other gives
    // left-to-left and left-to-right. Their sum is the room.
    right_.copyFrom(0, 0, buffer, 1, 0, numSamples);
    right_.copyFrom(1, 0, buffer, 1, 0, numSamples);
    buffer.copyFrom(1, 0, buffer, 0, 0, numSamples);

    juce::dsp::AudioBlock<float> leftBlock(buffer.getArrayOfWritePointers(), 2,
                                           static_cast<size_t>(numSamples));
    fromLeft_.process(juce::dsp::ProcessContextReplacing<float>(leftBlock));
    juce::dsp::AudioBlock<float> rightBlock(right_.getArrayOfWritePointers(), 2,
                                            static_cast<size_t>(numSamples));
    fromRight_.process(juce::dsp::ProcessContextReplacing<float>(rightBlock));
    buffer.addFrom(0, 0, right_, 0, 0, numSamples);
    buffer.addFrom(1, 0, right_, 1, 0, numSamples);
  } else {
    juce::dsp::AudioBlock<float> block(buffer);
    fromLeft_.process(juce::dsp::ProcessContextReplacing<float>(block));
  }

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
