#include "MasterpieceProcessor.h"
#include "../mp_archive/OrganArchive.h"

#include "../mp_core/GrandOrgueImport.h"
#include "../mp_core/Temperament.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace mp {

static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
  // Shoes/controls automatable (M3); M1: master gain + simple-wav toggle.
  // 0.9 clipped: a 15-stop tutti on a real set peaked at +0.3 dBFS. An organ
  // is meant to be quiet on one stop and overwhelming on full organ, so the
  // dynamic range is correct — what was missing is headroom for the top of it.
  // This is a provisional calibration: proper gain staging (per-rank levels
  // from the ODF, bus trims, a limiter on the master) is M4 mixer work.
  p.push_back(std::make_unique<juce::AudioParameterFloat>("masterGain", "Master Gain",
      // Up to +24 dB. A sample set is recorded at the level the recordist
      // chose, and a quiet one with a few stops drawn can sit 30 dB below a
      // tutti — so the fader has to be able to bring that up, not merely trim
      // a loud one down. The UI drives this in decibels, which is the only
      // scale on which a volume control feels linear.
      // Starts at unity rather than the 0.35 (-9 dB) it used to, which had
      // the organ sounding timid the first time anyone pressed a key. Unity
      // is what the recordist's own level gives, and the fader reaches +24 dB
      // above it for a quiet set.
      juce::NormalisableRange<float>(0.0f, 16.0f, 0.0001f), 1.0f));
  p.push_back(std::make_unique<juce::AudioParameterBool>("simpleWavOnly", "Simple WAV (no DSP)", false));
  return { p.begin(), p.end() };
}

MasterpieceProcessor::MasterpieceProcessor()
  : juce::AudioProcessor(juce::AudioProcessor::BusesProperties()
      .withOutput("Out", juce::AudioChannelSet::stereo(), true)),
    apvts_(*this, nullptr, "MP", makeLayout()) {
  // Before any organ is opened: a streamed set keeps many files open, and the
  // default allowance on macOS is 256.
  static const size_t tails = SampleLibrary::raiseOpenFileLimit();
  (void)tails;
}

void MasterpieceProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  maxBlock_ = juce::jmax(1, samplesPerBlock);
#if MP_ENABLE_DSP
  // One filter per enclosure and one LFO per tremulant, built here so the
  // audio thread never allocates. getTotalNumOutputChannels() is the widest
  // block we will be handed.
  const juce::dsp::ProcessSpec spec{
      sampleRate_, static_cast<juce::uint32>(juce::jmax(1, samplesPerBlock)),
      static_cast<juce::uint32>(juce::jmax(1, getTotalNumOutputChannels()))};

  enclosureFilters_.clear();
  busEnclosures_.clear();
  enclosureBusIndex_.clear();
  // Stable bus order: enclosure ids ascending, so a reload of the same organ
  // produces the same layout and a saved registration still lines up.
  for (const auto& [id, e] : model_.enclosures) {
    (void)e;
    busEnclosures_.push_back(id);
  }
  std::sort(busEnclosures_.begin(), busEnclosures_.end());
  for (size_t i = 0; i < busEnclosures_.size(); ++i) {
    const Id id = busEnclosures_[i];
    enclosureBusIndex_[id] = static_cast<int>(i);
    auto& f = enclosureFilters_[id];
    f.prepare(spec);
    // Start where the shoe actually is, so loading an organ with the swell
    // open does not ramp audibly through the first block.
    f.snapTo(controls_.shutterFor(model_.enclosures.at(id)));
  }
  // Everything no box encloses goes to one extra bus that no filter touches.
  unenclosedBus_ = static_cast<int>(busEnclosures_.size());

  busScratch_.setSize(juce::jmax(1, getTotalNumOutputChannels()),
                      juce::jmax(1, samplesPerBlock), false, true, false);

  tremulantLfos_.clear();
  tremOrder_.clear();
  tremIndexOf_.clear();
  for (const auto& [id, t] : model_.tremulants) {
    (void)t;
    tremulantLfos_[id].reset(sampleRate_);
    tremOrder_.push_back(id);
  }
  std::sort(tremOrder_.begin(), tremOrder_.end());
  for (size_t i = 0; i < tremOrder_.size(); ++i)
    tremIndexOf_[tremOrder_[i]] = static_cast<int>(i);
  tremMods_.assign(tremOrder_.size(), VoiceEngine::TremMod{});

  wind_.reset(model_);
  convolver_.prepare(spec);
#else
  (void)samplesPerBlock;
#endif
  // Resolve the organ's tuning once. An unresolved temperament leaves this
  // empty, which the solver reads as equal — the loader has already warned.
  organTuning_ = Temperament{};
  const auto tIt = model_.temperaments.find(model_.defaultTemperamentId);
  if (tIt != model_.temperaments.end() && tIt->second.resolved) {
    organTuning_.name = tIt->second.name;
    organTuning_.centsOffset12 = tIt->second.centsOffset12;
  }
  setTemperament(temperamentChoice_, nullptr, /*remember=*/false);

  // The producer's output trim, resolved once here rather than per block.
  // Clamped because this multiplies everything the organ makes and a corrupt
  // field should not be able to deafen anyone: +/-24 dB is far wider than any
  // real set declares (we have seen -4 to +2) and still finite.
  refreshMixerBuses();

  organTrimGain_ =
      applyOrganTrim_
          ? static_cast<float>(juce::Decibels::decibelsToGain(
                juce::jlimit(-24.0, 24.0, model_.audioOutputTrimDb)))
          : 1.0f;

  // Index the noise ranks by their trigger switch. Doing this once here keeps
  // a switch flip O(number of noises on that switch) instead of O(all ranks).
  noiseRanksBySwitch_.clear();
  for (const auto& [rankId, rank] : model_.ranks) {
    if (!rank.isNoise || rank.noiseTriggerSwitchId == 0) continue;
    noiseRanksBySwitch_[rank.noiseTriggerSwitchId].push_back(rankId);
  }

  // Voice pool is allocated once, here: render() must never allocate.
  voices_.setSimd(fasterEngine_.load());
  voices_.prepare(sampleRate_, graph_.maxVoices,
                  juce::jmax(1, getTotalNumOutputChannels()),
                  juce::jmax(1, samplesPerBlock));
  // Which path renders, for any report about the sound: the new engine is
  // the first thing to rule in or out.
  juce::Logger::writeToLog(juce::String("audio engine: ") +
                           (voices_.simd() == simd::Isa::None
                                ? "per-frame"
                                : juce::String("faster (") + simd::isaName(voices_.simd()) + ")"));

  // Worker pool (ADR-012). Auto means cores - 1, leaving one for the rest of
  // the system; the audio thread renders a share itself, so the total doing
  // voice work is that count. Threads are started here, never in the callback.
  int threads = graph_.parallel.renderThreads;
  if (threads <= 0) {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    threads = cores > 2 ? cores - 1 : 1;
    // But not every core of a big machine: a block is shared out only above
    // minVoicesPerThread voices per thread, so 23 threads on a 24-thread Mac
    // Pro (#120) wait for over 700 voices before helping, and every worker
    // woken is one more the audio thread waits on. Eight carry any console.
    threads = std::min(threads, 8);
  }
  voices_.setRenderThreads(threads, graph_.parallel.minVoicesPerThread);
  // Worst case one pipe per rank sounding on a single key.
  resolveScratch_.reserve(model_.ranks.empty() ? 64 : model_.ranks.size());
  soundingNotes_.reserve(128);
  // The key-flow walk runs on every note-on and must not allocate: one press
  // on a fully coupled console reaches every division at several pitches.
  keyFlow_.reserve(64);
  // std::max, not juce::jmax: on 64-bit macOS size_t is `unsigned long`,
  // and juce_dsp's jmax overload for SIMDRegister then instantiates
  // SIMDNativeOps<unsigned long>, which the SSE header does not define.
  expandScratch_.reserve(std::max<size_t>(16, model_.divisions.size() * 4));
  metronome_.prepare(sampleRate_);

  // Meter fall: 0.3 s to decay by 1/e. Computed here so the audio thread
  // never calls a transcendental function for the sake of a lamp.
  meterFall_ = sampleRate_ > 0.0
                   ? std::exp(-static_cast<float>(samplesPerBlock) /
                              static_cast<float>(sampleRate_ * 0.3))
                   : 0.5f;
  recorder_.prepare(sampleRate_);
  outgoing_.ensureSize(1024);
}

void MasterpieceProcessor::maybeLoadTick(juce::AudioBuffer<float>& buffer) {
  if (!loadTicks_.load(std::memory_order_acquire)) {
    loadTickLeft_ = 0;
    return;
  }

  // Which percent the load has reached, when that question has an answer.
  // Only the sample phase counts items; earlier phases have no bar to tap
  // along with, and Done is the 100% tap.
  const auto phase = loadProgress_.phase.load(std::memory_order_acquire);
  int pct = -1;
  if (phase == LoadProgress::Phase::Done) {
    pct = 100;
  } else if (phase == LoadProgress::Phase::LoadingSamples) {
    const double f = loadProgress_.fraction();
    if (f >= 0.0) pct = static_cast<int>(f * 100.0);
  } else if (phase == LoadProgress::Phase::Idle) {
    loadTickNext_.store(10, std::memory_order_release);
  }

  const int next = loadTickNext_.load(std::memory_order_acquire);
  const double rate0 = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
  const int numCh0 = buffer.getNumChannels();
  const int numFrames0 = buffer.getNumSamples();
  if (numFrames0 > 0) {
    loadTickCooldown_ -= static_cast<double>(numFrames0) / rate0;
    if (loadTickCooldown_ < 0.0) loadTickCooldown_ = 0.0;
  }
  if (pct >= next && next <= 100) {
    // At most one tap per block, and no faster than one every couple of
    // seconds: a cached load jumps several thresholds in one block and must
    // announce itself once rather than stuttering. Skipped thresholds are
    // still consumed, so they never fire late.
    if (loadTickCooldown_ <= 0.0 && numCh0 > 0) {
      loadTickLeft_ = static_cast<int>(rate0 * 0.03);
      loadTickPhase_ = 0.0;
      loadTickAmp_ = 0.2;
      loadTickCooldown_ = 2.0;
    }
    loadTickNext_.store(next + 10, std::memory_order_release);
  }

  if (loadTickLeft_ <= 0) return;
  if (numCh0 <= 0 || numFrames0 <= 0) return;
  const int n = std::min(loadTickLeft_, numFrames0);
  const double step = 2.0 * 3.141592653589793 * 1760.0 / rate0;
  // Exponential to near-silence across the tap: swift, with no click where it ends.
  const double decay = std::pow(0.001, 1.0 / (rate0 * 0.03));
  for (int i = 0; i < n; ++i) {
    const float s = static_cast<float>(loadTickAmp_ * std::sin(loadTickPhase_));
    for (int ch = 0; ch < numCh0; ++ch)
      buffer.addSample(ch, i, s);
    loadTickPhase_ += step;
    loadTickAmp_ *= decay;
  }
  loadTickLeft_ -= n;
}

void MasterpieceProcessor::refreshMixerBuses() {  mixBusOrder_.clear();
  mixBusIndexOf_.clear();
  for (const auto& b : mixer_.buses) {
    if (b.id.value == 0) continue;
    if (mixBusIndexOf_.count(b.id.value) != 0) continue;
    mixBusIndexOf_[b.id.value] = static_cast<int>(mixBusOrder_.size());
    mixBusOrder_.push_back(b.id);
  }
  // A config with no buses still has to render somewhere. One bus is what the
  // engine did before there was a mixer at all, so that is the fallback.
  if (mixBusOrder_.empty()) {
    mixBusOrder_.push_back(BusId{1});
    mixBusIndexOf_[1] = 0;
  }
  refreshBusReverbs();
}

void MasterpieceProcessor::refreshBusReverbs() {
#if MP_ENABLE_DSP
  // clear + resize, not assign: assign would copy the null unique_ptr.
  busConvolvers_.clear();
  busConvolvers_.resize(mixBusOrder_.size());
  if (sampleRate_ <= 0.0) return;  // not prepared yet; prepareToPlay redoes this

  juce::dsp::ProcessSpec spec;
  spec.sampleRate = sampleRate_;
  spec.maximumBlockSize = static_cast<juce::uint32>(juce::jmax(1, maxBlock_));
  spec.numChannels = 2;

  for (size_t i = 0; i < mixBusOrder_.size(); ++i) {
    const BusReverb* r = mixer_.reverbFor(mixBusOrder_[i]);
    if (r == nullptr || !r->active()) continue;
    const juce::File ir(juce::String(r->irFile));
    if (!ir.existsAsFile()) continue;  // ReverbPanel reports; silence is not a fix
    auto c = std::make_unique<Convolver>();
    c->prepare(spec);
    if (!c->loadImpulseResponse(ir)) continue;
    c->setMix(r->mix);
    c->setEnabled(true);
    busConvolvers_[i] = std::move(c);
  }
#endif
}

int MasterpieceProcessor::mixBusForPipe(Id rankId, int midiNote) const {
  // One bus is the overwhelmingly common case and the default: skip the
  // routing lookup entirely rather than pay for it on every voice start.
  if (mixBusOrder_.size() <= 1) return 0;

  const RankRouting routing = mixer_.routingFor(rankId);
  const auto& primary = routing.perspectives[0];
  BusId dest{0};
  if (std::holds_alternative<BusId>(primary.dest)) {
    dest = std::get<BusId>(primary.dest);
  } else if (const BusGroup* g = mixer_.group(std::get<int>(primary.dest))) {
    dest = allocateBus(*g, midiNote, static_cast<int>(rankId),
                       primary.algorithm, primary.noteOffset);
  }

  const auto it = mixBusIndexOf_.find(dest.value);
  // A routing to a bus that no longer exists lands on the first one rather
  // than on silence. The validator reports it as stale; going quiet here
  // would make a deleted bus look like a broken engine.
  return it == mixBusIndexOf_.end() ? 0 : it->second;
}

int MasterpieceProcessor::busForPipe(Id pipeId) const {
  const auto encIt = model_.pipeEnclosure.find(pipeId);
  if (encIt == model_.pipeEnclosure.end()) return unenclosedBus_;
  const auto busIt = enclosureBusIndex_.find(encIt->second);
  return busIt == enclosureBusIndex_.end() ? unenclosedBus_ : busIt->second;
}

void MasterpieceProcessor::advanceTremulants(int numFrames) {
#if MP_ENABLE_DSP
  // Bypassed wholesale rather than per sample. The LFO already answers zero
  // under these switches, but it was still being asked once per frame per
  // tremulant — a few thousand calls a block to compute nothing, on exactly
  // the machines the switch exists to rescue.
  if (graph_.engineSwitch.simpleWavOnly || !graph_.engineSwitch.enableTremulant) {
    voices_.setTremMods(nullptr, 0);
    return;
  }
  if (tremOrder_.empty() || numFrames <= 0) {
    voices_.setTremMods(nullptr, 0);
    return;
  }

  for (size_t i = 0; i < tremOrder_.size(); ++i) {
    const Id id = tremOrder_[i];
    const auto tIt = model_.tremulants.find(id);
    auto& lfo = tremulantLfos_[id];
    if (tIt == model_.tremulants.end()) {
      tremMods_[i] = VoiceEngine::TremMod{};
      continue;
    }
    const Tremulant& t = tIt->second;
    const bool engaged =
        t.controllingSwitchId != 0 && switchEngaged(t.controllingSwitchId);

    // Run the LFO across the block and take its value at each end. The voice
    // ramps between them, which is what keeps a six hertz wobble smooth at a
    // 256-frame block instead of stepping thirty times a cycle.
    const float start = lfo.nextSample(t, engaged, graph_.engineSwitch);
    float end = start;
    for (int f = 1; f < numFrames; ++f)
      end = lfo.nextSample(t, engaged, graph_.engineSwitch);

    const float step = numFrames > 1
                           ? (end - start) / static_cast<float>(numFrames - 1)
                           : 0.0f;
    tremMods_[i].ampStart = start;
    tremMods_[i].ampStep = step;
    tremMods_[i].pitchStart = start;
    tremMods_[i].pitchStep = step;
  }
  voices_.setTremMods(tremMods_.data(), static_cast<int>(tremMods_.size()));
#else
  (void)numFrames;
  voices_.setTremMods(nullptr, 0);
#endif
}

void MasterpieceProcessor::advanceWind(int numFrames) {
  if (windOrder_.empty()) {
    voices_.setWindMods(nullptr, 0);
    publishWindPressures();
    return;
  }

  // What is drawing air. The engine knows which voices are speaking; the voices
  // carry what their pipes cost.
  windDemand_.assign(windOrder_.size(), 0.0f);
  voices_.gatherWindDemand(windDemand_.data(),
                           static_cast<int>(windDemand_.size()));

  wind_.clearDemand();
  wind_.addDemandDirect(windOrder_, windDemand_);
  wind_.advance(static_cast<double>(numFrames) / sampleRate_,
                graph_.engineSwitch, engagedSwitches_);

  for (size_t i = 0; i < windOrder_.size(); ++i) {
    const auto mod = wind_.modFor(windOrder_[i]);
    // The solver's answer is physical. This scales the DEVIATION from
    // nominal, so depth 1 is exactly what the physics said and nothing is
    // altered by the knob existing. Above 1 is deliberately unphysical: a
    // listening aid for judging whether the effect is there at all, because
    // a real chest sags a few percent and a few percent is hard to hear.
    const double d = windDepth_;
    windMods_[i].ampMul = static_cast<float>(1.0 + (mod.ampMul - 1.0) * d);
    windMods_[i].pitchRatio = 1.0 + (mod.pitchRatio - 1.0) * d;
  }
  voices_.setWindMods(windMods_.data(), static_cast<int>(windMods_.size()));
  publishWindPressures();
}

// Every compartment that says so reports its pressure to a continuous control,
// which is what a console's wind gauges are driven from. The scale is 63.5 at
// zero and half an inch of water per step: that is what the sets' own gauge
// linkages undo, (value - 63.5) * 8 across a needle whose 81 frames run 0 to
// 20 inches. The next block's propagate carries it to the needle.
void MasterpieceProcessor::publishWindPressures() {
  for (const auto& [compartment, control] : windGauges_) {
    const double v = 63.5 + wind_.pressureFor(compartment) * 0.5;
    const int next = static_cast<int>(std::lround(std::clamp(v, 0.0, 127.0)));
    if (controls_.value(control) != next) controls_.setValue(control, next);
  }
}

// One mixer bus, summed ADDITIVELY into `dest`. Does not touch the block
// counter or the wind: the caller owns those, because they happen once per
// block however many buses there are.
//
// `mixBusFilter` < 0 means every voice, which is the single-bus default and
// costs nothing — the filter is not even consulted.
void MasterpieceProcessor::renderOneMixBus(juce::AudioBuffer<float>& dest,
                                           int mixBusFilter) {
  const int numCh = dest.getNumChannels();
  const int numFrames = dest.getNumSamples();
  if (numCh <= 0 || numFrames <= 0) return;

  const bool enclosuresActive =
#if MP_ENABLE_DSP
      !graph_.engineSwitch.simpleWavOnly && graph_.engineSwitch.enableEnclosure;
#else
      false;
#endif

  // Without expression there is nothing to separate: render every voice at
  // once and skip the per-enclosure scratch entirely. The same for a block
  // bigger than the host prepared us for, which the scratch cannot hold: the
  // shades are not applied for that block rather than memory being overrun.
  if (!enclosuresActive || busEnclosures_.empty() ||
      numFrames > busScratch_.getNumSamples() || numCh > busScratch_.getNumChannels()) {
    voices_.render(dest.getArrayOfWritePointers(), numCh, numFrames, -1,
                   mixBusFilter);
    return;
  }

  const int numBuses = unenclosedBus_ + 1;
  for (int bus = 0; bus < numBuses; ++bus) {
    busScratch_.clear(0, numFrames);
    voices_.render(busScratch_.getArrayOfWritePointers(), numCh, numFrames, bus,
                   mixBusFilter);

#if MP_ENABLE_DSP
    if (bus < static_cast<int>(busEnclosures_.size())) {
      const Id encId = busEnclosures_[static_cast<size_t>(bus)];
      const auto filterIt = enclosureFilters_.find(encId);
      const auto encIt = model_.enclosures.find(encId);
      if (filterIt != enclosureFilters_.end() && encIt != model_.enclosures.end()) {
        juce::dsp::AudioBlock<float> block(
            busScratch_.getArrayOfWritePointers(),
            static_cast<size_t>(numCh), static_cast<size_t>(numFrames));
        filterIt->second.processBlock(block, encIt->second,
                                      controls_.shutterFor(encIt->second),
                                      graph_.engineSwitch);
      }
    }
#endif

    for (int ch = 0; ch < numCh; ++ch)
      dest.addFrom(ch, 0, busScratch_, ch, 0, numFrames);
  }
}

void MasterpieceProcessor::renderBuses(juce::AudioBuffer<float>& buffer) {
  const int numCh = buffer.getNumChannels();
  const int numFrames = buffer.getNumSamples();
  if (numCh <= 0 || numFrames <= 0) return;

  // One block for the whole callback, whatever the bus count.
  voices_.beginBlock(numFrames);
  advanceWind(numFrames);
  advanceTremulants(numFrames);

  const int buses = static_cast<int>(mixBusOrder_.size());
  bool anyBusReverb = false;
#if MP_ENABLE_DSP
  if (!graph_.engineSwitch.simpleWavOnly)
    for (const auto& c : busConvolvers_)
      if (c != nullptr) anyBusReverb = true;
#endif

  // The ordinary case, and the default: one mixer bus means no routing axis at
  // all, so nothing is filtered and this is exactly what the engine did before
  // there was a mixer.
  if (buses <= 1 && mixBusCapture_ == nullptr && !anyBusReverb) {
    renderOneMixBus(buffer, -1);
    return;
  }

  // A bus with its own room has to be convolved on its own, which means it
  // needs somewhere of its own to be rendered into. The point of several buses
  // is that they stand in different places — a Positiv on the gallery rail and
  // a Pedal at the back of the case do not share a tail — and one IR over the
  // sum cannot express that.
  if (anyBusReverb && mixBusCapture_ == nullptr) {
    if (mixScratch_.getNumChannels() < numCh ||
        mixScratch_.getNumSamples() < numFrames)
      mixScratch_.setSize(numCh, numFrames, false, false, true);

    for (int i = 0; i < buses; ++i) {
      mixScratch_.clear(0, numFrames);
      renderOneMixBus(mixScratch_, buses <= 1 ? -1 : i);
#if MP_ENABLE_DSP
      if (i < static_cast<int>(busConvolvers_.size()) &&
          busConvolvers_[static_cast<size_t>(i)] != nullptr) {
        juce::AudioBuffer<float> view(mixScratch_.getArrayOfWritePointers(),
                                      numCh, numFrames);
        busConvolvers_[static_cast<size_t>(i)]->process(view);
      }
#endif
      for (int ch = 0; ch < numCh; ++ch)
        buffer.addFrom(ch, 0, mixScratch_, ch, 0, numFrames);
    }
    return;
  }

  // Several buses into one output pair. A player who has configured a mixer
  // but is listening in stereo must still hear the whole organ, so the buses
  // are summed rather than the extra ones dropped.
  for (int i = 0; i < buses; ++i) {
    if (mixBusCapture_ == nullptr) {
      // No capture: add straight into the output, which needs no per-bus
      // memory at all.
      renderOneMixBus(buffer, buses <= 1 ? -1 : i);
      continue;
    }
    if (i >= static_cast<int>(mixBusCapture_->size())) break;
    auto& dest = (*mixBusCapture_)[static_cast<size_t>(i)];
    dest.clear(0, numFrames);
    renderOneMixBus(dest, buses <= 1 ? -1 : i);
#if MP_ENABLE_DSP
    // The bus's own room belongs to the bus signal, so a captured bus carries
    // it. Only the MASTER convolver is downstream of this.
    if (!graph_.engineSwitch.simpleWavOnly &&
        i < static_cast<int>(busConvolvers_.size()) &&
        busConvolvers_[static_cast<size_t>(i)] != nullptr)
      busConvolvers_[static_cast<size_t>(i)]->process(dest);
#endif
    // Summed into the output as well, so capturing does not change what the
    // callback produces.
    for (int ch = 0; ch < numCh && ch < dest.getNumChannels(); ++ch)
      buffer.addFrom(ch, 0, dest, ch, 0, numFrames);
  }
}


