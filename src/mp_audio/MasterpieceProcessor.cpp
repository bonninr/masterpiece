#include "MasterpieceProcessor.h"

#include "../mp_core/Temperament.h"
#include <algorithm>
#include <cmath>
#include <memory>
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
    apvts_(*this, nullptr, "MP", makeLayout()) {}

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
  voices_.prepare(sampleRate_, graph_.maxVoices,
                  juce::jmax(1, getTotalNumOutputChannels()),
                  juce::jmax(1, samplesPerBlock));

  // Worker pool (ADR-012). Auto means cores - 1, leaving one for the rest of
  // the system; the audio thread renders a share itself, so the total doing
  // voice work is that count. Threads are started here, never in the callback.
  int threads = graph_.parallel.renderThreads;
  if (threads <= 0) {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    threads = cores > 2 ? cores - 1 : 1;
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
  // once and skip the per-enclosure scratch entirely.
  if (!enclosuresActive || busEnclosures_.empty()) {
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
  voices_.beginBlock();
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
    }
    source.channel = msg.getChannel();
    source.deviceId = deviceId;

    const bool logging = logMidi_.load(std::memory_order_acquire);
    if (logging && source.kind != MidiSourceKind::None)
      juce::Logger::writeToLog(
          "midi: in  dev=" + juce::String(deviceId) + " ch=" +
          juce::String(source.channel) + " " +
          (msg.isNoteOnOrOff() ? "note" : msg.isController() ? "cc" : "pc") +
          "=" + juce::String(source.number) + " val=" + juce::String(value));

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
          continue;
        case MidiTargetKind::StepperNext:
          stepperNext();
          continue;
        case MidiTargetKind::StepperPrev:
          stepperPrev();
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
          startNoteOnKeyboard(hit.keyboardId, key, hit.midiNote, hit.velocity);
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
    else if (msg.isAllNotesOff() || msg.isAllSoundOff()) {
      for (const auto& [note, held] : soundingNotes_) {
        (void)note;
        voices_.noteOff(held.id, NoteRelease{});
      }
      // Keys held as switches let go too, or their pallets stay open.
      while (!heldKeySwitches_.empty())
        stopNoteByKey(heldKeySwitches_.begin()->first, 0);
    }
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
  return sanitise(model_.organName) + "-" + pathHash(loadedOdf_);
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

juce::File MasterpieceProcessor::dataDirectory() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece");
}

juce::File MasterpieceProcessor::organFileForSaving(
    const juce::String& folder, const juce::String& extension) const {
  if (loadedOdf_.getFullPathName().isEmpty()) return {};
  // Always the organ's own identity, even when a legacy file was read: the
  // point of the migration is that it happens once.
  return dataDirectory()
      .getChildFile(folder)
      .getChildFile(juce::String(organKey()) + extension);
}

