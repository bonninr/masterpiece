// WavPack, as a JUCE audio format.
//
// GrandOrgue sets often ship their samples compressed with WavPack, and name
// the files .wav regardless -- GrandOrgue looks at the content, not the name.
// This reader does the same: it recognises a stream by its "wvpk" signature
// and leaves anything else to the formats after it, so it is registered
// FIRST, ahead of JUCE's own WAV reader.
//
// A WavPack file keeps the WAV header it was made from, chunks and all. The
// smpl and cue chunks in it -- the sustain loops, the release marker and the
// recorded pitch the engine needs -- are exposed in metadataValues under the
// same names JUCE's WAV reader uses, so nothing downstream can tell the two
// apart.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace mp {

class WavPackAudioFormat : public juce::AudioFormat {
public:
  WavPackAudioFormat();

  juce::Array<int> getPossibleSampleRates() override { return {}; }
  juce::Array<int> getPossibleBitDepths() override { return {16, 24, 32}; }
  bool canDoStereo() override { return true; }
  bool canDoMono() override { return true; }
  bool isCompressed() override { return true; }

  juce::AudioFormatReader* createReaderFor(juce::InputStream* source,
                                           bool deleteStreamIfOpeningFails) override;

  std::unique_ptr<juce::AudioFormatWriter> createWriterFor(
      std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&) override {
    return nullptr;
  }
  using juce::AudioFormat::createWriterFor;
};

// The formats a sample set can use, WavPack first. Every place that opens a
// sample goes through this rather than registerBasicFormats().
void registerSampleFormats(juce::AudioFormatManager& formats);

}  // namespace mp