void MasterpieceProcessor::handleMidi(const juce::MidiBuffer& midi) {
  // Two sources, one path. The host hands us a merged buffer with no device in
  // it; the per-device callbacks hand us the same messages tagged. Whichever a
  // message arrives by, it is handled identically below — the tag is only ever
  // an extra thing the mapping is allowed to match on.
  // A clock for debouncing. Block-resolution is plenty: contacts chatter over
  // milliseconds and a block is a few.
  blockTimeMs_ += 1000.0 * static_cast<double>(getBlockSize()) /
                  (sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
  midiScratch_.clear();
  for (const auto meta : midi)
    midiScratch_.emplace_back(MidiDeviceMap::kAnyDevice, meta.getMessage());
  drainTaggedMidi(midiScratch_);
  // Console input reaches the recorder here: it is not in the host's buffer,
  // which is all the recorder saw.
  if (recorder_.isRecording())
    for (const auto& [deviceId, msg] : midiScratch_)
      if (deviceId != MidiDeviceMap::kAnyDevice) recorder_.captureLive(msg);

  for (const auto& [deviceId, msg] : midiScratch_) {

    // Mirror device input in the keyboard state, so drawn manuals and the
    // piano strip light up for an external console exactly as they do for
    // file playback. processNextMidiEvent updates state WITHOUT queueing
    // for injection (unlike noteOn), so this cannot retrigger the note on
    // the next block; host-buffer messages already passed through the state
    // fold above and are skipped. A side benefit: Panic now releases
    // externally held notes too, instead of leaving them ciphering.
    if (deviceId != MidiDeviceMap::kAnyDevice)
      keyboardState_.processNextMidiEvent(msg);

    // What kind of message is this, in the terms the map matches on?
    MidiSource source;
    int value = 0;
    if (msg.isNoteOnOrOff()) {
      source.kind = MidiSourceKind::Note;
      source.number = msg.getNoteNumber();
      value = msg.isNoteOn() ? msg.getVelocity() : 0;
    } else if (msg.isController()) {
      source.kind = MidiSourceKind::ControlChange;
      source.number = msg.getControllerNumber();
      value = msg.getControllerValue();
    } else if (msg.isProgramChange()) {
      source.kind = MidiSourceKind::ProgramChange;
      source.number = msg.getProgramChangeNumber();
      value = 127;
    } else if (msg.isSysEx()) {
      // Universal Master Volume, F0 7F device 04 01 lsb msb F7: the volume
      // knob of a control surface such as the SubZero ControlPad (#138). Sets
      // the master fader: full scale is 0 dB, zero is silence, and the steps
      // between are even in dB over the fader's lower 40.
      const uint8_t* d = msg.getSysExData();
      if (msg.getSysExDataSize() == 6 && d[0] == 0x7F && d[2] == 0x04 && d[3] == 0x01) {
        const int v = (d[5] & 0x7F) << 7 | (d[4] & 0x7F);
        const float db = -40.0f * (1.0f - static_cast<float>(v) / 16383.0f);
        const float gain = v == 0 ? 0.0f : juce::Decibels::decibelsToGain(db);
        if (auto* p = apvts_.getParameter("masterGain")) {
          p->setValueNotifyingHost(p->convertTo0to1(gain));
          markMasterGainDirty();
        }
        if (logMidi_.load(std::memory_order_acquire))
          juce::Logger::writeToLog("midi: in  dev=" + juce::String(deviceId) + " master volume " +
                                   juce::String(v) + " -> " +
                                   (v == 0 ? juce::String("silence") : juce::String(db, 1) + " dB"));
        continue;
      }
      // A press each time it arrives: a console piston sent as system
      // exclusive has no release (#138).
      source.kind = MidiSourceKind::SysEx;
      source.number = sysExId(msg.getSysExData(), msg.getSysExDataSize());
      value = 127;
    }
    source.channel = source.kind == MidiSourceKind::SysEx ? 0 : msg.getChannel();
    source.deviceId = deviceId;
    // Where each controller last was, so a learned pedal's ends can be set
    // from its position (#90).
    if (source.kind == MidiSourceKind::ControlChange && source.number >= 0 && source.number < 128) {
      lastController_[static_cast<size_t>(source.number)].store(value + 1, std::memory_order_relaxed);
      if (source.channel >= 1 && source.channel <= 16)
        lastControllerOnChannel_[static_cast<size_t>((source.channel - 1) * 128 + source.number)].store(
            value + 1, std::memory_order_relaxed);
    }

    const bool logging = logMidi_.load(std::memory_order_acquire);
    if (logging && source.kind != MidiSourceKind::None)
      juce::Logger::writeToLog(
          "midi: in  dev=" + juce::String(deviceId) + " ch=" +
          juce::String(source.channel) + " " +
          (source.kind == MidiSourceKind::SysEx
               ? "sysex=" + juce::String::toHexString(msg.getRawData(), msg.getRawDataSize()).toUpperCase() +
                     " id=" + juce::String(source.number)
               : juce::String(msg.isNoteOnOrOff() ? "note" : msg.isController() ? "cc" : "pc") +
                     "=" + juce::String(source.number) + " val=" + juce::String(value)));

    // Listening for the MIDI window takes the press and keeps it; the
    // release that follows is swallowed too, while still listening.
    if (midiListen_.load(std::memory_order_acquire) && source.kind != MidiSourceKind::None) {
      if (value > 0) {
        heard_ = source;
        heardValue_ = value;
        midiListen_.store(false, std::memory_order_release);
        heardReady_.store(true, std::memory_order_release);
      }
      continue;
    }

    // Learning consumes the message: a control being mapped must not also
    // fire whatever it used to do.
    if (midiMap_.learning() && source.kind != MidiSourceKind::None) {
      // Only a press, never a release — otherwise letting go of the key
      // immediately re-learns it to the note-off.
      if (value > 0 && midiMap_.learnFrom(source)) {
        // Learned on the audio thread; written by the message thread.
        midiMapDirty_.store(true, std::memory_order_release);
        continue;
      }
      if (value == 0) continue;
    }

    const MidiAction action = midiMap_.actionFor(source, value);
    if (action.valid() && logging)
      juce::Logger::writeToLog("midi:     consumed by a mapping, kind=" +
                               juce::String(static_cast<int>(action.kind)));
    if (action.valid()) {
      switch (action.kind) {
        case MidiTargetKind::Switch:
          setSwitchEngaged(action.targetId, action.engage);
          continue;
        case MidiTargetKind::ContinuousControl:
          setControlValue(action.targetId, action.value);
          if (action.alsoDrives != nullptr)
            for (const auto& b : *action.alsoDrives)
              setControlValue(b.targetId, MidiMap::controlValue(b, value));
          // A note that sets a swell from its velocity is still a note: a
          // sequencer writes the expression into the notes it plays, and the
          // manual sounds them (#220).
          if (source.kind == MidiSourceKind::Note) break;
          continue;
        case MidiTargetKind::StepperNext:
          stepperNext();
          continue;
        case MidiTargetKind::StepperPrev:
          stepperPrev();
          continue;
        case MidiTargetKind::PlayerGeneral:
          pressGeneral(static_cast<int>(action.targetId));
          continue;
        case MidiTargetKind::PlayerGeneralCancel:
          pressGeneralCancel();
          continue;
        case MidiTargetKind::PlayerDivisional:
          pressDivisional(playerDivisionalDivision(action.targetId),
                          playerDivisionalPiston(action.targetId));
          continue;
        case MidiTargetKind::PlayerDivisionalCancel:
          pressDivisionalCancel(action.targetId);
          continue;
        case MidiTargetKind::Setter:
          setCaptureMode(action.engage);
          continue;
        case MidiTargetKind::RouteKeyboard:
          routeFromControl(action);
          continue;
        // The console belongs to the editor, and this is the audio thread, so
        // the action is left in a slot for the editor to collect. One slot is
        // enough: these are thumb pistons, pressed at human speed, and
        // dropping the earlier of two presses in the same tick is better than
        // a queue the audio thread has to manage.
        case MidiTargetKind::ConsoleNextPage:
        case MidiTargetKind::ConsolePrevPage:
        case MidiTargetKind::ConsoleNextLayout:
        case MidiTargetKind::ConsoleToggleStopList:
        case MidiTargetKind::ConsoleToggleKeyboard:
        case MidiTargetKind::ConsoleToggleCombinations:
        case MidiTargetKind::TransposeUp:
        case MidiTargetKind::TransposeDown:
        case MidiTargetKind::TemperamentNext:
        case MidiTargetKind::TemperamentPrev:
          pendingConsoleAction_.store(static_cast<int>(action.kind),
                                      std::memory_order_release);
          continue;
        case MidiTargetKind::Keyboard:
        case MidiTargetKind::None:
          break;
      }
    }

    // Unmapped messages keep the default behaviour, so an organ is playable
    // the moment it loads rather than only after a mapping session.
    // Which console this key came from, so an assignment can name one.
    noteDeviceId_ = deviceId;
    noteChannel_ = msg.getChannel();

    // Picking a keyboard takes one key press from a console, and that note
    // does not sound either.
    if (msg.isNoteOn() && deviceId > 0 && keyPick_.load(std::memory_order_acquire)) {
      pickedChannel_.store(msg.getChannel(), std::memory_order_release);
      keyPick_.store(false, std::memory_order_release);
      continue;
    }

    // Learning a manual consumes the key press: the note being used to teach
    // the range must not also sound.
    if (msg.isNoteOn() && keyboardLearn_ != 0) {
      learnKeyboardFrom(deviceId, msg.getChannel(), msg.getNoteNumber());
      continue;
    }
    if (msg.isNoteOff() && keyboardLearn_ != 0) continue;
    // A mapped rig, for the channels it claims: the binding decides which
    // manual, which note and what velocity. One press can reach more than
    // one manual — a split keyboard does exactly that — so every match is
    // played. A channel no binding claims falls through to the default path
    // below instead: a rig with three of four manuals mapped still plays
    // the fourth, and so do the on-screen keys, which carry no device.
    const bool claimed = msg.isNoteOnOrOff() &&
                         midiMap_.hasChannelBinding(deviceId, msg.getChannel());
    if (!claimed && logging && msg.isNoteOnOrOff() &&
        !midiMap_.keyboardBindingsEmpty())
      juce::Logger::writeToLog("midi:     no binding claims this device/channel"
                               " -- the organ's own default assignment");
    if (claimed) {
      keyHits_.clear();
      midiMap_.matchKeyboards(deviceId, msg.getChannel(), msg.getNoteNumber(),
                              msg.isNoteOn() ? msg.getVelocity() : 0,
                              blockTimeMs_, keyHits_);
      if (logging)
        juce::Logger::writeToLog(
            "midi:     mapped rig: " + juce::String(static_cast<int>(keyHits_.size())) +
            " manual(s) matched" +
            (keyHits_.empty() ? " -- NOTHING PLAYS: no binding covers this"
                                " device/channel/note"
                              : ""));
      for (const auto& hit : keyHits_) {
        // Keyed on the manual rather than the channel: two bindings can send
        // the same note to different manuals and each has to be released on
        // its own.
        const int key = noteKey(static_cast<int>(hit.keyboardId), hit.midiNote);
        if (hit.on && msg.isNoteOn())
          startNoteOnKeyboard(hit.keyboardId, key, transposed(hit.midiNote), hit.velocity);
        else
          stopNoteByKey(key, hit.velocity);
      }
      continue;
    }
    if (logging && msg.isNoteOnOrOff())
      juce::Logger::writeToLog("midi:     unmapped default path, keyboard for ch=" +
                               juce::String(msg.getChannel()) + " is " +
                               juce::String(static_cast<int>(
                                   keyboardForChannel(msg.getChannel(), deviceId))));
    if (msg.isNoteOn())
      startNote(msg.getChannel(), msg.getNoteNumber(), msg.getVelocity());
    else if (msg.isNoteOff())
      stopNote(msg.getChannel(), msg.getNoteNumber(), msg.getVelocity());
    // All Sound Off, All Notes Off and the mode messages that imply it
    // (120, 123-127) are CHANNEL messages. Releasing every manual on any of
    // them silenced a whole console from one keyboard: some keyboards and
    // encoders send All Notes Off on their own channel as a matter of
    // course, and every other manual's held notes stopped with it (#33).
    else if (msg.isController() &&
             (msg.getControllerNumber() == 120 || msg.getControllerNumber() >= 123))
      releaseChannel(msg.getChannel(), deviceId);
    else if (msg.isController())
      // With no mapping the control id IS the CC number, which is enough to
      // drive a swell shoe from a real pedal out of the box.
      setControlValue(msg.getControllerNumber(), msg.getControllerValue());
  }
}

namespace {

// A file name a person can read, from an organ's own name.
std::string sanitise(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    else if (c == ' ' || c == '-' || c == '_') out += '-';
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "organ" : out;
}

// GrandOrgue's fallback, for an organ that declares no id of its own: hash the
// normalised absolute path. Stable while the set stays where it is, which is
// the best a path can do.
std::string pathHash(const juce::File& odf) {
  const auto full = odf.getFullPathName().toLowerCase().toStdString();
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : full) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf), "p%016llx",
                static_cast<unsigned long long>(h));
  return buf;
}

} // namespace

std::string MasterpieceProcessor::organKey() const {
  if (model_.uniqueOrganId != 0)
    return sanitise(model_.organName) + "-" +
           std::to_string(model_.uniqueOrganId);
  // The same fallback name as organKeyFor, or an organ that declares no name
  // is written under one file and read from another.
  return sanitise(model_.organName.empty()
                      ? loadedOdf_.getFileNameWithoutExtension().toStdString()
                      : model_.organName) +
         "-" + pathHash(loadedOdf_);
}

std::string MasterpieceProcessor::organKeyFor(const juce::File& odf) {
  // Only the header is needed, and _General is the first table in the file —
  // so this does not pay for parsing a 60 000-row Sample table just to find
  // out where the settings live.
  OdfLoader loader;
  OdfLoader::Options opts;
  opts.headerOnly = true;
  OrganModel m;
  OdfDiagnostics d;
  if (loader.load(odf.getFullPathName().toStdString(), opts, m, d) &&
      m.uniqueOrganId != 0)
    return sanitise(m.organName) + "-" + std::to_string(m.uniqueOrganId);
  return sanitise(m.organName.empty()
                      ? odf.getFileNameWithoutExtension().toStdString()
                      : m.organName) +
         "-" + pathHash(odf);
}

juce::File MasterpieceProcessor::organFileForSaving(
    const juce::String& folder, const juce::String& extension) const {
  if (loadedOdf_.getFullPathName().isEmpty()) return {};
  // Always the organ's own identity, even when a legacy file was read: the
  // point of the migration is that it happens once.
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile(folder)
      .getChildFile(juce::String(organKey()) + extension);
}

juce::File MasterpieceProcessor::organFile(const juce::File& odf,
                                          const juce::String& folder,
                                          const juce::String& extension) const {
  if (odf.getFullPathName().isEmpty()) return {};
  // Beside the player's own data, never inside the sample set: writing into a
  // licensed package is not ours to do.
  const auto dir =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile("Masterpiece")
          .getChildFile(folder);

  // Named by the organ's own identity, so moving or renaming the sample set
  // does not orphan everything the player configured for it.
  const auto key = loadedOdf_ == odf ? organKey() : organKeyFor(odf);
  const auto wanted = dir.getChildFile(juce::String(key) + extension);
  if (wanted.existsAsFile()) return wanted;

  // Files were once named after the ODF's filename. Adopt one if it is there
  // and the new name is not: a player who configured an organ before this
  // should not lose it, and the next save writes the new name.
  const auto legacy =
      dir.getChildFile(odf.getFileNameWithoutExtension() + extension);
  if (legacy.existsAsFile()) return legacy;
  return wanted;
}

juce::File MasterpieceProcessor::midiMapFileFor(const juce::File& odf) const {
  return organFile(odf, "midi", ".mpmidi");
}

void MasterpieceProcessor::beginKeyboardLearn(Id keyboardId) {
  keyboardLearn_ = keyboardId;
  keyboardLearnLow_ = -1;
}

bool MasterpieceProcessor::learnKeyboardFrom(int deviceId, int channel,
                                             int note) {
  if (keyboardLearn_ == 0) return false;

  if (keyboardLearnLow_ < 0) {
    // First press: the bottom of the range, and the console and channel it
    // came from. Nothing is committed yet — a player who presses the wrong key
    // can press the right one after the second.
    keyboardLearnLow_ = note;
    keyboardLearnDevice_ = deviceId;
    keyboardLearnChannel_ = channel;
    return true;
  }

  MidiMap::KeyboardBinding b;
  b.keyboardId = keyboardLearn_;
  b.deviceId = keyboardLearnDevice_;
  b.channel = keyboardLearnChannel_;
  b.lowKey = std::min(keyboardLearnLow_, note);
  b.highKey = std::max(keyboardLearnLow_, note);

  // Line the pressed range up with the manual's own compass. A player taking
  // the top two octaves of one keyboard for a short manual wants those keys to
  // play that manual's bottom notes, not to fall off the end of it.
  const auto it = model_.keyboards.find(keyboardLearn_);
  if (it != model_.keyboards.end() && it->second.numKeys > 0)
    b.transpose = it->second.firstMidiNote - b.lowKey;

  midiMap_.removeKeyboardBindingsFor(keyboardLearn_);
  midiMap_.addKeyboardBinding(b);
  midiMapDirty_.store(true, std::memory_order_release);
  keyboardLearn_ = 0;
  keyboardLearnLow_ = -1;
  return true;
}

int MasterpieceProcessor::registerOwnMidiInput(const juce::String& name) {
  const int id = registerMidiDevice(name);
  ownInputs_.addIfNotAlreadyThere(name);
  if (id > 0 && id < kMaxMutedDevices)
    deviceMuted_[static_cast<size_t>(id)].store(inputsOff_.contains(name), std::memory_order_release);
  return id;
}

void MasterpieceProcessor::readMidiInputSwitches() {
  const auto f = globalSettingsFile();
  if (!f.existsAsFile()) return;
  for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString()))
    if (line.startsWith("midiinputoff ")) {
      const auto name = line.fromFirstOccurrenceOf(" ", false, false).trim();
      if (name.isNotEmpty()) inputsOff_.addIfNotAlreadyThere(name);
    }
}

void MasterpieceProcessor::setMidiInputEnabled(const juce::String& name, bool on) {
  if (midiInputEnabled(name) == on) return;
  if (on) inputsOff_.removeString(name);
  else inputsOff_.add(name);
  const int id = midiMap_.devices().lookup(name.toStdString());
  if (id > 0 && id < kMaxMutedDevices)
    deviceMuted_[static_cast<size_t>(id)].store(!on, std::memory_order_release);
  writeGlobalFile();
}

void MasterpieceProcessor::pushMidi(int deviceId, const juce::MidiMessage& msg) {
  if (deviceId > 0 && deviceId < kMaxMutedDevices &&
      deviceMuted_[static_cast<size_t>(deviceId)].load(std::memory_order_acquire))
    return;
  // Up to the slot's fixed size, system exclusive included: consoles send
  // their pistons that way, and dropping it here left every SysEx mapping deaf
  // to a real console (#210). A longer message is no piston, and copying an
  // unbounded one would mean allocating on a MIDI thread.
  const int size = msg.getRawDataSize();
  if (size <= 0 || size > kTaggedMidiBytes) return;

  const uint32_t index = midiWrite_.fetch_add(1, std::memory_order_acq_rel);
  TaggedMidi& t = midiQueue_[index % kMidiQueueSize];
  t.deviceId = deviceId;
  t.size = size;
  const auto* raw = msg.getRawData();
  for (int i = 0; i < size; ++i) t.bytes[i] = raw[i];
  // Published last: until this store the reader leaves the slot alone.
  t.ready.store(index + 1, std::memory_order_release);
}

void MasterpieceProcessor::drainTaggedMidi(
    std::vector<std::pair<int, juce::MidiMessage>>& out) {
  const uint32_t write = midiWrite_.load(std::memory_order_acquire);
  // Overrun: the queue wrapped past the reader. Skip to what is still there
  // rather than replaying stale bytes — a lost message is recoverable, a note
  // that never ends is not.
  if (write - midiRead_ > kMidiQueueSize) midiRead_ = write - kMidiQueueSize;
  while (midiRead_ != write) {
    const TaggedMidi& t = midiQueue_[midiRead_ % kMidiQueueSize];
    // Claimed but not yet written: stop here and take it next block, in
    // order, rather than reading a half-filled slot.
    if (t.ready.load(std::memory_order_acquire) != midiRead_ + 1) break;
    ++midiRead_;
    if (t.size <= 0) continue;
    out.emplace_back(t.deviceId, juce::MidiMessage(t.bytes, t.size));
  }
}

juce::File MasterpieceProcessor::settingsFileFor(const juce::File& odf) const {
  return organFile(odf, "organs", ".mporgan");
}

juce::String MasterpieceProcessor::settingsBody() const {
  const auto& sw = graph_.engineSwitch;
  juce::String text;
  // A bit width rather than an enum ordinal, so the file stays readable
  // and a width added later does not renumber the existing ones.
  text << "storage "
       << (samples_.storage() == SampleStorage::Int16   ? 16
           : samples_.storage() == SampleStorage::Int24 ? 24
                                                        : 32)
       << "\n";
  text << "mono " << (samples_.loadMono() ? 1 : 0) << "\n";
  text << "rate " << juce::String(samples_.loadSampleRate(), 0) << "\n";
  text << "cache " << static_cast<int>(samples_.cacheMode()) << "\n";
  text << "stream " << (samples_.streamReleases() ? 1 : 0) << "\n";
  text << "streamhead " << juce::String(samples_.streamHeadFrames()) << "\n";
  text << "streamheadpct " << samples_.streamHeadPercent() << "\n";
  text << "preload " << juce::String(preloadHead_) << "\n";
  if (organRootOverride_.getFullPathName().isNotEmpty())
    text << "root " << organRootOverride_.getFullPathName() << "\n";
  text << "simple " << (sw.simpleWavOnly ? 1 : 0) << "\n";
  text << "wind " << (sw.enableWindModel ? 1 : 0) << "\n";
  text << "tremulant " << (sw.enableTremulant ? 1 : 0) << "\n";
  text << "enclosure " << (sw.enableEnclosure ? 1 : 0) << "\n";
  text << "voicing " << (sw.enableVoicing ? 1 : 0) << "\n";
  text << "originalpitch " << (sw.playAtOriginalOrganPitch ? 1 : 0) << "\n";
  if (const auto* g = apvts_.getRawParameterValue("masterGain"))
    text << "gain " << juce::String(g->load(), 4) << "\n";

  // The mixer's BUSES and GROUPS, but not its routes. A bus is the player's
  // audio hardware — the same eight outputs whichever organ is loaded — so it
  // belongs in the tier that carries across organs. Routes name rank ids,
  // which mean nothing outside the organ that declared them, and are written
  // per organ in saveSettings.
  //
  // One line each, replacing wholesale rather than accumulating: a per-line
  // encoding has to define what a second `bus 1` means, and every answer to
  // that is a way to end up with duplicates.
  if (!mixer_.buses.empty()) {
    text << "buses";
    for (const auto& b : mixer_.buses) {
      text << " " << b.id.value << ":";
      for (size_t i = 0; i < b.deviceChannels.size(); ++i)
        text << (i ? "," : "") << b.deviceChannels[i];
    }
    text << "\n";
  }
  // A bus's own room. One line per bus rather than a single packed line,
  // because a path can contain anything including spaces, so it has to be last
  // on its line.
  {
    std::vector<int> withReverb;
    for (const auto& [busId, r] : mixer_.busReverb)
      if (!r.irFile.empty()) withReverb.push_back(busId);
    std::sort(withReverb.begin(), withReverb.end());
    for (int busId : withReverb) {
      const auto& r = mixer_.busReverb.at(busId);
      text << "busir " << busId << " " << (r.enabled ? 1 : 0) << " "
           << juce::String(r.mix, 3) << " " << juce::String(r.irFile) << "\n";
    }
  }
  if (!mixer_.groups.empty()) {
    text << "groups";
    for (const auto& g : mixer_.groups) {
      text << " " << g.groupId << ":";
      for (size_t i = 0; i < g.members.size(); ++i)
        text << (i ? "," : "") << g.members[i].value;
    }
    text << "\n";
  }
  return text;
}

void MasterpieceProcessor::applySettingsLine(const juce::String& key,
                                             const juce::String& val,
                                             EngineSwitch& sw) {
  const bool on = val.getIntValue() != 0;
  // A value given on the command line outranks the one in the file.
  if (isOverridden(key)) return;
  if (key == "storage")
    samples_.setStorage(val.getIntValue() == 16   ? SampleStorage::Int16
                        : val.getIntValue() == 32 ? SampleStorage::Float32
                                                  : SampleStorage::Int24);
  else if (key == "mono") samples_.setLoadMono(on);
  else if (key == "rate") samples_.setLoadSampleRate(val.getDoubleValue());
  else if (key == "cache") {
    const int v = val.getIntValue();
    samples_.setCacheMode(v == 0   ? SampleLibrary::CacheMode::Off
                          : v == 2 ? SampleLibrary::CacheMode::PerOrgan
                                   : SampleLibrary::CacheMode::Single);
  }
  else if (key == "stream") samples_.setStreamReleases(on);
  else if (key == "streamhead") samples_.setStreamHeadFrames(val.getLargeIntValue());
  else if (key == "streamheadpct") samples_.setStreamHeadPercent(val.getIntValue());
  else if (key == "preload") preloadHead_ = val.getLargeIntValue();
  // Where this organ's OrganInstallationPackages actually is, for a layout
  // the definition's path cannot reveal. Taken whole: a path may have spaces.
  else if (key == "root") organRootOverride_ = val.isEmpty() ? juce::File() : juce::File(val);
  else if (key == "simple") sw.simpleWavOnly = on;
  else if (key == "wind") sw.enableWindModel = on;
  else if (key == "tremulant") sw.enableTremulant = on;
  else if (key == "enclosure") sw.enableEnclosure = on;
  else if (key == "voicing") sw.enableVoicing = on;
  else if (key == "originalpitch") sw.playAtOriginalOrganPitch = on;
  else if (key == "gain") {
    if (auto* p = apvts_.getParameter("masterGain"))
      p->setValueNotifyingHost(p->convertTo0to1(val.getFloatValue()));
  } else if (key == "buses") {
    mixer_.buses.clear();
    for (const auto& tok : juce::StringArray::fromTokens(val, " ", "")) {
      if (tok.isEmpty()) continue;
      MixerBus b;
      b.id = BusId{tok.upToFirstOccurrenceOf(":", false, false).getIntValue()};
      if (b.id.value == 0) continue;
      for (const auto& ch : juce::StringArray::fromTokens(
               tok.fromFirstOccurrenceOf(":", false, false), ",", ""))
        if (ch.isNotEmpty()) b.deviceChannels.push_back(ch.getIntValue());
      mixer_.buses.push_back(std::move(b));
    }
    refreshMixerBuses();
  } else if (key == "busir") {
    // "busir <busId> <enabled> <mix> <path...>". The path is last because it
    // can contain spaces, and splitting it would quietly lose the file.
    auto rest = val.trim();
    const int busId = rest.upToFirstOccurrenceOf(" ", false, false).getIntValue();
    rest = rest.fromFirstOccurrenceOf(" ", false, false).trim();
    const bool on = rest.upToFirstOccurrenceOf(" ", false, false).getIntValue() != 0;
    rest = rest.fromFirstOccurrenceOf(" ", false, false).trim();
    const float mix = rest.upToFirstOccurrenceOf(" ", false, false).getFloatValue();
    const juce::String path = rest.fromFirstOccurrenceOf(" ", false, false).trim();
    if (busId != 0 && path.isNotEmpty()) {
      BusReverb r;
      r.enabled = on;
      r.mix = juce::jlimit(0.0f, 1.0f, mix);
      r.irFile = path.toStdString();
      mixer_.busReverb[busId] = std::move(r);
      refreshBusReverbs();
    }
  } else if (key == "groups") {
    mixer_.groups.clear();
    for (const auto& tok : juce::StringArray::fromTokens(val, " ", "")) {
      if (tok.isEmpty()) continue;
      BusGroup g;
      g.groupId = tok.upToFirstOccurrenceOf(":", false, false).getIntValue();
      if (g.groupId == 0) continue;
      for (const auto& m : juce::StringArray::fromTokens(
               tok.fromFirstOccurrenceOf(":", false, false), ",", ""))
        if (m.isNotEmpty()) g.members.push_back(BusId{m.getIntValue()});
      mixer_.groups.push_back(std::move(g));
    }
  }
}

juce::String MasterpieceProcessor::rememberedStateLines() const {
  juce::String text;
  // Where the player left the organ's own controls: noise levels, audio-group
  // balance, detuning. Only the ones the ORGAN says to remember — a swell shoe
  // and a crescendo are marked otherwise and must start where the organ puts
  // them, not where a previous session happened to stop.
  //
  // Per-organ only, never in the global defaults: a control id means nothing
  // outside the organ that declared it.
  //
  // And only controls that are SET rather than DERIVED. A control on the
  // receiving end of an unconditional linkage is computed from its source
  // every load, so writing it down records an answer that is about to be
  // recalculated -- Nancy has some 380 of them, all internal, and they turned
  // a settings file into a wall of noise. A control fed only by CONDITIONAL
  // linkages is different: those are preset buttons, they do not fire at
  // load, and the value really is the player's.
  std::unordered_set<Id> derived;
  for (const auto& l : model_.controlLinkages)
    if (l.destControlId != 0 && l.conditionSwitchId == 0)
      derived.insert(l.destControlId);

  for (const auto& [id, c] : model_.continuousControls) {
    if (!c.rememberState || derived.count(id) != 0) continue;
    const int v = controls_.value(id);
    if (v == c.defaultValue) continue;  // nothing to say
    text << "control " << juce::String(id) << " " << juce::String(v) << "\n";
  }

  // And its switches, the few it marks the same way: Nancy's "start blower on
  // organ load" and "enable anches on load". Only latching ones -- a button
  // that springs back has no state to keep -- and only where they differ from
  // what the organ starts with.
  std::vector<Id> kept;
  for (const auto& [id, s] : model_.switches)
    if (s.rememberState && s.latching && switches_.engaged(id) != s.defaultEngaged)
      kept.push_back(id);
  std::sort(kept.begin(), kept.end());
  for (Id id : kept)
    text << "switch " << juce::String(id) << " " << (switches_.engaged(id) ? 1 : 0) << "\n";
  return text;
}

bool MasterpieceProcessor::saveRememberedState() const {
  const auto f = organFileForSaving("organs", ".mporgan");
  if (f.getFullPathName().isEmpty()) return false;
  // Every line but these, exactly as it was; see markRememberedStateMoved.
  const juce::String before = f.existsAsFile() ? f.loadFileAsString() : juce::String();
  juce::StringArray lines;
  for (const auto& line : juce::StringArray::fromLines(before)) {
    const auto key = line.upToFirstOccurrenceOf(" ", false, false).trim();
    if (key == "control" || key == "switch") continue;
    lines.add(line);
  }
  while (!lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty())
    lines.remove(lines.size() - 1);
  if (lines.isEmpty()) lines.add("# Masterpiece per-organ settings");
  const juce::String text = lines.joinIntoString("\n") + "\n" + rememberedStateLines();
  // A stop drawn and put back: nothing to write. What is on disk has the
  // CRLF endings replaceWithText writes on Windows.
  if (text == before.replace("\r\n", "\n")) return true;
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(text);
}

bool MasterpieceProcessor::saveRememberedStateIfSettled(uint32_t quietMs) {
  const uint32_t at = rememberedMovedAtMs_.load(std::memory_order_acquire);
  if (at == 0 || juce::Time::getMillisecondCounter() - at < quietMs) return false;
  // A move that lands between the load and the exchange is kept for the next
  // tick rather than lost.
  uint32_t expected = at;
  if (!rememberedMovedAtMs_.compare_exchange_strong(expected, 0u, std::memory_order_acq_rel))
    return false;
  return saveRememberedState();
}

