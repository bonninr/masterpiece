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
  // One biquad per channel: a shelf holds state, and two channels sharing it
  // would leak one into the other.
  const size_t channels = std::max<size_t>(2, spec.numChannels);
  shelf_.resize(channels);
  for (auto& f : shelf_) {
    f.prepare(spec);
    f.reset();
  }
  smoothShutter_.reset(sampleRate_, kShutterRampSeconds);
  smoothShutter_.setCurrentAndTargetValue(1.0);
  gain_.reset(sampleRate_, kShutterRampSeconds);
  gain_.setCurrentAndTargetValue(1.0f);
  cutoffHz_ = 0.0;
  shelfDb_ = -1.0;   // forces the first updateFor to place the filter
  shelfLowPass_ = false;
}

void EnclosureFilter::reset(double sampleRate) {
  prepare({sampleRate, 512u, 1u});
}

void EnclosureFilter::snapTo(double shutter01) {
  smoothShutter_.setCurrentAndTargetValue(clamp01(shutter01));
}

EnclosureFilter::Shelf EnclosureFilter::keyOf(double maxHz, double minHz) {
  // Guard the logs — a malformed ODF can carry a zero or negative frequency —
  // and keep both ends inside hearing, where the format's figures live.
  const double hi = std::clamp(maxHz > 0.0 ? maxHz : kOpenEnclosureHz, 20.0, 20000.0);
  const double lo = std::clamp(minHz > 0.0 ? minHz : hi, 20.0, 20000.0);
  // A MinFreqHz above MaxFreqHz is the format's low-pass: the loudest point
  // is the lower one and the response falls towards the higher. Below it,
  // a high-pass. The two frequencies span the shelf's transition, so its
  // corner sits between them on the octave scale, where they are stated.
  return Shelf{0.5 * (std::log2(hi) + std::log2(lo)), lo > hi};
}

void EnclosureFilter::updateFor(const Enclosure& e) {
  const double s = clamp01(smoothShutter_.getCurrentValue());

  // The shelf's corner moves geometrically between the closed and open
  // figures: shades are heard in octaves, not hertz, so a linear sweep would
  // spend most of its travel doing nothing audible. The shape is the closed
  // end's: a box that dulls the treble closed does not go the other way part
  // way open.
  const Shelf closed = keyOf(e.closedMaxHz, e.closedMinHz);
  const Shelf open = keyOf(e.openMaxHz, e.openMinHz);
  const double octaves = closed.octaves + s * (open.octaves - closed.octaves);
  cutoffHz_ = std::clamp(std::exp2(octaves), 20.0, sampleRate_ * 0.45);

  // Gain interpolates linearly in dB — that is how the attenuations are
  // specified, and it is what the ear tracks.
  const double db = e.closedAttnDb + s * (e.openAttnDb - e.closedAttnDb);
  gain_.setTargetValue(juce::Decibels::decibelsToGain(static_cast<float>(db), -100.0f));

  // The shelf's depth, from the set's own figure, shallower as the shades
  // open and gone when open: the shape the format describes, and an open box
  // filtering nothing (#205).
  const double depthDb = std::max(0.0, e.closedShelfDb) * (1.0 - s);

  // Rebuilt only when something moved: four biquads a block cost nothing,
  // but a player holding the shoe still is the common case.
  if (std::abs(depthDb - shelfDb_) < 0.01 && closed.lowPass == shelfLowPass_ &&
      std::abs(cutoffHz_ - shelfHz_) < 1.0)
    return;
  shelfDb_ = depthDb;
  shelfLowPass_ = closed.lowPass;
  shelfHz_ = cutoffHz_;

  const float depthGain =
      juce::Decibels::decibelsToGain(static_cast<float>(-depthDb), -100.0f);
  const auto hz = static_cast<float>(cutoffHz_);
  const auto q = static_cast<float>(juce::MathConstants<double>::sqrt2 / 2.0);
  for (auto& f : shelf_) {
    // A shelf that cuts the treble is JUCE's high shelf — it passes the lows
    // and takes the highs down — and the format's high-pass is its low shelf.
    if (shelfLowPass_)
      f.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
          sampleRate_, hz, q, depthGain);
    else
      f.coefficients = juce::dsp::IIR::Coefficients<float>::makeLowShelf(
          sampleRate_, hz, q, depthGain);
  }
}

float EnclosureFilter::process(float x, const Enclosure& e, double shutter01,
                               const EngineSwitch& sw) {
  if (sw.simpleWavOnly || !sw.enableEnclosure) return x;

  smoothShutter_.setTargetValue(clamp01(shutter01));
  smoothShutter_.getNextValue();
  updateFor(e);
  return shelf_[0].processSample(x) * gain_.getNextValue();
}

void EnclosureFilter::processBlock(juce::dsp::AudioBlock<float> block,
                                   const Enclosure& e, double shutter01,
                                   const EngineSwitch& sw) {
  if (sw.simpleWavOnly || !sw.enableEnclosure) return;

  const auto numSamples = static_cast<int>(block.getNumSamples());
  if (numSamples <= 0) return;

  // Advance the shutter smoother across the block and set the filter from the
  // block's end position: the biquad and the gain smoother each interpolate
  // within the block, so per-sample coefficient recalculation buys nothing.
  smoothShutter_.setTargetValue(clamp01(shutter01));
  smoothShutter_.skip(numSamples);
  updateFor(e);

  const auto numCh = block.getNumChannels();
  if (numCh > shelf_.size()) {
    shelf_.resize(numCh);
    for (auto& f : shelf_) {
      f.prepare({sampleRate_, 512u, static_cast<juce::uint32>(numCh)});
      f.reset();
    }
  }
  for (int i = 0; i < numSamples; ++i) {
    const float g = gain_.getNextValue();
    for (size_t ch = 0; ch < numCh; ++ch)
      block.setSample(static_cast<int>(ch), i,
                      shelf_[ch].processSample(
                          block.getSample(static_cast<int>(ch), i)) * g);
  }
  for (auto& f : shelf_) f.snapToZero();
}


} // namespace mp::dsp
