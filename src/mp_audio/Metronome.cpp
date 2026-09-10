#include "Metronome.h"

#include <cmath>

namespace mp {
namespace {
// An accented downbeat an octave up reads as "one" without needing to be
// louder, which matters when the organ is already at full tutti.
constexpr double kBeatHz = 1000.0;
constexpr double kAccentHz = 1500.0;
constexpr double kClickSeconds = 0.035;
} // namespace

void Metronome::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  setTempo(bpm_);
  reset();
}

void Metronome::reset() {
  counter_ = 0.0; // click on the very next sample, so starting feels immediate
  beat_ = 0;
  clickEnv_ = 0.0f;
  clickPhase_ = 0.0;
}

void Metronome::setTempo(double bpm) {
  bpm_ = juce::jlimit(20.0, 300.0, bpm);
  samplesPerBeat_ = (60.0 / bpm_) * sampleRate_;
}

void Metronome::process(juce::AudioBuffer<float>& buffer) {
  if (!enabled_) return;

  const int numSamples = buffer.getNumSamples();
  const int numCh = buffer.getNumChannels();
  if (numSamples <= 0 || numCh <= 0) return;

  // Decay per sample for a fixed click length, independent of sample rate.
  clickDecay_ = static_cast<float>(
      std::exp(-1.0 / (kClickSeconds * sampleRate_ * 0.25)));

  for (int i = 0; i < numSamples; ++i) {
    if (counter_ <= 0.0) {
      const bool accent = beatsPerBar_ > 0 && beat_ == 0;
      clickInc_ = 2.0 * juce::MathConstants<double>::pi *
                  (accent ? kAccentHz : kBeatHz) / sampleRate_;
      clickPhase_ = 0.0;
      clickEnv_ = accent ? 1.0f : 0.7f;
      counter_ += samplesPerBeat_;
      if (beatsPerBar_ > 0) beat_ = (beat_ + 1) % beatsPerBar_;
    }
    counter_ -= 1.0;

    if (clickEnv_ > 0.0005f) {
      const float s =
          static_cast<float>(std::sin(clickPhase_)) * clickEnv_ * level_;
      clickPhase_ += clickInc_;
      clickEnv_ *= clickDecay_;
      for (int ch = 0; ch < numCh; ++ch)
        buffer.getWritePointer(ch)[i] += s;
    }
  }
}

} // namespace mp