bool MasterpieceProcessor::saveRememberedStateIfPending() {
  if (rememberedMovedAtMs_.exchange(0u, std::memory_order_acq_rel) == 0) return false;
  return saveRememberedState();
}

bool MasterpieceProcessor::saveSettings() const {
  const auto f = organFileForSaving("organs", ".mporgan");
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();

  juce::String text = "# Masterpiece per-organ settings\n";
  text << settingsBody();
  text << rememberedStateLines();

  // Where each rank speaks. Per organ because a rank id means nothing
  // elsewhere, and only the ranks the player actually routed: the rest fall
  // back to the simple default, and writing them down would record an answer
  // that is recomputed anyway.
  //
  // Sorted, so saving the same mixer twice produces the same file. Routings
  // live in an unordered_map and would otherwise reshuffle on every save,
  // which makes the file impossible to diff and noisy in a backup.
  std::vector<Id> routed;
  routed.reserve(mixer_.rankRoutings.size());
  for (const auto& [rankId, routing] : mixer_.rankRoutings)
    routed.push_back(rankId);
  std::sort(routed.begin(), routed.end());
  for (Id rankId : routed) {
    const auto& primary = mixer_.rankRoutings.at(rankId).perspectives[0];
    if (std::holds_alternative<BusId>(primary.dest))
      text << "route " << juce::String(rankId) << " bus "
           << juce::String(std::get<BusId>(primary.dest).value) << "\n";
    else
      text << "route " << juce::String(rankId) << " group "
           << juce::String(std::get<int>(primary.dest)) << "\n";
  }

  // Voicing, per organ for the same reason as the routes: a rank or pipe id
  // means nothing in another instrument. BOTH slots are written, and which one
  // is live, so an A/B survives a reload — the comparison is the work, and
  // losing the other side of it on quit throws that work away.
  {
    auto writeSet = [&text](const char* slot, const VoicingSet& v) {
      auto line = [&](const char* what, Id id, const PipeVoicing& pv) {
        text << "voicingadj " << slot << " " << what << " " << juce::String(id)
             << " " << juce::String(pv.gainDb, 3) << " "
             << juce::String(pv.tuningCents, 3) << " "
             << juce::String(pv.brightnessDb, 3) << " "
             << juce::String(pv.balance, 3) << "\n";
      };
      for (Id id : v.rankIds()) line("rank", id, v.rank(id));
      for (Id id : v.pipeIds()) line("pipe", id, v.pipe(id));
    };
    writeSet("a", voicing_.a);
    writeSet("b", voicing_.b);
    if (voicing_.usingB) text << "voicingslot b\n";
    // Which named set this organ was last using. Stored rather than assumed:
    // coming back and finding the recital registrations instead of the service
    // ones is a nasty surprise to meet mid-piece.
    if (licenceConfirmed_) text << "licenceconfirmed 1\n";
    if (!combinationSet_.empty())
      text << "combset " << juce::String(combinationSet_) << "\n";
    for (Id stop : excludedStops_) text << "skipstop " << juce::String(stop) << "\n";
    for (const auto& p : excludedPerspectives_) text << "skipperspective " << juce::String(p) << "\n";
    for (Id rank : excludedRanks_) text << "skiprank " << juce::String(rank) << "\n";
    if (keepPortable_) text << "portable 1\n";
    if (combWindow_.w > 0)
      text << "combwindow " << combWindow_.x << " " << combWindow_.y << " "
           << combWindow_.w << " " << combWindow_.h << " "
           << (combWindow_.open ? 1 : 0) << "\n";
    for (const auto& p : pageWindows_)
      text << "pagewindow " << p.page << " " << p.x << " " << p.y << " " << p.w << " " << p.h << " "
           << p.layout << "\n";
    if (!temperamentChoice_.empty())
      text << "temperament " << juce::String(temperamentChoice_) << "\n";
    if (masterPitchSetting() > 0.0)
      text << "pitchhz " << juce::String(masterPitchSetting(), 2) << "\n";
    if (transpose() != 0) text << "transpose " << transpose() << "\n";
  }

  return f.replaceWithText(text);
}

bool MasterpieceProcessor::loadSettingsFor(const juce::File& odf) {
  pendingControlValues_.clear();
  pendingSwitchStates_.clear();
  // Routes and voicing belong to the organ being left, not the one arriving.
  // Keeping either would point this organ's rank ids at the previous organ's
  // mix, or worse, at its tuning.
  mixer_.rankRoutings.clear();
  voicing_.a.clear();
  voicing_.b.clear();
  voicing_.usingB = false;
  pageWindows_.clear();
  combinationSet_.clear();
  combWindow_ = {};
  excludedStops_.clear();
  excludedPerspectives_.clear();
  excludedRanks_.clear();
  keepPortable_ = false;
  // Tuning belongs to the organ it was chosen for.
  temperamentChoice_.clear();
  licenceConfirmed_ = false;
  playerTuning_.store(nullptr, std::memory_order_release);
  masterPitchHz_.store(0.0, std::memory_order_relaxed);
  transpose_.store(0, std::memory_order_relaxed);
  const auto f = settingsFileFor(odf);
  if (f.getFullPathName().isEmpty() || !f.existsAsFile()) return false;

  auto sw = graph_.engineSwitch;
  for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString())) {
    if (line.trim().isEmpty() || line.trimStart().startsWith("#")) continue;
    const auto key = line.upToFirstOccurrenceOf(" ", false, false).trim();
    const auto val = line.fromFirstOccurrenceOf(" ", false, false).trim();
    if (key == "control") {
      // "control <id> <value>". Held rather than applied: the bank does not
      // exist yet and reset() would discard anything set now.
      pendingControlValues_.emplace_back(
          static_cast<Id>(val.upToFirstOccurrenceOf(" ", false, false).getLargeIntValue()),
          val.fromFirstOccurrenceOf(" ", false, false).trim().getIntValue());
      continue;
    }
    if (key == "switch") {
      // "switch <id> <0|1>", held for the same reason as the controls.
      pendingSwitchStates_.emplace_back(
          static_cast<Id>(val.upToFirstOccurrenceOf(" ", false, false).getLargeIntValue()),
          val.fromFirstOccurrenceOf(" ", false, false).trim().getIntValue() != 0);
      continue;
    }
    if (key == "licenceconfirmed") {
      licenceConfirmed_ = val.getIntValue() != 0;
      continue;
    }
    if (key == "voicingadj") {
      // "voicingadj a|b rank|pipe <id> <gainDb> <cents> <brightness> <balance>"
      //
      // NOT "voicing": settingsBody already writes `voicing 0|1` for the DSP
      // engine switch, and this handler runs BEFORE applySettingsLine. Sharing
      // the key made this swallow the switch's line and silently stop
      // restoring it -- turn Voicing off, save, reload, and it was on again.
      auto tok = juce::StringArray::fromTokens(val, " ", "");
      tok.removeEmptyStrings();
      if (tok.size() < 7) continue;
      PipeVoicing pv;
      pv.gainDb = tok[3].getFloatValue();
      pv.tuningCents = tok[4].getFloatValue();
      pv.brightnessDb = tok[5].getFloatValue();
      pv.balance = tok[6].getFloatValue();
      VoicingSet& set = tok[0] == "b" ? voicing_.b : voicing_.a;
      const Id id = static_cast<Id>(tok[2].getLargeIntValue());
      if (tok[1] == "pipe") set.setPipe(id, pv);
      else set.setRank(id, pv);
      continue;
    }
    if (key == "combset") {
      combinationSet_ = val.trim().toStdString();
      continue;
    }
    if (key == "temperament") {
      // Resolved against the organ's own once the organ is prepared.
      temperamentChoice_ = val.trim().toStdString();
      continue;
    }
    if (key == "pitchhz") {
      // Read back as written; the setter's limits applied when it was set.
      masterPitchHz_.store(juce::jmax(0.0, val.getDoubleValue()), std::memory_order_relaxed);
      continue;
    }
    if (key == "pagewindow") {
      // "pagewindow <page> <x> <y> <w> <h> [<layout>]"
      auto tok = juce::StringArray::fromTokens(val, " ", "");
      tok.removeEmptyStrings();
      if (tok.size() >= 5 && tok[3].getIntValue() > 0 && tok[4].getIntValue() > 0)
        pageWindows_.push_back({tok[0].getIntValue(), tok[1].getIntValue(), tok[2].getIntValue(),
                                tok[3].getIntValue(), tok[4].getIntValue(),
                                tok.size() >= 6 ? tok[5].getIntValue() : -1});
      continue;
    }
    if (key == "transpose") {
      transpose_.store(juce::jlimit(-12, 12, val.getIntValue()), std::memory_order_relaxed);
      continue;
    }
    if (key == "skipstop") {
      excludedStops_.insert(static_cast<Id>(val.getLargeIntValue()));
      continue;
    }
    if (key == "portable") {
      keepPortable_ = val.getIntValue() != 0;
      continue;
    }
    if (key == "skiprank") {
      excludedRanks_.insert(static_cast<Id>(val.getLargeIntValue()));
      continue;
    }
    if (key == "skipperspective") {
      if (val.isNotEmpty()) excludedPerspectives_.insert(val.toStdString());
      continue;
    }
    if (key == "combwindow") {
      // "combwindow <x> <y> <w> <h> <open>"
      auto tok = juce::StringArray::fromTokens(val, " ", "");
      tok.removeEmptyStrings();
      if (tok.size() >= 5) {
        combWindow_.x = tok[0].getIntValue();
        combWindow_.y = tok[1].getIntValue();
        combWindow_.w = tok[2].getIntValue();
        combWindow_.h = tok[3].getIntValue();
        combWindow_.open = tok[4].getIntValue() != 0;
      }
      continue;
    }
    if (key == "voicingslot") {
      voicing_.usingB = val.trim() == "b";
      continue;
    }
    if (key == "route") {
      // "route <rankId> bus|group <id>". Applied straight away: unlike a
      // control value there is nothing downstream that resets it.
      const auto rankStr = val.upToFirstOccurrenceOf(" ", false, false).trim();
      const auto rest = val.fromFirstOccurrenceOf(" ", false, false).trim();
      const auto kind = rest.upToFirstOccurrenceOf(" ", false, false).trim();
      const int destId =
          rest.fromFirstOccurrenceOf(" ", false, false).trim().getIntValue();
      const Id rankId = static_cast<Id>(rankStr.getLargeIntValue());
      if (rankId == 0 || destId == 0) continue;
      RankRouting r = mixer_.routingFor(rankId);
      r.rankId = rankId;
      if (kind == "group") r.perspectives[0].dest = destId;
      else r.perspectives[0].dest = BusId{destId};
      mixer_.rankRoutings[rankId] = r;
      continue;
    }
    applySettingsLine(key, val, sw);
  }
  graph_.engineSwitch = sw;
  return true;
}

juce::File MasterpieceProcessor::globalSettingsFile() const {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("settings.mpglobal");
}

bool MasterpieceProcessor::writeGlobalFile() const {
  const auto f = globalSettingsFile();
  f.getParentDirectory().createDirectory();
  juce::String text;
  text << "# Masterpiece defaults for organs that have no settings of their own\n";
  text << globalBody_;
  // A new key: the old one was written on every save, whether or not anyone
  // chose it, so it cannot tell a choice from a default. Only this one counts.
  text << "reopenlastorgan " << (reopenLastOrgan_ ? 1 : 0) << "\n";
  if (const float db = keepAliveDb_.load(std::memory_order_relaxed); db < 0.0f)
    text << "speakerkeepalive " << juce::String(db, 1) << "\n";
  if (memoryLimitMB_ > 0) text << "memlimit " << memoryLimitMB_ << "\n";
  if (runningOrgan_.getFullPathName().isNotEmpty())
    text << "running " << runningOrgan_.getFullPathName() << "\n";
  text << "loadticks "
       << (loadTicks_.load(std::memory_order_acquire) ? 1 : 0) << "\n";
  if (cancelResetsKeyboards_.load()) text << "cancelresetskeyboards 1\n";
  if (combinationsOnTop_) text << "combinationsontop 1\n";
  if (setOffAfterStore_) text << "setoffafterstore 1\n";
  if (fasterEngine_.load()) text << "fasterengine 1\n";
  for (const auto& name : inputsOff_) text << "midiinputoff " << name << "\n";
  for (const auto& [role, channel] : defaultConsole_)
    text << "consolechannel " << role << " " << channel << "\n";
  for (const auto& lib : libraries_)
    text << "library " << lib.getFullPathName() << "\n";
  if (cacheDir_.getFullPathName().isNotEmpty())
    text << "cachedir " << cacheDir_.getFullPathName() << "\n";
  if (lastOrgan_.getFullPathName().isNotEmpty())
    text << "lastorgan " << lastOrgan_.getFullPathName() << "\n";

  // Favourites are global by nature: the point of one is to get to a
  // DIFFERENT organ, so storing them inside the organ being left would be
  // useless. The target goes last on the line because a path can contain
  // spaces, and a bar separates it from the name because both are free text.
  for (auto kind : {FavouriteKind::Organ, FavouriteKind::Temperament,
                    FavouriteKind::CombinationSet}) {
    const auto& bank = favourites_.bank(kind);
    for (int slot : bank.used()) {
      const auto& fav = bank.at(slot);
      text << "favourite " << Favourites::kindKey(kind) << " " << slot << " "
           << juce::String(fav.name).replaceCharacter('|', '/') << " | "
           << juce::String(fav.target) << "\n";
    }
  }
  return f.replaceWithText(text);
}

bool MasterpieceProcessor::saveGlobalDefaults() {
  // The one place the live state becomes everyone's starting point, and it
  // happens only because a player asked for it.
  globalBody_ = settingsBody();
  return writeGlobalFile();
}

bool MasterpieceProcessor::loadGlobalDefaults() {
  const auto f = globalSettingsFile();
  if (!f.existsAsFile()) {
    globalsReadOnce_ = true;  // a first start: nothing can have crashed
    return false;
  }

  juce::String body;
  auto sw = graph_.engineSwitch;
  for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString())) {
    if (line.trim().isEmpty() || line.trimStart().startsWith("#")) continue;
    const auto key = line.upToFirstOccurrenceOf(" ", false, false).trim();
    // A path may contain spaces, so the value is taken whole, not tokenised.
    const auto val = line.fromFirstOccurrenceOf(" ", false, false).trim();
    if (key == "reopenlast") {
      // Superseded by reopenlastorgan: dropped, so everyone starts with the
      // reopen off and turns it on only by choosing to.
    } else if (key == "reopenlastorgan") {
      reopenLastOrgan_ = val.getIntValue() != 0;
    } else if (key == "speakerkeepalive") {
      const float db = val.getFloatValue();
      keepAliveDb_.store(db < 0.0f ? juce::jlimit(-80.0f, -40.0f, db) : 0.0f,
                         std::memory_order_relaxed);
    } else if (key == "memlimit") {
      memoryLimitMB_ = std::max(0, val.getIntValue());
    } else if (key == "running") {
      // Still here at the first read of a session: the last one did not exit
      // cleanly. Every later read finds this session's own entry, which says
      // nothing about a crash.
      if (!globalsReadOnce_) crashedOrgan_ = juce::File(val);
    } else if (key == "consolechannel") {
      // "consolechannel <role> <channel>": the player's own console.
      const int role = val.upToFirstOccurrenceOf(" ", false, false).getIntValue();
      const int channel = val.fromFirstOccurrenceOf(" ", false, false).getIntValue();
      if (role >= 0 && role < 16 && channel >= 1 && channel <= 16) defaultConsole_[role] = channel;
    } else if (key == "loadticks") {
      loadTicks_.store(val.getIntValue() != 0, std::memory_order_release);
    } else if (key == "cancelresetskeyboards") {
      cancelResetsKeyboards_.store(val.getIntValue() != 0);
    } else if (key == "combinationsontop") {
      combinationsOnTop_ = val.getIntValue() != 0;
    } else if (key == "setoffafterstore") {
      setOffAfterStore_ = val.getIntValue() != 0;
    } else if (key == "fasterengine") {
      fasterEngine_.store(val.getIntValue() != 0);
    } else if (key == "midiinputoff") {
      if (val.isNotEmpty()) inputsOff_.addIfNotAlreadyThere(val);
    } else if (key == "library") {
      const juce::File dir(val);
      if (val.isNotEmpty() &&
          std::find(libraries_.begin(), libraries_.end(), dir) == libraries_.end())
        libraries_.push_back(dir);
    } else if (key == "cachedir") {
      // A path, taken whole: the sample cache can be gigabytes, and a player
      // with a small fast disk and a large slow one wants to choose which of
      // them holds it.
      cacheDir_ = val.isEmpty() ? juce::File() : juce::File(val);
    } else if (key == "lastorgan") {
      lastOrgan_ = juce::File(val);
    } else if (key == "favourite") {
      // "favourite <kind> <slot> <name> | <target>". The bar separates them
      // because both halves are free text and the target can contain spaces;
      // a bar inside a name is rewritten on the way out rather than escaped.
      auto rest = val.trim();
      const auto kindKey = rest.upToFirstOccurrenceOf(" ", false, false).trim();
      rest = rest.fromFirstOccurrenceOf(" ", false, false);
      const int slot = rest.upToFirstOccurrenceOf(" ", false, false).getIntValue();
      rest = rest.fromFirstOccurrenceOf(" ", false, false);
      Favourite fav;
      fav.name = rest.upToFirstOccurrenceOf("|", false, false).trim().toStdString();
      fav.target = rest.fromFirstOccurrenceOf("|", false, false).trim().toStdString();
      if (slot > 0 && !fav.target.empty())
        favourites_.bank(Favourites::kindFromKey(kindKey.toStdString()))
            .set(slot, std::move(fav));
    } else {
      applySettingsLine(key, val, sw);
      body << line << "\n";
    }
  }
  graph_.engineSwitch = sw;
  globalBody_ = body;
  globalsReadOnce_ = true;
  return true;
}

int MasterpieceProcessor::addCurrentOrganToFavourites(int slot) {
  if (loadedOdf_.getFullPathName().isEmpty()) return 0;
  const std::string target = loadedOdf_.getFullPathName().toStdString();

  // Already on a slot? Return that one rather than making a second copy: the
  // same organ under two names is a way to wonder later which is the real one.
  if (const int existing = favourites_.organs.slotOf(target)) return existing;

  const int use = slot > 0 ? slot : favourites_.organs.firstFree();
  if (use == 0) return 0;  // bank full; the caller says so

  Favourite fav;
  // The organ's own name, not the file's: a player calls it "Raszczyce", and
  // the file is called Raszczyce.Organ_Hauptwerk_xml.
  fav.name = model_.organName.empty()
                 ? loadedOdf_.getFileNameWithoutExtension().toStdString()
                 : model_.organName;
  fav.target = target;
  favourites_.organs.set(use, std::move(fav));
  writeGlobalFile();
  return use;
}

void MasterpieceProcessor::setLastOrgan(const juce::File& odf) {
  if (lastOrgan_ == odf) return;
  lastOrgan_ = odf;
  writeGlobalFile();
}

juce::File MasterpieceProcessor::lastOrgan() const {
  // Answer only for a file that is still there: a set on a drive that is not
  // plugged in should open the file chooser, not an error.
  return lastOrgan_.existsAsFile() ? lastOrgan_ : juce::File();
}

void MasterpieceProcessor::markRunningOrgan() {
  // The same file the load marked, which is what the next start compares.
  if (clearedRunning_.getFullPathName().isEmpty()) return;
  runningOrgan_ = clearedRunning_;
  clearedRunning_ = juce::File();
  writeGlobalFile();
}

void MasterpieceProcessor::clearRunningOrgan() {
  if (runningOrgan_.getFullPathName().isEmpty()) return;
  clearedRunning_ = runningOrgan_;
  runningOrgan_ = juce::File();
  writeGlobalFile();
}

int MasterpieceProcessor::defaultMemoryLimitMB() {
  return std::max(512, juce::SystemStats::getMemorySizeInMegabytes() * 8 / 10);
}

void MasterpieceProcessor::setMemoryLimitMB(int mb) {
  mb = std::max(0, mb);
  if (memoryLimitMB_ == mb) return;
  memoryLimitMB_ = mb;
  writeGlobalFile();
}

int64_t MasterpieceProcessor::memoryLimitBytes() const {
  const int mb = memoryLimitOverrideMB_ > 0 ? memoryLimitOverrideMB_
                 : memoryLimitMB_ > 0 ? memoryLimitMB_ : defaultMemoryLimitMB();
  return static_cast<int64_t>(mb) * 1024 * 1024;
}

void MasterpieceProcessor::setSpeakerKeepAlive(float levelDb) {
  const float db = levelDb < 0.0f ? juce::jlimit(-80.0f, -40.0f, levelDb) : 0.0f;
  if (keepAliveDb_.exchange(db, std::memory_order_relaxed) == db) return;
  // A preference about the audio hardware, like reopening: written alone.
  writeGlobalFile();
}

void MasterpieceProcessor::addKeepAlive(juce::AudioBuffer<float>& buffer) {
  const float db = keepAliveDb_.load(std::memory_order_relaxed);
  if (db >= 0.0f || sampleRate_ <= 0.0) return;
  const float amp = juce::Decibels::decibelsToGain(db);
  const double step = juce::MathConstants<double>::twoPi * 20.0 / sampleRate_;
  const int n = buffer.getNumSamples();
  for (int i = 0; i < n; ++i) {
    const float v = amp * static_cast<float>(std::sin(keepAlivePhase_));
    keepAlivePhase_ += step;
    if (keepAlivePhase_ >= juce::MathConstants<double>::twoPi)
      keepAlivePhase_ -= juce::MathConstants<double>::twoPi;
    for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.addSample(c, i, v);
  }
}

void MasterpieceProcessor::setReopenLastOrgan(bool on) {
  if (reopenLastOrgan_ == on) return;
  reopenLastOrgan_ = on;
  // Not saveGlobalDefaults(): a preference about startup is not a request to
  // adopt the open organ's settings as everyone's.
  writeGlobalFile();
}

// Does one of the known libraries hold the packages this organ names? The
// matching itself lives in the core, where it can be tested without a
// processor writing to anyone's settings.
juce::File MasterpieceProcessor::libraryHolding(const OrganModel& model) const {
  std::vector<std::string> roots;
  for (const auto& lib : libraries_) roots.push_back(lib.getFullPathName().toStdString());
  const std::string found = mp::findLibraryHolding(roots, model);
  return found.empty() ? juce::File() : juce::File(found);
}

// The place a Hauptwerk installation keeps its libraries, so the first load
// after installing Masterpiece already knows where to look. Added only if it
// is really there.
void MasterpieceProcessor::seedSampleLibraries() {
  const auto standard =
      juce::File::getSpecialLocation(juce::File::userHomeDirectory)
          .getChildFile("Hauptwerk")
          .getChildFile("HauptwerkSampleSetsAndComponents");
  if (standard.getChildFile("OrganInstallationPackages").isDirectory() &&
      std::find(libraries_.begin(), libraries_.end(), standard) == libraries_.end())
    libraries_.push_back(standard);
}

void MasterpieceProcessor::rememberSampleLibrary(const juce::File& root) {
  if (!root.isDirectory()) return;
  if (!root.getChildFile("OrganInstallationPackages").isDirectory()) return;
  if (std::find(libraries_.begin(), libraries_.end(), root) != libraries_.end())
    return;
  libraries_.push_back(root);
  writeGlobalFile();
}

juce::File MasterpieceProcessor::defaultCacheDirectory() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("cache");
}

juce::File MasterpieceProcessor::cacheDirectory() const {
  // The folder the player chose, as long as it can be created: a cache on a
  // drive that is not plugged in must not stop an organ from loading. It only
  // means this load is not cached.
  if (cacheDir_.getFullPathName().isNotEmpty()) {
    cacheDir_.createDirectory();
    if (cacheDir_.isDirectory()) return cacheDir_;
  }
  return defaultCacheDirectory();
}

void MasterpieceProcessor::setCacheDirectory(const juce::File& dir) {
  if (dir == cacheDir_) return;
  cacheDir_ = dir;
  samples_.setCacheDir(cacheDirectory().getFullPathName().toStdString());
  // Written at once, like the other general preferences: where the cache
  // lives is a property of the machine, not of the organ that is open.
  writeGlobalFile();
}

void MasterpieceProcessor::setLoadTicks(bool on) {
  if (loadTicks_.load(std::memory_order_acquire) == on) return;
  loadTicks_.store(on, std::memory_order_release);
  // Written at once, like the startup preference above: a general config is
  // not something a player sets per organ.
  writeGlobalFile();
}

bool MasterpieceProcessor::saveMidiMapIfDirty() {
  if (!midiMapDirty_.exchange(false, std::memory_order_acq_rel)) return false;
  return saveMidiMap();
}

bool MasterpieceProcessor::saveSettingsIfDirty() {
  if (!settingsDirty_.exchange(false, std::memory_order_acq_rel)) return false;
  return saveSettings();
}

bool MasterpieceProcessor::saveMasterGain() const {
  const auto f = organFileForSaving("organs", ".mporgan");
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();

  const auto* g = apvts_.getRawParameterValue("masterGain");
  if (g == nullptr) return false;
  const juce::String gainLine = "gain " + juce::String(g->load(), 4);

  // Rewrite only the "gain" line, keeping every other line exactly as it
  // was. saveSettings() writes the whole file from the live engine state,
  // which is right when the player asks for it in the Settings dialog but
  // wrong here: a change made there and only "kept" for the session must
  // not get dragged onto disk just because the player also moved the
  // volume slider.
  juce::StringArray lines;
  if (f.existsAsFile())
    lines = juce::StringArray::fromLines(f.loadFileAsString());
  // fromLines returns an empty last entry for text that ends in a newline,
  // which every file written here does. Kept, it would be joined back with a
  // newline of its own and the file would gain a blank line on every save --
  // one per touch of the volume slider, for ever.
  while (!lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty())
    lines.remove(lines.size() - 1);
  bool replaced = false;
  for (auto& line : lines) {
    if (line.upToFirstOccurrenceOf(" ", false, false).trim() == "gain") {
      line = gainLine;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    if (lines.isEmpty()) lines.add("# Masterpiece per-organ settings");
    lines.add(gainLine);
  }
  return f.replaceWithText(lines.joinIntoString("\n") + "\n");
}

bool MasterpieceProcessor::saveMasterGainIfDirty() {
  if (!masterGainDirty_.exchange(false, std::memory_order_acq_rel)) return false;
  return saveMasterGain();
}

juce::File MasterpieceProcessor::consoleMidiFile() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("midi")
      .getChildFile("console.mpmidi");
}

bool MasterpieceProcessor::saveMidiMap() const {
  // Two files: what belongs to this organ -- its stops, shoes, manuals,
  // divisionals -- and what belongs to the player's console on every organ:
  // the stepper, the generals, the setter (a Buckeburg report asked for the
  // piston assignments to work for every organ).
  const std::string all = midiMap_.toText();
  MidiMap console;
  console.fromText(all);
  console.removeBindings([](const MidiBinding& b) { return !MidiMap::isConsoleTarget(b.targetKind); });
  console.clearKeyboardBindings();
  console.keepOnlyConsoleSendsAndShortcuts();
  const auto cf = consoleMidiFile();
  cf.getParentDirectory().createDirectory();
  cf.replaceWithText(juce::String(console.toText()));

  const auto f = organFileForSaving("midi", ".mpmidi");
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  MidiMap organ;
  organ.fromText(all);
  // Keyboards moved by a piston are saved where they were before it.
  if (pistonRouted_.load()) {
    const AudioLock lock(const_cast<MasterpieceProcessor&>(*this));
    if (pistonRouted_.load()) {
      organ.clearKeyboardBindings();
      for (const auto& b : routingBeforePistons_) organ.addKeyboardBinding(b);
    }
  }
  organ.removeBindings([](const MidiBinding& b) { return MidiMap::isConsoleTarget(b.targetKind); });
  return f.replaceWithText(juce::String(organ.toText()));
}

