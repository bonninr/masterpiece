#include "Dsp.h"

#include <algorithm>
#include <cmath>

namespace mp::dsp {
namespace {

// Shared band-limited sine table for every tremulant LFO. Built once; reading
// it is a lerp instead of a std::sin call per sample per tremulant.
const juce::dsp::LookupTableTransform<float>& sineTable() {
  static const juce::dsp::LookupTableTransform<float> table(
      [](float x) { return std::sin(x); }, 0.0f,
      juce::MathConstants<float>::twoPi, 2048);
  return table;
}

constexpr double kTwoPi = juce::MathConstants<double>::twoPi;

// Shutter inertia: how long the shades take to follow the shoe. Slow enough to
// kill zipper noise, fast enough that the swell still feels immediate.
constexpr double kShutterRampSeconds = 0.05;
// Tremulant rate glide when the stop is drawn or cancelled.
constexpr double kRateRampSeconds = 0.25;

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

} // namespace

// ------------------------------------------------------------ tremulant

// How long the tremulant takes to reach full depth, from the ODF's start/stop
// rate percentages. Hauptwerk states them as a RATE — a bigger percentage
// means the tremulant arrives sooner — so this is an inverse, not a scale.
// 100% is as fast as a tremulant plausibly starts; the floor keeps a file that
// says 0 from asking for an instantaneous jump, which clicks.
float TremulantLfo::depthRampSeconds(double percent) {
  constexpr double kFastest = 0.05; // seconds, at 100%
  constexpr double kSlowest = 1.0;  // seconds, at nothing at all
  const double p = std::clamp(percent, 0.0, 100.0) / 100.0;
  return static_cast<float>(kSlowest + (kFastest - kSlowest) * p);
}

void TremulantLfo::prepare(const juce::dsp::ProcessSpec& spec) {
  sampleRate_ = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
  phase_ = 0.0;
  rateHz_.reset(sampleRate_, kRateRampSeconds);
  rateHz_.setCurrentAndTargetValue(6.0);
  depthEnv_.reset(sampleRate_, 0.03);
  depthEnv_.setCurrentAndTargetValue(0.0f);
  lastStartPercent_ = lastStopPercent_ = -1.0;
  lastEngaged_ = false;
  sineTable(); // build the shared table off the audio thread
}

void TremulantLfo::reset(double sampleRate) {
  prepare({sampleRate, 512u, 1u});
}

void TremulantLfo::snapTo(const Tremulant& t, bool engaged) {
  rateHz_.setCurrentAndTargetValue(engaged ? t.engagedHz : t.disengagedHz);
  depthEnv_.setCurrentAndTargetValue(engaged ? 1.0f : 0.0f);
  lastEngaged_ = engaged;
}

float TremulantLfo::nextSample(const Tremulant& t, bool engaged,
                               const EngineSwitch& sw) {
  if (sw.simpleWavOnly || !sw.enableTremulant) return 0.0f;

  // Re-arm the ramps only when the request or the file's ramp settings change;
  // SmoothedValue does the per-sample work.
  if (engaged != lastEngaged_ || t.startPercent != lastStartPercent_ ||
      t.stopPercent != lastStopPercent_) {
    rateHz_.setTargetValue(engaged ? t.engagedHz : t.disengagedHz);
    depthEnv_.reset(sampleRate_,
                    depthRampSeconds(engaged ? t.startPercent : t.stopPercent));
    depthEnv_.setTargetValue(engaged ? 1.0f : 0.0f);
    lastEngaged_ = engaged;
    lastStartPercent_ = t.startPercent;
    lastStopPercent_ = t.stopPercent;
  }

  // A rate of zero or a nonsense rate from a malformed ODF must not spin the
  // phase accumulator out of range; clamp to something an organ can produce.
  const double hz = std::clamp(rateHz_.getNextValue(), 0.0, 40.0);
  const float depth = std::clamp(depthEnv_.getNextValue(), 0.0f, 1.0f);

  phase_ += kTwoPi * hz / sampleRate_;
  if (phase_ >= kTwoPi) phase_ -= kTwoPi * std::floor(phase_ / kTwoPi);

  // M3: when t.hasWaveform, read the sampled TremulantWaveform (t.waveformId)
  // here instead of the sine table.
  return depth * sineTable()(static_cast<float>(phase_));
}

// ----------------------------------------------------------- enclosure

void EnclosureFilter::prepare(const juce::dsp::ProcessSpec& spec) {
  sampleRate_ = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
  svf_.prepare(spec);
  svf_.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
  svf_.setResonance(juce::MathConstants<float>::sqrt2 / 2.0f); // Butterworth
  svf_.setCutoffFrequency(static_cast<float>(
      std::clamp(12000.0, 20.0, sampleRate_ * 0.45)));
  svf_.reset();
  smoothShutter_.reset(sampleRate_, kShutterRampSeconds);
  smoothShutter_.setCurrentAndTargetValue(1.0);
  gain_.reset(sampleRate_, kShutterRampSeconds);
  gain_.setCurrentAndTargetValue(1.0f);
  cutoffHz_ = 0.0;
}

void EnclosureFilter::reset(double sampleRate) {
  prepare({sampleRate, 512u, 1u});
}

void EnclosureFilter::snapTo(double shutter01) {
  smoothShutter_.setCurrentAndTargetValue(clamp01(shutter01));
}

void EnclosureFilter::updateFor(const Enclosure& e) {
  const double s = clamp01(smoothShutter_.getCurrentValue());

  // Cutoff interpolates geometrically: shades are heard in octaves, not hertz,
  // so a linear sweep would spend most of its travel doing nothing audible.
  // Guard the logs — a malformed ODF can carry a zero or negative frequency.
  const double closedHz = e.closedFilterHz > 0.0 ? e.closedFilterHz : 20.0;
  const double openHz = e.openFilterHz > 0.0 ? e.openFilterHz : 20000.0;
  const double hz = std::exp(std::log(closedHz) + s * (std::log(openHz) - std::log(closedHz)));
  cutoffHz_ = std::clamp(hz, 20.0, sampleRate_ * 0.45);
  svf_.setCutoffFrequency(static_cast<float>(cutoffHz_));

  // Gain interpolates linearly in dB — that is how the attenuations are
  // specified, and it is what the ear tracks.
  const double db = e.closedAttnDb + s * (e.openAttnDb - e.closedAttnDb);
  gain_.setTargetValue(juce::Decibels::decibelsToGain(static_cast<float>(db), -100.0f));
}

float EnclosureFilter::process(float x, const Enclosure& e, double shutter01,
                               const EngineSwitch& sw) {
  if (sw.simpleWavOnly || !sw.enableEnclosure) return x;

  smoothShutter_.setTargetValue(clamp01(shutter01));
  smoothShutter_.getNextValue();
  updateFor(e);
  return svf_.processSample(0, x) * gain_.getNextValue();
}

void EnclosureFilter::processBlock(juce::dsp::AudioBlock<float> block,
                                   const Enclosure& e, double shutter01,
                                   const EngineSwitch& sw) {
  if (sw.simpleWavOnly || !sw.enableEnclosure) return;

  const auto numSamples = static_cast<int>(block.getNumSamples());
  if (numSamples <= 0) return;

  // Advance the shutter smoother across the block and set the filter from the
  // block's end position: the SVF and the gain smoother each interpolate
  // internally, so per-sample coefficient recalculation buys nothing.
  smoothShutter_.setTargetValue(clamp01(shutter01));
  smoothShutter_.skip(numSamples);
  updateFor(e);

  juce::dsp::ProcessContextReplacing<float> ctx(block);
  svf_.process(ctx);

  const auto numCh = block.getNumChannels();
  for (int i = 0; i < numSamples; ++i) {
    const float g = gain_.getNextValue();
    for (size_t ch = 0; ch < numCh; ++ch)
      block.setSample(static_cast<int>(ch), i,
                      block.getSample(static_cast<int>(ch), i) * g);
  }
}

} // namespace mp::dsp
