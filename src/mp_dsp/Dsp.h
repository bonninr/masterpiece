// Full DSP (ADR-005): wind solver + tremulant + enclosure + voicing.
//
// DSP always ships. It is disabled two ways, neither of which is "absent":
//   - config time: MP_ENABLE_DSP=OFF drops the whole path for slow machines,
//   - run time:    EngineSwitch (simpleWavOnly / enableWindModel / ... ) takes
//                  the fast path per feature, per organ, while the engine runs.
// Every kernel below must honour the runtime switches in its own hot path.
//
// JUCE (ADR-012): the filters are JUCE's, not hand-rolled. The enclosure is a
// juce::dsp::StateVariableTPTFilter — a topology-preserving state-variable
// filter, which is the right structure when the cutoff is swept continuously
// by the swell shoe (a direct-form biquad zippers and can go unstable when its
// coefficients move that fast). Hot kernels vectorise through juce::dsp; the
// tremulant reads a juce::dsp::LookupTableTransform rather than calling
// std::sin per sample. Because this TU includes juce_dsp, mp_dsp is built and
// tested in the full CMake build (RunPod / CI), not in the JUCE-free WSL fast
// loop, which now covers mp_core / mp_sampler / mp_control only.
//
#pragma once
#include "../mp_core/OrganModel.h"

#include <juce_dsp/juce_dsp.h>

namespace mp::dsp {

// The wind solver moved to mp_control/WindSolver.h. It turned out to be
// control-rate state rather than an audio process — it advances pressures once
// a block and never touches a sample — so it belongs where the JUCE-free fast
// loop can test it.

class TremulantLfo {
public:
  void prepare(const juce::dsp::ProcessSpec& spec);
  void reset(double sampleRate); // convenience wrapper over prepare()

  // Waveform-sampled when the ODF provides a TremulantWaveform (M3), else the
  // band-limited sine table below. Rate morphs engaged<->disengaged; depth
  // envelopes in and out over the start/stop % ramps.
  float nextSample(const Tremulant& t, bool engaged, const EngineSwitch& sw);
  // Jump straight to the steady state for `engaged` (registration recall, or
  // starting a render with the tremulant already drawn).
  void snapTo(const Tremulant& t, bool engaged);

  float currentDepth() const { return depthEnv_.getCurrentValue(); }
  double currentRateHz() const { return rateHz_.getCurrentValue(); }

private:
  static float depthRampSeconds(double percent);

  double sampleRate_ = 48000.0;
  double phase_ = 0.0; // radians, wrapped to [0, 2pi)
  juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> rateHz_{6.0};
  juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> depthEnv_{0.0f};
  double lastStartPercent_ = -1.0, lastStopPercent_ = -1.0;
  bool lastEngaged_ = false;
};

// Swell-shade filter. Cutoff morphs geometrically and gain linearly in dB
// between the enclosure's closed and open settings, driven by a smoothed
// shutter position so there is no zipper noise.
class EnclosureFilter {
public:
  void prepare(const juce::dsp::ProcessSpec& spec);
  void reset(double sampleRate); // convenience wrapper over prepare()

  // shutter01: 0 = fully closed, 1 = fully open.
  float process(float x, const Enclosure& e, double shutter01, const EngineSwitch& sw);
  // Block form — this is the path the audio graph should call. Processes in
  // place; honours the same runtime switches as process().
  void processBlock(juce::dsp::AudioBlock<float> block, const Enclosure& e,
                    double shutter01, const EngineSwitch& sw);
  // Jump the smoother without a ramp (used when recalling a registration).
  void snapTo(double shutter01);

  double smoothedShutter() const { return smoothShutter_.getCurrentValue(); }
  double cutoffHz() const { return cutoffHz_; }
  float gainLinear() const { return gain_.getCurrentValue(); }

private:
  // Recompute cutoff/gain from the smoothed shutter. Cheap enough to run once
  // per block; the filter itself interpolates within the block.
  void updateFor(const Enclosure& e);

  juce::dsp::StateVariableTPTFilter<float> svf_;
  juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> smoothShutter_{1.0};
  juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> gain_{1.0f};
  double sampleRate_ = 48000.0;
  double cutoffHz_ = 0.0;
};

struct VoicingOverride {
  double gainDb = 0.0;
  double tuningCents = 0.0;
  double brightnessDb = 0.0; // HarmonicShaping / VoicingEQ01 simplified in M1
  double stereoBalance = 0.0;
};

} // namespace mp::dsp