void MasterpieceProcessor::applyConsoleMidi() {
  // Nothing saved for the console yet: whatever the organ's own file carried
  // stays, and moves to the console file with the next save.
  const auto cf = consoleMidiFile();
  if (!cf.existsAsFile()) return;
  MidiMap console;
  if (!console.fromText(cf.loadFileAsString().toStdString())) return;
  midiMap_.removeBindings([](const MidiBinding& b) { return MidiMap::isConsoleTarget(b.targetKind); });
  for (const auto& b : console.bindings())
    if (MidiMap::isConsoleTarget(b.targetKind)) midiMap_.bind(b);
}

int MasterpieceProcessor::consoleRoleOf(Id keyboardId) const {
  const int code = couplers_.assignmentCodeFor(keyboardId);
  return code >= 1 && code <= 16 ? code - 1 : -1;
}

void MasterpieceProcessor::applyDefaultConsole() {
  // Shared, not exclusive: a default that puts two roles on one channel was
  // the player's choice, and the MIDI page says so.
  for (Id kb : couplers_.inputKeyboards()) {
    const auto it = defaultConsole_.find(consoleRoleOf(kb));
    if (it != defaultConsole_.end()) setKeyboardForChannel(it->second, kb, 0, false);
  }
}

void MasterpieceProcessor::useChannelsAsDefaultConsole() {
  defaultConsole_.clear();
  for (Id kb : couplers_.inputKeyboards()) {
    const int role = consoleRoleOf(kb);
    if (role >= 0) defaultConsole_[role] = channelForKeyboard(kb);
  }
  writeGlobalFile();
}

void MasterpieceProcessor::clearDefaultConsole() {
  defaultConsole_.clear();
  writeGlobalFile();
}

bool MasterpieceProcessor::loadMidiMap() {
  midiMapRepaired_ = 0;
  // The last organ's stops and shoes go: their switch and control numbers
  // mean other things here. They lingered when this organ had no file.
  midiMap_.removeBindings([](const MidiBinding&) { return true; });
  const auto f = midiMapFileFor(loadedOdf_);
  // Nothing saved for this organ: the player's own console, when there is one.
  if (f.getFullPathName().isEmpty() || !f.existsAsFile()) {
    applyDefaultConsole();
    applyConsoleMidi();
    return false;
  }
  const auto text = f.loadFileAsString();
  const bool ok = midiMap_.fromText(text.toStdString());
  applyConsoleMidi();
  // A mapping saved for other things -- pistons, stops -- but no manuals.
  if (midiMap_.keyboardBindings().empty()) applyDefaultConsole();

  // A mapping that sends two manuals to one channel, or names a manual this
  // organ does not have, is repaired here rather than obeyed. Up to 0.3.7 the
  // settings page could write such a file, and obeying it silently left
  // manuals unplayable. The original is kept beside the repaired one, so a
  // player who wants to see what was there can.
  midiMapRepaired_ = midiMap_.repairKeyboardBindings(couplers_.inputKeyboards());
  if (midiMapRepaired_ > 0) {
    const auto backup = f.getSiblingFile(f.getFileName() + ".before-repair");
    if (!backup.existsAsFile()) backup.replaceWithText(text);
    saveMidiMap();
    juce::Logger::writeToLog(
        "midi: repaired the saved mapping: " + juce::String(midiMapRepaired_) +
        " conflicting or stale manual assignment(s) removed; the organ's own "
        "channels apply. The original is kept as " + backup.getFileName());
  }
  return ok;
}

void MasterpieceProcessor::resolveSamplePitches() {
  // What every sample actually holds, decided once per load rather than per
  // note-on, and written into the pipework where the ratio is computed.
  //
  // The registry and the pipework hold SEPARATE COPIES of each SampleRef
  // (OdfLoader assigns `attack.sample = sampleIt->second`), and it is the
  // pipework's copies that a sounding note reads. Resolving only the registry
  // would look entirely correct in a debugger and change nothing anybody can
  // hear.
  const auto provider = samples_.provider();
  std::unordered_map<Id, std::pair<double, double>> resolved; // id -> {fileNote, hz}
  std::array<int, 8> tally{};
  // Samples decided to be noise placeholders without a pipe to judge by: a
  // pipe that plays one looks again with its own pitch in hand (#154).
  std::unordered_set<Id> placeholders;

  auto resolveOne = [&](SampleRef& ref, double pipeHz = 0.0) {
    const auto it = resolved.find(ref.sampleId);
    if (it != resolved.end() && !(pipeHz > 0.0 && placeholders.count(ref.sampleId) != 0)) {
      ref.fileMidiNote = it->second.first;
      ref.resolvedPitchHz = it->second.second;
      return;
    }
    SamplePitchInputs in;
    in.methodCode = ref.pitchMethodCode;
    in.exactHz = ref.pitchHz;
    in.normalMidiNote = ref.midiNote;
    in.rankBasePitch64ftHarmonicNum = ref.rankBasePitch64ftHarmonicNum;
    in.fileName = ref.fileName;
    in.pipeNominalHz = pipeHz;
    if (const SampleBuffer* buf = provider(ref.sampleId))
      in.fileMidiNote = buf->fileMidiNote;

    const SamplePitchResult r =
        resolveSamplePitch(in, 440.0, model_.basePitchHz);
    ref.fileMidiNote = in.fileMidiNote;
    ref.resolvedPitchHz = r.hz;
    // A pipe's own judgement stays with that pipe; the shared answer is the
    // one made without one.
    if (pipeHz > 0.0 && it != resolved.end()) return;
    resolved.emplace(ref.sampleId,
                     std::make_pair(in.fileMidiNote, r.hz));
    if (r.route == PitchRoute::NoisePlaceholder) placeholders.insert(ref.sampleId);
    const auto slot = static_cast<size_t>(r.route);
    if (slot < tally.size()) ++tally[slot];
  };

  for (auto& [id, ref] : model_.samples) resolveOne(ref);
  for (auto& [rankId, rank] : model_.ranks)
    for (auto& pipe : rank.pipes) {
      // The pitch the pipe is keyed to, untempered: enough to tell a real
      // declaration of the base pitch from a placeholder.
      const double base = model_.basePitchHz > 0.0 ? model_.basePitchHz : 440.0;
      const int harm = pipe.basePitch64ftHarmonicNum > 0 ? pipe.basePitch64ftHarmonicNum : 8;
      const double pipeHz = rank.isNoise || pipe.midiNote < 0
                                ? 0.0
                                : base * std::pow(2.0, (pipe.midiNote - 69) / 12.0) * harm / 8.0;
      for (auto& layer : pipe.layers) {
        for (auto& a : layer.attacks) resolveOne(a.sample, pipeHz);
        for (auto& rel : layer.releases) resolveOne(rel.sample, pipeHz);
      }
    }

  // Said out loud, because a set resolving entirely by file name or not at
  // all is a set whose pitch nobody has checked -- and it sounds plausible
  // right up to the first rank that reuses one recording across pipes.
  juce::String line = "pitch: samples resolved by";
  for (size_t i = 0; i < tally.size(); ++i) {
    if (tally[i] == 0) continue;
    line << " " << pitchRouteName(static_cast<PitchRoute>(i)) << "="
         << tally[i];
  }
  juce::Logger::writeToLog(line);
}

double MasterpieceProcessor::playbackRatioFor(const Pipe& pipe,
                                             const SampleRef& sample,
                                             const PipeLayer& layer) const {
  // What this pipe must sound at. Two modes: at the original instrument's own
  // pitch (which is why anyone samples a particular organ), or at a tempered
  // pitch derived from the keyboard. A pipe with no declared original pitch
  // falls back to the tempered path rather than going silent.
  double targetHz = 0.0;
  if (graph_.engineSwitch.playAtOriginalOrganPitch &&
      pipe.originalOrganPitchHz > 0.0) {
    targetHz = pipe.originalOrganPitchHz;
  } else {
    targetHz = pipeTargetHz(pipe.midiNote, pipe.basePitch64ftHarmonicNum,
                            model_.basePitchHz, pipe.baseTuningDeviationCents,
                            activeTuning(), 0);
  }
  // The player's pitch moves the whole organ, in either mode.
  targetHz *= pitchFactor();

  // Detuning rides on the target, not on the recorded pitch: it is a change
  // to what this pipe should sound, not a claim about what the file holds.
  // Applied to the original-organ path too — an instrument left out of tune
  // was out of tune at its own pitch as well. Zero under simpleWavOnly.
  if (layer.pitchControlId != 0)
    targetHz = detunedTargetHz(targetHz, detuneControlValue(layer),
                               detuneCentre(layer),
                               layer.pitchSensitivityHzPerUnit);

  // What the file actually holds. An organ sample is recorded from its own
  // pipe, so this is normally close to targetHz and the ratio near 1.0 —
  // resampling only trims it into tune. Getting this wrong is not subtle: use
  // the organ's reference pitch instead of the sample's and every note but A
  // plays at the wrong speed, collapsing the rank toward one pitch.
  // Decided at load time by resolveSamplePitches(), which obeys the set's own
  // Pitch_SpecificationMethodCode instead of guessing from whichever field
  // happens to be filled in. Zero means the set declares no pitch at all.
  double recordedHz = sample.resolvedPitchHz;
  if (recordedHz <= 0.0) {
    // Nothing declares a pitch: assume the sample was recorded at the pipe's
    // own nominal pitch, which makes the ratio 1.0 and is what an untagged
    // organ sample set means.
    return 1.0;
  }
  return playbackRatio(targetHz, recordedHz);
}

Id MasterpieceProcessor::keyboardForChannel(int channel, int deviceId) const {
  // A binding that names this console beats one that does not, so a rig can be
  // set up loosely and one keyboard pinned exactly.
  Id loose = 0;
  for (const auto& b : midiMap_.keyboardBindings()) {
    if (b.channel != 0 && b.channel != channel) continue;
    if (b.deviceId == deviceId && b.deviceId != MidiDeviceMap::kAnyDevice)
      return b.keyboardId;
    if (b.deviceId == MidiDeviceMap::kAnyDevice && loose == 0)
      loose = b.keyboardId;
  }
  if (loose != 0) return loose;

  // Hauptwerk's own default: the assignment code IS the channel. Code 1 is the
  // pedal, 2 the first manual, and so on, so an organ plays the way its author
  // expected before anyone maps anything.
  for (Id kb : couplers_.inputKeyboards())
    if (couplers_.assignmentCodeFor(kb) == channel) return kb;

  // A channel nothing claims plays the default manual only while nothing says
  // what the channels are: an organ that declares no channels, played before
  // any are set. Once the player has set some, or the organ declares its own,
  // a channel neither names is silent -- a console's piston buttons sending
  // notes on a channel of their own played the manual (a Buckeburg report).
  if (!midiMap_.keyboardBindings().empty()) return 0;
  for (Id kb : couplers_.inputKeyboards())
    if (couplers_.assignmentCodeFor(kb) > 0) return 0;
  return fallbackKeyboard_;
}

int MasterpieceProcessor::channelForKeyboard(Id keyboardId) const {
  // A drawn manual answers to the channel of the keyboard it stands for: the
  // player assigns channels to the keyboards they play, not to the pictures
  // of them. Asked of the drawn one directly, a console whose Manual I was
  // moved to channel 1 lit its pedalboard, which kept the organ's default 1.
  const Id input = couplers_.inputKeyboardFor(keyboardId);
  const Id played = input != 0 ? input : keyboardId;
  for (const auto& b : midiMap_.keyboardBindings())
    if (b.keyboardId == played && b.channel > 0) return b.channel;
  for (const auto& b : midiMap_.keyboardBindings())
    if (b.keyboardId == keyboardId && b.channel > 0) return b.channel;
  int code = couplers_.assignmentCodeFor(played);
  if (code < 1) code = couplers_.assignmentCodeFor(keyboardId);
  if (code >= 1 && code <= 16) return code;
  return 1;
}

namespace {
// A drawn manual that leads to none of the keyboards a player can reach -- an
// organ whose wiring we could not follow -- keeps the plain channel lookup.
// Stricter rules would leave it dead to the mouse and never lit.
bool reachable(const CouplerMatrix& couplers, Id keyboardId) {
  if (couplers.inputKeyboardFor(keyboardId) != 0) return true;
  const auto& inputs = couplers.inputKeyboards();
  return std::find(inputs.begin(), inputs.end(), keyboardId) != inputs.end();
}
} // namespace

int MasterpieceProcessor::litChannelForKeyboard(Id keyboardId) const {
  if (!reachable(couplers_, keyboardId)) return channelForKeyboard(keyboardId);
  const Id input = couplers_.inputKeyboardFor(keyboardId);
  const Id played = input != 0 ? input : keyboardId;
  const int channel = channelForKeyboard(keyboardId);
  // Mapped to it by the player: its notes are this keyboard's, from whichever
  // console the binding names.
  bool claimed = false;
  for (const auto& b : midiMap_.keyboardBindings()) {
    if (b.channel != 0 && b.channel != channel) continue;
    if (b.keyboardId == played || b.keyboardId == keyboardId) return channel;
    claimed = true;
  }
  // The organ's own default, unless the player has given the channel away.
  // A mapped channel plays only what is mapped to it (keyboardForChannel), so
  // a keyboard sitting on it by default hears none of its notes.
  if (claimed) return 0;
  const Id reached = keyboardForChannel(channel, MidiDeviceMap::kAnyDevice);
  return reached == played || reached == keyboardId ? channel : 0;
}

int MasterpieceProcessor::clickChannelForKeyboard(Id keyboardId) const {
  if (!reachable(couplers_, keyboardId)) return channelForKeyboard(keyboardId);
  // A click arrives with no console attached, so it is routed the way
  // keyboardForChannel routes a message from no particular device.
  const Id input = couplers_.inputKeyboardFor(keyboardId);
  const Id played = input != 0 ? input : keyboardId;
  auto reaches = [&](int channel) {
    const Id k = keyboardForChannel(channel, MidiDeviceMap::kAnyDevice);
    return k == played || k == keyboardId;
  };
  const int preferred = channelForKeyboard(keyboardId);
  if (reaches(preferred)) return preferred;
  // A keyboard mapped only for one console, or left without a channel of its
  // own: any channel that does reach it, so its drawn keys still play it.
  for (int channel = 1; channel <= 16; ++channel)
    if (reaches(channel)) return channel;
  return 0;
}

void MasterpieceProcessor::startNote(int channel, int midiNote, int velocity) {
  // Keyed on the key the player pressed, sounded on the transposed one: the
  // release finds its note by the key, so changing the transposer while a
  // chord is held cannot leave any of it sounding.
  const Id keyboard = keyboardForChannel(channel, noteDeviceId_);
  if (keyboard == 0) {
    if (logMidi_.load(std::memory_order_acquire))
      juce::Logger::writeToLog("midi:     channel " + juce::String(channel) +
                               " plays no manual: not set in Settings > MIDI, nor by the organ");
    return;
  }
  startNoteOnKeyboard(keyboard, noteKey(channel, midiNote), transposed(midiNote), velocity);
}

void MasterpieceProcessor::stopNote(int channel, int midiNote, int velocity) {
  stopNoteByKey(noteKey(channel, midiNote), velocity);
}

void MasterpieceProcessor::releaseChannel(int channel, int deviceId) {
  // A message from a device releases only what that device played; one that
  // came with no device (the host's merged buffer) speaks for the channel.
  auto fromHere = [channel, deviceId](int ch, int dev) {
    return ch == channel && (deviceId == MidiDeviceMap::kAnyDevice || dev == deviceId);
  };
  std::vector<int> keys;
  for (const auto& [key, held] : soundingNotes_)
    if (fromHere(held.channel, held.device)) keys.push_back(key);
  for (const auto& [key, origin] : heldKeySwitchOrigin_)
    if (fromHere(origin.first, origin.second)) keys.push_back(key);
  for (int key : keys) stopNoteByKey(key, 0);
}

void MasterpieceProcessor::stopNoteByKey(int key, int velocity) {
  if (const auto sent = keySends_.find(key); sent != keySends_.end()) {
    sendKey(sent->second.first, sent->second.second, velocity, false);
    keySends_.erase(sent);
  }
  heldKeySwitchOrigin_.erase(key);
  if (const auto ks = heldKeySwitches_.find(key); ks != heldKeySwitches_.end()) {
    const Id switchId = ks->second;
    heldKeySwitches_.erase(ks);
    palletVelocity_ = velocity;
    setSwitchEngaged(switchId, false);
  }
  const auto it = soundingNotes_.find(key);
  if (it == soundingNotes_.end()) return;
  NoteRelease rel;
  rel.velocity = velocity;
  voices_.noteOff(it->second.id, rel);
  soundingNotes_.erase(it);
}

bool MasterpieceProcessor::startVoicesForKey(Id keyboard, int midiNote,
                                             int velocity, uint64_t noteId,
                                             const std::unordered_set<Id>& stops) {
  bool anyStarted = false;
  // Which divisions this key actually reaches, at which pitches. This is where
  // couplers live: a drawn "Great to Pedal" is an edge of the key-flow graph
  // that is only walkable while its switch is engaged.
  expandScratch_.clear();
  couplers_.expandInto(static_cast<int>(keyboard), midiNote,
                       static_cast<float>(velocity) / 127.0f, engagedSwitches_,
                       keyFlow_, expandScratch_);

  for (const ExpandedNote& reached : expandScratch_) {
    const int divisionId = reached.divisionId;
    resolveScratch_.clear();
    const auto pipes =
        resolvePipes(model_, divisionId, reached.midiNote, stops, &engagedSwitches_);
    for (const auto& rp : pipes) {
      const auto rankIt = model_.ranks.find(rp.rankId);
      if (rankIt == model_.ranks.end()) continue;
      for (const auto& pipe : rankIt->second.pipes) {
        if (pipe.pipeId != rp.pipeId) continue;
        if (startPipeLayers(pipe, rp.rankId, reached.midiNote, velocity, noteId))
          anyStarted = true;
        break;
      }
    }
  }

  return anyStarted;
}

// One pipe, every layer of it, under `noteId`. Shared by the key path, which
// reaches pipes through the stops, and the pallet path, which reaches them
// through the organ's own switch wiring: a pipe sounds the same whichever way
// it was asked for.
bool MasterpieceProcessor::startPipeLayers(const Pipe& pipe, Id rankId,
                                           int midiNote, int velocity,
                                           uint64_t noteId) {
  bool anyStarted = false;
  for (const auto& layer : pipe.layers) {
    NoteStrike strike;
    strike.velocity = velocity;
    strike.timeSinceCloseMs = voices_.msSincePipeClosed(pipe.pipeId);
    const int attackIndex = selectAttack(layer, strike);
    if (attackIndex < 0) continue; // this layer stays silent, by design

    VoiceStart vs;
    vs.pipe = &pipe;
    vs.layer = &layer;
    vs.attackIndex = attackIndex;
    vs.attackId = layer.attacks[static_cast<size_t>(attackIndex)].id;
    vs.velocity = velocity;
    // Pitch comes from the solver, against the pitch the FILE holds —
    // not against the organ's reference A. See playbackRatioFor().
    vs.ratio = playbackRatioFor(
        pipe, layer.attacks[static_cast<size_t>(attackIndex)].sample,
        layer);
    vs.gain = juce::Decibels::decibelsToGain(
                  static_cast<float>(layer.gainDb), -100.0f) *
              layerLevel(layer);

    // How hard the key was struck. The organ states the attenuation at the
    // softest touch; full velocity is unattenuated. Inverted, the sense
    // swaps. Applied here and not per sample: a pipe keeps the level it
    // began with until the next strike, which is what an organ does.
    //
    // The MAGNITUDE is the attenuation: every set stores one constant for
    // its whole pipework, and while some write it +5 dB others write -5 or
    // -6 (Alessandria +5, Giubiasco -6, Cracow -10). Read as a signed gain
    // the negative sets would get LOUDER when played softly, which no
    // tracker organ does; the field's own name is MaxAttenuation.
    if (layer.velSensMaxAttenDb != 0.0) {
      const double v01 = juce::jlimit(0.0, 1.0, static_cast<double>(velocity) / 127.0);
      const double attn = layer.invertVelocitySens ? v01 : 1.0 - v01;
      vs.gain *= juce::Decibels::decibelsToGain(
          static_cast<float>(-std::fabs(layer.velSensMaxAttenDb) * attn), -100.0f);
    }

    // The player's own voicing, on top of what the organ declares.
    // Gain and tuning only: they are a multiply and a ratio at note-on
    // and cost nothing per sample, so they apply even with the DSP
    // switch off. Brightness and balance need per-voice filtering and
    // are stored but NOT applied — see PipeVoicing.
    //
    // The empty() guard is the point of the whole lookup: an organ
    // nobody has voiced must not pay two hash lookups for every pipe of
    // every chord.
    if (!voicing_.live().empty()) {
      const PipeVoicing pv =
          voicing_.live().effective(rankId, pipe.pipeId);
      if (pv.gainDb != 0.0f)
        vs.gain *= juce::Decibels::decibelsToGain(pv.gainDb, -100.0f);
      if (pv.tuningCents != 0.0f)
        vs.ratio *= centsRatio(pv.tuningCents);
    }

    // A layer may declare its own loop, overriding the audio file's.
    vs.loopStartOverride = layer.loopStartFrames;
    vs.loopEndOverride = layer.loopEndFrames;
    vs.busIndex = busForPipe(pipe.pipeId);
    vs.mixBus = mixBusForPipe(rankId, midiNote);
    {
      const auto wIt = pipeWindIndex_.find(pipe.pipeId);
      vs.windIndex = wIt == pipeWindIndex_.end() ? -1 : wIt->second;
      // What this pipe costs its chest. An organ that declares nothing
      // still has to sag under a tutti, or the model is decorative.
      vs.windFlowKgPerSec =
          pipe.windMassFlowKgPerSec > 0.0
              ? static_cast<float>(pipe.windMassFlowKgPerSec)
              : 0.0005f;

      // Which tremulant reaches this pipe, and how far it moves it. The
      // organ states the depth per pipe, so a flute and a reed on the
      // same chest wobble by different amounts — and the LAYER trims that
      // depth again, which is how one stop on a chest can be left nearly
      // steady while its neighbour shakes.
      const auto tm = model_.tremulantPipes.find(pipe.pipeId);
      if (tm != model_.tremulantPipes.end()) {
        const auto ti = tremIndexOf_.find(tm->second.tremulantId);
        if (ti != tremIndexOf_.end()) {
          vs.tremIndex = ti->second;
          // The pipe's and the layer's adjustments to the waveform's own
          // depth, as a linear swing about unity (tremulantAmpSwing); and
          // percent of a semitone to semitones.
          vs.tremAmpDepth = static_cast<float>(
              tremulantAmpSwing(tm->second.ampDepthDb + layer.tremAmpDepthAdjustDb));
          vs.tremPitchDepth =
              tm->second.pitchDepthPct / 100.0 *
              juce::jlimit(0.0, 4.0, layer.tremPitchDepthAdjustPct / 100.0);
        }
      }
    }
    // LoopCrossfadeLengthInSrcSampleMs is stated against the SOURCE
    // sample rate, so convert with the file's rate, not the engine's.
    vs.loopCrossfadeFrames = static_cast<int>(
        layer.loopCrossfadeMs * 0.001 * sampleRate_);
    if (voices_.startVoice(vs, noteId) >= 0) anyStarted = true;
  }
  return anyStarted;
}

void MasterpieceProcessor::startNoteOnKeyboard(Id keyboard, int noteKeyId,
                                               int midiNote, int velocity) {
  // A manual that sends its keys on: before anything else, so a key played
  // with no stop drawn still reaches the module it drives.
  if (midiOut_ != nullptr && midiMap_.hasSends(MidiTargetKind::Keyboard, keyboard)) {
    if (const auto held = keySends_.find(noteKeyId); held != keySends_.end())
      sendKey(held->second.first, held->second.second, 0, false);
    sendKey(keyboard, midiNote, velocity, true);
    keySends_[noteKeyId] = {keyboard, midiNote};
  }
  // The key is a switch, too, when the organ says so. Engaging it lets the
  // wiring open whatever pallets it reaches -- which is how an organ with no
  // StopRank plays at all, and how every organ's key action sounds. This comes
  // before the no-stops check: key action speaks with nothing drawn.
  if (!keySwitchByKey_.empty()) {
    const auto ks = keySwitchByKey_.find(static_cast<int>(keyboard) * 256 + midiNote);
    if (ks != keySwitchByKey_.end()) {
      palletVelocity_ = velocity;
      heldKeySwitches_[noteKeyId] = ks->second;
      heldKeySwitchOrigin_[noteKeyId] = {noteChannel_, noteDeviceId_};
      setSwitchEngaged(ks->second, true);
    }
  }

  // A key that is already down is being struck again. Let go of it first.
  //
  // soundingNotes_ holds ONE note id per key, and the last line of this
  // function overwrites it. Without this the previous id is simply lost: its
  // voices are still running, nothing holds their handle any more, and no
  // note-off will ever reach them. A pipe has no decay, so each orphan sounds
  // until the organ is unloaded.
  //
  // It went unnoticed because it needs a repeated note to happen at all. One
  // note is perfect; a piece full of them silts up as it plays, which is what
  // a toccata sounds like when its rests are louder than its chords.
  //
  // A real key cannot be pressed twice without being released, so releasing
  // the old note is also what the instrument would do.
  const auto already = soundingNotes_.find(noteKeyId);
  if (already != soundingNotes_.end()) {
    voices_.noteOff(already->second.id, NoteRelease{});
    soundingNotes_.erase(already);
  }

  const uint64_t noteId = nextNoteId_++;

  const bool anyStarted =
      !engagedStops_.empty() &&
      startVoicesForKey(keyboard, midiNote, velocity, noteId, engagedStops_);

  // Held whether or not anything sounds yet. A key pressed before its stop is
  // drawn speaks the moment the stop is drawn, as on a real organ; recorded
  // only when a pipe had started, such a key was unknown to the code that
  // follows stops for held notes, so drawing the stop did nothing (#120: "it
  // isn't always possible to change stops while holding a note").
  soundingNotes_[noteKeyId] =
      HeldNote{noteId, keyboard, midiNote, velocity, noteChannel_, noteDeviceId_};

  if (engagedStops_.empty()) {
    if (logMidi_.load(std::memory_order_acquire))
      juce::Logger::writeToLog(
          "midi:     NOTHING PLAYS: no stop is drawn, so no pipe can sound");
    return;
  }

  if (logMidi_.load(std::memory_order_acquire)) {
    // Which divisions, and what is drawn on them: "no pipe answered" is either
    // nothing drawn on THIS division or a division whose stops resolve to no
    // pipe at this note, and those are different faults.
    juce::String divs;
    for (const auto& r : expandScratch_)
      divs << (divs.isEmpty() ? "" : ",") << juce::String(r.divisionId) << "@"
           << juce::String(r.midiNote);
    juce::String drawnOn;
    for (Id sid : engagedStops_) {
      const auto it = model_.stops.find(sid);
      if (it != model_.stops.end())
        drawnOn << (drawnOn.isEmpty() ? "" : ",") << juce::String(it->second.divisionId);
    }
    juce::Logger::writeToLog(
        "midi:     keyboard=" + juce::String(static_cast<int>(keyboard)) +
        " note=" + juce::String(midiNote) + " -> division(s) " + divs +
        "; " + juce::String(static_cast<int>(engagedStops_.size())) +
        " stop(s) drawn on division(s) " + drawnOn + "; " +
        (anyStarted ? "SOUNDING" : "NOTHING PLAYS: no pipe answered"));
  }
}

void MasterpieceProcessor::reflowHeldNotes() {
  const auto same = [](const ExpandedNote& a, const ExpandedNote& b) {
    return a.divisionId == b.divisionId && a.midiNote == b.midiNote;
  };
  const auto contains = [&same](const std::vector<ExpandedNote>& v, const ExpandedNote& n) {
    return std::any_of(v.begin(), v.end(), [&](const ExpandedNote& x) { return same(x, n); });
  };
  for (const auto& [key, held] : soundingNotes_) {
    (void)key;
    const float vel = static_cast<float>(held.velocity) / 127.0f;
    reflowBefore_.clear();
    couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote, vel, heldFlowBefore_,
                         keyFlow_, reflowBefore_);
    reflowAfter_.clear();
    couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote, vel, engagedSwitches_,
                         keyFlow_, reflowAfter_);
    // Newly reached: its pipes speak, under the key's own note id so the
    // note-off still to come releases them with the rest.
    for (const ExpandedNote& reached : reflowAfter_) {
      if (contains(reflowBefore_, reached)) continue;
      for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                         engagedStops_, &engagedSwitches_)) {
        const auto rankIt = model_.ranks.find(rp.rankId);
        if (rankIt == model_.ranks.end()) continue;
        for (const auto& pipe : rankIt->second.pipes)
          if (pipe.pipeId == rp.pipeId) {
            startPipeLayers(pipe, rp.rankId, reached.midiNote, held.velocity, held.id);
            break;
          }
      }
    }
    // No longer reached: those pipes let go; the key is still down.
    for (const ExpandedNote& reached : reflowBefore_) {
      if (contains(reflowAfter_, reached)) continue;
      for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                         engagedStops_, &heldFlowBefore_))
        voices_.noteOffPipe(held.id, rp.pipeId, NoteRelease{});
    }
  }
}