juce::File MasterpieceProcessor::organFile(const juce::File& odf,
                                          const juce::String& folder,
                                          const juce::String& extension) const {
  if (odf.getFullPathName().isEmpty()) return {};
  // Beside the player's own data, never inside the sample set: writing into a
  // licensed package is not ours to do.
  const auto dir = dataDirectory().getChildFile(folder);

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

void MasterpieceProcessor::pushMidi(int deviceId, const juce::MidiMessage& msg) {
  // Short messages only. Sysex is not something an organ console sends for a
  // stop or a key, and copying an unbounded blob here would mean allocating on
  // a real-time callback.
  const int size = msg.getRawDataSize();
  if (size <= 0 || size > 3) return;

  const uint32_t slot =
      midiWrite_.fetch_add(1, std::memory_order_acq_rel) % kMidiQueueSize;
  TaggedMidi& t = midiQueue_[slot];
  t.deviceId = deviceId;
  t.size = size;
  const auto* raw = msg.getRawData();
  for (int i = 0; i < size; ++i) t.bytes[i] = raw[i];
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

bool MasterpieceProcessor::saveSettings() const {
  const auto f = organFileForSaving("organs", ".mporgan");
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();

  juce::String text = "# Masterpiece per-organ settings\n";
  text << settingsBody();

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
    if (!combinationSet_.empty())
      text << "combset " << juce::String(combinationSet_) << "\n";
  }

  return f.replaceWithText(text);
}

bool MasterpieceProcessor::loadSettingsFor(const juce::File& odf) {
  pendingControlValues_.clear();
  // Routes and voicing belong to the organ being left, not the one arriving.
  // Keeping either would point this organ's rank ids at the previous organ's
  // mix, or worse, at its tuning.
  mixer_.rankRoutings.clear();
  voicing_.a.clear();
  voicing_.b.clear();
  voicing_.usingB = false;
  combinationSet_.clear();
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
  return dataDirectory().getChildFile("settings.mpglobal");
}

bool MasterpieceProcessor::writeGlobalFile() const {
  const auto f = globalSettingsFile();
  f.getParentDirectory().createDirectory();
  juce::String text;
  text << "# Masterpiece defaults for organs that have no settings of their own\n";
  text << globalBody_;
  text << "reopenlast " << (reopenLastOrgan_ ? 1 : 0) << "\n";
  text << "loadticks "
       << (loadTicks_.load(std::memory_order_acquire) ? 1 : 0) << "\n";
  if (cacheDir_.getFullPathName().isNotEmpty())
    text << "cachedir " << cacheDir_.getFullPathName() << "\n";
  if (lastOrgan_.getFullPathName().isNotEmpty())
    text << "lastorgan " << lastOrgan_.getFullPathName() << "\n";
  for (const auto& f : recentOrgans_) {
    if (f.getFullPathName().isNotEmpty() && f != lastOrgan_)
      text << "recentorgan " << f.getFullPathName() << "\n";
  }
  for (const auto& f : hiddenOrgans_) {
    if (f.getFullPathName().isNotEmpty())
      text << "hiddenorgan " << f.getFullPathName() << "\n";
  }

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
  if (!f.existsAsFile()) return false;

  juce::String body;
  auto sw = graph_.engineSwitch;
  for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString())) {
    if (line.trim().isEmpty() || line.trimStart().startsWith("#")) continue;
    const auto key = line.upToFirstOccurrenceOf(" ", false, false).trim();
    // A path may contain spaces, so the value is taken whole, not tokenised.
    const auto val = line.fromFirstOccurrenceOf(" ", false, false).trim();
    if (key == "reopenlast") {
      reopenLastOrgan_ = val.getIntValue() != 0;
    } else if (key == "loadticks") {
      loadTicks_.store(val.getIntValue() != 0, std::memory_order_release);
    } else if (key == "cachedir") {
      // A path, taken whole: the sample cache can be gigabytes, and a player
      // with a small fast disk and a large slow one wants to choose which of
      // them holds it.
      cacheDir_ = val.isEmpty() ? juce::File() : juce::File(val);
    } else if (key == "lastorgan") {
      lastOrgan_ = juce::File(val);
    } else if (key == "recentorgan") {
      if (val.isNotEmpty()) {
        const juce::File f(val);
        if (std::find(recentOrgans_.begin(), recentOrgans_.end(), f) == recentOrgans_.end())
          recentOrgans_.push_back(f);
      }
    } else if (key == "hiddenorgan") {
      if (val.isNotEmpty()) {
        const juce::File f(val);
        if (std::find(hiddenOrgans_.begin(), hiddenOrgans_.end(), f) == hiddenOrgans_.end())
          hiddenOrgans_.push_back(f);
      }
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
  if (lastOrgan_.existsAsFile()) {
    auto it = std::find(recentOrgans_.begin(), recentOrgans_.end(), lastOrgan_);
    if (it != recentOrgans_.end()) recentOrgans_.erase(it);
    recentOrgans_.insert(recentOrgans_.begin(), lastOrgan_);
  }
  graph_.engineSwitch = sw;
  globalBody_ = body;
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

void MasterpieceProcessor::addRecentOrgan(const juce::File& odf) {
  if (odf == juce::File() || odf.getFullPathName().isEmpty()) return;
  auto it = std::find(recentOrgans_.begin(), recentOrgans_.end(), odf);
  if (it != recentOrgans_.end())
    recentOrgans_.erase(it);
  recentOrgans_.insert(recentOrgans_.begin(), odf);
  if (recentOrgans_.size() > 50)
    recentOrgans_.resize(50);
  writeGlobalFile();
}

void MasterpieceProcessor::removeRecentOrgan(const juce::File& odf) {
  auto it = std::find(recentOrgans_.begin(), recentOrgans_.end(), odf);
  if (it != recentOrgans_.end()) {
    recentOrgans_.erase(it);
    writeGlobalFile();
  }
}

void MasterpieceProcessor::hideOrgan(const juce::File& odf) {
  if (odf.getFullPathName().isEmpty()) return;
  if (std::find(hiddenOrgans_.begin(), hiddenOrgans_.end(), odf) == hiddenOrgans_.end()) {
    hiddenOrgans_.push_back(odf);
    writeGlobalFile();
  }
}

void MasterpieceProcessor::unhideOrgan(const juce::File& odf) {
  auto it = std::find(hiddenOrgans_.begin(), hiddenOrgans_.end(), odf);
  if (it != hiddenOrgans_.end()) {
    hiddenOrgans_.erase(it);
    writeGlobalFile();
  }
}

bool MasterpieceProcessor::isOrganHidden(const juce::File& odf) const {
  return std::find(hiddenOrgans_.begin(), hiddenOrgans_.end(), odf) != hiddenOrgans_.end();
}

void MasterpieceProcessor::unloadOrgan() {
  loadedOdf_ = juce::File();
  model_ = OrganModel();
  organRootDir_.clear();
  stopsBySwitch_.clear();
  switches_.reset(model_);
  controls_.reset(model_);
  samples_.clear();
  engagedSwitches_.clear();
  buildPalletIndex();
  combinations_.reset(model_);
  stepper_.reset(model_);
}

void MasterpieceProcessor::setLastOrgan(const juce::File& odf) {
  if (odf.existsAsFile()) {
    auto it = std::find(recentOrgans_.begin(), recentOrgans_.end(), odf);
    if (it != recentOrgans_.end()) recentOrgans_.erase(it);
    recentOrgans_.insert(recentOrgans_.begin(), odf);
    if (recentOrgans_.size() > 50) recentOrgans_.resize(50);
  }
  if (lastOrgan_ == odf) return;
  lastOrgan_ = odf;
  writeGlobalFile();
}

juce::File MasterpieceProcessor::lastOrgan() const {
  // Answer only for a file that is still there: a set on a drive that is not
  // plugged in should open the file chooser, not an error.
  return lastOrgan_.existsAsFile() ? lastOrgan_ : juce::File();
}

void MasterpieceProcessor::setReopenLastOrgan(bool on) {
  if (reopenLastOrgan_ == on) return;
  reopenLastOrgan_ = on;
  // Not saveGlobalDefaults(): a preference about startup is not a request to
  // adopt the open organ's settings as everyone's.
  writeGlobalFile();
}

juce::File MasterpieceProcessor::defaultCacheDirectory() {
  return dataDirectory().getChildFile("cache");
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

bool MasterpieceProcessor::saveMidiMap() const {
  const auto f = organFileForSaving("midi", ".mpmidi");
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(juce::String(midiMap_.toText()));
}

bool MasterpieceProcessor::loadMidiMap() {
  midiMapRepaired_ = 0;
  const auto f = midiMapFileFor(loadedOdf_);
  if (f.getFullPathName().isEmpty() || !f.existsAsFile()) return false;
  const auto text = f.loadFileAsString();
  const bool ok = midiMap_.fromText(text.toStdString());

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

  auto resolveOne = [&](SampleRef& ref) {
    const auto it = resolved.find(ref.sampleId);
    if (it != resolved.end()) {
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
    if (const SampleBuffer* buf = provider(ref.sampleId))
      in.fileMidiNote = buf->fileMidiNote;

    const SamplePitchResult r =
        resolveSamplePitch(in, 440.0, model_.basePitchHz);
    ref.fileMidiNote = in.fileMidiNote;
    ref.resolvedPitchHz = r.hz;
    resolved.emplace(ref.sampleId,
                     std::make_pair(in.fileMidiNote, r.hz));
    const auto slot = static_cast<size_t>(r.route);
    if (slot < tally.size()) ++tally[slot];
  };

  for (auto& [id, ref] : model_.samples) resolveOne(ref);
  for (auto& [rankId, rank] : model_.ranks)
    for (auto& pipe : rank.pipes)
      for (auto& layer : pipe.layers) {
        for (auto& a : layer.attacks) resolveOne(a.sample);
        for (auto& rel : layer.releases) resolveOne(rel.sample);
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
                            organTuning_, 0);
  }

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

  return fallbackKeyboard_;
}

int MasterpieceProcessor::channelForKeyboard(Id keyboardId) const {
  for (const auto& b : midiMap_.keyboardBindings())
    if (b.keyboardId == keyboardId && b.channel > 0) return b.channel;
  const int code = couplers_.assignmentCodeFor(keyboardId);
  if (code >= 1 && code <= 16) return code;
  return 1;
}

void MasterpieceProcessor::startNote(int channel, int midiNote, int velocity) {
  startNoteOnKeyboard(keyboardForChannel(channel, noteDeviceId_),
                      noteKey(channel, midiNote), midiNote, velocity);
}

void MasterpieceProcessor::stopNote(int channel, int midiNote, int velocity) {
  stopNoteByKey(noteKey(channel, midiNote), velocity);
}

void MasterpieceProcessor::stopNoteByKey(int key, int velocity) {
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
        resolvePipes(model_, divisionId, reached.midiNote, stops);
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
      // same chest wobble by different amounts.
      const auto tm = model_.tremulantPipes.find(pipe.pipeId);
      if (tm != model_.tremulantPipes.end()) {
        const auto ti = tremIndexOf_.find(tm->second.tremulantId);
        if (ti != tremIndexOf_.end()) {
          vs.tremIndex = ti->second;
          // Decibels to a linear swing about unity, and percent of a
          // semitone to semitones.
          vs.tremAmpDepth = static_cast<float>(
              juce::Decibels::decibelsToGain(tm->second.ampDepthDb, -60.0) -
              1.0);
          vs.tremPitchDepth = tm->second.pitchDepthPct / 100.0;
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
  // The key is a switch, too, when the organ says so. Engaging it lets the
  // wiring open whatever pallets it reaches -- which is how an organ with no
  // StopRank plays at all, and how every organ's key action sounds. This comes
  // before the no-stops check: key action speaks with nothing drawn.
  if (!keySwitchByKey_.empty()) {
    const auto ks = keySwitchByKey_.find(static_cast<int>(keyboard) * 256 + midiNote);
    if (ks != keySwitchByKey_.end()) {
      palletVelocity_ = velocity;
      heldKeySwitches_[noteKeyId] = ks->second;
      setSwitchEngaged(ks->second, true);
    }
  }

  if (engagedStops_.empty()) {
    // nothing drawn: the organ is silent
    if (logMidi_.load(std::memory_order_acquire))
      juce::Logger::writeToLog(
          "midi:     NOTHING PLAYS: no stop is drawn, so no pipe can sound");
    return;
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
      startVoicesForKey(keyboard, midiNote, velocity, noteId, engagedStops_);

  if (anyStarted)
    soundingNotes_[noteKeyId] = HeldNote{noteId, keyboard, midiNote, velocity};

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
                                        reached.midiNote, stopSetScratch_);
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

namespace {
// Firing a frame is the same act whichever direction the sequencer moved.
} // namespace

bool MasterpieceProcessor::stepperNext() {
  const Id combo = stepper_.next();
  if (combo == 0) return false;
  fireCombination(combo);
  return true;
}

bool MasterpieceProcessor::stepperPrev() {
  const Id combo = stepper_.prev();
  if (combo == 0) return false;
  fireCombination(combo);
  return true;
}

bool MasterpieceProcessor::stepperGoto(int frame) {
  const Id combo = stepper_.gotoFrame(frame);
  if (combo == 0) return false;
  fireCombination(combo);
  return true;
}

void MasterpieceProcessor::fireCombination(Id comboId) {
  if (combinations_.captureMode()) {
    // Holding the setter and stepping SETS each frame as you pass it, which is
    // how an organist builds a sequence for a piece.
    combinations_.capture(comboId, [this](Id id) { return switchEngaged(id); });
    combinationsDirty_.store(true, std::memory_order_release);
    return;
  }
  recallScratch_.clear();
  combinations_.recall(comboId, recallScratch_);
  for (const auto& change : recallScratch_)
    setSwitchEngaged(change.switchId, change.engage);
}

void MasterpieceProcessor::setControlValue(Id controlId, int value) {
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
  s.temperament = organTuning_.name.empty() ? "Equal" : organTuning_.name;
  s.pitchHz = model_.basePitchHz;
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
  if (switches_.engaged(switchId) == engaged) return; // no edge, no noise

  // The organ's own setter. Holding it turns every piston press into a
  // capture, which is how a console works and how a player expects it to.
  if (switchId == setterSwitchId_ && setterSwitchId_ != 0)
    combinations_.setCaptureMode(engaged);

  // Set what the player set, then let the organ's own wiring decide what that
  // means. On a wired console the two are different switches: Lemmer's "Pedaal
  // koppel" is 1006 and every key action that reads it looks at 10101.
  switches_.set(switchId, engaged);
  engagedSwitches_ = switches_.engagedSwitches();

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
    if (midiFeedback_ && midiOut_ != nullptr) {
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
  return loadCombinations();
}

bool MasterpieceProcessor::copyCombinationSetTo(const std::string& name) const {
  const std::string clean = sanitiseSetName(name);
  if (clean == sanitiseSetName(combinationSet_)) return false;  // itself
  const auto f = organFileForSaving("combinations", setExtension(clean));
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(juce::String(combinations_.toText()));
}

bool MasterpieceProcessor::deleteCombinationSet(const std::string& name) const {
  const std::string clean = sanitiseSetName(name);
  // The default set is the organ's registrations, not a set someone made, so
  // there is no "delete" that leaves the organ in a sane state.
  if (clean.empty()) return false;
  const auto f = organFileForSaving("combinations", setExtension(clean));
  return !f.getFullPathName().isEmpty() && f.existsAsFile() && f.deleteFile();
}

bool MasterpieceProcessor::saveCombinations() const {
  const auto f = organFileForSaving("combinations", setExtension(combinationSet_));
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(juce::String(combinations_.toText()));
}

bool MasterpieceProcessor::saveCombinationsIfDirty() {
  if (!combinationsDirty_.exchange(false, std::memory_order_acq_rel))
    return false;
  return saveCombinations();
}

bool MasterpieceProcessor::loadCombinations() {
  const auto f = combinationFileFor(loadedOdf_);
  if (f.getFullPathName().isEmpty() || !f.existsAsFile()) return false;
  return combinations_.fromText(f.loadFileAsString().toStdString());
}

void MasterpieceProcessor::buildPalletIndex() {
  palletPipes_.clear();
  keySwitchByKey_.clear();
  heldKeySwitches_.clear();
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

  for (const auto& [switchId, key] : model_.keyboardKeys)
    keySwitchByKey_[static_cast<int>(key.keyboardId) * 256 + key.midiNote] = switchId;
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

    const uint64_t noteId = nextNoteId_++;
    for (const auto& layer : pipe.layers) {
      NoteStrike strike;
      strike.velocity = 100;
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

void MasterpieceProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  buffer.clear();

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
  // The audio thread keeps running through a load; keep it off the stage
  // table until it has been rebuilt for the new organ.
  stagesReady_.store(false, std::memory_order_release);
  // Starting the organ moves switches -- the blower, the init controls -- and
  // those can open pallets. No pallet may start a voice before the new
  // organ's audio is in place.
  palletsLive_.store(false, std::memory_order_release);

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

  // A Hauptwerk set puts its definitions in <root>/OrganDefinitions and its
  // audio in <root>/OrganInstallationPackages, so the root is the definition's
  // grandparent. deriveOrganRoot (mp_core, shared with the loader so the two
  // never disagree) also copes with a set that has been reorganised with
  // symlinks -- OrganDefinitions or OrganInstallationPackages relocated onto
  // another drive -- where the plain parent walk can land somewhere that no
  // longer has OrganInstallationPackages beside it.
  const juce::File root(mp::deriveOrganRoot(odfFile.getFullPathName().toStdString()));

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
  loadSettingsFor(odfFile);

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
  if (!loader.load(odfFile.getFullPathName().toStdString(), opts, loaded,
                   result.diagnostics)) {
    result.error = result.diagnostics.errors.empty()
                       ? "the organ definition could not be parsed"
                       : result.diagnostics.errors.front();
    return result;
  }

  phases.mark("odf parse");

  // Publish the model before the audio, so a note-on during loading resolves
  // pipes that simply have no sound yet rather than reading a half-built map.
  model_ = std::move(loaded);
  organRootDir_ = opts.organRootDir;
  loadedOdf_ = odfFile;

  // Console click -> stop. Without this a drawstop would move on screen and
  // the organ would stay silent, which is the worst of both.
  stopBySwitch_.clear();
  for (const auto& [stopId, stop] : model_.stops)
    if (stop.controllingSwitchId != 0)
      stopBySwitch_[stop.controllingSwitchId] = stopId;

  // The organ's switch wiring, solved once from the declared defaults. An
  // organ that ships with its blower running or a unison coupler drawn comes
  // up that way rather than needing the player to find a switch nobody told
  // them about.
  phases.mark("model: stop map");
  switches_.reset(model_);
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
  stepper_.reset(model_);

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

  if (graphicsOnly) {
    // Not merely "skip the load": the library may still hold the PREVIOUS
    // organ's audio, and pipe indices from this model would read into it.
    samples_.clear();
  } else {
    const int64_t head =
        maxFramesPerSample > 0 ? maxFramesPerSample : preloadHead_;
    // The ranks the caller asked for, if it asked for any.
    std::unordered_set<Id> onlyRanks;
    for (Id stopId : preloadStops_) {
      const auto it = model_.stops.find(stopId);
      if (it == model_.stops.end()) continue;
      for (const auto& e : it->second.ranks) onlyRanks.insert(e.rankId);
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
    if (!onlyRanks.empty())
      juce::Logger::writeToLog("load: PARTIAL -- " +
                               juce::String((int)onlyRanks.size()) +
                               " rank(s) of " + juce::String((int)model_.ranks.size()) +
                               "; every other stop will be silent");
    // What the cache is keyed to: which organ, and whether its definition has
    // changed since the cache was written. Both are cheap to read and neither
    // is guessable from the model alone.
    samples_.setCacheDir(cacheDirectory().getFullPathName().toStdString());
    samples_.setCacheIdentity(
        organKey(),
        odfFile.getFullPathName().toStdString() + "|" +
            std::to_string(odfFile.getSize()) + "|" +
            std::to_string(odfFile.getLastModificationTime().toMilliseconds()));

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
  if (loadProgress_.isCancelled()) {
    juce::Logger::writeToLog("load: cancelled, discarding partial organ");
    samples_.clear();
    model_ = OrganModel{};
    voices_.setSampleProvider(samples_.provider());
    loadProgress_.phase.store(LoadProgress::Phase::Cancelled,
                              std::memory_order_release);
    result.ok = false;
    result.error = "cancelled";
    return result;
  }
  voices_.setSampleProvider(samples_.provider());
  phases.mark(graphicsOnly ? "samples (skipped)" : "samples");

  // Nearly six seconds on a large set, after the sample counter has reached
  // its total and stopped moving. Without a phase of its own the dialog sat
  // at "12148 of 12148 samples" while it did something else entirely.
  loadProgress_.beginPhase(LoadProgress::Phase::Preparing);

  // Rebuild everything that is derived from the model. prepareToPlay may not
  // have run yet (headless), in which case it will pick this up when it does.
  if (sampleRate_ > 0.0)
    prepareToPlay(sampleRate_, juce::jmax(1, getBlockSize()));

  // A mapping and a set of combinations saved for this organ come back with
  // it. Missing is normal: it means the player has not saved any yet.
  loadMidiMap();
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
    else if (samples_.cacheBytesWritten() > 0)
      juce::Logger::writeToLog("cache: wrote " + mb(samples_.cacheBytesWritten()) +
                               " MB for the next load");
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
  setLastOrgan(odfFile);

  result.stopsEngaged = 0;
  // Only now: starting the organ above moves switches on this thread, and
  // the audio thread's own stage check must not move them at the same time.
  stagesReady_.store(true, std::memory_order_release);
  palletsLive_.store(true, std::memory_order_release);
  loadProgress_.phase.store(LoadProgress::Phase::Done,
                            std::memory_order_release);
  result.ok = true;
  return result;
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
  // Through the console, not around it: drawing every stop by hand is what a
  // player does to hear a tutti, and doing it any other way leaves the switch
  // states disagreeing with the stop list.
  for (const auto& [id, stop] : model_.stops) {
    (void)stop;
    setStopEngaged(id, true);
  }
  return static_cast<int>(engagedStops_.size());
}

void MasterpieceProcessor::loadOrganAsync(const juce::File& odfFile) {
  // Loading a 19 GB set must not block the message thread. The audio thread is
  // unaffected either way: it only ever reads the sample store through an
  // atomic pointer, and an unfinished load simply has no audio for a pipe yet.
  juce::Thread::launch([this, odfFile] { loadOrgan(odfFile); });
}

} // namespace mp
