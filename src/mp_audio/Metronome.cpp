#include "Metronome.h"

#include <algorithm>
#include <cmath>

namespace mp {
namespace {
// An accented downbeat an octave up reads as "one" without needing to be
// louder, which matters when the organ is already at full tutti.
constexpr double kBeatHz = 1000.0;
constexpr double kAccentHz = 1500.0;
constexpr double kClickSeconds = 0.035;

// A struck click: three partials, each with its own decay time (seconds to
// fall to 1/e), over a knock of noise. Partials a struck bar's inharmonic
// ratios apart read as wood; a high, very short group as the dry tick of a
// pendulum's escapement; a long low partial with a high one at 2.7 times its
// frequency as its bell.
struct PartialSpec {
  double hz;
  float amp;
  double tau;
};
struct StruckSpec {
  PartialSpec partials[3];
  float noise;
  double noiseTau;
  float gain;
};
constexpr StruckSpec kWood{{{1150.0, 1.0f, 0.018}, {2950.0, 0.45f, 0.009}, {5300.0, 0.2f, 0.004}}, 0.5f, 0.0012, 0.6f};
constexpr StruckSpec kWoodAccent{
    {{1550.0, 1.0f, 0.020}, {3980.0, 0.45f, 0.010}, {7150.0, 0.2f, 0.004}}, 0.5f, 0.0012, 0.85f};
constexpr StruckSpec kTick{{{2900.0, 1.0f, 0.005}, {4700.0, 0.6f, 0.0035}, {7300.0, 0.35f, 0.0025}}, 0.9f, 0.0008, 0.6f};
constexpr StruckSpec kBell{{{2093.0, 1.0f, 0.12}, {5650.0, 0.35f, 0.06}, {2900.0, 0.8f, 0.005}}, 0.9f, 0.0008, 0.7f};
// A partial is silent below 1/2000 of its start: ln 2000 decay times.
constexpr double kDecayTimesToSilence = 7.6;
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
  for (auto& p : partials_) p = Partial{};
  noiseEnv_ = 0.0f;
  struckLeft_ = 0;
}

void Metronome::startStruck(bool accent) {
  const StruckSpec& spec = clickSound_ == static_cast<int>(Sound::pendulum) ? (accent ? kBell : kTick)
                                                                           : (accent ? kWoodAccent : kWood);
  double longest = spec.noiseTau;
  for (size_t k = 0; k < partials_.size(); ++k) {
    const auto& ps = spec.partials[k];
    auto& p = partials_[k];
    p.phase = 0.0;
    p.inc = 2.0 * juce::MathConstants<double>::pi * ps.hz / sampleRate_;
    p.env = ps.amp * spec.gain;
    p.decay = static_cast<float>(std::exp(-1.0 / (ps.tau * sampleRate_)));
    longest = std::max(longest, ps.tau);
  }
  noiseEnv_ = spec.noise * spec.gain;
  noiseDecay_ = static_cast<float>(std::exp(-1.0 / (spec.noiseTau * sampleRate_)));
  struckLeft_ = static_cast<int>(std::ceil(longest * kDecayTimesToSilence * sampleRate_));
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
      clickSound_ = sound_.load(std::memory_order_relaxed);
      if (clickSound_ == static_cast<int>(Sound::beep)) {
        clickInc_ = 2.0 * juce::MathConstants<double>::pi *
                    (accent ? kAccentHz : kBeatHz) / sampleRate_;
        clickPhase_ = 0.0;
        clickEnv_ = accent ? 1.0f : 0.7f;
        struckLeft_ = 0;
      } else {
        clickEnv_ = 0.0f;
        startStruck(accent);
      }
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

    if (struckLeft_ > 0) {
      --struckLeft_;
      float s = 0.0f;
      for (auto& p : partials_) {
        s += static_cast<float>(std::sin(p.phase)) * p.env;
        p.phase += p.inc;
        p.env *= p.decay;
      }
      // A fixed-seed generator: the same clicks every run.
      noiseState_ = noiseState_ * 1664525u + 1013904223u;
      s += static_cast<float>(static_cast<std::int32_t>(noiseState_)) * (1.0f / 2147483648.0f) * noiseEnv_;
      noiseEnv_ *= noiseDecay_;
      s *= level_;
      for (int ch = 0; ch < numCh; ++ch)
        buffer.getWritePointer(ch)[i] += s;
    }
  }
}

} // namespace mp