// A stop moved while keys are down. On a real organ the slider admits wind to
// a rank that is already being asked for, so the pipe speaks at once and stops
// at once when it is pushed in -- without the key moving. Reported by a player:
// "if I'm playing a note and turn on a stop the pipe doesn't play until I play
// the note again. The note doesn't stop when I turn the stop off."
void MasterpieceProcessor::applyStopChangeToHeldNotes() {
  if (soundingNotes_.empty()) {
    appliedStops_ = engagedStops_;
    return;
  }

  // What was drawn, and what was pushed in, since the last block.
  stopDiffScratch_.clear();
  for (Id s : engagedStops_)
    if (appliedStops_.count(s) == 0) stopDiffScratch_.push_back(s);
  if (!stopDiffScratch_.empty()) {
    stopSetScratch_.clear();
    stopSetScratch_.insert(stopDiffScratch_.begin(), stopDiffScratch_.end());
    // Started under the key's own note id, so the note-off still to come
    // releases these along with the rest of the note.
    for (const auto& [key, held] : soundingNotes_) {
      (void)key;
      startVoicesForKey(held.keyboard, held.midiNote, held.velocity, held.id,
                        stopSetScratch_);
    }
  }

  stopDiffScratch_.clear();
  for (Id s : appliedStops_)
    if (engagedStops_.count(s) == 0) stopDiffScratch_.push_back(s);
  if (!stopDiffScratch_.empty()) {
    stopSetScratch_.clear();
    stopSetScratch_.insert(stopDiffScratch_.begin(), stopDiffScratch_.end());
    for (const auto& [key, held] : soundingNotes_) {
      (void)key;
      expandScratch_.clear();
      couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote,
                           static_cast<float>(held.velocity) / 127.0f,
                           engagedSwitches_, keyFlow_, expandScratch_);
      for (const ExpandedNote& reached : expandScratch_) {
        const auto pipes = resolvePipes(model_, reached.divisionId,
                                        reached.midiNote, stopSetScratch_, &engagedSwitches_);
        // Only this rank's pipes let go; the rest of the note plays on, and
        // the key is still down.
        for (const auto& rp : pipes)
          voices_.noteOffPipe(held.id, rp.pipeId, NoteRelease{});
      }
    }
  }

  appliedStops_ = engagedStops_;
}

void MasterpieceProcessor::setStopEngaged(Id stopId, bool engaged) {
  const AudioLock audio(*this);
  if (!applyingPistons_) player_.registrationMoved();
  if (engaged) engagedStops_.insert(stopId);
  else engagedStops_.erase(stopId);
  stopsChanged_.store(true, std::memory_order_release);

  // Drawing a stop is a physical act on a real console, and sample sets record
  // it. Move the knob a player would move, not the internal node it feeds:
  // the node has no wire back, so engaging it directly leaves the knob out and
  // a general cancel with nothing to push.
  const auto it = model_.stops.find(stopId);
  if (it != model_.stops.end() && it->second.controllingSwitchId != 0)
    setSwitchEngaged(playerSwitchFor(it->second.controllingSwitchId), engaged);

  // And the drawn knob, when the wiring did not lead to one. Without this the
  // stop speaks and the console shows nothing moving, which reads as a stop
  // that failed to engage.
  const auto knob = stopKnob_.find(stopId);
  if (knob != stopKnob_.end()) setSwitchEngaged(knob->second, engaged);
}

bool MasterpieceProcessor::switchEngaged(Id switchId) const {
  return engagedSwitches_.count(switchId) != 0;
}

// ------------------------------------------------------ load estimates

std::vector<SampleLibrary::ShapeJob> MasterpieceProcessor::sampleShapeJobs() const {
  // A packaged organ's samples are in its archives; their saved index is
  // enough to know each file's size.
  std::unique_ptr<OrganArchive> archive;
  const std::string archivePath = readArchiveMarker(organRootDir_);
  if (!archivePath.empty()) {
    archive = std::make_unique<OrganArchive>();
    const std::string index = juce::File(juce::String::fromUTF8(organRootDir_.c_str()))
                                  .getChildFile("archive-index.txt")
                                  .getFullPathName()
                                  .toStdString();
    if (!archive->loadIndex(index)) archive.reset();
  }
  return samples_.shapeJobs(model_, organRootDir_, archive.get());
}

bool MasterpieceProcessor::stopDrawn(Id stopId) const {
  auto drawn = [this](Id sw) {
    const auto it = model_.switches.find(sw);
    return it != model_.switches.end() && it->second.dispInstanceId != 0 && it->second.clickable;
  };
  bool anyDrawn = false;
  for (const auto& [id, sw] : model_.switches) {
    (void)sw;
    if (drawn(id)) {
      anyDrawn = true;
      break;
    }
  }
  if (!anyDrawn) return true;
  if (stopKnob_.count(stopId) != 0) return true;
  const auto it = model_.stops.find(stopId);
  if (it == model_.stops.end() || it->second.controllingSwitchId == 0) return false;
  const Id knob = playerSwitchFor(it->second.controllingSwitchId);
  if (!drawn(knob)) return false;
  // A stop sounding from a coupler's or a tremulant's own knob is that
  // knob's noise -- Jak's CouplerEffect_1002 and TremulantEffect_1710 --
  // not a stop of its own.
  for (const auto& ka : model_.keyActions)
    if (ka.conditionSwitchId != 0 && playerSwitchFor(ka.conditionSwitchId) == knob) return false;
  for (const auto& [id, div] : model_.divisions) {
    (void)id;
    for (const auto& ka : div.keyActions)
      if (ka.conditionSwitchId != 0 && playerSwitchFor(ka.conditionSwitchId) == knob) return false;
  }
  for (const auto& [id, t] : model_.tremulants) {
    (void)id;
    if (t.controllingSwitchId != 0 && playerSwitchFor(t.controllingSwitchId) == knob) return false;
  }
  return true;
}

std::vector<Id> MasterpieceProcessor::samplesOfRanks(const std::vector<Id>& rankIds) const {
  std::vector<Id> out;
  for (Id rankId : rankIds) {
    const auto rit = model_.ranks.find(rankId);
    if (rit == model_.ranks.end()) continue;
    for (const auto& pipe : rit->second.pipes)
      for (const auto& layer : pipe.layers) {
        for (const auto& a : layer.attacks)
          if (a.sample.sampleId != 0) out.push_back(a.sample.sampleId);
        for (const auto& r : layer.releases)
          if (r.sample.sampleId != 0) out.push_back(r.sample.sampleId);
      }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<Id> MasterpieceProcessor::samplesOfStop(Id stopId) const {
  std::vector<Id> out;
  const auto it = model_.stops.find(stopId);
  if (it == model_.stops.end()) return out;
  for (const auto& e : it->second.ranks) {
    const auto rit = model_.ranks.find(e.rankId);
    if (rit == model_.ranks.end()) continue;
    for (const auto& pipe : rit->second.pipes)
      for (const auto& layer : pipe.layers) {
        for (const auto& a : layer.attacks)
          if (a.sample.sampleId != 0) out.push_back(a.sample.sampleId);
        for (const auto& r : layer.releases)
          if (r.sample.sampleId != 0) out.push_back(r.sample.sampleId);
      }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// ------------------------------------------------------------ tuning

bool MasterpieceProcessor::setTemperament(const std::string& choice,
                                          std::string* error, bool remember) {
  const AudioLock audio(*this);
  const Temperament* t = nullptr;
  std::string why;
  if (choice.rfind("scala:", 0) == 0) {
    const juce::File file(juce::String(choice.substr(6)));
    Temperament scala;
    if (!file.existsAsFile()) why = "the Scala file is missing";
    else if (parseScala(file.loadFileAsString().toStdString(), scala, why)) {
      // Reuse a copy already held, so switching back and forth does not grow
      // the list for the life of the organ.
      for (const auto& held : scalaTunings_)
        if (held.name == scala.name && held.centsOffset12 == scala.centsOffset12) t = &held;
      if (t == nullptr) {
        scalaTunings_.push_back(std::move(scala));
        t = &scalaTunings_.back();
      }
    }
  } else if (!choice.empty()) {
    t = findTemperament(choice);
    if (t == nullptr) why = "no temperament called " + choice;
  }
  if (!why.empty()) {
    if (error != nullptr) *error = why;
    juce::Logger::writeToLog("tuning: " + juce::String(why) + "; keeping the organ's own");
    return false;
  }
  temperamentChoice_ = choice;
  playerTuning_.store(t, std::memory_order_release);
  if (remember) markSettingsDirty();
  return true;
}

void MasterpieceProcessor::setReleaseLogging(bool on) {
  if (on == releaseLogging()) return;
  voices_.setReleaseLogging(on);
  if (!on) {
    releaseLog_.reset();
    juce::Logger::writeToLog("release: logging off");
    return;
  }
  struct Drain : juce::Timer {
    explicit Drain(MasterpieceProcessor& p) : proc(p) {}
    void timerCallback() override {
      for (const auto& line : proc.takeReleaseLog()) juce::Logger::writeToLog(line);
    }
    MasterpieceProcessor& proc;
  };
  releaseLog_ = std::make_unique<Drain>(*this);
  releaseLog_->startTimer(250);
  juce::Logger::writeToLog("release: logging on. Every key release follows.");
}

void MasterpieceProcessor::keepLoadingChoiceForSession() {
  if (loadedOdf_ == juce::File()) return;
  LoadingChoice c;
  c.organ = loadedOdf_;
  c.storage = samples_.storage();
  c.mono = samples_.loadMono();
  c.stream = samples_.streamReleases();
  c.streamHead = samples_.streamHeadFrames();
  c.streamHeadPercent = samples_.streamHeadPercent();
  c.preload = preloadHead_;
  c.engine = graph_.engineSwitch;
  sessionLoading_ = c;
}

int MasterpieceProcessor::lastControllerValue(const MidiSource& source) const {
  if (source.kind != MidiSourceKind::ControlChange || source.number < 0 || source.number >= 128) return -1;
  if (source.channel >= 1 && source.channel <= 16)
    return lastControllerOnChannel_[static_cast<size_t>((source.channel - 1) * 128 + source.number)].load(
               std::memory_order_relaxed) - 1;
  return lastController_[static_cast<size_t>(source.number)].load(std::memory_order_relaxed) - 1;
}

int MasterpieceProcessor::setControlPedalEnd(Id controlId, bool open) {
  const MidiBinding* found = midiMap_.bindingFor(MidiTargetKind::ContinuousControl, controlId);
  if (found == nullptr) return -1;
  const int raw = lastControllerValue(found->source);
  if (raw < 0) return -1;
  MidiBinding b = *found;
  const int lo = std::min(b.lowValue, b.highValue), hi = std::max(b.lowValue, b.highValue);
  int closedAt = b.invert ? hi : lo;
  int openAt = b.invert ? lo : hi;
  (open ? openAt : closedAt) = raw;
  if (closedAt == openAt) return -1;
  b.lowValue = std::min(closedAt, openAt);
  b.highValue = std::max(closedAt, openAt);
  b.invert = closedAt > openAt;
  if (!midiMap_.replace(b)) return -1;
  saveMidiMap();
  return raw;
}

bool MasterpieceProcessor::resetControlPedalRange(Id controlId) {
  const MidiBinding* found = midiMap_.bindingFor(MidiTargetKind::ContinuousControl, controlId);
  if (found == nullptr) return false;
  MidiBinding b = *found;
  b.lowValue = 0;
  b.highValue = 127;
  b.invert = false;
  if (!midiMap_.replace(b)) return false;
  saveMidiMap();
  return true;
}

MasterpieceProcessor::AudioLoad MasterpieceProcessor::takeAudioLoad() {
  AudioLoad load;
  load.blocks = audioBlocks_.load(std::memory_order_relaxed);
  load.late = lateBlocks_.load(std::memory_order_relaxed);
  load.worstPercent = worstBlockPermille_.exchange(0, std::memory_order_relaxed) / 10.0;
  return load;
}

std::vector<std::string> MasterpieceProcessor::takeReleaseLog() {
  std::vector<VoiceEngine::ReleaseEvent> events;
  const int64_t dropped = voices_.takeReleaseEvents(events);
  std::vector<std::string> lines;
  auto fileOf = [this](Id id) -> std::string {
    const auto it = model_.samples.find(id);
    return it != model_.samples.end() ? it->second.fileName : "sample " + std::to_string(id);
  };
  auto seconds = [](int64_t frames, double rate) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f s", rate > 0.0 ? static_cast<double>(frames) / rate : 0.0);
    return std::string(b);
  };
  for (const auto& e : events) {
    std::string l = "release: pipe " + std::to_string(e.pipeId) + ", held " +
                    std::to_string(e.heldMs) + " ms, velocity " + std::to_string(e.velocity) +
                    " | attack " + fileOf(e.attackSampleId) + " at " +
                    seconds(e.attackFrame, e.attackRate) +
                    (e.attackLooping ? " (in its loop)" : " (before its loop)") + " | ";
    if (e.releaseSampleId == 0) {
      l += e.releaseCount == 0 ? "the pipe has no releases"
                               : "none of its " + std::to_string(e.releaseCount) + " releases matched";
      l += e.chosen >= 0 ? " (release sample not loaded)" : "";
      l += ": the attack fades out";
    } else {
      char xf[32];
      std::snprintf(xf, sizeof xf, "%.1f ms", e.crossfadeMs);
      l += "release " + std::to_string(e.chosen + 1) + " of " + std::to_string(e.releaseCount) +
           ": " + fileOf(e.releaseSampleId) + ", " + seconds(e.releaseFrames, e.releaseRate) +
           " at " + std::to_string(static_cast<int>(e.releaseRate)) + " Hz, from " +
           seconds(e.startFrame, e.releaseRate);
      if (e.cueWanted) l += e.cueFound ? " (its release marker)" : " (marker asked for, none in the file)";
      l += " | crossfade " + std::string(xf);
      l += e.streamed ? " | streamed, " + seconds(e.residentFrames, e.releaseRate) + " resident"
                      : " | resident";
    }
    lines.push_back(std::move(l));
  }
  if (dropped > 0)
    lines.push_back("release: " + std::to_string(dropped) + " more not recorded (too many at once)");
  const int64_t underruns = voices_.streamUnderruns();
  if (underruns > loggedUnderruns_)
    lines.push_back("release: " + std::to_string(underruns - loggedUnderruns_) +
                    " stream underrun(s): a streamed tail was not read from disk in time");
  loggedUnderruns_ = underruns;
  return lines;
}

std::string MasterpieceProcessor::temperamentName() const {
  const Temperament& t = activeTuning();
  if (!t.name.empty()) return t.name;
  return "Equal";
}

void MasterpieceProcessor::stepTemperament(int direction) {
  const AudioLock audio(*this);
  // The organ's own first, then the library, and round again: a thumb
  // piston cycling temperaments has no end to stop at.
  std::vector<std::string> order{""};
  for (const auto& t : temperamentLibrary()) order.push_back(t.name);
  int at = 0;
  for (size_t i = 0; i < order.size(); ++i)
    if (order[i] == temperamentChoice_) at = static_cast<int>(i);
  const int n = static_cast<int>(order.size());
  setTemperament(order[static_cast<size_t>(((at + direction) % n + n) % n)]);
}

void MasterpieceProcessor::setMasterPitchHz(double hz) {
  const AudioLock audio(*this);
  // Beyond a fourth either way the samples are being stretched further than
  // any organ's pitch has ever differed from another's.
  if (hz > 0.0) hz = juce::jlimit(nativePitchHz() * 0.75, nativePitchHz() * 1.34, hz);
  masterPitchHz_.store(hz > 0.0 ? hz : 0.0, std::memory_order_relaxed);
  markSettingsDirty();
}

double MasterpieceProcessor::masterPitchHz() const {
  const double hz = masterPitchHz_.load(std::memory_order_relaxed);
  return hz > 0.0 ? hz : nativePitchHz();
}

void MasterpieceProcessor::setTranspose(int semitones) {
  const AudioLock audio(*this);
  transpose_.store(juce::jlimit(-12, 12, semitones), std::memory_order_relaxed);
  markSettingsDirty();
}

// ------------------------------------------------ the player's own pistons

bool MasterpieceProcessor::playerElementEngaged(
    const PlayerCombinations::Element& e) const {
  return e.kind == PlayerCombinations::ElementKind::Stop ? stopEngaged(e.id)
                                                          : switchEngaged(e.id);
}

void MasterpieceProcessor::applyPlayerChanges() {
  applyingPistons_ = true;
  for (const auto& c : playerScratch_) {
    if (c.kind == PlayerCombinations::ElementKind::Stop) setStopEngaged(c.id, c.engage);
    else setSwitchEngaged(c.id, c.engage);
  }
  applyingPistons_ = false;
  playerScratch_.clear();
}

void MasterpieceProcessor::pressGeneral(int n) {
  const AudioLock audio(*this);
  const auto read = [this](const PlayerCombinations::Element& e) {
    return playerElementEngaged(e);
  };
  if (captureMode()) {
    if (player_.captureGeneral(n, read))
      combinationsDirty_.store(true, std::memory_order_release);
    return;
  }
  if (!player_.generalSet(n)) return;  // never captured: does nothing
  playerScratch_.clear();
  player_.recallGeneral(n, playerScratch_);
  applyPlayerChanges();
  player_.lightGeneral(n);
}

void MasterpieceProcessor::pressDivisional(Id divisionId, int n) {
  const AudioLock audio(*this);
  const auto read = [this](const PlayerCombinations::Element& e) {
    return playerElementEngaged(e);
  };
  if (captureMode()) {
    if (player_.captureDivisional(divisionId, n, read))
      combinationsDirty_.store(true, std::memory_order_release);
    return;
  }
  if (!player_.divisionalSet(divisionId, n)) return;
  playerScratch_.clear();
  player_.recallDivisional(divisionId, n, playerScratch_);
  applyPlayerChanges();
  player_.lightDivisional(divisionId, n);
}

void MasterpieceProcessor::routeFromControl(const MidiAction& action) {
  // Every keyboard and manual this control was learned for. One manual is a
  // piston that brings it to the keyboard; several on one keyboard are a
  // control that steps the keyboard through them, in the organ's own order
  // (#90). Several keyboards can move at once, from one button.
  routeScratch_.clear();
  routeScratch_.push_back(action.targetId);
  if (action.alsoDrives != nullptr)
    for (const auto& b : *action.alsoDrives) routeScratch_.push_back(b.targetId);
  const auto& order = couplers_.inputKeyboards();
  const auto rank = [&order](Id target) {
    const auto it = std::find(order.begin(), order.end(), routedKeyboard(target));
    return static_cast<int>(it - order.begin());
  };
  std::sort(routeScratch_.begin(), routeScratch_.end(), [&rank](Id a, Id b) {
    if (routedChannel(a) != routedChannel(b)) return routedChannel(a) < routedChannel(b);
    return rank(a) < rank(b);
  });
  for (size_t i = 0; i < routeScratch_.size();) {
    const int channel = routedChannel(routeScratch_[i]);
    size_t end = i;
    while (end < routeScratch_.size() && routedChannel(routeScratch_[end]) == channel) ++end;
    if (channel >= 1 && channel <= 16) {
      const Id now = keyboardForChannel(channel, MidiDeviceMap::kAnyDevice);
      Id next = routedKeyboard(routeScratch_[i]);
      for (size_t k = i; k < end; ++k)
        if (routedKeyboard(routeScratch_[k]) == now) {
          next = routedKeyboard(routeScratch_[k + 1 < end ? k + 1 : i]);
          break;
        }
      if (next != 0 && next != now) {
        // Where the keyboards were before any of this, for General Cancel
        // and for saving: a piston moves a keyboard for the piece, and the
        // next load starts from the player's own setup.
        if (!pistonRouted_.exchange(true)) routingBeforePistons_ = midiMap_.keyboardBindings();
        // Whatever the keyboard was holding lets go first, or those pipes
        // would sound on forever; then the keys still down speak on the
        // manual they now play, at once, as a coupler reaches held keys
        // (#190). Same key ids, so their note-offs still find them.
        rerouteScratch_.clear();
        for (const auto& [key, held] : soundingNotes_)
          if (held.channel == channel) rerouteScratch_.emplace_back(key, held);
        releaseChannel(channel, MidiDeviceMap::kAnyDevice);
        // Only this channel changes manual. Another keyboard already on the
        // manual keeps it: two keyboards both switched to the Solo play it
        // together (#190).
        midiMap_.releaseChannel(channel, 0, next);
        if (keyboardForChannel(channel, MidiDeviceMap::kAnyDevice) != next) {
          MidiMap::KeyboardBinding b;
          b.channel = channel;
          b.keyboardId = next;
          midiMap_.addKeyboardBinding(b);
        }
        midiMapDirty_.store(true, std::memory_order_release);
        // Each key struck again as the key that holds it, on its own channel
        // and device, filed where its note-off will look. A bound channel
        // files a note under its manual and note, so the key now plays `next`
        // and is filed there; filed under the old manual, its note-off found
        // nothing and it sounded on (#206). Under the button's own channel,
        // the next switch passed it by.
        const int savedChannel = noteChannel_, savedDevice = noteDeviceId_;
        for (const auto& [key, held] : rerouteScratch_) {
          noteChannel_ = held.channel;
          noteDeviceId_ = held.device;
          const bool byManual = midiMap_.hasChannelBinding(held.device, held.channel);
          const int newKey = byManual ? noteKey(static_cast<int>(next), key & 0xff) : key;
          startNoteOnKeyboard(next, newKey, held.midiNote, held.velocity);
        }
        noteChannel_ = savedChannel;
        noteDeviceId_ = savedDevice;
      }
    }
    i = end;
  }
}

void MasterpieceProcessor::restoreRouting() {
  if (!pistonRouted_.exchange(false)) return;
  Id before[17] = {};
  for (int ch = 1; ch <= 16; ++ch) before[ch] = keyboardForChannel(ch, MidiDeviceMap::kAnyDevice);
  midiMap_.clearKeyboardBindings();
  for (const auto& b : routingBeforePistons_) midiMap_.addKeyboardBinding(b);
  for (int ch = 1; ch <= 16; ++ch)
    if (keyboardForChannel(ch, MidiDeviceMap::kAnyDevice) != before[ch])
      releaseChannel(ch, MidiDeviceMap::kAnyDevice);
}

// A cancel has nothing to store, so the setter does not change it. When the
// player has asked for it, it puts every keyboard back where it started (#90).
void MasterpieceProcessor::pressGeneralCancel() {
  const AudioLock audio(*this);
  if (cancelResetsKeyboards_.load()) restoreRouting();
  playerScratch_.clear();
  player_.generalCancel(playerScratch_);
  applyPlayerChanges();
  player_.registrationMoved();
}

void MasterpieceProcessor::pressDivisionalCancel(Id divisionId) {
  const AudioLock audio(*this);
  playerScratch_.clear();
  player_.divisionalCancel(divisionId, playerScratch_);
  applyPlayerChanges();
  player_.lightDivisional(divisionId, 0);
}

void MasterpieceProcessor::setPlayerPistonCounts(int generals, int divisionals) {
  player_.setGeneralCount(generals);
  player_.setDivisionalCount(divisionals);
  combinationsDirty_.store(true, std::memory_order_release);
}

bool MasterpieceProcessor::stepperNext() {
  const AudioLock audio(*this);
  const bool capturing = captureMode();
  playerScratch_.clear();
  const bool moved = player_.stepNext(
      capturing, [this](const auto& e) { return playerElementEngaged(e); },
      playerScratch_);
  if (!moved) return false;
  if (capturing) combinationsDirty_.store(true, std::memory_order_release);
  else applyPlayerChanges();
  return true;
}

bool MasterpieceProcessor::stepperPrev() {
  const AudioLock audio(*this);
  const bool capturing = captureMode();
  playerScratch_.clear();
  const bool moved = player_.stepPrev(
      capturing, [this](const auto& e) { return playerElementEngaged(e); },
      playerScratch_);
  if (!moved) return false;
  if (capturing) combinationsDirty_.store(true, std::memory_order_release);
  else applyPlayerChanges();
  return true;
}

bool MasterpieceProcessor::stepperGoto(int frame) {
  const bool capturing = captureMode();
  playerScratch_.clear();
  const bool moved = player_.gotoFrame(
      frame, capturing, [this](const auto& e) { return playerElementEngaged(e); },
      playerScratch_);
  if (!moved) return false;
  if (capturing) combinationsDirty_.store(true, std::memory_order_release);
  else applyPlayerChanges();
  return true;
}

bool MasterpieceProcessor::stepperInsertFrame() {
  const AudioLock audio(*this);
  if (!player_.insertFrame()) return false;
  combinationsDirty_.store(true, std::memory_order_release);
  return true;
}

bool MasterpieceProcessor::stepperDeleteFrame() {
  const AudioLock audio(*this);
  if (!player_.deleteFrame()) return false;
  combinationsDirty_.store(true, std::memory_order_release);
  return true;
}

// What the organ offers a player to register, worked out once the console's
// wiring is known: which switch a player moves is what gets captured.
void MasterpieceProcessor::resetPlayerCombinations() {
  auto drawn = [this](Id sw) {
    const auto it = model_.switches.find(sw);
    return it != model_.switches.end() && it->second.dispInstanceId != 0 &&
           it->second.clickable;
  };
  bool drawnConsole = false;
  for (const auto& [id, sw] : model_.switches)
    if (drawn(id)) {
      drawnConsole = true;
      break;
    }
  // On a drawn console, what the player can reach is what has a knob: the
  // one the wiring leads to, or the one matched by name (stopKnob_).
  std::function<bool(const PlayerCombinations::Element&)> onConsole;
  if (drawnConsole)
    onConsole = [this, drawn](const PlayerCombinations::Element& e) {
      if (e.kind == PlayerCombinations::ElementKind::Switch) return drawn(e.id);
      if (stopKnob_.count(e.id) != 0) return true;
      const auto it = model_.stops.find(e.id);
      return it != model_.stops.end() && it->second.controllingSwitchId != 0 &&
             drawn(playerSwitchFor(it->second.controllingSwitchId));
    };
  auto elements = PlayerCombinations::collect(
      model_, [this](Id sw) { return playerSwitchFor(sw); }, onConsole);
  auto divisions = PlayerCombinations::divisionsOf(model_, elements);
  registrationSwitches_.clear();
  for (const auto& e : elements) {
    if (e.kind == PlayerCombinations::ElementKind::Switch) {
      registrationSwitches_.insert(e.id);
      continue;
    }
    const auto it = model_.stops.find(e.id);
    if (it != model_.stops.end() && it->second.controllingSwitchId != 0)
      registrationSwitches_.insert(playerSwitchFor(it->second.controllingSwitchId));
    const auto knob = stopKnob_.find(e.id);
    if (knob != stopKnob_.end()) registrationSwitches_.insert(knob->second);
  }
  playerScratch_.clear();
  playerScratch_.reserve(elements.size() + 16);
  rerouteScratch_.reserve(128);
  player_.reset(std::move(elements), std::move(divisions));
}

void MasterpieceProcessor::fireCombination(Id comboId) {
  if (combinations_.captureMode()) {
    // Holding the setter and stepping SETS each frame as you pass it, which is
    // how an organist builds a sequence for a piece.
    combinations_.capture(comboId, [this](Id id) { return switchEngaged(id); });
    combinationsDirty_.store(true, std::memory_order_release);
    return;
  }
  // The organ's own General Cancel (type 100) does the same, when asked.
  if (const auto c = model_.combinations.find(comboId);
      c != model_.combinations.end() && c->second.type == 100)
    if (cancelResetsKeyboards_.load()) restoreRouting();
  recallScratch_.clear();
  combinations_.recall(comboId, recallScratch_);
  for (const auto& change : recallScratch_)
    setSwitchEngaged(change.switchId, change.engage);
}

void MasterpieceProcessor::setControlValue(Id controlId, int value) {
  const AudioLock audio(*this);
  markRememberedStateMoved();
  controls_.setValue(controlId, value);
  controls_.propagate(controlId, &engagedSwitches_);
  fireMovedStages();
}

// Every staged control whose value moved since it was last looked at fires
// the switches it sweeps past. Called after a player's move and after the
// per-block solve: a control the organ drives itself -- a pipe-delay ramp that
// opens a pallet once it reaches the top -- moves without anyone setting it.
void MasterpieceProcessor::fireMovedStages() {
  // Fire on whatever MOVED, not on what was set. The control a player moves is
  // often not the one with the steps behind it: Nancy's visible crescendo
  // pedal drives control 51, "Crescendo pedal (extension)", through a linkage,
  // and setting 51 directly is undone by the next propagate.
  for (auto& [id, previous] : stageValues_) {
    const int now = controls_.value(id);
    if (now == previous) continue;

    // A shoe that drives switches fires every threshold it sweeps past, in the
    // order it passes them. For a crescendo that means each step's
    // registration lands in turn and the one belonging to where the shoe
    // stopped is the one that survives — which is what makes dragging it back
    // down work as well as dragging it up.
    stageScratch_.clear();
    stages_.moveControl(id, previous, now, stageScratch_);
    previous = now;
    for (const auto& change : stageScratch_)
      setSwitchEngaged(change.switchId, change.engage);
  }
}

// What the panels show, gathered from the engine in one place. Message thread
// only: it builds strings.
LcdState MasterpieceProcessor::lcdState() const {
  LcdState s;
  s.organName = model_.organName;
  s.temperament = temperamentName();
  s.pitchHz = masterPitchHz();
  s.transpose = transpose();
  s.stopsDrawn = static_cast<int>(engagedStops_.size());
  if (const Id cres = stages_.crescendoControl())
    s.crescendoStep = static_cast<int>(stages_.currentStep(cres));
  // Transpose and combination-set name have no engine-side owner yet; a panel
  // asking for them reads the default rather than a made-up value.
  return s;
}

int MasterpieceProcessor::pumpLcdPanels() {
  if (lcd_.empty() || midiOut_ == nullptr) return 0;
  auto msgs = lcd_.update(lcdState());
  if (msgs.empty()) return 0;
  {
    std::lock_guard<std::mutex> lk(lcdQueueLock_);
    for (auto& m : msgs) lcdQueue_.push_back(std::move(m));
  }
  return static_cast<int>(msgs.size());
}

int MasterpieceProcessor::refreshLcdPanels() {
  // Drop what the displays are believed to show, then send the real state —
  // rather than rendering a blank state, which would make any line whose true
  // value matched the blank one look unchanged and stay unsent.
  lcd_.forgetDisplayed();
  return pumpLcdPanels();
}

Id MasterpieceProcessor::playerSwitchFor(Id switchId) const {
  const auto it = playerSwitch_.find(switchId);
  return it == playerSwitch_.end() ? switchId : it->second;
}

bool MasterpieceProcessor::firePiston(Id switchId) {
  const Id comboId = combinations_.combinationForSwitch(switchId);
  if (comboId == 0) return false;
  // Capture reads the RESOLVED state, because that is what the player can see
  // and hear; the base state would miss a stop pulled by a coupler or by
  // another piston.
  fireCombination(comboId);
  return true;
}

void MasterpieceProcessor::setSwitchEngaged(Id switchId, bool engaged) {
  const AudioLock audio(*this);
  if (switches_.engaged(switchId) == engaged) return; // no edge, no noise
  if (!applyingPistons_ && registrationSwitches_.count(switchId) != 0)
    player_.registrationMoved();

  // The organ's own setter. Holding it turns every piston press into a
  // capture, which is how a console works and how a player expects it to.
  if (switchId == setterSwitchId_ && setterSwitchId_ != 0)
    combinations_.setCaptureMode(engaged);

  // Set what the player set, then let the organ's own wiring decide what that
  // means. On a wired console the two are different switches: Lemmer's "Pedaal
  // koppel" is 1006 and every key action that reads it looks at 10101.
  switches_.set(switchId, engaged);
  markRememberedStateMoved();
  const bool swapsRanks = !alternateStopsBySwitch_.empty();
  if (swapsRanks) previousSwitches_ = engagedSwitches_;
  const bool keysHeld = !soundingNotes_.empty();
  if (keysHeld) heldFlowBefore_ = engagedSwitches_;
  engagedSwitches_ = switches_.engagedSwitches();
  // A coupler drawn or pushed in while keys are down reaches, or stops
  // reaching, the divisions it couples at once, as on a console with
  // electric action (#131). Only what the switch changed moves.
  if (keysHeld) reflowHeldNotes();
  // A switch that swaps a stop's rank for its alternate -- a tremulant whose
  // pipes were also recorded with it running -- re-sounds the notes held on
  // those stops, when the organ asks for that: what was sounding lets go and
  // the other rank's pipes speak.
  if (swapsRanks && !soundingNotes_.empty())
    for (const auto& [movedId, nowEngaged] : switches_.lastChanges()) {
      (void)nowEngaged;
      const auto alt = alternateStopsBySwitch_.find(movedId);
      if (alt == alternateStopsBySwitch_.end()) continue;
      stopSetScratch_.clear();
      for (Id s : alt->second)
        if (engagedStops_.count(s) != 0) stopSetScratch_.insert(s);
      if (stopSetScratch_.empty()) continue;
      for (const auto& [key, held] : soundingNotes_) {
        (void)key;
        expandScratch_.clear();
        couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote,
                             static_cast<float>(held.velocity) / 127.0f,
                             previousSwitches_, keyFlow_, expandScratch_);
        for (const ExpandedNote& reached : expandScratch_)
          for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                             stopSetScratch_, &previousSwitches_))
            voices_.noteOffPipe(held.id, rp.pipeId, NoteRelease{});
        startVoicesForKey(held.keyboard, held.midiNote, held.velocity, held.id,
                          stopSetScratch_);
      }
    }

  // Every switch whose RESOLVED state moved — which on a wired console is
  // usually more than the one clicked.
  for (const auto& [movedId, nowEngaged] : switches_.lastChanges()) {
    const auto stopIt = stopBySwitch_.find(movedId);
    if (stopIt != stopBySwitch_.end()) {
      if (nowEngaged) engagedStops_.insert(stopIt->second);
      else engagedStops_.erase(stopIt->second);
      stopsChanged_.store(true, std::memory_order_release);
    }
    // And every stop the moved switch STANDS for, whether or not the wiring
    // reaches it: on Friesach the knob and the stop's own switch are
    // separate chains, so the knob moves alone and this is the only thing
    // that draws the stop with it.
    if (const auto stoodFor = stopsBySwitch_.find(movedId);
        stoodFor != stopsBySwitch_.end())
      for (Id stopId : stoodFor->second) {
        if (nowEngaged) engagedStops_.insert(stopId);
        else engagedStops_.erase(stopId);
      }

    // Reflect the change on the physical console, if the player wants that.
    // A switch with sends of its own is lit by those, every block.
    if (midiFeedback_ && midiOut_ != nullptr &&
        !midiMap_.hasSends(MidiTargetKind::Switch, movedId)) {
      if (const auto* b = midiMap_.bindingFor(MidiTargetKind::Switch, movedId)) {
        const int ch = b->source.channel > 0 ? b->source.channel : 1;
        if (b->source.kind == MidiSourceKind::Note) {
          outgoing_.addEvent(
              nowEngaged ? juce::MidiMessage::noteOn(ch, b->source.number, 1.0f)
                         : juce::MidiMessage::noteOff(ch, b->source.number),
              0);
        } else if (b->source.kind == MidiSourceKind::ControlChange) {
          outgoing_.addEvent(juce::MidiMessage::controllerEvent(
                                 ch, b->source.number, nowEngaged ? 127 : 0),
                             0);
        }
      }
    }

    // The mechanical sound belongs to the switch that actually moved.
    triggerNoiseFor(movedId, nowEngaged);
    palletMoved(movedId, nowEngaged);
  }

  // A piston fires on the way in, never on the way out, and then lets itself
  // out again: it is a button, not a drawstop, and leaving it latched would
  // make the second press do nothing.
  if (engaged && firePiston(switchId)) {
    const auto sw = model_.switches.find(switchId);
    const bool momentary = sw == model_.switches.end() || !sw->second.latching;
    if (momentary) setSwitchEngaged(switchId, false);
  }
}

namespace {
// A set name becomes part of a file name, so it has to survive being one.
// Anything a filesystem might object to becomes an underscore rather than an
// error: the player is naming a registration, not a path, and "Bach: Advent"
// should not be a failure.
std::string sanitiseSetName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    out.push_back(u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
                          c == '?' || c == '"' || c == '<' || c == '>' ||
                          c == '|'
                      ? '_'
                      : c);
  }
  // Trailing dots and spaces are legal in the name a player types and illegal
  // at the end of a Windows file name.
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out;
}

// The default set keeps the plain extension it has always had, so an organ
// that never uses sets is untouched by this feature existing.
juce::String setExtension(const std::string& setName) {
  const std::string clean = sanitiseSetName(setName);
  return clean.empty() ? juce::String(".mpcomb")
                       : juce::String("." + clean + ".mpcomb");
}
}  // namespace

juce::File MasterpieceProcessor::combinationFileFor(const juce::File& odf) const {
  return organFile(odf, "combinations", setExtension(combinationSet_));
}

std::vector<std::string> MasterpieceProcessor::combinationSets() const {
  std::vector<std::string> out;
  const auto base = organFileForSaving("combinations", ".mpcomb");
  if (base.getFullPathName().isEmpty()) return out;

  const juce::String key = base.getFileNameWithoutExtension();
  for (const auto& f : base.getParentDirectory().findChildFiles(
           juce::File::findFiles, false, key + "*.mpcomb")) {
    // "<key>.mpcomb" is the default set; "<key>.<name>.mpcomb" is a named one.
    juce::String rest = f.getFileName().fromFirstOccurrenceOf(key, false, false);
    rest = rest.dropLastCharacters(juce::String(".mpcomb").length());
    if (rest.startsWithChar('.')) rest = rest.substring(1);
    out.push_back(rest.toStdString());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool MasterpieceProcessor::switchCombinationSet(const std::string& name) {
  // Save first. Switching away from unsaved registrations and silently losing
  // them is the one thing this must not do.
  saveCombinations();
  combinationSet_ = sanitiseSetName(name);
  // Back to what the ORGAN declares before reading the new set, so a set that
  // defines fewer combinations than the last one leaves no stragglers from it.
  combinations_.reset(model_);
  player_.clearAll();
  return loadCombinations();
}

bool MasterpieceProcessor::copyCombinationSetTo(const std::string& name) const {
  const std::string clean = sanitiseSetName(name);
  if (clean == sanitiseSetName(combinationSet_)) return false;  // itself
  const auto f = organFileForSaving("combinations", setExtension(clean));
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(juce::String(combinationFileText()));
}

bool MasterpieceProcessor::deleteCombinationSet(const std::string& name) const {
  const std::string clean = sanitiseSetName(name);
  // The default set is the organ's registrations, not a set someone made, so
  // there is no "delete" that leaves the organ in a sane state.
  if (clean.empty()) return false;
  const auto f = organFileForSaving("combinations", setExtension(clean));
  return !f.getFullPathName().isEmpty() && f.existsAsFile() && f.deleteFile();
}

// One file per set holds both kinds of piston: the organ's own, in
// CombinationSystem's form, and the player's, each line prefixed "player ".
// Files written before the player's pistons existed have no such lines and
// read exactly as they did.
std::string MasterpieceProcessor::combinationFileText() const {
  std::string text = combinations_.toText();
  text += "# The player's own pistons\n";
  std::istringstream in(player_.toText());
  std::string line;
  while (std::getline(in, line))
    if (!line.empty()) text += "player " + line + "\n";
  return text;
}

bool MasterpieceProcessor::saveCombinations() const {
  const auto f = organFileForSaving("combinations", setExtension(combinationSet_));
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(juce::String(combinationFileText()));
}

bool MasterpieceProcessor::saveCombinationsIfDirty() {
  if (!combinationsDirty_.exchange(false, std::memory_order_acq_rel))
    return false;
  return saveCombinations();
}

bool MasterpieceProcessor::loadCombinations() {
  const auto f = combinationFileFor(loadedOdf_);
  if (f.getFullPathName().isEmpty() || !f.existsAsFile()) return false;
  std::istringstream in(f.loadFileAsString().toStdString());
  std::string organ, player, line;
  const std::string prefix = "player ";
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, prefix.size(), prefix) == 0)
      player += line.substr(prefix.size()) + "\n";
    else
      organ += line + "\n";
  }
  const bool organOk = combinations_.fromText(organ);
  const bool playerOk = player_.fromText(player);
  return organOk && playerOk;
}

void MasterpieceProcessor::buildPalletIndex() {
  palletPipes_.clear();
  keySwitchByKey_.clear();
  heldKeySwitches_.clear();
  heldKeySwitchOrigin_.clear();
  palletNotes_.clear();

  std::unordered_set<Id> viaStops;
  for (const auto& [stopId, stop] : model_.stops) {
    (void)stopId;
    for (const StopRankEntry& e : stop.ranks) {
      viaStops.insert(e.rankId);
      if (e.alternateRankId != 0) viaStops.insert(e.alternateRankId);
    }
  }
  for (const auto& [rankId, rank] : model_.ranks) {
    if (viaStops.count(rankId) != 0) continue;
    // A noise the Noise table already triggers has its own path.
    if (rank.isNoise && rank.noiseTriggerSwitchId != 0) continue;
    for (const Pipe& pipe : rank.pipes)
      if (pipe.palletSwitchId != 0 && !pipe.layers.empty())
        palletPipes_[pipe.palletSwitchId].emplace_back(rankId, &pipe);
  }
  if (palletPipes_.empty()) return; // nothing to open: keys stay plain keys

  keySwitchIds_.clear();
  for (const auto& [switchId, key] : model_.keyboardKeys) {
    keySwitchByKey_[static_cast<int>(key.keyboardId) * 256 + key.midiNote] = switchId;
    keySwitchIds_.insert(switchId);
  }
  palletNotes_.reserve(palletPipes_.size());
  heldKeySwitches_.reserve(256);
}

void MasterpieceProcessor::palletMoved(Id switchId, bool engaged) {
  if (palletPipes_.empty() || !palletsLive_.load(std::memory_order_acquire))
    return;
  const auto it = palletPipes_.find(switchId);
  if (it == palletPipes_.end()) return;

  const auto open = palletNotes_.find(switchId);
  if (!engaged) {
    if (open == palletNotes_.end()) return;
    NoteRelease rel;
    rel.velocity = palletVelocity_;
    voices_.noteOff(open->second, rel);
    palletNotes_.erase(open);
    return;
  }
  if (open != palletNotes_.end()) return; // already speaking

  const uint64_t noteId = nextNoteId_++;
  bool any = false;
  for (const auto& [rankId, pipe] : it->second)
    if (startPipeLayers(*pipe, rankId, pipe->midiNote, palletVelocity_, noteId)) {
      any = true;
    }
  if (any) palletNotes_[switchId] = noteId;
}

void MasterpieceProcessor::triggerNoiseFor(Id switchId, bool engaged) {
  const auto it = noiseRanksBySwitch_.find(switchId);
  if (it == noiseRanksBySwitch_.end()) return;

  for (const Id rankId : it->second) {
    const auto rankIt = model_.ranks.find(rankId);
    if (rankIt == model_.ranks.end()) continue;
    const Rank& rank = rankIt->second;
    if (rank.pipes.empty()) continue;

    // A noise rank is not played by key: HW convention is that the pipes are
    // indexed by event rather than pitch, so pipe 0 is the "on" sound and
    // pipe 1, when present, the "off" one. A rank with only one pipe uses it
    // for both directions.
    size_t index = 0;
    if (!engaged && rank.pipes.size() > 1) index = 1;
    const Pipe& pipe = rank.pipes[index];

    // A key-action noise is the sound of the strike, so it takes the strike's
    // velocity; a stop or blower noise is a mechanical event at a medium
    // touch. The sets state a velocity response for the former and this is
    // the only place their figures can act — the noise is not played by a
    // key, so startPipeLayers never sees it.
    const int noiseVelocity =
        keySwitchIds_.count(switchId) != 0
            ? juce::jlimit(1, 127, palletVelocity_)
            : 100;

    const uint64_t noteId = nextNoteId_++;
    for (const auto& layer : pipe.layers) {
      NoteStrike strike;
      strike.velocity = noiseVelocity;
      const int attackIndex = selectAttack(layer, strike);
      if (attackIndex < 0) continue;

      VoiceStart vs;
      vs.pipe = &pipe;
      vs.layer = &layer;
      vs.attackIndex = attackIndex;
      vs.attackId = layer.attacks[static_cast<size_t>(attackIndex)].id;
      vs.velocity = strike.velocity;
      // Noises play at their recorded pitch: they are mechanical sounds, not
      // pipe speech, so temperament must not touch them.
      vs.ratio = 1.0;
      vs.gain = juce::Decibels::decibelsToGain(
                    static_cast<float>(layer.gainDb), -100.0f) *
                layerLevel(layer);
      // The organ's velocity response reaches noises too, and on every set
      // that declares one it is the NOISE layers that carry it.
      if (layer.velSensMaxAttenDb != 0.0) {
        const double v01 = static_cast<double>(noiseVelocity) / 127.0;
        const double attn = layer.invertVelocitySens ? v01 : 1.0 - v01;
        vs.gain *= juce::Decibels::decibelsToGain(
            static_cast<float>(-std::fabs(layer.velSensMaxAttenDb) * attn), -100.0f);
      }
      // A noise is a one-shot; looping it would leave the console rattling.
      vs.oneShot = true;
      vs.busIndex = busForPipe(pipe.pipeId);
      // A noise belongs to the console, not to a division, so it has no rank
      // routing of its own and stays on the first bus.
      vs.mixBus = 0;
      voices_.startVoice(vs, noteId);
    }
  }
}

MasterpieceProcessor::EngineSuspension::EngineSuspension(MasterpieceProcessor& p) : proc(p) {
  proc.engineSuspended_.store(true);
  // A block that started before the flag was set is still using the engine.
  // It is at most a few milliseconds of work; nothing is changed until it
  // has left.
  while (proc.inAudioCallback_.load() != 0) std::this_thread::yield();
}

MasterpieceProcessor::EngineSuspension::~EngineSuspension() {
  proc.engineSuspended_.store(false);
}

void MasterpieceProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  buffer.clear();

  // Counted in before looking at the flag, counted out on every way out.
  inAudioCallback_.fetch_add(1);
  struct Leave {
    std::atomic<int>& count;
    ~Leave() { count.fetch_sub(1); }
  } leave{inAudioCallback_};

  // Timed on every way out too: how long this block took against the time
  // the audio it makes lasts (#120).
  struct Timed {
    Timed(MasterpieceProcessor& owner, double seconds) : p(owner), budget(seconds) {}
    MasterpieceProcessor& p;
    const juce::int64 start = juce::Time::getHighResolutionTicks();
    const double budget;
    ~Timed() {
      if (budget <= 0.0) return;
      const double took = juce::Time::highResolutionTicksToSeconds(
          juce::Time::getHighResolutionTicks() - start);
      const int permille = static_cast<int>(1000.0 * took / budget);
      p.audioBlocks_.fetch_add(1, std::memory_order_relaxed);
      if (permille >= 1000) p.lateBlocks_.fetch_add(1, std::memory_order_relaxed);
      int worst = p.worstBlockPermille_.load(std::memory_order_relaxed);
      while (permille > worst &&
             !p.worstBlockPermille_.compare_exchange_weak(worst, permille,
                                                          std::memory_order_relaxed)) {}
    }
  } timed(*this, getSampleRate() > 0.0 ? buffer.getNumSamples() / getSampleRate() : 0.0);
  // An organ is being loaded: the engine is being rebuilt under us. Silence,
  // and the notes that arrive meanwhile are dropped -- the organ they were
  // played on is the one being replaced.
  if (engineSuspended_.load()) {
    midi.clear();
    blocksSkippedForLoad_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // The runtime DSP switch is a plain parameter so a slow machine can drop to
  // the simple-WAV path without a rebuild (ADR-005).
  graph_.engineSwitch.simpleWavOnly =
      *apvts_.getRawParameterValue("simpleWavOnly") > 0.5f;

  // Merge the on-screen keyboard into the same buffer a device would fill, so
  // mouse and MIDI take one identical path.
  keyboardState_.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);

  // Recording captures what arrived; playback merges its events into the same
  // buffer, so a recorded performance drives exactly the live path.
  recorder_.process(midi, buffer.getNumSamples());

  // Then fold the playback back into the keyboard state.
  //
  // The state was read above, BEFORE the recorder added anything, so a note
  // played from a file was never in it. Two things read that state and both
  // were quietly wrong because of it: the drawn manuals, which stayed still
  // through an entire recorded performance, and Panic, which had nothing to
  // release and so did nothing at all.
  //
  // injectIndirectEvents is false here: the on-screen keyboard's own events
  // were already merged by the call above, and adding them twice would play
  // every moused note twice.
  keyboardState_.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), false);

  // Panic. Every key on every channel is released, as note-offs in the same
  // buffer, so they take the ordinary path: a voice stopped this way still
  // gets its release sample and the room's own decay, rather than being cut
  // dead. Done after the recorder so a stuck note cannot be re-triggered by
  // an event already queued this block.
  if (releaseAll_.exchange(false, std::memory_order_acq_rel)) {
    for (int ch = 1; ch <= 16; ++ch) {
      for (int note = 0; note < 128; ++note)
        if (keyboardState_.isNoteOn(ch, note))
          midi.addEvent(juce::MidiMessage::noteOff(ch, note), 0);
      keyboardState_.allNotesOff(ch);
    }
  }

  // A stop drawn or pushed in since the last block reaches the notes already
  // sounding, before this block's own keys are dealt with.
  if (stopsChanged_.exchange(false, std::memory_order_acq_rel))
    applyStopChangeToHeldNotes();

  outgoing_.clear();
  handleMidi(midi);
  controls_.propagate(0, &engagedSwitches_);
  if (stagesReady_.load(std::memory_order_acquire)) fireMovedStages();
  // A pallet switch that was engaged before the organ went live never moved
  // while pallets listened, so its pipes are opened here, once.
  if (palletsOpenEngaged_.exchange(false, std::memory_order_acq_rel))
    for (const auto& [switchId, pipes] : palletPipes_)
      if (switches_.engaged(switchId)) palletMoved(switchId, true);
  emitSends();

  // LCD text, built on the message thread, joins the same outgoing stream so
  // there is one sender to the port. try_lock rather than lock: a panel line
  // arriving a block late is invisible, and waiting on a message-thread lock
  // here would not be.
  if (midiOut_ != nullptr) {
    std::unique_lock<std::mutex> lk(lcdQueueLock_, std::try_to_lock);
    if (lk.owns_lock() && !lcdQueue_.empty()) {
      for (const auto& m : lcdQueue_)
        outgoing_.addEvent(
            juce::MidiMessage(m.data(), static_cast<int>(m.size())), 0);
      lcdQueue_.clear();
    }
  }

  // Send anything the console should reflect (lit drawstops, moved shoes).
  if (midiOut_ != nullptr && !outgoing_.isEmpty())
    midiOut_->sendBlockOfMessagesNow(outgoing_);

  // Each enclosure renders its own voices and filters only those, so an
  // unenclosed Great stays unenclosed while the Swell shades move.
  renderBuses(buffer);

  // Room before level: the convolver is part of the instrument's sound, and
  // the master fader is the last thing in the chain.
  //
  // Skipped entirely under simpleWavOnly. An FFT convolution is the most
  // expensive thing in this callback by a wide margin, and the switch is
  // called "no DSP" — a machine that needs it needs this gone more than it
  // needs anything else gone.
  if (!graph_.engineSwitch.simpleWavOnly) convolver_.process(buffer);

  // The organ's own output trim, before the player's fader: it is part of how
  // this set is meant to sound, not a setting. Folded into the same multiply
  // so it costs nothing, and deliberately NOT gated behind the DSP switch — a
  // constant gain is not an effect, and a slow machine should still hear the
  // set at the level its producer intended.
  buffer.applyGain(organTrimGain_ *
                   *apvts_.getRawParameterValue("masterGain"));


  // Capture before the metronome. A click track belongs to the practice room,
  // not to the recording.
  audioRecorder_.write(buffer);

  // The metronome is a monitoring aid, not part of the instrument, so it sits
  // after the master fader and is not affected by it.
  metronome_.process(buffer);

  // Load-progress taps, if asked for. Audible, post-recording, beside the
  // metronome: the same kind of thing for the same reason.
  maybeLoadTick(buffer);

  // Under everything and after the recorder, so a recording never carries it.
  addKeepAlive(buffer);

  // Meter last, so it shows what actually leaves. The rise is instant — a
  // meter that eases upward under-reads exactly when it matters — and the
  // fall is slow, which is what makes a sustained tutti readable.
  //
  // getMagnitude is a SIMD scan and the coefficient was computed once in
  // prepareToPlay, so the whole meter costs one vectorised pass over a block
  // the engine has already touched. It adds no latency whatever: this runs
  // after the audio is finished and only reads it.
  const int chans = juce::jmin(2, buffer.getNumChannels());
  const float fall = meterFall_;
  for (int c = 0; c < chans; ++c) {
    const float block = buffer.getMagnitude(c, 0, buffer.getNumSamples());
    peakHeld_[c] = juce::jmax(block, peakHeld_[c] * fall);
    outPeak_[c].store(peakHeld_[c], std::memory_order_relaxed);
  }
  // A mono device must not leave the right lamp lit at whatever it last was.
  for (int c = chans; c < 2; ++c) outPeak_[c].store(0.0f, std::memory_order_relaxed);
}

void MasterpieceProcessor::setContinuousControl(Id controlId, int value) {
  setControlValue(controlId, value);
}

Id MasterpieceProcessor::playerControlFor(Id controlId) const {
  Id at = controlId;
  std::unordered_set<Id> seen{at};
  for (;;) {
    // Prefer a feeder the player can click, since that is the one drawn on
    // the console; otherwise any unconditional one will do.
    Id next = 0;
    for (const auto& l : model_.controlLinkages) {
      if (l.destControlId != at || l.conditionSwitchId != 0 || l.sourceControlId == 0)
        continue;
      const auto cit = model_.continuousControls.find(l.sourceControlId);
      const bool clickable = cit != model_.continuousControls.end() && cit->second.clickable;
      if (next == 0 || clickable) next = l.sourceControlId;
      if (clickable) break;
    }
    if (next == 0 || !seen.insert(next).second) return at;
    at = next;
  }
}

namespace {
// Phase timing for a load. A load is the one operation a player actually
// waits on, and "it took a while" is not a diagnosis: on a slow disk the
// same organ can spend its time in the XML, in the artwork or in the audio,
// and only the split says which. juce::Logger is a no-op until the host
// installs one (Masterpiece --log), so this costs a clock read otherwise.
struct LoadPhases {
  void mark(const char* phase) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    juce::Logger::writeToLog("load: " + juce::String(phase).paddedRight(' ', 14)
                             + juce::String(now - last_, 1) + " ms");
    last_ = now;
  }
  double started_ = juce::Time::getMillisecondCounterHiRes();
  double last_ = started_;
  double sinceStart() const {
    return juce::Time::getMillisecondCounterHiRes() - started_;
  }
};
}  // namespace

MasterpieceProcessor::LoadResult MasterpieceProcessor::loadOrgan(
    const juce::File& odfFile, int64_t maxFramesPerSample, bool graphicsOnly) {
  LoadResult result;
  LoadPhases phases;
  // The organ being left keeps what the player just set on it: the file is
  // named after the organ that is loaded, which is about to change.
  saveRememberedStateIfPending();
  // The audio thread keeps running through a load; keep it off the stage
  // table until it has been rebuilt for the new organ.
  stagesReady_.store(false, std::memory_order_release);
  // Starting the organ moves switches -- the blower, the init controls -- and
  // those can open pallets. No pallet may start a voice before the new
  // organ's audio is in place.
  palletsLive_.store(false, std::memory_order_release);
  palletsOpenEngaged_.store(false, std::memory_order_release);

  // Whoever starts a load clears the cancel flag, so a Cancel that arrived
  // after the previous load already finished cannot kill this one.
  loadProgress_.cancelled.store(false, std::memory_order_release);
  // And the tap thresholds start over, so the first 10% of this load taps
  // rather than whatever the last one reached.
  loadTickNext_.store(10, std::memory_order_release);
  loadProgress_.beginPhase(LoadProgress::Phase::ReadingDefinition);

  if (!odfFile.existsAsFile()) {
    result.error = "no such file: " + odfFile.getFullPathName().toStdString();
    return result;
  }

  // From here to the end the engine is being replaced -- the settings and
  // routing first, then the model and everything built from it -- so the
  // audio thread stands aside until this returns, however it returns.
  const EngineSuspension suspended(*this);

  // What this organ was last set to. Has to happen before a byte of audio is
  // read: the resident format, streaming and the preload head all decide how
  // the samples are read and cannot be changed afterwards.
  //
  // `loadedOdf_` is deliberately NOT set yet. organFile() takes it matching
  // as licence to name the file from organKey(), which reads model_ — and the
  // model is parsed further down. Setting it here made the reader look for a
  // file under the previous organ's name (or none at all) while the writer
  // used this one's, so nothing ever loaded back. Leaving it unset sends the
  // lookup through organKeyFor(), which parses the header only and is what
  // that function exists for.
  // The fader belongs to the organ about to load, not the one just left.
  // Neither file below is guaranteed to mention "gain" — an organ that was
  // never touched simply has no line for it — so without this the slider
  // would sit wherever the previous organ left it instead of at unity.
  if (auto* p = apvts_.getParameter("masterGain"))
    p->setValueNotifyingHost(p->convertTo0to1(1.0f));

  loadGlobalDefaults();
  seedSampleLibraries();

  // After the defaults, which hold both: the ceiling a player set, and the
  // rest of the global file, which writing the marker rewrites around.
  // The samples of this load may take up to the memory ceiling, and no more.
  loadProgress_.resetBudget(graphicsOnly ? 0 : memoryLimitBytes());
  // Written before the load can crash, cleared only by a clean exit.
  if (crashGuard_ && !graphicsOnly && odfFile.existsAsFile()) {
    runningOrgan_ = odfFile;
    writeGlobalFile();
  }

  // The path the definition is known by. A native file chooser can hand back
  // a symlinked OrganDefinitions already resolved, which loses the folder its
  // packages sit beside; when the file lies under a known library's linked
  // OrganDefinitions, the path is rebuilt through that link. Everything below
  // -- the root, the settings, the cache and the organ to reopen -- uses it.
  std::vector<std::string> libraryRoots;
  libraryRoots.reserve(libraries_.size());
  for (const auto& library : libraries_)
    libraryRoots.push_back(library.getFullPathName().toStdString());
  const juce::File effectiveOdf(mp::restoreLogicalOdfPath(
      odfFile.getFullPathName().toStdString(), libraryRoots));

  // A Hauptwerk set puts its definitions in <root>/OrganDefinitions and its
  // audio in <root>/OrganInstallationPackages, so the root is the definition's
  // grandparent. deriveOrganRoot (mp_core, shared with the loader so the two
  // never disagree) also copes with a set that has been reorganised with
  // symlinks -- OrganDefinitions or OrganInstallationPackages relocated onto
  // another drive -- where the plain parent walk can land somewhere that no
  // longer has OrganInstallationPackages beside it.
  const juce::File root(
      mp::deriveOrganRoot(effectiveOdf.getFullPathName().toStdString()));

  loadSettingsFor(effectiveOdf);
  // The Loading tab's choices, kept for this session, win over the file for
  // the organ they were made for.
  if (sessionLoading_ && sessionLoading_->organ == effectiveOdf) {
    const auto& c = *sessionLoading_;
    samples_.setStorage(c.storage);
    samples_.setLoadMono(c.mono);
    samples_.setStreamReleases(c.stream);
    samples_.setStreamHeadFrames(c.streamHead);
    samples_.setStreamHeadPercent(c.streamHeadPercent);
    preloadHead_ = c.preload;
    graph_.engineSwitch = c.engine;
    juce::Logger::writeToLog("load: using the Loading choices kept for this session");
  } else {
    sessionLoading_.reset();
  }

  OdfLoader loader;
  OdfLoader::Options opts;
  opts.organRootDir = root.getFullPathName().toStdString();
  // A folder the player named for this organ wins over anything derived from
  // the definition's own path. Some layouts cannot be worked out from the
  // path at all: a link followed on the way in can leave the definition in a
  // tree that holds no packages, and only the player knows where they are.
  if (organRootOverride_.isDirectory()) {
    opts.organRootDir = organRootOverride_.getFullPathName().toStdString();
    juce::Logger::writeToLog("load: organ root set by hand: " +
                             organRootOverride_.getFullPathName());
  }

  OrganModel loaded;
  if (!loader.load(effectiveOdf.getFullPathName().toStdString(), opts, loaded,
                   result.diagnostics)) {
    result.error = result.diagnostics.errors.empty()
                       ? "the organ definition could not be parsed"
                       : result.diagnostics.errors.front();
    return result;
  }

  phases.mark("odf parse");

  // Publish the model before the audio, so a note-on during loading resolves
  // pipes that simply have no sound yet rather than reading a half-built map.
  // The definition parsed, so its package ids are known. If the root worked
  // out from the path does not hold them -- both standard folders linked to
  // unrelated drives is the reported case, and no path can bridge that -- ask
  // the libraries this machine knows about.
  if (!organRootOverride_.isDirectory()) {
    const juce::File derived(opts.organRootDir);
    const auto packages = derived.getChildFile("OrganInstallationPackages");
    if (!packages.isDirectory()) {
      seedSampleLibraries();
      const juce::File lib = libraryHolding(loaded);
      if (lib.isDirectory()) {
        opts.organRootDir = lib.getFullPathName().toStdString();
        juce::Logger::writeToLog("load: packages found in a known library: " +
                                 lib.getFullPathName());
      }
    }
  }

  model_ = std::move(loaded);
  organRootDir_ = opts.organRootDir;
  loadedOdf_ = effectiveOdf;

  // Console click -> stop. Without this a drawstop would move on screen and
  // the organ would stay silent, which is the worst of both.
  stopBySwitch_.clear();
  alternateStopsBySwitch_.clear();
  for (const auto& [stopId, stop] : model_.stops)
    for (const StopRankEntry& e : stop.ranks)
      if (e.alternateRankId != 0 && e.alternateSwitchId != 0 && e.retriggerOnAlternate)
        alternateStopsBySwitch_[e.alternateSwitchId].push_back(stopId);
  for (const auto& [stopId, stop] : model_.stops)
    if (stop.controllingSwitchId != 0)
      stopBySwitch_[stop.controllingSwitchId] = stopId;

  // The organ's switch wiring, solved once from the declared defaults. An
  // organ that ships with its blower running or a unison coupler drawn comes
  // up that way rather than needing the player to find a switch nobody told
  // them about.
  phases.mark("model: stop map");
  switches_.reset(model_);
  // Where the player left the switches the organ remembers, set as the player
  // would so their wiring follows: Nancy's "start blower on organ load" is
  // read by the controls that start the blower further down.
  hasRememberedState_ = false;
  for (const auto& [id, s] : model_.switches)
    if (s.rememberState) hasRememberedState_ = true;
  for (const auto& [id, c] : model_.continuousControls)
    if (c.rememberState) hasRememberedState_ = true;
  for (const auto& [id, on] : pendingSwitchStates_) {
    const auto it = model_.switches.find(id);
    if (it == model_.switches.end() || !it->second.rememberState || !it->second.latching)
      continue;
    switches_.set(id, on);
  }
  engagedSwitches_ = switches_.engagedSwitches();
  buildPalletIndex();

  // Continuous controls. This was never reset, so the bank held no model and
  // no values: every shoe read as absent, shutterFor() answered "fully open"
  // for everything, and no swell pedal did anything. It looked healthy from
  // the outside because a stuck-open enclosure sounds like an organ.
  phases.mark("model: switches");
  phases.mark("model: switch solve");
  controls_.reset(model_);

  // Now, and not before: reset() has just put every control at the organ's
  // default, so positions restored from the player's file go on top of it.
  // Only controls the organ marks as remembered are ever written, so this
  // cannot resurrect a swell shoe or a crescendo from a previous session.
  for (const auto& [id, v] : pendingControlValues_) {
    const auto it = model_.continuousControls.find(id);
    if (it == model_.continuousControls.end() || !it->second.rememberState)
      continue;
    controls_.setValue(id, v);
  }
  // Settle the whole graph once, WITH the switch states.
  //
  // ContinuousControlBank::reset() propagates too, but it knows no switches,
  // so every conditional linkage is skipped — and a set's tremulant crossfade
  // is built entirely out of those. Azzio pairs them: one linkage fires while
  // switch 49 is engaged and its partner while it is not, swapping two levels
  // between the normal and tremmed scaling controls. Without this call both
  // sit at their declared defaults and the crossfade never happens.
  controls_.propagate(0, &engagedSwitches_);

  // Pistons. The organ's own setter is the switch Hauptwerk assigns code 12,
  // "Comb. Master Capture"; an organ without one leaves capture to the UI.
  phases.mark("model: controls");
  // Five seconds on a large set, and it used to report itself as
  // "reading the organ definition", which was finished long before.
  loadProgress_.beginPhase(LoadProgress::Phase::BuildingWind);
  combinations_.reset(model_);

  // The wind system. Indices are assigned once, in a stable order, so a voice
  // started in one block still points at the right chest in the next.
  wind_.reset(model_);
  windOrder_.clear();
  windIndexOf_.clear();
  pipeWindIndex_.clear();
  for (const auto& [id, wc] : model_.wind) {
    if (wc.infiniteVolume) continue;
    windOrder_.push_back(id);
  }
  std::sort(windOrder_.begin(), windOrder_.end());
  windGauges_.clear();
  for (const auto& [id, wc] : model_.wind)
    if (wc.pressureOutputControlId != 0 &&
        model_.continuousControls.count(wc.pressureOutputControlId) != 0)
      windGauges_.emplace_back(id, wc.pressureOutputControlId);
  std::sort(windGauges_.begin(), windGauges_.end());
  for (size_t i = 0; i < windOrder_.size(); ++i)
    windIndexOf_[windOrder_[i]] = static_cast<int>(i);
  windMods_.assign(windOrder_.size(), VoiceEngine::WindMod{});
  for (const auto& [rankId, rank] : model_.ranks) {
    (void)rankId;
    for (const Pipe& pipe : rank.pipes) {
      const auto it = windIndexOf_.find(wind_.compartmentForPipe(pipe));
      if (it != windIndexOf_.end()) pipeWindIndex_[pipe.pipeId] = it->second;
    }
  }
  phases.mark("model: wind");
  stages_.reset(model_);
  stageScratch_.reserve(64);
  stageValues_.clear();
  for (Id id : stages_.stagedControls())
    stageValues_.emplace_back(id, controls_.value(id));

  // Start the organ. An organ does not come up running: it has controls whose
  // only job is to move once at load and fire the things that have to happen
  // then — Nancy's are called "__DelayBlower" and "__DelayInit", and the first
  // of them is what opens the valve between the blower and the rest of the
  // wind system. Leaving them at rest leaves the blower off, and then every
  // chest drains the moment a key goes down.
  //
  // Two kinds of control are NOT one of these, and both exclusions are load-
  // bearing:
  //
  //   - one that something else drives. Nancy's visible crescendo pedal is fed
  //     through a linkage, and sweeping it would register the organ for a
  //     fortissimo nobody asked for.
  //
  //   - one the player can see. A control with an image is drawn on the
  //     console: it is the player's, and an organ does not come up with its
  //     pedals pushed to the floor. Cracow's crescendo is control 2, declared
  //     default 0, drawn as image set instance 75, and driven by NOTHING — so
  //     the driven test alone let it through and the organ loaded with all 49
  //     crescendo steps engaged. A start-up control is internal by nature:
  //     Nancy's are "__DelayBlower" and "__DelayInit" and no one ever sees
  //     them.
  {
    std::unordered_set<Id> driven;
    for (const auto& l : model_.controlLinkages)
      if (l.destControlId != 0) driven.insert(l.destControlId);
    for (Id id : stages_.stagedControls()) {
      if (driven.count(id) != 0) continue;
      const auto cit = model_.continuousControls.find(id);
      if (cit == model_.continuousControls.end()) continue;
      if (cit->second.imageSetInstanceId != 0) continue;
      setControlValue(id, std::max(cit->second.maxValue, cit->second.minValue));
    }
  }

  // Now that the blower is on and every valve is where the organ puts it, work
  // out what "full wind" actually is. Doing this at reset() instead would
  // measure an organ that is switched off.
  phases.mark("model: stages");
  loadProgress_.beginPhase(LoadProgress::Phase::WiringConsole);
  wind_.settleWith(engagedSwitches_);
  if (wind_.active())
    juce::Logger::writeToLog(
        wind_.lastSettleSteps() > 0
            ? "load: wind settled by integration, " + juce::String(wind_.lastSettleSteps()) + " steps"
            : "load: wind solved in " + juce::String(wind_.lastSettleSweeps()) + " sweeps");
  setterSwitchId_ = 0;
  for (const auto& [id, sw] : model_.switches)
    if (sw.asgnCode == 12) {
      setterSwitchId_ = id;
      break;
    }
  // Worst case a general moves every switch the organ has.
  recallScratch_.reserve(model_.switches.empty() ? 64 : model_.switches.size());

  // For every switch, the drawn one upstream of it. A stop on a wired console
  // is three switches deep — the knob, the logical stop and the node the
  // engine reads — and only the knob is a thing a player can move.
  // Reverse the linkages once. Walking the whole list per switch is O(n*m),
  // and a large organ has thousands of each.
  std::unordered_map<Id, std::vector<Id>> feeders;
  for (const auto& l : model_.switchLinkages)
    if (l.sourceSwitchId != l.destSwitchId && l.sourceWhenEngaged)
      feeders[l.destSwitchId].push_back(l.sourceSwitchId);

  // A switch wired straight into the nodes of several stops is a registration
  // aid -- a Tutti, a reversible, a general -- and the knob of none of them.
  // Coral Pipes' sets wire their Tutti into every stop node beside the stop's
  // own switch, one level closer than the stop's knob: taken for that knob,
  // drawing any stop pulled the Tutti (#211).
  std::unordered_map<Id, int> controlsOf;
  for (const auto& [sid, stop] : model_.stops)
    if (stop.controllingSwitchId != 0) ++controlsOf[stop.controllingSwitchId];
  std::unordered_map<Id, int> stopsFed;
  for (const auto& l : model_.switchLinkages)
    if (l.sourceSwitchId != l.destSwitchId && controlsOf.count(l.destSwitchId) != 0)
      stopsFed[l.sourceSwitchId] += controlsOf[l.destSwitchId];
  auto isAid = [&stopsFed](Id id) {
    const auto it = stopsFed.find(id);
    return it != stopsFed.end() && it->second > 1;
  };

  auto isKnob = [this](Id id) {
    const auto it = model_.switches.find(id);
    return it != model_.switches.end() && it->second.dispInstanceId != 0 &&
           it->second.clickable;
  };

  // Breadth-first, not a single chain.
  //
  // This used to follow one unconditional edge at a time and give up if that
  // chain did not reach a drawn switch. On Friesach that is every stop: the
  // knob reaches the stop's switch through a branch, so the walk ended on an
  // undrawn node, and drawing a stop from a piston or the command line left
  // the console showing a registration it was in fact playing.
  //
  // Conditional edges are followed too. A conditional linkage is how a
  // console wires a knob that acts only when something else is set, and the
  // knob at the far end of one is still the thing a player pulls.
  playerSwitch_.clear();
  std::vector<Id> queue;
  std::unordered_set<Id> seen;
  for (const auto& [id, sw] : model_.switches) {
    (void)sw;
    if (isKnob(id)) { playerSwitch_[id] = id; continue; }

    queue.clear();
    seen.clear();
    queue.push_back(id);
    seen.insert(id);
    Id found = id;
    for (size_t head = 0; head < queue.size() && head < 512; ++head) {
      const auto fit = feeders.find(queue[head]);
      if (fit == feeders.end()) continue;
      bool done = false;
      for (Id up : fit->second) {
        if (!seen.insert(up).second) continue;
        if (isAid(up)) continue;
        if (isKnob(up)) { found = up; done = true; break; }
        queue.push_back(up);
      }
      if (done) break;
    }
    playerSwitch_[id] = found;
  }

  // The knob that belongs to each stop, for the cases where the wiring does
  // not lead to one.
  //
  // Friesach points every Stop at a switch named "DelayedStop: N" which no
  // linkage in the file drives and nothing draws -- the console's knobs are a
  // separate chain, paired to their shadow switch by assignment code rather
  // than by a linkage. Registering from a piston or the command line
  // therefore sounded correct and left every drawstop sitting in.
  //
  // Matched on the name, and only as a fallback, because that is what the set
  // actually gives us to go on: the knob is "01. P Untersatz 32'" where the
  // stop is "P Untersatz 32'". A set whose wiring reaches a real knob never
  // reaches this code.
  auto tidy = [](std::string s) {
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                            s[i] == '.' || s[i] == '_' || s[i] == ' '))
      ++i;
    s.erase(0, i);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  };
  std::unordered_map<std::string, Id> knobByName;
  for (const auto& [id, sw] : model_.switches) {
    if (sw.dispInstanceId == 0 || !sw.clickable || sw.name.empty()) continue;
    // First wins: the console page is listed before the shadow copies, and
    // either lights the same stop anyway.
    knobByName.emplace(tidy(sw.name), id);
  }
  stopKnob_.clear();
  for (const auto& [stopId, stop] : model_.stops) {
    if (stop.controllingSwitchId == 0) continue;
    const Id ps = playerSwitchFor(stop.controllingSwitchId);
    if (isKnob(ps)) continue;                 // the wiring already found one
    const auto it = knobByName.find(tidy(stop.name));
    if (it != knobByName.end()) stopKnob_[stopId] = it->second;
  }
  // Drawn switch -> stops, for the console click path. The wiring carries a
  // knob to its stop through linkages on most sets; on the rest (Friesach)
  // there is no linkage and only the two maps built here know they belong
  // together. Without this a clicked knob animates and sounds nothing.
  stopsBySwitch_.clear();
  {
    auto standsFor = [&](Id switchId, Id stopId) {
      auto& v = stopsBySwitch_[switchId];
      if (std::find(v.begin(), v.end(), stopId) == v.end())
        v.push_back(stopId);
    };
    for (const auto& [stopId, stop] : model_.stops) {
      if (stop.controllingSwitchId == 0) continue;
      standsFor(stop.controllingSwitchId, stopId);
      standsFor(playerSwitchFor(stop.controllingSwitchId), stopId);
      const auto knob = stopKnob_.find(stopId);
      if (knob != stopKnob_.end()) standsFor(knob->second, stopId);
    }
  }
  engagedStops_.clear();
  for (const auto& [switchId, stopId] : stopBySwitch_)
    if (switches_.engaged(switchId)) engagedStops_.insert(stopId);
  // A new organ starts a new registration, so what the audio thread believes
  // is drawn has to be replaced rather than compared with the last organ's.
  stopsChanged_.store(true, std::memory_order_release);

  // Key flow. This is what makes couplers work at all: without it every
  // division sounds on every key, and drawing "Great to Pedal" changes
  // nothing because everything is already coupled to everything.
  couplers_.reset(model_);
  // Channel assignments belong to the organ they were made for; loadMidiMap()
  // below brings back the ones saved for this one.
  midiMap_.clearKeyboardBindings();

  // Where an unassigned channel goes when the organ declares no assignment
  // code. The widest compass when the organ declares one, else the unenclosed
  // manual shipping the most pipework: on a set that declares no compass at
  // all (Nancy) widest-of-nothing lands on the pedal, and the fallback piano
  // plays the one division nobody drew stops for.
  fallbackKeyboard_ = defaultKeyboard(model_, couplers_);
  // An explicit argument wins; otherwise use the configured preload head, so
  // the setting in the UI actually governs a load from the file chooser.
  //
  // Graphics-only stops here. Everything above this line is the console —
  // model, artwork, switch network, key flow — and everything below it is
  // audio. The provider is still set, to an empty library: the voice engine
  // then finds no audio for a pipe and stays silent, which is the same path a
  // set with a missing sample already takes.
  phases.mark("model");
  unloadedStops_.clear();

  // The library still holds the PREVIOUS organ's audio, and nothing else ever
  // let it go: every generation was kept for the voices that might be reading
  // it, so each reload added a whole organ -- 34 GB for a 20 GB one (#163).
  // The engine is suspended and the model those voices belonged to is gone, so
  // they are stopped, and the old audio freed before the new is read.
  // Graphics-only, it must go too: pipe indices from this model would read
  // into it.
  voices_.reset();
  samples_.clear();
  if (!graphicsOnly) {
    const int64_t head =
        maxFramesPerSample > 0 ? maxFramesPerSample : preloadHead_;
    // The ranks the caller asked for, if it asked for any.
    std::unordered_set<Id> onlyRanks;
    for (Id stopId : preloadStops_) {
      const auto it = model_.stops.find(stopId);
      if (it == model_.stops.end()) continue;
      for (const auto& e : it->second.ranks) {
        onlyRanks.insert(e.rankId);
        if (e.alternateRankId != 0) onlyRanks.insert(e.alternateRankId);
      }
    }
    // Pallet-wired ranks belong to no stop the loader can name without
    // walking the wiring, so a partial load keeps all of them. On an organ
    // wired only this way that is the whole organ, which is also what an
    // empty list means.
    if (!onlyRanks.empty())
      for (const auto& [palletId, pipes] : palletPipes_) {
        (void)palletId;
        for (const auto& [rankId, pipe] : pipes) {
          (void)pipe;
          onlyRanks.insert(rankId);
        }
      }
    if (!preloadRanks_.empty())
      onlyRanks = std::unordered_set<Id>(preloadRanks_.begin(), preloadRanks_.end());
    // The player's own choice, when no test flag has made one: every rank
    // except those that only left-out stops use.
    // Perspectives left out: their ranks, whatever stop plays them. Only
    // names this organ has count, so a choice saved for a set that has since
    // been renamed does not silence it.
    std::unordered_set<Id> perspectiveOut;
    perspectivesLeftOut_.clear();
    if (onlyRanks.empty() && !excludedPerspectives_.empty()) {
      for (const auto& [name, ranks] : perspectivesOf(model_))
        if (excludedPerspectives_.count(name) != 0) {
          perspectivesLeftOut_.insert(name);
          perspectiveOut.insert(ranks.begin(), ranks.end());
        }
      // Leaving out every perspective would leave out the organ.
      if (perspectivesLeftOut_.size() == perspectivesOf(model_).size()) {
        perspectivesLeftOut_.clear();
        perspectiveOut.clear();
      }
    }
    // Single ranks left out go the same way as a perspective's: out, whatever
    // stop plays them. Only ranks this organ has count, and never all of them.
    ranksLeftOut_.clear();
    if (onlyRanks.empty())
      for (Id rankId : excludedRanks_)
        if (model_.ranks.count(rankId) != 0) ranksLeftOut_.insert(rankId);
    if (ranksLeftOut_.size() >= model_.ranks.size()) ranksLeftOut_.clear();
    perspectiveOut.insert(ranksLeftOut_.begin(), ranksLeftOut_.end());
    if (onlyRanks.empty() && !perspectiveOut.empty() && excludedStops_.empty()) {
      for (const auto& [rankId, rank] : model_.ranks) {
        (void)rank;
        if (perspectiveOut.count(rankId) == 0) onlyRanks.insert(rankId);
      }
    }
    if (onlyRanks.empty() && !excludedStops_.empty()) {
      // A stop's rank and the alternate its tremulant swaps in go together:
      // the tremulant's recordings are as much that stop's as the rank
      // itself, and a stop left out kept loading them (#165).
      std::unordered_set<Id> wanted;
      for (const auto& [stopId, stop] : model_.stops)
        if (excludedStops_.count(stopId) == 0)
          for (const auto& e : stop.ranks) {
            wanted.insert(e.rankId);
            if (e.alternateRankId != 0) wanted.insert(e.alternateRankId);
          }
      std::unordered_set<Id> leftOut;
      for (Id stopId : excludedStops_) {
        const auto it = model_.stops.find(stopId);
        if (it == model_.stops.end()) continue;
        unloadedStops_.insert(stopId);
        for (const auto& e : it->second.ranks)
          for (const Id rankId : {e.rankId, e.alternateRankId})
            if (rankId != 0 && wanted.count(rankId) == 0) leftOut.insert(rankId);
      }
      for (const auto& [rankId, rank] : model_.ranks) {
        (void)rank;
        if (leftOut.count(rankId) == 0 && perspectiveOut.count(rankId) == 0) onlyRanks.insert(rankId);
      }
    }
    // A stop none of whose ranks came is as good as left out: the console
    // dims it rather than leaving a drawstop that makes no sound.
    if (!perspectiveOut.empty())
      for (const auto& [stopId, stop] : model_.stops) {
        bool any = false;
        for (const auto& e : stop.ranks)
          if (onlyRanks.count(e.rankId) != 0) any = true;
        if (!stop.ranks.empty() && !any) unloadedStops_.insert(stopId);
      }
    if (!perspectivesLeftOut_.empty()) {
      juce::String names;
      for (const auto& p : perspectivesLeftOut_) names << (names.isEmpty() ? "" : ", ") << juce::String(p);
      juce::Logger::writeToLog("load: perspectives left out: " + names + " (" +
                               juce::String(static_cast<int>(perspectiveOut.size())) + " ranks)");
    }
    if (!onlyRanks.empty())
      juce::Logger::writeToLog("load: PARTIAL -- " +
                               juce::String((int)onlyRanks.size()) +
                               " rank(s) of " + juce::String((int)model_.ranks.size()) +
                               "; every other stop will be silent");
    // What the cache is keyed to: which organ, and whether its definition has
    // changed since the cache was written. Both are cheap to read and neither
    // is guessable from the model alone.
    samples_.setCacheDir(cacheDirectory().getFullPathName().toStdString());
    std::string odfStamp = effectiveOdf.getFullPathName().toStdString() + "|" +
                           std::to_string(effectiveOdf.getSize()) + "|" +
                           std::to_string(effectiveOdf.getLastModificationTime().toMilliseconds());
    // A converted organ's sample table is the importer's work, not the
    // file's: a newer importer can number the same samples differently
    // while the .organ file stays as it was. The stamp covers the table
    // itself, so a cache from another importer is never read against it.
    if (isGrandOrgueDefinition(effectiveOdf.getFullPathName().toStdString())) {
      std::vector<std::pair<Id, const SampleRef*>> rows;
      for (const auto& [id, ref] : model_.samples) rows.emplace_back(id, &ref);
      std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](const std::string& s) {
        for (unsigned char c : s) {
          h ^= c;
          h *= 1099511628211ull;
        }
      };
      for (const auto& [id, ref] : rows)
        mix(std::to_string(id) + "|" + ref->fileName + "|" + std::to_string(ref->pitchHz) + ";");
      odfStamp += "|samples " + std::to_string(h);
    }
    // Opened from its copy: the cache was written against the original, so
    // the copy carries the original's stamp and is keyed to it.
    juce::File bundleRoot = effectiveOdf.getParentDirectory();
    while (bundleRoot.getParentDirectory() != portableRoot() &&
           bundleRoot.getParentDirectory() != bundleRoot)
      bundleRoot = bundleRoot.getParentDirectory();
    if (bundleRoot.getParentDirectory() == portableRoot()) {
      const auto kept = bundleRoot.getChildFile("stamp.txt").loadFileAsString().trim();
      if (kept.isNotEmpty()) odfStamp = kept.toStdString();
    }
    // An organ kept playable without its files needs a cache of its own: the
    // shared one is replaced by the next organ loaded.
    if (keepPortable_) samples_.setCacheMode(SampleLibrary::CacheMode::PerOrgan);
    samples_.setCacheIdentity(organKey(), odfStamp);
    loadedStamp_ = odfStamp;

    // An organ unpacked from its packages keeps its samples there: the
    // folder says which archives, and its saved index says where in them.
    {
      std::shared_ptr<const OrganArchive> packaged;
      const std::string archivePath = readArchiveMarker(opts.organRootDir);
      if (!archivePath.empty()) {
        auto archive = std::make_shared<OrganArchive>();
        std::string error;
        const std::string index = juce::File(juce::String::fromUTF8(opts.organRootDir.c_str()))
                                      .getChildFile("archive-index.txt")
                                      .getFullPathName()
                                      .toStdString();
        if (archive->loadIndex(index) || archive->open(archivePath, error)) {
          packaged = archive;
          juce::Logger::writeToLog("load: samples from " + juce::String((int)archive->archives().size()) +
                                   " archive(s) beside " + juce::String(archivePath));
        } else {
          juce::Logger::writeToLog("load: the organ's archives cannot be read: " + juce::String(error));
        }
      }
      samples_.setArchive(packaged);
    }
    if (model_.hasLicensedSamples && !licenceConfirmed_ && licenceAsker) {
      result.licenceAsked = true;
      if (licenceAsker(model_.organName, licencePublisher())) {
        licenceConfirmed_ = true;
        markSettingsDirty();
        juce::Logger::writeToLog("licence: the player confirmed a licence for this organ");
      }
    }
    samples_.setLicenceConfirmed(licenceConfirmed_);
    if (model_.hasLicensedSamples) {
      const std::string who = licencePublisher();
      juce::Logger::writeToLog(
          licenceConfirmed_
              ? "load: the player confirmed a licence" +
                    juce::String(who.empty() ? "" : " from " + who) +
                    " to play this set here; its licensed samples load"
              : "load: this set's samples need a licence" +
                    juce::String(who.empty() ? "" : " from " + who) +
                    " that has not been confirmed; they are left out");
    }

    result.samples = samples_.loadAll(model_, opts.organRootDir, head,
                                      LoopSelection::Longest, &loadProgress_,
                                      onlyRanks.empty() ? nullptr : &onlyRanks);

    // Now that the files have been opened, and only now, each sample can be
    // asked what pitch it holds. This has to happen after loading because one
    // of the routes the format offers is "it is in the file".
    resolveSamplePitches();
  }

  // A cancelled load is NOT a partly-loaded organ. Half an instrument that
  // plays some notes and silently drops others is worse than none: the player
  // would be debugging their sample set rather than remembering they pressed
  // Cancel. Drop what was read and say so plainly.
  // Except at the memory limit: there the player did not ask to stop, and
  // what fitted is kept and played. An organ that is mostly there is worth
  // more than none, provided it says it is incomplete -- and the player can
  // then leave out perspectives or stops until it fits.
  if (loadProgress_.isCancelled() && loadProgress_.overBudget.load(std::memory_order_acquire) &&
      result.samples.loaded > 0) {
    juce::Logger::writeToLog("load: stopped at the memory limit; keeping the " +
                             juce::String(result.samples.loaded) + " of " +
                             juce::String(result.samples.wanted) +
                             " samples read -- the organ is INCOMPLETE");
    result.incomplete = true;
    result.outOfMemory = true;
    loadProgress_.cancelled.store(false, std::memory_order_release);
  }
  if (loadProgress_.isCancelled()) {
    const bool outOfMemory = loadProgress_.overBudget.load(std::memory_order_acquire);
    juce::Logger::writeToLog(outOfMemory
                                 ? "load: stopped at the memory limit, discarding partial organ"
                                 : "load: cancelled, discarding partial organ");
    samples_.clear();
    model_ = OrganModel{};
    // The organ is not loaded, so nothing may name its files after it: with
    // loadedOdf_ still set, organFile() took the name from this empty model,
    // the next load read its settings from a file that does not exist, and
    // the next save wrote that over the real one -- the stops left out came
    // back after a cancelled load (#129).
    loadedOdf_ = juce::File();
    voices_.setSampleProvider(samples_.provider());
    loadProgress_.phase.store(outOfMemory ? LoadProgress::Phase::Failed
                                          : LoadProgress::Phase::Cancelled,
                              std::memory_order_release);
    result.ok = false;
    result.outOfMemory = outOfMemory;
    result.error = outOfMemory
                       ? "out of memory: this organ's samples need more than the " +
                             std::to_string(memoryLimitBytes() / (1024 * 1024)) +
                             " MB memory limit"
                       : "cancelled";
    return result;
  }
  // Still suspended, with no voice sounding: nothing else needs keeping.
  samples_.retireOldGenerations();
  voices_.setSampleProvider(samples_.provider());
  // A release to be read from its file's marker, in a file with no marker
  // and nothing after its loop, holds nothing to play. A GrandOrgue pipe
  // offers its own file's release beside any separate ones; started at the
  // file's head, it replayed the whole attack at full level as the "release"
  // and every note sounded on for the length of its recording.
  {
    const auto provide = samples_.provider();
    int empties = 0;
    for (auto& [rankId, rank] : model_.ranks)
      for (auto& pipe : rank.pipes)
        for (auto& layer : pipe.layers)
          for (auto& rel : layer.releases) {
            if (rel.loadStartValue <= 0 && rel.loadStartType <= 0) continue;
            const SampleBuffer* buf = provide ? provide(rel.sample.sampleId) : nullptr;
            if (buf == nullptr) continue;
            rel.empty = buf->releaseHoldsNothing();
            empties += rel.empty ? 1 : 0;
          }
    if (empties > 0)
      juce::Logger::writeToLog("load: " + juce::String(empties) +
                               " release(s) named in their attack files, which hold none, left out");
  }
  phases.mark(graphicsOnly ? "samples (skipped)" : "samples");

  // Nearly six seconds on a large set, after the sample counter has reached
  // its total and stopped moving. Without a phase of its own the dialog sat
  // at "12148 of 12148 samples" while it did something else entirely.
  loadProgress_.beginPhase(LoadProgress::Phase::Preparing);

  // Rebuild everything that is derived from the model. prepareToPlay may not
  // have run yet (headless), in which case it will pick this up when it does.
  //
  // At the block size it was last prepared for, not getBlockSize(): that is
  // only what a host has REPORTED, and with none reporting -- the render tool,
  // a test -- it is 0, which sized the scratch buffers to one frame for the
  // next 256-frame block to write past.
  if (sampleRate_ > 0.0)
    prepareToPlay(sampleRate_, juce::jmax(1, maxBlock_));

  // A mapping and a set of combinations saved for this organ come back with
  // it. Missing is normal: it means the player has not saved any yet.
  loadMidiMap();
  unloadedSwitches_.clear();
  for (Id stopId : unloadedStops_) {
    const auto it = model_.stops.find(stopId);
    if (it != model_.stops.end() && it->second.controllingSwitchId != 0)
      unloadedSwitches_.insert(playerSwitchFor(it->second.controllingSwitchId));
    const auto knob = stopKnob_.find(stopId);
    if (knob != stopKnob_.end()) unloadedSwitches_.insert(knob->second);
  }
  // A stop whose ranks ship no pipes (a demo set's locked stops) sounds no
  // more than one left out. Its own knob is left alone: a set that ships
  // locked stops draws them greyed in its own artwork, and a veil on top of
  // that only darkens it. Its copies on other pages get the veil (below).
  std::unordered_set<Id> silent;
  for (const auto& e : stopList()) {
    if (e.playable) continue;
    const auto it = model_.stops.find(e.stopId);
    if (it != model_.stops.end() && it->second.controllingSwitchId != 0)
      silent.insert(playerSwitchFor(it->second.controllingSwitchId));
    if (const auto knob = stopKnob_.find(e.stopId); knob != stopKnob_.end())
      silent.insert(knob->second);
  }
  // And every copy of those knobs on other pages: a set that draws a stop on
  // its console and again on a simple jamb links the two both ways, so each
  // follows the other (#53: Nancy's Simple Jamb showed demo-locked stops
  // undimmed). Both ways, unconditionally, is what makes a copy; a piston or
  // a tutti that drives stops one way is not one, and stays as it is.
  if (!unloadedSwitches_.empty() || !silent.empty()) {
    std::set<std::pair<Id, Id>> links;
    for (const auto& l : model_.switchLinkages)
      if (l.conditionSwitchId == 0 && l.sourceWhenEngaged && l.engageAction == 1 &&
          l.disengageAction == 2)
        links.insert({l.sourceSwitchId, l.destSwitchId});
    std::unordered_set<Id> seen(unloadedSwitches_.begin(), unloadedSwitches_.end());
    seen.insert(silent.begin(), silent.end());
    std::vector<Id> todo(seen.begin(), seen.end());
    while (!todo.empty()) {
      const Id at = todo.back();
      todo.pop_back();
      for (auto it = links.lower_bound({at, 0}); it != links.end() && it->first == at; ++it)
        if (links.count({it->second, at}) != 0 && seen.insert(it->second).second) {
          todo.push_back(it->second);
          if (silent.count(it->second) == 0) unloadedSwitches_.insert(it->second);
        }
    }
  }
  resetPlayerCombinations();
  loadCombinations();

  phases.mark("prepare");
  juce::Logger::writeToLog("load: TOTAL         " +
                           juce::String(phases.sinceStart(), 1) + " ms  (" +
                           odfFile.getFileName() + ")");

  // What the organ costs to hold, in the library's own terms. The process
  // will always be larger than this -- artwork, JUCE, the heap it has not
  // returned -- but this is the part that the memory settings actually move,
  // and it is the number to compare between two configurations of the same
  // set.
  {
    const auto mb = [](int64_t b) {
      return juce::String(b / (1024.0 * 1024.0), 1);
    };
    if (samples_.cacheBytesRead() > 0)
      juce::Logger::writeToLog("cache: read " + mb(samples_.cacheBytesRead()) +
                               " MB, samples not decoded");
    else if (samples_.cacheWriting())
      juce::Logger::writeToLog("cache: saving the samples for the next load, in the background");
    juce::Logger::writeToLog(
        "memory: resident " + mb(samples_.residentBytes()) + " MB" +
        ", streamed " + mb(samples_.streamedBytesSaved()) + " MB not held" +
        ", storage=" +
        (samples_.storage() == SampleStorage::Int16   ? "int16"
         : samples_.storage() == SampleStorage::Int24 ? "int24"
                                                      : "float32") +
        ", mono=" + (samples_.loadMono() ? "on" : "off") +
        ", rate=" + (samples_.loadSampleRate() > 0.0
                         ? juce::String(samples_.loadSampleRate(), 0)
                         : juce::String("as recorded")) +
        ", streamReleases=" + (samples_.streamReleases() ? "on" : "off"));
  }

  // Only now, having got this far: an organ that failed to load is not one
  // worth reopening on the next start.
  setLastOrgan(effectiveOdf);
  // And the library it came from, so a definition moved away from its audio
  // later can still be matched to it.
  if (!graphicsOnly) rememberSampleLibrary(juce::File(organRootDir_));

  result.stopsEngaged = 0;
  // Only now: starting the organ above moves switches on this thread, and
  // the audio thread's own stage check must not move them at the same time.
  stagesReady_.store(true, std::memory_order_release);
  palletsLive_.store(true, std::memory_order_release);
  palletsOpenEngaged_.store(true, std::memory_order_release);
  loadProgress_.phase.store(LoadProgress::Phase::Done,
                            std::memory_order_release);
  // Starting the organ moved its controls and switches, and that is the
  // organ's doing, not the player's: nothing to write.
  rememberedMovedAtMs_.store(0, std::memory_order_release);
  if (keepPortable_ && !graphicsOnly && result.samples.loaded > 0 && result.samples.failed == 0 &&
      result.samples.missing == 0 && !result.incomplete)
    writePortableCopy(effectiveOdf);
  result.ok = true;
  return result;
}

juce::File MasterpieceProcessor::portableRoot() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("Portable");
}

juce::File MasterpieceProcessor::portableCopyFor(const juce::File& originalOdf) {
  // "<original definition>\t<its copy>", one per line.
  juce::StringArray lines;
  lines.addLines(portableRoot().getChildFile("index.txt").loadFileAsString());
  for (const auto& line : lines) {
    const auto original = line.upToFirstOccurrenceOf("\t", false, false);
    if (original == originalOdf.getFullPathName())
      return juce::File(line.fromFirstOccurrenceOf("\t", false, false));
  }
  return {};
}

void MasterpieceProcessor::writePortableCopy(const juce::File& odf) {
  const juce::File root = portableRoot();
  // Already a copy, or opened from its packages: those keep their definition
  // and pictures unpacked, and need only the cache.
  if (odf.isAChildOf(root) || !readArchiveMarker(organRootDir_).empty()) return;
  const juce::File bundle = root.getChildFile(juce::String(organKey()));
  const juce::File stamp = bundle.getChildFile("stamp.txt");
  if (stamp.loadFileAsString().trim() == juce::String(loadedStamp_)) return;  // up to date

  const double started = juce::Time::getMillisecondCounterHiRes();
  bundle.deleteRecursively();
  // Everything under the organ's root but its audio -- the definition, the
  // console's pictures, their masks -- in the same places, so the copy is an
  // organ root of its own for either format.
  const juce::File organRoot{juce::String(organRootDir_)};
  const juce::File copy = bundle.getChildFile(odf.getRelativePathFrom(organRoot));
  if (!odf.isAChildOf(organRoot)) return;
  int64_t bytes = 0;
  int files = 0;
  for (const auto& entry : juce::RangedDirectoryIterator(organRoot, true, "*", juce::File::findFiles)) {
    const auto ext = entry.getFile().getFileExtension().toLowerCase();
    if (ext == ".wav" || ext == ".wv" || ext == ".aif" || ext == ".aiff" || ext == ".flac" ||
        ext == ".hbw" || ext == ".hbx" || ext == ".rar" || ext == ".zip" || ext == ".orgue")
      continue;
    const juce::File to = bundle.getChildFile(entry.getFile().getRelativePathFrom(organRoot));
    to.getParentDirectory().createDirectory();
    if (entry.getFile().copyFileTo(to)) {
      bytes += entry.getFileSize();
      ++files;
    }
  }
  if (!copy.existsAsFile()) {
    juce::Logger::writeToLog("portable: could not copy the definition to " + copy.getFullPathName());
    return;
  }
  stamp.replaceWithText(juce::String(loadedStamp_));

  // The index: this original now has a copy.
  juce::StringArray lines;
  lines.addLines(root.getChildFile("index.txt").loadFileAsString());
  lines.removeEmptyStrings();
  for (int i = lines.size(); --i >= 0;)
    if (lines[i].upToFirstOccurrenceOf("\t", false, false) == odf.getFullPathName()) lines.remove(i);
  lines.add(odf.getFullPathName() + "\t" + copy.getFullPathName());
  root.getChildFile("index.txt").replaceWithText(lines.joinIntoString("\n") + "\n");
  juce::Logger::writeToLog("portable: a copy of the definition and " + juce::String(files - 1) + " other files (" +
                           juce::String(bytes / (1024.0 * 1024.0), 1) + " MB) in " + bundle.getFullPathName() +
                           ", after " + juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) +
                           " s; with its cache, the organ opens without its installation files");
}

std::vector<MasterpieceProcessor::StopEntry> MasterpieceProcessor::stopList() const {
  std::vector<StopEntry> out;
  out.reserve(model_.stops.size());
  for (const auto& [id, stop] : model_.stops) {
    StopEntry e;
    e.stopId = id;
    e.divisionId = stop.divisionId;
    e.name = stop.name;
    // A stop whose ranks have no pipes cannot sound. Demo sets are full of
    // them, and a console that shows no difference between a stop that works
    // and one that was never shipped is actively misleading.
    for (const auto& entry : stop.ranks) {
      const auto rit = model_.ranks.find(entry.rankId);
      if (rit != model_.ranks.end() && !rit->second.pipes.empty()) {
        e.playable = true;
        break;
      }
    }
    out.push_back(std::move(e));
  }
  std::sort(out.begin(), out.end(), [](const StopEntry& a, const StopEntry& b) {
    if (a.divisionId != b.divisionId) return a.divisionId < b.divisionId;
    return a.stopId < b.stopId;
  });
  return out;
}

int MasterpieceProcessor::engageAllStops() {
  const AudioLock audio(*this);
  // Through the console, not around it: drawing every stop by hand is what a
  // player does to hear a tutti, and doing it any other way leaves the switch
  // states disagreeing with the stop list.
  for (const auto& [id, stop] : model_.stops) {
    (void)stop;
    setStopEngaged(id, true);
  }
  return static_cast<int>(engagedStops_.size());
}

namespace {
// The folder Masterpiece unpacked a package into, if this definition came
// from one: the nearest folder up that carries the archive marker.
juce::File packageFolderOf(const juce::File& odf) {
  juce::File dir = odf.getParentDirectory();
  for (int up = 0; up < 3 && dir.isDirectory(); ++up) {
    if (!readArchiveMarker(dir.getFullPathName().toStdString()).empty()) return dir;
    dir = dir.getParentDirectory();
  }
  return {};
}
constexpr const char* kChosenDefinition = "chosen-definition.txt";
constexpr const char* kDefinitionPatterns =
    "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.organ";
}  // namespace

juce::Array<juce::File> MasterpieceProcessor::organVersions(const juce::File& odf) {
  juce::Array<juce::File> out;
  if (!odf.existsAsFile()) return out;
  const juce::File package = packageFolderOf(odf);
  if (package != juce::File())
    package.findChildFiles(out, juce::File::findFiles, true, kDefinitionPatterns);
  else
    odf.getParentDirectory().findChildFiles(out, juce::File::findFiles, false, kDefinitionPatterns);
  out.sort();
  return out;
}

void MasterpieceProcessor::rememberDefinition(const juce::File& odf) {
  const juce::File package = packageFolderOf(odf);
  if (package == juce::File()) return;
  package.getChildFile(kChosenDefinition).replaceWithText(odf.getRelativePathFrom(package));
}

juce::File MasterpieceProcessor::rememberedDefinition(const juce::Array<juce::File>& definitions) {
  if (definitions.isEmpty()) return {};
  const juce::File package = packageFolderOf(definitions.getFirst());
  if (package == juce::File()) return {};
  const juce::File marker = package.getChildFile(kChosenDefinition);
  if (!marker.existsAsFile()) return {};
  const juce::File chosen = package.getChildFile(marker.loadFileAsString().trim());
  return definitions.contains(chosen) ? chosen : juce::File();
}

juce::Array<juce::File> MasterpieceProcessor::openPackagedOrgan(const juce::File& archiveFile,
                                                               juce::String& error) {
  OrganArchive archive;
  std::string why;
  std::vector<std::string> damaged;
  const std::string archivePath = archiveFile.getFullPathName().toStdString();
  // Every step is written down, and every failure with the file it was in:
  // "it does not open" is all a player can say otherwise, and a report
  // from someone else's machine is only as good as its log (#53).
  const auto started = juce::Time::getMillisecondCounterHiRes();
  const auto seconds = [started] {
    return juce::String((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 1) + " s";
  };
  const auto fail = [&](const std::string& message) {
    error = message;
    loadProgress_.beginPhase(message == "cancelled" ? LoadProgress::Phase::Cancelled
                                                    : LoadProgress::Phase::Failed);
    juce::Logger::writeToLog("archive: FAILED after " + seconds() + ": " + juce::String(message));
    return juce::Array<juce::File>();
  };
  juce::Logger::writeToLog("archive: opening " + archiveFile.getFullPathName());
  if (!archive.discover(archivePath, why)) return fail(why);
  const bool inspected = archive.inspect(why);
  juce::Logger::writeToLog("archive: " + juce::String(static_cast<int>(archive.archives().size())) +
                           " archive(s) for this organ:");
  for (const auto& line : archive.report()) juce::Logger::writeToLog("archive:   " + juce::String(line));
  if (!inspected) return fail(why);
  // Named by the archives themselves, so the same packages open the same
  // folder -- and the organ keeps its settings -- however they were reached.
  const juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                             .getChildFile("Masterpiece")
                             .getChildFile("Packaged")
                             .getChildFile(archive.identity());
  const std::string index = dir.getChildFile("archive-index.txt").getFullPathName().toStdString();
  const std::string marked = readArchiveMarker(dir.getFullPathName().toStdString());
  const bool ready = !marked.empty() && archive.loadIndexFor(index);
  if (loadProgress_.isCancelled()) return fail("cancelled");
  if (ready) {
    juce::Logger::writeToLog("archive: already unpacked in " + dir.getFullPathName());
    // The same set opened from somewhere else -- copied to a faster disk, or
    // moved: its samples are read from here from now on, not from the first
    // place it was opened, which may be slower or gone.
    if (marked != archivePath) {
      archive.saveIndex(index);
      writeArchiveMarker(dir.getFullPathName().toStdString(), archivePath);
      juce::Logger::writeToLog("archive: now read from " + juce::String(archivePath) +
                               " (was " + juce::String(marked) + ")");
    }
  } else {
    dir.deleteRecursively();
    dir.createDirectory();
    juce::Logger::writeToLog("archive: reading the file lists");
    const auto progress = [this](double fraction) {
      loadProgress_.archiveFraction.store(fraction, std::memory_order_relaxed);
      return !loadProgress_.isCancelled();
    };
    loadProgress_.beginPhase(LoadProgress::Phase::ReadingArchive);
    if (!archive.index(why, progress)) {
      dir.deleteRecursively();
      return fail(why);
    }
    for (const auto& line : archive.report())
      if (line.find(" files") != std::string::npos)
        juce::Logger::writeToLog("archive:   " + juce::String(line));
    juce::Logger::writeToLog("archive: " + juce::String(static_cast<int>(archive.entries().size())) +
                             " files indexed after " + seconds() + "; unpacking the definitions and artwork");
    loadProgress_.beginPhase(LoadProgress::Phase::ExtractingArchive);
    if (!archive.unpackSmallFiles(dir.getFullPathName().toStdString(), why, damaged, progress)) {
      dir.deleteRecursively();
      return fail(why);
    }
    for (const auto& line : damaged) juce::Logger::writeToLog("archive: " + juce::String(line));
    archive.saveIndex(index);
    // Written last: a folder without it is an unpack that did not finish. Not
    // written at all when something was damaged, so the same packages
    // downloaded again -- same names, same sizes, same folder -- are unpacked
    // afresh rather than left with the holes.
    if (damaged.empty()) writeArchiveMarker(dir.getFullPathName().toStdString(), archivePath);
  }
  juce::Array<juce::File> definitions;
  dir.findChildFiles(definitions, juce::File::findFiles, true,
                     "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.organ");
  definitions.sort();
  if (loadProgress_.isCancelled()) return fail("cancelled");
  // A damaged archive is said in so many words, with what to do about it:
  // the path inside the archive alone reads like a file missing from disk.
  juce::String damage;
  if (!damaged.empty()) {
    for (size_t i = 0; i < damaged.size() && i < 3; ++i) damage << juce::String(damaged[i]) << "\n";
    if (damaged.size() > 3) damage << "and " << static_cast<int>(damaged.size() - 3) << " more\n";
    damage << "\nThe archive is damaged, usually by a download that was cut short or "
              "corrupted. Test it in 7-Zip or WinRAR, and download it again.";
  }
  if (definitions.isEmpty()) {
    error = damage.isNotEmpty()
                ? "the organ definition could not be read.\n\n" + damage
                : "there is no organ definition file (.Organ_Hauptwerk_xml) in \"" +
                      archiveFile.getFileName() + "\" or the packages beside it.";
    juce::Logger::writeToLog("archive: FAILED after " + seconds() + ": " + error);
    loadProgress_.beginPhase(LoadProgress::Phase::Failed);
    return definitions;
  }
  juce::Logger::writeToLog("archive: ready after " + seconds() + ", " +
                           juce::String(definitions.size()) + " organ definition(s):");
  for (const auto& d : definitions) juce::Logger::writeToLog("archive:   " + d.getFileName());
  loadProgress_.beginPhase(LoadProgress::Phase::Done);
  // The organ opens, and the player is told what is missing from it.
  if (damage.isNotEmpty())
    error = "Some of its files could not be read, so parts of the console may be missing.\n\n" +
            damage;
  return definitions;
}

void MasterpieceProcessor::loadOrganAsync(const juce::File& odfFile) {
  // Loading a 19 GB set must not block the message thread. The audio thread is
  // unaffected either way: it only ever reads the sample store through an
  // atomic pointer, and an unfinished load simply has no audio for a pipe yet.
  juce::Thread::launch([this, odfFile] { loadOrgan(odfFile); });
}

// ------------------------------------------------------- the MIDI window

void MasterpieceProcessor::sendKey(Id keyboard, int midiNote, int velocity, bool on) {
  RawMidi raw;
  for (const auto& s : midiMap_.sends())
    if (s.targetKind == MidiTargetKind::Keyboard && s.targetId == keyboard &&
        MidiMap::keyMessage(s, midiNote, velocity, on, raw))
      outgoing_.addEvent(raw.bytes, raw.size, 0);
}

void MasterpieceProcessor::emitSends() {
  const auto& sends = midiMap_.sends();
  if (midiOut_ == nullptr || sends.empty()) return;
  // Sized to the list; a list that changed shape starts over, which sends
  // every lamp's current state -- what a console needs when it is set up.
  if (sentValues_.size() != sends.size()) sentValues_.assign(sends.size(), -1);
  RawMidi raw;
  for (size_t i = 0; i < sends.size(); ++i) {
    const MidiSend& s = sends[i];
    int now;
    if (s.targetKind == MidiTargetKind::Switch)
      now = switches_.engaged(s.targetId) ? 1 : 0;
    else if (s.targetKind == MidiTargetKind::ContinuousControl)
      now = controls_.value(s.targetId);
    else
      continue;
    if (now == sentValues_[i]) continue;
    sentValues_[i] = now;
    const bool made = s.targetKind == MidiTargetKind::Switch
                          ? MidiMap::switchMessage(s, now != 0, raw)
                          : MidiMap::controlMessage(s, now, raw);
    if (made) outgoing_.addEvent(raw.bytes, raw.size, 0);
  }
}

void MasterpieceProcessor::setObjectBindings(MidiTargetKind kind, Id id,
                                             const std::vector<MidiBinding>& rows) {
  {
    const AudioLock lock(*this);
    midiMap_.setBindingsFor(kind, id, rows);
  }
  saveMidiMap();
}

void MasterpieceProcessor::setObjectSends(MidiTargetKind kind, Id id,
                                          const std::vector<MidiSend>& rows) {
  {
    const AudioLock lock(*this);
    midiMap_.setSendsFor(kind, id, rows);
    sentValues_.clear();
  }
  saveMidiMap();
}

void MasterpieceProcessor::setObjectShortcuts(MidiTargetKind kind, Id id,
                                              const std::vector<KeyShortcut>& rows) {
  {
    const AudioLock lock(*this);
    midiMap_.setShortcutsFor(kind, id, rows);
  }
  saveMidiMap();
}

bool MasterpieceProcessor::applyKeyShortcut(const std::string& key) {
  const auto hits = midiMap_.shortcutsForKey(key);
  for (const auto& k : hits) {
    if (k.targetKind == MidiTargetKind::Switch) {
      const auto sw = model_.switches.find(k.targetId);
      const bool latching = sw == model_.switches.end() || sw->second.latching;
      if (latching) {
        setSwitchEngaged(k.targetId, !switchEngaged(k.targetId));
      } else {
        // A piston: pressed and let go, as one click of it would be.
        setSwitchEngaged(k.targetId, true);
        setSwitchEngaged(k.targetId, false);
      }
    } else if (k.targetKind == MidiTargetKind::ContinuousControl) {
      setContinuousControl(k.targetId,
                           juce::jlimit(0, 127, continuousControlValue(k.targetId) + k.step));
    }
  }
  return !hits.empty();
}

} // namespace mp
