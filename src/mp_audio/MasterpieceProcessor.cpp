#include "MasterpieceProcessor.h"
#include "DataFolder.h"
#include "SystemMemory.h"
#include "../mp_archive/OrganArchive.h"

#include "../mp_core/GrandOrgueImport.h"
#include "../mp_core/LoadPlan.h"
#include "../mp_core/PathCase.h"
#include "../mp_core/Utf8Path.h"
#include "../mp_core/SparseLoading.h"
#include "../mp_core/Temperament.h"
#include "RealtimeThread.h"
#include "Translations.h"
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <cmath>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace mp {

// The cores this program may run on. The machine's count is the usual
// answer; on Linux a program can be held to fewer of them -- taskset, a
// container, a cgroup -- and render threads beyond those only wait on each
// other for the same core: a full organ held to one core of eight ran its
// audio thread behind seven workers it had started for the other seven.
static int usableCores() {
  int cores = static_cast<int>(std::thread::hardware_concurrency());
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    const int allowed = CPU_COUNT(&set);
    if (allowed > 0 && (cores <= 0 || allowed < cores)) cores = allowed;
  }
#endif
  return cores;
}

int MasterpieceProcessor::defaultPolyphony() const {
  // GrandOrgue's 725 voices a processor thread, never below 0.7.11's 1,536:
  // a full pool takes a sounding pipe's place, and 768 on four threads took a
  // pedal release on every note of a Widor toccata (#320). Streamed releases
  // hold a 2-second ring a voice, 768 KB, so they keep 1,536.
  if (samples_.streamReleases()) return 1536;
  return std::min(maxPolyphonySetting(), std::max(1536, 725 * usableCores()));
}

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
  extremesDue_.reserve(16);  // a manual each, at most
  // The split keyboard's input, before any saved mapping names it (#358).
  split_.attach(registerMidiDevice(SplitKeyboardHost::kInputName),
                [this] { midiMapDirty_.store(true, std::memory_order_release); });
  // Before any organ is opened: a streamed set keeps many files open, and the
  // default allowance on macOS is 256.
  static const size_t tails = SampleLibrary::raiseOpenFileLimit();
  (void)tails;
  // A cache written while the organ plays waits for silence.
  samples_.setCacheBusy([this] { return soundingVoices_.load(std::memory_order_relaxed) > 0; });
  setReleaseCutoffDb(releaseCutoffDb_);
  if (const char* path = std::getenv("MASTERPIECE_BLOCKLOG"); path != nullptr && *path != 0) {
    blockLogPath_ = path;
    blockLog_.resize(400000);
  }
}

void MasterpieceProcessor::writeBlockLog() {
  if (blockLogPath_.empty() || blockLogCount_ == 0) return;
  std::FILE* f = std::fopen(blockLogPath_.c_str(), "w");
  if (f == nullptr) return;
  std::fprintf(f, "block total_us budget_us pre_us midi_us controls_us wind_us render_us dsp_us "
                  "note_ons note_offs active_before active_after stolen\n");
  for (size_t i = 0; i < blockLogCount_; ++i) {
    const auto& r = blockLog_[i];
    std::fprintf(f, "%zu %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %d %d %d %d %d\n", i, r.totalUs,
                 r.budgetUs, r.preUs, r.midiUs, r.controlsUs, r.windUs, r.renderUs, r.dspUs,
                 r.noteOns, r.noteOffs, r.activeBefore, r.activeAfter, r.stolen);
  }
  std::fclose(f);
}

static juce::String realtimeState(bool on);

// How many releases streaming at once the rings are sized for (#548). On an
// NVMe disk the reckoning gives the shortest ring, a quarter of a second,
// and thirty notes a second on one stop of four ranks, 899 releases
// streaming at once, played with no shortfall at that length on a two-core
// computer. On a hard disk it gives the longest.
static constexpr int kStreamsToFeed = 256;

// The ring a streamed voice can have, at most `wanted`, from the memory the
// ceiling and the system leave once the samples are in: the shortest ring is
// part of the load's own reckoning, and anything longer comes out of what is
// still free.
double MasterpieceProcessor::ringThatFits(double wanted) const {
  if (wanted <= kShortestRingSeconds) return wanted;
  const int voices = std::max(1, voicePool());
  const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
  const double bytesPerSecond = static_cast<double>(voices) * rate * 2.0 * sizeof(float);
  int64_t room = memoryLimitBytes() - samples_.residentBytes() -
                 voicePoolBytes(voicePool(), kShortestRingSeconds);
  if (const int64_t free = roomForPool(); free >= 0) room = std::min(room, free);
  if (room <= 0) return kShortestRingSeconds;
  return std::clamp(kShortestRingSeconds + static_cast<double>(room) / bytesPerSecond,
                    kShortestRingSeconds, wanted);
}

void MasterpieceProcessor::sizeStreamRings() {
  if (releaseBufferSeconds_ > 0.0) {
    const double ring = ringThatFits(releaseBufferSeconds_);
    voices_.setStreamSeconds(ring);
    if (ring < releaseBufferSeconds_)
      juce::Logger::writeToLog("streaming: rings of " + juce::String(ring, 2) +
                               " s, shorter than the release buffer chosen: the memory left allows no more");
    tailReadCost_ = {};
    if (samples_.streamedCount() > 0)
      juce::Logger::writeToLog("streaming: rings of " + juce::String(releaseBufferSeconds_, 2) +
                               " s, the release buffer chosen for this organ");
    return;
  }
  const auto tails = samples_.streamedTails();
  if (tails.empty()) {
    // Nothing streamed, and the pool still has rings while releases may
    // stream: the length kept from before has to fit what the load left. The
    // load counted the shortest; Giubiasco on an iPad (#650) got 2 s rings
    // for 1536 voices, 1125 MB past its limit, and iPadOS ended it.
    voices_.setStreamSeconds(ringThatFits(voices_.streamSeconds()));
    return;
  }
  const double t0 = juce::Time::getMillisecondCounterHiRes();
  tailReadCost_ = measureTailReads(tails);
  const double took = juce::Time::getMillisecondCounterHiRes() - t0;
  const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
  // Nothing long enough to time is little to stream: the shortest ring
  // serves it. The longest was given before, 2 s a voice, to the sets with
  // the least to read.
  const double wanted = tailReadCost_.files > 0 ? ringSecondsFor(tailReadCost_, rate, kStreamsToFeed)
                                                : kShortestRingSeconds;
  const double ring = ringThatFits(wanted);
  voices_.setStreamSeconds(ring);
  if (ring < wanted)
    juce::Logger::writeToLog("streaming: rings of " + juce::String(ring, 2) + " s, where the disk asks " +
                             juce::String(wanted, 2) + " s: the memory left allows no more");
  if (tailReadCost_.files <= 0) {
    juce::Logger::writeToLog("streaming: no release long enough to time; rings of " +
                             juce::String(ring, 2) + " s");
    return;
  }
  const double perSecond =
      tailReadCost_.framesPerMs > 0.0 ? rate / tailReadCost_.framesPerMs : 0.0;
  juce::Logger::writeToLog(
      "streaming: reads of " + juce::String(tailReadCost_.files) + " release files, " +
      juce::String(tailReadCost_.latencyMs, 2) + " ms each plus " + juce::String(perSecond, 1) +
      " ms a second of sound, the first " + juce::String(tailReadCost_.openMs, 1) +
      " ms; rings of " + juce::String(ring, 2) + " s (measured in " + juce::String(took, 0) +
      " ms)");
}

// The voice pool and the threads that render it, at the pool size chosen.
// prepareToPlay builds it, and a larger polyphony builds it again with the
// organ loaded: the samples stay, only the voices are new.
void MasterpieceProcessor::buildVoicePool(int blockFrames) {
  // Voice pool is allocated once, here: render() must never allocate.
  voices_.setSimd(fasterEngine_.load());
  // The pool the player chose, with a ring per voice only when releases can
  // stream.
  // What is loaded may stream though the choice for the next load says
  // otherwise: either way, rings.
  const bool rings = samples_.streamReleases() || samples_.streamedCount() > 0;
  voices_.setStreamRings(rings);
  // Windows to read samples held compressed, when the organ loaded has any.
  const bool coded = samples_.codedCount() > 0;
  voices_.setCodedWindows(coded);
  // As many voices as the system can give: a pool past what is free would
  // have the system end the program as it is made.
  graph_.maxVoices = voicesThatFit(voicePool(), rings, coded);
  voices_.prepare(sampleRate_, graph_.maxVoices,
                  juce::jmax(1, getTotalNumOutputChannels()),
                  juce::jmax(1, blockFrames));
  juce::Logger::writeToLog("voices: " + juce::String(graph_.maxVoices) + ", " +
                           juce::String(voicePoolBytes(graph_.maxVoices, voices_.streamSeconds()) / (1024 * 1024)) +
                           " MB" + (samples_.streamReleases() ? " with release streaming" : ""));
  // Which path renders, for any report about the sound: the new engine is
  // the first thing to rule in or out.
  juce::Logger::writeToLog(juce::String("audio engine: ") +
                           (voices_.simd() == simd::Isa::None
                                ? "per-frame"
                                : juce::String("faster (") + simd::isaName(voices_.simd()) + ")"));

  startRenderThreads(blockFrames);
}

int MasterpieceProcessor::usableProcessors() { return usableCores(); }

int MasterpieceProcessor::autoRenderThreads(int cores, bool everyProcessor) {
  // Every logical processor, when the player asks for it: the audio thread
  // and a worker on each of the others, with no ceiling.
  if (everyProcessor) return std::max(1, cores);
  // Otherwise cores - 1, leaving one for the rest of the system.
  int threads = cores > 2 ? cores - 1 : 1;
  // But not every core of a big machine: a block is shared out only above
  // minVoicesPerThread voices per thread, so 23 threads on a 24-thread Mac
  // Pro (#120) wait for over 700 voices before helping, and every worker
  // woken is one more the audio thread waits on. Eight carry any console.
  return std::min(threads, 8);
}

void MasterpieceProcessor::startRenderThreads(int blockFrames) {
  // Worker pool (ADR-012). The audio thread renders a share itself, so the
  // total doing voice work is the count. Threads are started here, never in
  // the callback.
  int threads = graph_.parallel.renderThreads;
  if (threads <= 0) threads = autoRenderThreads(usableCores(), everyProcessor_.load());
  voices_.setRenderThreads(threads, graph_.parallel.minVoicesPerThread);
  juce::Logger::writeToLog("render threads: " + juce::String(voices_.renderThreads()) + " of " +
                           juce::String(usableCores()) + " logical processors" +
                           (everyProcessor_.load() ? ", every processor chosen" : ""));
  // New workers, and the length of a block may be new: each sets itself
  // again, and the audio thread with them.
  blockSeconds_.store(juce::jmax(1, blockFrames) / sampleRate_);
  realtimeGeneration_.fetch_add(1);
  if (realtimeThreads_.load()) {
    voices_.setWorkerHook([this] { realtimeHere(); });
    juce::Logger::writeToLog("real-time render threads: " + realtimeState(true));
  }
}

// For the log: how the switch works here, or that the system will not let it.
static juce::String realtimeState(bool on) {
  if (!on) return "off";
  const juce::String how = realtime::method();
  return how.isEmpty() ? juce::String("on, but this system gives the user no real-time rights (the manual's "
                                      "Real-time render threads says how to get them)")
                       : "on (" + how + ")";
}

juce::String MasterpieceProcessor::realtimeMethod() { return realtime::method(); }
juce::String MasterpieceProcessor::realtimeRefusal() { return juce::String(realtime::refusal()); }

// Render workers real-time now, and of those in an audio workgroup: up when
// one is, down when it stops or its thread ends.
static std::atomic<int> realtimeWorkersNow{0};
static std::atomic<int> workgroupWorkersNow{0};

int MasterpieceProcessor::realtimeWorkers() { return realtimeWorkersNow.load(); }
int MasterpieceProcessor::workgroupWorkers() { return workgroupWorkersNow.load(); }

void MasterpieceProcessor::realtimeHere() {
  // The thread's own: the workgroup token is left on the thread that joined,
  // and everything is undone when the thread ends.
  struct State {
    juce::WorkgroupToken token;
    bool inGroup = false;
    bool realtime = false;
    void leaveGroup() {
      token.reset();
      if (inGroup) workgroupWorkersNow.fetch_sub(1);
      inGroup = false;
    }
    void normal() {
      leaveGroup();
      if (realtime) {
        realtime::makeNormal();
        realtimeWorkersNow.fetch_sub(1);
      }
      realtime = false;
    }
    ~State() { normal(); }
  };
  thread_local State here;

  if (!realtimeThreads_.load()) {
    here.normal();
    return;
  }
  here.leaveGroup();
  const bool was = here.realtime;
  here.realtime = realtime::makeRealtime(blockSeconds_.load());
  if (here.realtime != was) realtimeWorkersNow.fetch_add(here.realtime ? 1 : -1);
  if (!here.realtime) return;

  // macOS and iOS: with the audio thread, to its deadline.
  juce::AudioWorkgroup workgroup;
  {
    const std::lock_guard<std::mutex> lock(workgroupMutex_);
    workgroup = workgroup_;
  }
  if (!workgroup) return;
  workgroup.join(here.token);
  if (here.token) {
    workgroupWorkersNow.fetch_add(1);
    here.inGroup = true;
  }
}

void MasterpieceProcessor::setRealtimeThreads(bool on) {
  realtimeThreads_.store(on);
  // Run on every worker, to become real-time or to stop, and on the audio
  // thread at its next block.
  voices_.setWorkerHook([this] { realtimeHere(); });
  realtimeGeneration_.fetch_add(1);
  juce::Logger::writeToLog("real-time render threads: " + realtimeState(on));
  writeGlobalFile();
}

bool MasterpieceProcessor::realtimeThreadsSupported() { return realtime::supported(); }

void MasterpieceProcessor::setEveryProcessor(bool on) {
  if (everyProcessor_.load() == on) return;
  everyProcessor_.store(on);
  // The workers start again under the audio lock, between two blocks; the
  // voice pool and what sounds stay as they are.
  {
    const AudioLock lock(*this);
    startRenderThreads(getBlockSize());
  }
  writeGlobalFile();
}

void MasterpieceProcessor::audioWorkgroupContextChanged(const juce::AudioWorkgroup& workgroup) {
  {
    const std::lock_guard<std::mutex> lock(workgroupMutex_);
    workgroup_ = workgroup;
  }
  juce::Logger::writeToLog(
      workgroup ? "audio workgroup: the device has one, for up to " +
                      juce::String(static_cast<int>(workgroup.getMaxParallelThreadCount())) +
                      " threads"
                : juce::String("audio workgroup: the device has none"));
  if (realtimeThreads_.load()) voices_.setWorkerHook([this] { realtimeHere(); });
}

void MasterpieceProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  maxBlock_ = juce::jmax(1, samplesPerBlock);
  pairScratch_.setSize(2, maxBlock_, false, true, false);
  recordScratch_.setSize(2, maxBlock_, false, true, false);
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
  busScratchAll_.setSize(juce::jmax(1, getTotalNumOutputChannels()) * (unenclosedBus_ + 1),
                         juce::jmax(1, samplesPerBlock), false, true, false);
  busOuts_.assign(static_cast<size_t>(unenclosedBus_ + 1 + RankEq::kMaxGroups), nullptr);
  voices_.setBusCount(unenclosedBus_ + 1 + RankEq::kMaxGroups);
  eqScratch_.setSize(juce::jmax(1, getTotalNumOutputChannels()) * RankEq::kMaxGroups,
                     juce::jmax(1, samplesPerBlock), false, true, false);
  configureRankEq();

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
  voicingEq_.configure(voicingEqSettings_, sampleRate);
  voicingEq_.reset();
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

  buildVoicePool(samplesPerBlock);
  // Worst case one pipe per rank sounding on a single key.
  resolveScratch_.reserve(model_.ranks.empty() ? 64 : model_.ranks.size());
  soundingNotes_.reserve(128);
  // A block's MIDI, and the place of each message in it.
  midiScratch_.reserve(1024);
  midiOffsets_.reserve(1024);
  // The key-flow walk runs on every note-on and must not allocate: one press
  // on a fully coupled console reaches every division at several pitches.
  keyFlow_.reserve(64);
  // std::max, not juce::jmax: on 64-bit macOS size_t is `unsigned long`,
  // and juce_dsp's jmax overload for SIMDRegister then instantiates
  // SIMDNativeOps<unsigned long>, which the SSE header does not define.
  expandScratch_.reserve(std::max<size_t>(16, model_.divisions.size() * 4));
  metronome_.prepare(sampleRate_);
  limiter_.prepare(sampleRate_);
  compressor_.prepare(sampleRate_);

  // Meter fall: 0.3 s to decay by 1/e. Computed here so the audio thread
  // never calls a transcendental function for the sake of a lamp.
  meterFall_ = sampleRate_ > 0.0
                   ? std::exp(-static_cast<float>(samplesPerBlock) /
                              static_cast<float>(sampleRate_ * 0.3))
                   : 0.5f;
  recorder_.prepare(sampleRate_);
  scorePlayer_.prepare(sampleRate_);
  archive_.prepare(sampleRate_);
  // A block of a take, and Panic's 2048 releases, without allocating on the
  // audio thread.
  recordedPlayback_.reserve(4096);
  panicNotes_.reserve(16 * 128);
  outgoing_.ensureSize(1024);
  lampOut_.ensureSize(512);
  sustainedKeys_.reserve(256);
}

void MasterpieceProcessor::releaseResources() {
  const AudioLock audio(*this);
  voices_.quiesce();
  soundingNotes_.clear();
  releaseAllCoupledInputs();
  sustainedKeys_.clear();
  sustainDown_ = false;
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
  } else if (phase == LoadProgress::Phase::LoadingSamples ||
             phase == LoadProgress::Phase::ReadingCache) {
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
  busOutputs_.assign(mixBusOrder_.size(), {-1, -1});
  for (size_t i = 0; i < mixBusOrder_.size(); ++i)
    if (const MixerBus* b = mixer_.bus(mixBusOrder_[i]); b != nullptr && !b->deviceChannels.empty()) {
      const int left = b->deviceChannels[0];
      const int right = b->deviceChannels.size() > 1 ? b->deviceChannels[1] : left;
      if (left >= 0 && right >= 0 && left < kMaxOutputChannels && right < kMaxOutputChannels)
        busOutputs_[i] = {left, right};
    }
  refreshBusReverbs();
  refreshBusBass();
  if (const int wanted = outputChannelsWanted(); wanted != lastOutputChannelsWanted_) {
    lastOutputChannelsWanted_ = wanted;
    if (onOutputChannelsWanted) onOutputChannelsWanted();
  }
}

std::pair<int, int> MasterpieceProcessor::busOutputFor(int index, int numCh) const {
  if (index >= 0 && index < static_cast<int>(busOutputs_.size())) {
    const auto own = busOutputs_[static_cast<size_t>(index)];
    if (own.first >= 0 && own.first < numCh && own.second < numCh) return own;
  }
  return {0, numCh > 1 ? 1 : 0};
}

void MasterpieceProcessor::refreshBusBass() {
  busBass_.clear();
  busBass_.resize(mixBusOrder_.size());
  for (size_t i = 0; i < mixBusOrder_.size(); ++i) {
    const BusBass* b = mixer_.bassFor(mixBusOrder_[i]);
    if (b == nullptr || !b->active()) continue;
    auto m = std::make_unique<BassManager>();
    m->setCrossoverHz(b->crossoverHz);
    m->prepare(sampleRate_ > 0.0 ? sampleRate_ : 48000.0, juce::jmax(1, maxBlock_));
    busBass_[i] = std::move(m);
  }
}

void MasterpieceProcessor::setBusBass(int busId, float crossoverHz, int subChannel) {
  if (crossoverHz <= 0.0f || subChannel < 0) {
    mixer_.busBass.erase(busId);
  } else {
    BusBass b;
    b.crossoverHz = juce::jlimit(BassManager::kMinHz, BassManager::kMaxHz, crossoverHz);
    b.subChannel = juce::jlimit(0, kMaxOutputChannels - 1, subChannel);
    mixer_.busBass[busId] = b;
  }
  refreshMixerBuses();
  // Kept with the pairs: in the organ's settings, and in the defaults when
  // the player saves them as such.
  markSettingsDirty();
}

bool MasterpieceProcessor::needsRoutedOutput(int numCh) const {
  if (anyBusOnItsOwnChannels(numCh)) return true;
  for (size_t i = 0; i < busBass_.size() && i < mixBusOrder_.size(); ++i)
    if (busBass_[i] != nullptr)
      if (const BusBass* b = mixer_.bassFor(mixBusOrder_[i]); b != nullptr && b->subChannel < numCh) return true;
  return false;
}

bool MasterpieceProcessor::anyBusOnItsOwnChannels(int numCh) const {
  if (numCh <= 2) return false;
  for (size_t i = 0; i < busOutputs_.size(); ++i) {
    const auto at = busOutputFor(static_cast<int>(i), numCh);
    if (at.first != 0 || at.second != 1) return true;
  }
  return false;
}

int MasterpieceProcessor::outputChannelsWanted() const {
  int highest = 1;
  for (const auto& b : mixer_.buses)
    for (int ch : b.deviceChannels)
      if (ch >= 0 && ch < kMaxOutputChannels) highest = juce::jmax(highest, ch);
  for (const auto& [id, b] : mixer_.busBass)
    if (b.active() && b.subChannel < kMaxOutputChannels) highest = juce::jmax(highest, b.subChannel);
  return highest + 1;
}

bool MasterpieceProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  for (const auto& in : layouts.inputBuses)
    if (!in.isDisabled()) return false;
  if (layouts.outputBuses.size() != 1) return false;
  const int outs = layouts.outputBuses[0].size();
  return outs >= 1 && outs <= kMaxOutputChannels;
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
//
// The control reports the nearest step with a half rounding down: no pressure
// is 63, below the 63.5 the sets' linkages subtract. Rounded up it read 64,
// and a delay ramp built on (value - 63.5) * 63.5 -- Azzio's Sesquialtera and
// tremulant -- sat a quarter open at rest (#285). The linkages read the
// pressure itself: the same ramp at its 4 inch working pressure is 65.5, which
// as the step 65 stopped it at 95 of 127 (#546).
void MasterpieceProcessor::publishWindPressures() {
  for (const auto& [compartment, control] : windGauges_)
    controls_.setExact(control, 63.5 + wind_.pressureFor(compartment) * 0.5);
}

// The ranks and divisions in id order, as the control API lists them, each
// rank under the division of the first stop (by id) that draws it.
void MasterpieceProcessor::configureRankEq() {
  rankEqIndex_.clear();
  rankEqRanks_.clear();
  rankEqDivisions_.clear();
  std::vector<Id> stops;
  for (const auto& [id, r] : model_.ranks) rankEqRanks_.push_back(id);
  for (const auto& [id, d] : model_.divisions) rankEqDivisions_.push_back(id);
  for (const auto& [id, s] : model_.stops) stops.push_back(id);
  std::sort(rankEqRanks_.begin(), rankEqRanks_.end());
  std::sort(rankEqDivisions_.begin(), rankEqDivisions_.end());
  std::sort(stops.begin(), stops.end());
  std::unordered_map<Id, int> divisionIndex;
  for (size_t i = 0; i < rankEqDivisions_.size(); ++i) divisionIndex[rankEqDivisions_[i]] = static_cast<int>(i);
  std::unordered_map<Id, int> rankDivision;
  for (Id stopId : stops) {
    const Stop& stop = model_.stops.at(stopId);
    const auto d = divisionIndex.find(stop.divisionId);
    if (d == divisionIndex.end()) continue;
    for (const auto& e : stop.ranks) rankDivision.emplace(e.rankId, d->second);
  }
  std::vector<int> divisionOfRank;
  for (size_t i = 0; i < rankEqRanks_.size(); ++i) {
    rankEqIndex_[rankEqRanks_[i]] = static_cast<int>(i);
    const auto d = rankDivision.find(rankEqRanks_[i]);
    divisionOfRank.push_back(d == rankDivision.end() ? -1 : d->second);
  }
  rankEq_.configure(std::move(divisionOfRank), static_cast<int>(rankEqDivisions_.size()), unenclosedBus_ + 1,
                    sampleRate_);
  pushVoicingEq();
}

void MasterpieceProcessor::pushVoicingEq() {
  std::vector<ShelfEq> ranks(rankEqRanks_.size()), divisions(rankEqDivisions_.size());
  if (graph_.engineSwitch.enableVoicing && !graph_.engineSwitch.simpleWavOnly) {
    const VoicingSet& v = voicing_.live();
    for (size_t i = 0; i < rankEqRanks_.size(); ++i) {
      const PipeVoicing pv = v.rank(rankEqRanks_[i]);
      ranks[i] = {pv.bassDb, pv.brightnessDb, pv.balance, pv.width};
    }
    for (size_t i = 0; i < rankEqDivisions_.size(); ++i) {
      const PipeVoicing pv = v.division(rankEqDivisions_[i]);
      divisions[i] = {pv.bassDb, pv.brightnessDb, pv.balance, pv.width};
    }
  }
  rankEq_.setParams(ranks, divisions);
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
  // The ranks with an EQ play into group buses of their own (RankEq.h); with
  // none, there are none, and this is the engine as it was.
  const int groups = rankEq_.groupBuses();
  if (groups == 0 && (!enclosuresActive || busEnclosures_.empty() ||
                      numFrames > busScratch_.getNumSamples() || numCh > busScratch_.getNumChannels())) {
    voices_.render(dest.getArrayOfWritePointers(), numCh, numFrames, -1,
                   mixBusFilter);
    return;
  }

  const int numBuses = unenclosedBus_ + 1;
  // Every bus in one go: one pass over the voices and one hand-off to the
  // render threads for the block, where each bus made its own (#356 follow-up).
  const int perBus = busScratchAll_.getNumChannels() / juce::jmax(1, numBuses);
  const int eqPer = eqScratch_.getNumChannels() / RankEq::kMaxGroups;
  const bool groupsFit = groups == 0 || (numCh <= eqPer && numFrames <= eqScratch_.getNumSamples());
  if (numCh <= perBus && numFrames <= busScratchAll_.getNumSamples() &&
      static_cast<int>(busOuts_.size()) >= numBuses + groups && groupsFit) {
    busScratchAll_.clear(0, numFrames);
    float* const* all = busScratchAll_.getArrayOfWritePointers();
    for (int bus = 0; bus < numBuses; ++bus) busOuts_[static_cast<size_t>(bus)] = all + bus * perBus;
    float* const* eq = eqScratch_.getArrayOfWritePointers();
    for (int g = 0; g < groups; ++g) {
      busOuts_[static_cast<size_t>(numBuses + g)] = eq + g * eqPer;
      for (int ch = 0; ch < numCh; ++ch) juce::FloatVectorOperations::clear(eq[g * eqPer + ch], numFrames);
    }
    voices_.renderBuses(busOuts_.data(), numBuses + groups, numCh, numFrames, mixBusFilter);
    // Each EQ group filtered, with this pair's filter memory, and added into
    // its enclosure bus before the shades act on it.
    for (int g = 0; g < groups; ++g) {
      float* const* planes = busOuts_[static_cast<size_t>(numBuses + g)];
      rankEq_.process(g, mixBusFilter < 0 ? 0 : mixBusFilter, planes, numCh, numFrames);
      float* const* into = busOuts_[static_cast<size_t>(rankEq_.parentOf(g))];
      for (int ch = 0; ch < numCh; ++ch) juce::FloatVectorOperations::add(into[ch], planes[ch], numFrames);
    }
    for (int bus = 0; bus < numBuses; ++bus) {
      float* const* planes = busOuts_[static_cast<size_t>(bus)];
#if MP_ENABLE_DSP
      if (enclosuresActive && bus < static_cast<int>(busEnclosures_.size())) {
        const Id encId = busEnclosures_[static_cast<size_t>(bus)];
        const auto filterIt = enclosureFilters_.find(encId);
        const auto encIt = model_.enclosures.find(encId);
        if (filterIt != enclosureFilters_.end() && encIt != model_.enclosures.end()) {
          juce::dsp::AudioBlock<float> block(planes, static_cast<size_t>(numCh),
                                             static_cast<size_t>(numFrames));
          filterIt->second.processBlock(block, encIt->second,
                                        controls_.shutterFor(encIt->second),
                                        graph_.engineSwitch);
        }
      }
#endif
      for (int ch = 0; ch < numCh; ++ch) dest.addFrom(ch, 0, planes[ch], numFrames);
    }
    return;
  }
  // A block bigger than the buses were made for: every voice at once, and the
  // EQ left out of this one block, as the shades are.
  if (groups > 0) {
    voices_.render(dest.getArrayOfWritePointers(), numCh, numFrames, -1, mixBusFilter);
    return;
  }
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

void MasterpieceProcessor::renderBuses(juce::AudioBuffer<float>& output) {
  const int outCh = output.getNumChannels();
  const int numFrames = output.getNumSamples();
  if (outCh <= 0 || numFrames <= 0) return;

  // A block bigger than the one prepareToPlay was told of is played as blocks
  // of that size. A Jack or PipeWire server can raise its buffer size while
  // the player runs, with no new prepareToPlay, and the scratch the swell
  // boxes, the EQ groups and the rooms render through holds the prepared size
  // only: without this, a bigger block played every enclosed pipe as if the
  // box stood open (#554).
  if (numFrames > maxBlock_) {
    float* const* planes = output.getArrayOfWritePointers();
    for (int start = 0; start < numFrames; start += maxBlock_) {
      juce::AudioBuffer<float> part(planes, outCh, start, std::min(maxBlock_, numFrames - start));
      renderBuses(part);
    }
    return;
  }

  // One block for the whole callback, whatever the bus count.
  voices_.beginBlock(numFrames);
  // EQ groups whose EQ was taken away go once their last pipe has stopped.
  if (rankEq_.groupBuses() > 0) rankEq_.beginBlock([this](int bus) { return voices_.voicesOnBus(bus); });
  const juce::int64 w0 = blockLog_.empty() ? 0 : juce::Time::getHighResolutionTicks();
  advanceWind(numFrames);
  advanceTremulants(numFrames);
  advanceSway(numFrames);
  // The live levels: handed to the voices only while a fader is down.
  if (liveLevels_.advance(numFrames, sampleRate_))
    voices_.setLiveGains(liveLevels_.rankGains(), liveLevels_.rankCount(), liveLevels_.pairGains(),
                         LiveLevels::kMaxPairs);
  else
    voices_.setLiveGains(nullptr, 0, nullptr, 0);
  if (!blockLog_.empty())
    blockWindUs_ = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - w0) * 1.0e6;

  const int buses = static_cast<int>(mixBusOrder_.size());
  bool anyBusReverb = false;
#if MP_ENABLE_DSP
  if (!graph_.engineSwitch.simpleWavOnly)
    for (const auto& c : busConvolvers_)
      if (c != nullptr) anyBusReverb = true;
#endif

  // An output wider than a pair, with buses that name device channels it
  // has: each of those buses is rendered on its own, with its own room, and
  // added to its channels; the others go to the first pair. Without them,
  // below, every bus is summed into the first pair, as on a stereo device.
  if (needsRoutedOutput(outCh)) {
    if (pairScratch_.getNumSamples() < numFrames) pairScratch_.setSize(2, numFrames, false, false, true);
    for (int i = 0; i < buses; ++i) {
      const bool captured = mixBusCapture_ != nullptr && i < static_cast<int>(mixBusCapture_->size());
      juce::AudioBuffer<float> pair(pairScratch_.getArrayOfWritePointers(), 2, numFrames);
      juce::AudioBuffer<float>& dest = captured ? (*mixBusCapture_)[static_cast<size_t>(i)] : pair;
      dest.clear(0, numFrames);
      renderOneMixBus(dest, buses <= 1 ? -1 : i);
#if MP_ENABLE_DSP
      if (!graph_.engineSwitch.simpleWavOnly && i < static_cast<int>(busConvolvers_.size()) &&
          busConvolvers_[static_cast<size_t>(i)] != nullptr)
        busConvolvers_[static_cast<size_t>(i)]->process(dest);
#endif
      // Its lows to the subwoofer's channel, when the output has it; the
      // highs go on to the pair.
      if (i < static_cast<int>(busBass_.size()) && busBass_[static_cast<size_t>(i)] != nullptr)
        if (const BusBass* bass = mixer_.bassFor(mixBusOrder_[static_cast<size_t>(i)]);
            bass != nullptr && bass->subChannel < outCh)
          busBass_[static_cast<size_t>(i)]->process(dest.getWritePointer(0),
                                                    dest.getNumChannels() > 1 ? dest.getWritePointer(1) : nullptr,
                                                    output.getWritePointer(bass->subChannel), numFrames);
      const auto [left, right] = busOutputFor(i, outCh);
      const int last = dest.getNumChannels() - 1;
      output.addFrom(left, 0, dest, 0, 0, numFrames);
      output.addFrom(right, 0, dest, juce::jmin(1, last), 0, numFrames);
    }
    return;
  }

  // The first pair of the output: the whole of it on a stereo device.
  juce::AudioBuffer<float> buffer(output.getArrayOfWritePointers(), juce::jmin(2, outCh), numFrames);
  const int numCh = buffer.getNumChannels();

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
  // A chord that has settled: its top and bottom keys take the melody and
  // bass couplers now.
  if (!extremesDue_.empty()) settleExtremes();
  learnHeldKeys();
  midiScratch_.clear();
  midiOffsets_.clear();
  for (const auto meta : midi) {
    midiScratch_.emplace_back(MidiDeviceMap::kAnyDevice, meta.getMessage());
    midiOffsets_.push_back(meta.samplePosition);
  }
  drainTaggedMidi(midiScratch_);
  midiOffsets_.resize(midiScratch_.size(), 0);
  // One keyboard playing the whole organ (#358): its notes become the split
  // keyboard's, which the mapping below sends to the manuals.
  split_.process(midiScratch_, midiOffsets_, blockTimeMs_);
  // Console input reaches the recorder here: it is not in the host's buffer,
  // which is all the recorder saw.
  // The console's own recorder buttons are not part of the performance: a
  // take that held its Record press would press it again when played. The
  // learned Quit and Power-off buttons stay out for the same reason: a take
  // played back would quit the program or turn the computer off.
  const auto consoleCommand = [this](int deviceId, const juce::MidiMessage& msg) {
    MidiSource source;
    if (msg.isNoteOn()) {
      source.kind = MidiSourceKind::Note;
      source.number = msg.getNoteNumber();
    } else if (msg.isController()) {
      source.kind = MidiSourceKind::ControlChange;
      source.number = msg.getControllerNumber();
    } else if (msg.isProgramChange()) {
      source.kind = MidiSourceKind::ProgramChange;
      source.number = msg.getProgramChangeNumber();
    } else {
      return false;
    }
    source.channel = msg.getChannel();
    source.deviceId = deviceId;
    const auto kind = midiMap_.actionFor(source, 127).kind;
    return kind == MidiTargetKind::RecorderRecord || kind == MidiTargetKind::RecorderPlay ||
           kind == MidiTargetKind::QuitProgram || kind == MidiTargetKind::PowerOffComputer;
  };
  if (recorder_.isRecording())
    for (const auto& [deviceId, msg] : midiScratch_)
      if (deviceId != MidiDeviceMap::kAnyDevice && !consoleCommand(deviceId, msg))
        recorder_.captureLive(msg, deviceId);
  // The archive takes all of it, the host's buffer and the on-screen keys
  // included; a take being played back is added below and stays out.
  if (archiveOn_.load(std::memory_order_relaxed)) {
    for (const auto& [deviceId, msg] : midiScratch_)
      if (!consoleCommand(deviceId, msg)) archive_.capture(msg, deviceId);
  }
  // A take being played back: each event with the console it came from, so it
  // is routed the way it was when it was played. They go in after the split
  // keyboard, which has already had its say over what was recorded.
  const size_t takeFrom = midiScratch_.size();
  for (const auto& e : recordedPlayback_) {
    midiScratch_.emplace_back(e.deviceId, e.message);
    midiOffsets_.push_back(e.offset);
  }
  const size_t takeTo = midiScratch_.size();
  // And Panic's releases last of all, so a note a take puts down in this
  // block is released with the rest.
  for (const auto& m : panicNotes_) {
    midiScratch_.emplace_back(MidiDeviceMap::kAnyDevice, m);
    midiOffsets_.push_back(0);
  }
  split_.retagReplay(midiScratch_);

  for (size_t m = 0; m < midiScratch_.size(); ++m) {
    const int deviceId = midiScratch_[m].first;
    const juce::MidiMessage& msg = midiScratch_[m].second;
    // The voices this message starts begin at its place in the block. A
    // MIDI file played back, or a host's sequence, keeps its timing to the
    // sample this way; at the block's start, every note in a block of 2048
    // frames moved by up to 43 ms.
    eventOffset_ = midiOffsets_[m];

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

    // The noise filter: while learning, a key is held back until it has been
    // down a quarter second, and one let go sooner is dropped.
    if (source.kind == MidiSourceKind::Note && learnNoiseFilter_.load(std::memory_order_acquire) &&
        learningFrom(deviceId)) {
      holdForLearn(source, value);
      continue;
    }

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
        case MidiTargetKind::Switch: {
          // A button that toggles flips the switch as it is now: a stop a
          // cancel or a piston pushed in draws on the next press, where the
          // button's own memory said it was still out and took one press to
          // agree. A piston on such a button is pressed and let go each time,
          // so every press fires it (#366).
          auto apply = [this](const MidiAction& a) {
            if (a.toggled) {
              const auto sw = model_.switches.find(a.targetId);
              if (sw != model_.switches.end() && !sw->second.latching) {
                setSwitchEngaged(a.targetId, true);
                setSwitchEngaged(a.targetId, false);
              } else {
                setSwitchEngaged(a.targetId, !switches_.engaged(a.targetId));
              }
              return;
            }
            setSwitchEngaged(a.targetId, a.engage);
          };
          if (!action.idle) apply(action);
          // Every other stop the same button works, each by its own trigger
          // (#551).
          if (action.alsoDrives != nullptr)
            for (const auto& b : *action.alsoDrives)
              if (const MidiAction more = midiMap_.switchAction(b, value); more.valid()) apply(more);
          continue;
        }
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
        case MidiTargetKind::StepperFirst:
          stepperFirst();
          continue;
        case MidiTargetKind::StepperLast:
          stepperLast();
          continue;
        case MidiTargetKind::StepperForward10:
          stepperJump(10);
          continue;
        case MidiTargetKind::StepperBack10:
          stepperJump(-10);
          continue;
        case MidiTargetKind::StepperDigit:
          stepperDigit(static_cast<int>(action.targetId));
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
          setCaptureMode(action.toggled ? !captureMode() : action.engage);
          continue;
        // On the audio thread, so the file is written later, by the message
        // thread's next save of the global settings.
        case MidiTargetKind::PedalCombinationsCoupled:
          pedalCoupled_.store(action.toggled ? !pedalCoupled_.load() : action.engage);
          globalDirty_.store(true, std::memory_order_release);
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
        // The EQ's filters are worked out on the message thread, so the
        // preset waits there too: its id, or -1 and -2 for the next and the
        // one before.
        case MidiTargetKind::VoicingEqPreset:
          pendingEqPreset_.store(static_cast<int>(action.targetId), std::memory_order_release);
          continue;
        case MidiTargetKind::VoicingEqPresetNext:
          pendingEqPreset_.store(-1, std::memory_order_release);
          continue;
        case MidiTargetKind::VoicingEqPresetPrev:
          pendingEqPreset_.store(-2, std::memory_order_release);
          continue;
        // A temperament waits for the message thread, as the EQ presets do.
        case MidiTargetKind::Temperament:
          pendingTemperament_.store(action.targetId + 1, std::memory_order_release);
          continue;
        case MidiTargetKind::QuitProgram:
        case MidiTargetKind::PowerOffComputer:
          powerButton(action.kind, action.engage,
                      source.kind == MidiSourceKind::Note || source.kind == MidiSourceKind::ControlChange);
          continue;
        case MidiTargetKind::ScorePageForward:
        case MidiTargetKind::ScorePageBack:
          turnScorePage(action.kind == MidiTargetKind::ScorePageForward);
          continue;
        // Raised here, carried out on the message thread. Never from a take
        // being played: a button's press in a take is not the player's.
        case MidiTargetKind::RecorderRecord:
          if (m < takeFrom || m >= takeTo) pendingRecorder_.store(1, std::memory_order_release);
          continue;
        case MidiTargetKind::RecorderPlay:
          if (m < takeFrom || m >= takeTo) pendingRecorder_.store(2, std::memory_order_release);
          continue;
        case MidiTargetKind::RankLevel:
        case MidiTargetKind::DivisionLevel:
        case MidiTargetKind::PairLevel:
          setLiveLevelFromMidi(action.kind, action.targetId, action.value);
          if (action.alsoDrives != nullptr)
            for (const auto& b : *action.alsoDrives)
              setLiveLevelFromMidi(b.targetKind, b.targetId, MidiMap::controlValue(b, value));
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
          startNoteOnKeyboard(hit.keyboardId, key, transposed(hit.midiNote), hit.velocity,
                              hit.midiNote);
        else
          keyUp(key, hit.velocity);
      }
      continue;
    }
    if (logging && msg.isNoteOnOrOff())
      juce::Logger::writeToLog("midi:     unmapped default path, keyboard for ch=" +
                               juce::String(msg.getChannel()) + " is " +
                               juce::String(static_cast<int>(
                                   keyboardForChannel(msg.getChannel(), deviceId))));
    // The sustain pedal, when the player has turned it on and nothing is
    // learned for controller 64: a learned one was taken above.
    if (msg.isController() && msg.getControllerNumber() == 64 && sustainPedal()) {
      setSustainDown(msg.getControllerValue() >= 64);
      continue;
    }
    if (msg.isNoteOn())
      startNote(msg.getChannel(), msg.getNoteNumber(), msg.getVelocity());
    else if (msg.isNoteOff())
      stopNote(msg.getChannel(), msg.getNoteNumber(), msg.getVelocity());
    // All Sound Off, All Notes Off and the mode messages that imply it
    // (120, 123-127) are CHANNEL messages. Releasing every manual on any of
    // them silenced a whole console from one keyboard: some keyboards and
    // encoders send All Notes Off on their own channel as a matter of
    // course, and every other manual's held notes stopped with it (#33).
    //
    // Any other controller moves only what it was learned for, as in
    // GrandOrgue. A control's id is the organ's own number: taken as a CC
    // number, a MIDI file's breath controller swung the Ashton demo's Solo
    // swell and its RPN messages set Nancy's bellows pressure.
    else if (msg.isController() &&
             (msg.getControllerNumber() == 120 || msg.getControllerNumber() >= 123))
      releaseChannel(msg.getChannel(), deviceId);
  }
  // Voices started from anywhere else -- a stop drawn, a pallet -- begin at
  // the block's start.
  eventOffset_ = 0;
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
  return mp::dataFolder()
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
      mp::dataFolder()
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

bool MasterpieceProcessor::learningFrom(int deviceId) const {
  return midiListen_.load(std::memory_order_acquire) || midiMap_.learning() || keyboardLearn_ != 0 ||
         (deviceId > 0 && keyPick_.load(std::memory_order_acquire));
}

void MasterpieceProcessor::holdForLearn(const MidiSource& source, int value) {
  const auto same = [&source](const HeldForLearn& h) {
    return h.used && h.source.deviceId == source.deviceId && h.source.channel == source.channel &&
           h.source.number == source.number;
  };
  if (value == 0) {
    for (auto& h : heldForLearn_)
      if (same(h)) h.used = false;
    return;
  }
  HeldForLearn* slot = nullptr;
  for (auto& h : heldForLearn_)
    if (same(h)) slot = &h;
  for (auto& h : heldForLearn_)
    if (slot == nullptr && !h.used) slot = &h;
  if (slot == nullptr) return;  // sixteen keys down at once is not a learn
  *slot = {true, source, value, blockTimeMs_};
}

void MasterpieceProcessor::learnHeldKeys() {
  const HeldForLearn* first = nullptr;
  for (const auto& h : heldForLearn_)
    if (h.used && blockTimeMs_ - h.sinceMs >= kLearnHoldMs && (first == nullptr || h.sinceMs < first->sinceMs))
      first = &h;
  if (first == nullptr) {
    // Nothing learning any more: what was held is forgotten.
    if (!learningFrom(1))
      for (auto& h : heldForLearn_) h.used = false;
    return;
  }
  const HeldForLearn chosen = *first;
  for (auto& h : heldForLearn_) h.used = false;
  if (learningFrom(chosen.source.deviceId)) learnFromPress(chosen.source, chosen.value);
}

// What a press does to whichever learn is waiting, in the order the MIDI
// handling asks them.
void MasterpieceProcessor::learnFromPress(const MidiSource& source, int value) {
  if (midiListen_.load(std::memory_order_acquire)) {
    heard_ = source;
    heardValue_ = value;
    midiListen_.store(false, std::memory_order_release);
    heardReady_.store(true, std::memory_order_release);
    return;
  }
  if (midiMap_.learning()) {
    if (midiMap_.learnFrom(source)) midiMapDirty_.store(true, std::memory_order_release);
    return;
  }
  if (source.deviceId > 0 && keyPick_.load(std::memory_order_acquire)) {
    pickedChannel_.store(source.channel, std::memory_order_release);
    keyPick_.store(false, std::memory_order_release);
    return;
  }
  if (keyboardLearn_ != 0) learnKeyboardFrom(source.deviceId, source.channel, source.number);
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

void MasterpieceProcessor::mirrorSimpleWavParameter() {
  auto* p = apvts_.getParameter("simpleWavOnly");
  if (p == nullptr) return;
  const float value = graph_.engineSwitch.simpleWavOnly ? 1.0f : 0.0f;
  if (p->getValue() != value) p->setValueNotifyingHost(value);
}

juce::String MasterpieceProcessor::settingsBody() const {
  const auto& sw = graph_.engineSwitch;
  juce::String text;
  // A bit width rather than an enum ordinal, so the file stays readable
  // and a width added later does not renumber the existing ones.
  text << "storage "
       << (samples_.storage() == SampleStorage::Int16   ? 16
           : samples_.storage() == SampleStorage::Int20 ? 20
           : samples_.storage() == SampleStorage::Int24 ? 24
                                                        : 32)
       << "\n";
  text << "mono " << (samples_.loadMono() ? 1 : 0) << "\n";
  text << "compress " << (samples_.codeSamples() ? 1 : 0) << "\n";
  text << "programcouplers " << (programCouplers_ ? 1 : 0) << "\n";
  text << "releases " << static_cast<int>(samples_.releaseChoice()) << "\n";
  text << "releaselength " << samples_.releaseMaxMs() << "\n";
  text << "releasecutoff " << releaseCutoffDb_ << "\n";
  // The room this organ is played in: kept with the organ, the path last
  // because it can hold spaces.
  text << "reverb " << (convolver_.enabled() ? 1 : 0) << " " << juce::String(convolver_.mix(), 3)
       << " " << convolver_.impulseResponseFile().getFullPathName() << "\n";
  text << "rate " << juce::String(samples_.loadSampleRate(), 0) << "\n";
  text << "stream " << (samples_.streamReleases() ? 1 : 0) << "\n";
  text << "streamhead " << juce::String(samples_.streamHeadFrames()) << "\n";
  text << "streamheadpct " << samples_.streamHeadPercent() << "\n";
  text << "releasebuffer " << juce::String(releaseBufferSeconds_, 2) << "\n";
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
  // A bus's bass management: "busbass <busId> <crossoverHz> <subChannel>".
  {
    std::vector<int> withBass;
    for (const auto& [busId, b] : mixer_.busBass)
      if (b.active()) withBass.push_back(busId);
    std::sort(withBass.begin(), withBass.end());
    for (int busId : withBass) {
      const auto& b = mixer_.busBass.at(busId);
      text << "busbass " << busId << " " << juce::String(b.crossoverHz, 0) << " " << b.subChannel << "\n";
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
                        : val.getIntValue() == 20 ? SampleStorage::Int20
                        : val.getIntValue() == 32 ? SampleStorage::Float32
                                                  : SampleStorage::Int24);
  else if (key == "mono") samples_.setLoadMono(on);
  else if (key == "compress") samples_.setCodeSamples(on);
  else if (key == "programcouplers") programCouplers_ = on;
  else if (key == "releases")
    samples_.setReleaseChoice(static_cast<SampleLibrary::ReleaseChoice>(juce::jlimit(0, 3, val.getIntValue())));
  else if (key == "releaselength") samples_.setReleaseMaxMs(val.getIntValue());
  else if (key == "releasecutoff") setReleaseCutoffDb(val.getIntValue());
  else if (key == "reverb") {
    // "reverb <enabled> <mix> <path...>"
    auto rest = val.trim();
    const bool wet = rest.upToFirstOccurrenceOf(" ", false, false).getIntValue() != 0;
    rest = rest.fromFirstOccurrenceOf(" ", false, false).trim();
    const float mix = rest.upToFirstOccurrenceOf(" ", false, false).getFloatValue();
    const juce::String path = rest.fromFirstOccurrenceOf(" ", false, false).trim();
    if (path.isEmpty()) {
      if (convolver_.hasImpulseResponse()) convolver_.clear();
    } else if (juce::File(path) != convolver_.impulseResponseFile()) {
      if (!convolver_.loadImpulseResponse(juce::File(path)))
        juce::Logger::writeToLog("reverb: the impulse response saved with this organ was not found: " + path);
    }
    convolver_.setMix(mix);
    convolver_.setEnabled(wet && convolver_.hasImpulseResponse());
  }
  else if (key == "rate") samples_.setLoadSampleRate(val.getDoubleValue());
  // "cache" is a program setting now, read from the global file only. The
  // line every organ's file still carries from earlier versions is passed
  // over here, so it cannot undo the player's choice at each load.
  else if (key == "cache") {
  }
  else if (key == "stream") samples_.setStreamReleases(on);
  else if (key == "streamhead") samples_.setStreamHeadFrames(val.getLargeIntValue());
  else if (key == "streamheadpct") samples_.setStreamHeadPercent(val.getIntValue());
  else if (key == "releasebuffer") releaseBufferSeconds_ = std::max(0.0, val.getDoubleValue());
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
  } else if (key == "busbass") {
    const auto parts = juce::StringArray::fromTokens(val, " ", "");
    if (parts.size() >= 3 && parts[0].getIntValue() != 0) {
      BusBass b;
      b.crossoverHz = juce::jlimit(BassManager::kMinHz, BassManager::kMaxHz, parts[1].getFloatValue());
      b.subChannel = juce::jlimit(0, kMaxOutputChannels - 1, parts[2].getIntValue());
      mixer_.busBass[parts[0].getIntValue()] = b;
      refreshMixerBuses();
    }
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
  if (savesHeld()) return false;
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
  // CRLF endings writeTextFile writes.
  if (text == before.replace("\r\n", "\n")) return true;
  f.getParentDirectory().createDirectory();
  return mp::writeTextFile(f, text);
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

bool MasterpieceProcessor::saveCurrentState() {
  // Every latching switch that is not where the organ starts is recorded, so
  // the organ comes up with the registration the player left it in. A switch
  // at its default is left out: it starts there anyway.
  snapshotStates_.clear();
  snapshotStops_.clear();
  for (const auto& [id, s] : model_.switches)
    if (s.latching && switches_.engaged(id) != s.defaultEngaged)
      snapshotStates_.emplace_back(id, switches_.engaged(id));
  std::sort(snapshotStates_.begin(), snapshotStates_.end());
  snapshotStops_.assign(engagedStops_.begin(), engagedStops_.end());
  std::sort(snapshotStops_.begin(), snapshotStops_.end());
  return saveSettings();
}

bool MasterpieceProcessor::saveSettings() const {
  if (savesHeld()) return false;
  const auto f = organFileForSaving("organs", ".mporgan");
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();

  juce::String text = "# Masterpiece per-organ settings\n";
  text << settingsBody();
  text << rememberedStateLines();
  for (const auto& [id, on] : snapshotStates_)
    text << "state " << juce::String(id) << " " << (on ? 1 : 0) << "\n";
  for (Id id : snapshotStops_) text << "stopon " << juce::String(id) << "\n";

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
             << juce::String(pv.balance, 3) << " "
             << juce::String(pv.bassDb, 3) << " "
             << juce::String(pv.width, 3) << "\n";
      };
      for (Id id : v.rankIds()) line("rank", id, v.rank(id));
      for (Id id : v.pipeIds()) line("pipe", id, v.pipe(id));
      // A key of its own: a version that knows no division EQ skips it,
      // where a "division" kind on voicingadj would be read as a rank.
      for (Id id : v.divisionIds()) {
        const PipeVoicing pv = v.division(id);
        text << "voicingdiv " << slot << " " << juce::String(id) << " " << juce::String(pv.brightnessDb, 3)
             << " " << juce::String(pv.bassDb, 3) << " " << juce::String(pv.balance, 3) << " "
             << juce::String(pv.width, 3) << "\n";
      }
    };
    writeSet("a", voicing_.a);
    writeSet("b", voicing_.b);
    if (voicing_.usingB) text << "voicingslot b\n";
    // Which named set this organ was last using. Stored rather than assumed:
    // coming back and finding the recital registrations instead of the service
    // ones is a nasty surprise to meet mid-piece.
    if (licenceConfirmed_) text << "licenceconfirmed 1\n";
    // How many notes of a rank are loaded. Absent means every note.
    if (sparseEvery_ > 1) text << "sparse " << juce::String(sparseEvery_) << "\n";
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
           << p.layout << " " << (p.borderless ? 1 : 0) << "\n";
    if (!temperamentChoice_.empty())
      text << "temperament " << juce::String(temperamentChoice_) << "\n";
    if (masterPitchSetting() > 0.0)
      text << "pitchhz " << juce::String(masterPitchSetting(), 2) << "\n";
    if (transpose() != 0) text << "transpose " << transpose() << "\n";
    if (pipeDetuneCents() > 0.0f) text << "pipedetune " << juce::String(pipeDetuneCents(), 1) << "\n";
    if (windSwayCents() > 0.0f) text << "windsway " << juce::String(windSwayCents(), 1) << "\n";
    // Written whatever it is, off and flat too: an organ with a line keeps
    // it, and one without plays the default EQ.
    if (organOwnsEq_) text << "eq " << juce::String(voicingEqSettings_.toLine()) << "\n";
    if (attackDb() != 0.0f) text << "attack " << juce::String(attackDb(), 1) << " " << attackMs() << "\n";
  }

  return mp::writeTextFile(f, text);
}

bool MasterpieceProcessor::loadSettingsFor(const juce::File& odf) {
  pendingControlValues_.clear();
  pendingSwitchStates_.clear();
  snapshotStates_.clear();
  snapshotStops_.clear();
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
  voicingEqSettings_ = defaultEq_;
  organOwnsEq_ = false;
  voicingEq_.configure(voicingEqSettings_, sampleRate_);
  // Sparse loading belongs to the organ: another organ starts with every
  // note, and this one's own file says which notes it keeps.
  sparseEvery_ = 1;
  samples_.setSparseEvery(1);
  attackDb_.store(0.0f, std::memory_order_relaxed);
  attackMs_.store(40, std::memory_order_relaxed);
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
  pipeDetuneCents_.store(0.0f, std::memory_order_relaxed);
  windSwayCents_.store(0.0f, std::memory_order_relaxed);
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
    if (key == "state") {
      // "state <id> <0|1>": the starting state saved with saveCurrentState().
      // Kept as the snapshot as well, so the next save keeps what was loaded.
      snapshotStates_.emplace_back(
          static_cast<Id>(val.upToFirstOccurrenceOf(" ", false, false).getLargeIntValue()),
          val.fromFirstOccurrenceOf(" ", false, false).trim().getIntValue() != 0);
      continue;
    }
    if (key == "stopon") {
      snapshotStops_.push_back(static_cast<Id>(val.getLargeIntValue()));
      continue;
    }
    if (key == "licenceconfirmed") {
      licenceConfirmed_ = val.getIntValue() != 0;
      continue;
    }
    if (key == "voicingadj") {
      // "voicingadj a|b rank|pipe <id> <gainDb> <cents> <brightness> <balance> [<bass> [<width>]]"
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
      if (tok.size() >= 8) pv.bassDb = tok[7].getFloatValue();
      if (tok.size() >= 9) pv.width = tok[8].getFloatValue();
      VoicingSet& set = tok[0] == "b" ? voicing_.b : voicing_.a;
      const Id id = static_cast<Id>(tok[2].getLargeIntValue());
      if (tok[1] == "pipe") set.setPipe(id, pv);
      else set.setRank(id, pv);
      continue;
    }
    if (key == "voicingdiv") {
      // "voicingdiv a|b <divisionId> <brightness> <bass> [<pan> <width>]"
      auto tok = juce::StringArray::fromTokens(val, " ", "");
      tok.removeEmptyStrings();
      if (tok.size() < 4) continue;
      PipeVoicing pv;
      pv.brightnessDb = tok[2].getFloatValue();
      pv.bassDb = tok[3].getFloatValue();
      if (tok.size() >= 6) {
        pv.balance = tok[4].getFloatValue();
        pv.width = tok[5].getFloatValue();
      }
      (tok[0] == "b" ? voicing_.b : voicing_.a).setDivision(static_cast<Id>(tok[1].getLargeIntValue()), pv);
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
                                tok.size() >= 6 ? tok[5].getIntValue() : -1,
                                tok.size() >= 7 ? tok[6].getIntValue() != 0 : false});
      continue;
    }
    if (key == "eq") {
      VoicingEqSettings eq;
      if (eq.fromLine(val.toStdString())) {
        voicingEqSettings_ = eq;
        organOwnsEq_ = true;
        voicingEq_.configure(eq, sampleRate_);
      }
      continue;
    }
    if (key == "transpose") {
      transpose_.store(juce::jlimit(-12, 12, val.getIntValue()), std::memory_order_relaxed);
      continue;
    }
    if (key == "pipedetune") {
      pipeDetuneCents_.store(juce::jlimit(0.0f, kMaxUnevennessCents, val.getFloatValue()), std::memory_order_relaxed);
      continue;
    }
    if (key == "windsway") {
      windSwayCents_.store(juce::jlimit(0.0f, kMaxUnevennessCents, val.getFloatValue()), std::memory_order_relaxed);
      continue;
    }
    if (key == "attack") {
      const auto tok = juce::StringArray::fromTokens(val, " ", "");
      if (tok.size() >= 2) {
        attackDb_.store(juce::jlimit(-12.0f, 12.0f, tok[0].getFloatValue()), std::memory_order_relaxed);
        attackMs_.store(juce::jlimit(5, 300, tok[1].getIntValue()), std::memory_order_relaxed);
      }
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
    if (key == "sparse") {
      // How many notes are loaded: 1 for every note, 2 for every second, 3 for
      // every third. Absent means every note.
      sparseEvery_ = juce::jlimit(1, 3, val.getIntValue());
      samples_.setSparseEvery(sparseEvery_);
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
  return settingsRoot().getChildFile("settings.mpglobal");
}

juce::File MasterpieceProcessor::settingsRoot() {
  return mp::dataFolder();
}

void MasterpieceProcessor::saveEverythingPending() {
  saveCombinationsIfDirty();
  saveSettingsIfDirty();
  saveMidiMapIfDirty();
  saveMasterGainIfDirty();
  saveRememberedStateIfPending();
}

void MasterpieceProcessor::holdSavesUntilReload() {
  holdSaves_.store(true, std::memory_order_release);
  combinationsDirty_.store(false, std::memory_order_release);
  settingsDirty_.store(false, std::memory_order_release);
  midiMapDirty_.store(false, std::memory_order_release);
  masterGainDirty_.store(false, std::memory_order_release);
  rememberedMovedAtMs_.store(0u, std::memory_order_release);
}

bool MasterpieceProcessor::writeGlobalFile() const {
  if (savesHeld()) return false;
  const auto f = globalSettingsFile();
  f.getParentDirectory().createDirectory();
  juce::String text;
  text << "# Masterpiece defaults for organs that have no settings of their own\n";
  text << globalBody_;
  // A new key: the old one was written on every save, whether or not anyone
  // chose it, so it cannot tell a choice from a default. Only this one counts.
  text << "reopenlastorgan " << (reopenLastOrgan_ ? 1 : 0) << "\n";
  if (!updateCheck_) text << "updatecheck 0\n";
  if (updateCheckedAt_ > 0) text << "updatecheckedat " << updateCheckedAt_ << "\n";
  if (remote_.enabled) text << "api 1\n";
  if (remote_.port != RemoteSettings{}.port) text << "apiport " << remote_.port << "\n";
  if (!remote_.allowChanges) text << "apiwrite 0\n";
  if (!remote_.token.empty()) text << "apitoken " << juce::String::fromUTF8(remote_.token.c_str()) << "\n";
  if (remote_.allowPowerOff) text << "apipoweroff 1\n";
  if (const auto lamp = readyLamp(); lamp.kind != MidiSourceKind::None)
    text << "readylamp " << (lamp.kind == MidiSourceKind::Note ? "note" : "cc") << " " << lamp.channel << " "
         << lamp.number << "\n";
  if (!loadMessage_.empty()) text << "loadmessage " << loadMessage() << "\n";
  if (!unloadMessage_.empty()) text << "unloadmessage " << unloadMessage() << "\n";
  if (!lampLit_.empty()) text << "readylamplit " << readyLampLitMessages() << "\n";
  if (!lampDark_.empty()) text << "readylampdark " << readyLampDarkMessages() << "\n";
  if (remote_.thisComputerOnly) text << "apilocal 1\n";
  if (remote_.sysex) text << "apisysex 1\n";
  if (remote_.sysexUnit != 0) text << "apisysexunit " << remote_.sysexUnit << "\n";
  if (remote_.osc) text << "apiosc 1\n";
  if (remote_.oscPort != RemoteSettings{}.oscPort) text << "apioscport " << remote_.oscPort << "\n";
  if (const float db = keepAliveDb_.load(std::memory_order_relaxed); db < 0.0f)
    text << "speakerkeepalive " << juce::String(db, 1) << "\n";
  if (memoryLimitMB_ > 0) text << "memlimit " << memoryLimitMB_ << "\n";
  if (defaultEq_.on || !defaultEq_.flat()) text << "defaulteq " << juce::String(defaultEq_.toLine()) << "\n";
  for (const auto& line : eqPresets_.toLines()) text << juce::String::fromUTF8(line.c_str()) << "\n";
  if (polyphony_ > 0) text << "polyphony " << polyphony_ << "\n";
  if (runningOrgan_.getFullPathName().isNotEmpty())
    text << "running " << runningOrgan_.getFullPathName() << "\n";
  if (language_.isNotEmpty()) text << "language " << language_ << "\n";
  text << "metronome " << juce::String(metronome_.tempo(), 1) << " " << metronome_.beatsPerBar() << " "
       << juce::String(metronome_.level(), 2) << "\n";
  // The limiter's three settings on one line: it is on or off, it holds the
  // output under a ceiling, and it lets the gain come back at a rate.
  text << "limiter " << (limiter_.enabled() ? 1 : 0) << " "
       << juce::String(limiter_.ceilingDb(), 1) << " "
       << juce::String(limiter_.releaseMs(), 0) << "\n";
  // The compressor's on one line too: on or off, threshold, ratio, attack,
  // release, make-up gain and the detector's high-pass.
  text << "compressor " << (compressor_.enabled() ? 1 : 0) << " "
       << juce::String(compressor_.thresholdDb(), 1) << " "
       << juce::String(compressor_.ratio(), 1) << " "
       << juce::String(compressor_.attackMs(), 0) << " "
       << juce::String(compressor_.releaseMs(), 0) << " "
       << juce::String(compressor_.makeupDb(), 1) << " "
       << compressor_.detectorHighPassHz() << "\n";
  text << "loadticks "
       << (loadTicks_.load(std::memory_order_acquire) ? 1 : 0) << "\n";
  if (cancelResetsKeyboards_.load()) text << "cancelresetskeyboards 1\n";
  if (stopsLeaveHeldNotes_.load()) text << "stopsleaveheldnotes 1\n";
  if (sustainPedal_.load()) text << "sustainpedal 1\n";
  if (pedalCoupled_.load()) text << "pedalcombinationscoupled 1\n";
  if (learnNoiseFilter_.load()) text << "learnnoisefilter 1\n";
  if (archiveOn_.load()) text << "archive 1\n";
  if (archiveSilence_ != 30) text << "archivesilence " << archiveSilence_ << "\n";
  if (archiveMegabytes_ != 500) text << "archivemb " << archiveMegabytes_ << "\n";
  if (archiveDays_ != 90) text << "archivedays " << archiveDays_ << "\n";
  if (combinationsOnTop_) text << "combinationsontop 1\n";
  if (cacheWhilePlaying_.load()) text << "cachewhileplaying 1\n";
  text << "samplecache " << static_cast<int>(cacheMode_) << "\n";
  if (setOffAfterStore_.load()) text << "setoffafterstore 1\n";
  if (leaveBassAndMelody_) text << "pistonsleavebass 1\n";
  if (!showPlayedKeys_.load()) text << "showkeys 0\n";
  if (smallPagesFill_.load()) text << "smallpagesfill 1\n";
  text << "displaymode " << (simpleMode_.load() ? "simple" : "professional") << "\n";
  // Written either way: on is the default since 0.7.11, and a player who
  // turned it off has to find it off.
  text << "fasterengine " << (fasterEngine_.load() ? 1 : 0) << "\n";
  // Written either way too: on is the default since 0.9.0.
  text << "realtimethreads " << (realtimeThreads_.load() ? 1 : 0) << "\n";
  text << "everyprocessor " << (everyProcessor_.load() ? 1 : 0) << "\n";
  for (const auto& name : inputsOff_) text << "midiinputoff " << name << "\n";
  if (midiOutputName_.isNotEmpty()) text << "midioutput " << midiOutputName_ << "\n";
  if (const auto page = scorePageMessage(); !(page == ScorePageMessage{}))
    text << "scorepage " << page.kind << " " << page.channel << " " << page.forward << " " << page.back << "\n";
  if (midiFeedback_) text << "midifeedback 1\n";
  for (const auto& [role, channel] : defaultConsole_)
    text << "consolechannel " << role << " " << channel << "\n";
  for (const auto& lib : libraries_)
    text << "library " << lib.getFullPathName() << "\n";
  // The prefix last: it is hex bytes with spaces between them.
  text << "lcd " << (lcdSetup_.on ? 1 : 0) << " " << lcdSetup_.displayNumber << " " << lcdSetup_.width;
  for (int item : lcdSetup_.lines) text << " " << item;
  text << " " << lcdSetup_.prefix << "\n";
  if (cacheDir_.getFullPathName().isNotEmpty())
    text << "cachedir " << cacheDir_.getFullPathName() << "\n";
  if (packageDir_.getFullPathName().isNotEmpty())
    text << "packagedir " << packageDir_.getFullPathName() << "\n";
  if (lastOrgan_.getFullPathName().isNotEmpty())
    text << "lastorgan " << lastOrgan_.getFullPathName() << "\n";
  if (openFolder_.getFullPathName().isNotEmpty())
    text << "openfolder " << openFolder_.getFullPathName() << "\n";
  for (const auto& [kind, folder] : dialogFolders_)
    if (folder.getFullPathName().isNotEmpty())
      text << "dialogfolder " << kind << " " << folder.getFullPathName() << "\n";

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
  return mp::writeTextFile(f, text);
}

bool MasterpieceProcessor::saveGlobalDefaults() {
  // The one place the live state becomes everyone's starting point, and it
  // happens only because a player asked for it.
  globalBody_ = settingsBody();
  return writeGlobalFile();
}

void MasterpieceProcessor::applyChosenLanguage() {
  const auto f = globalSettingsFile();
  if (f.existsAsFile())
    for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString()))
      if (line.upToFirstOccurrenceOf(" ", false, false).trim() == "language")
        language_ = line.fromFirstOccurrenceOf(" ", false, false).trim().toLowerCase();
  applyLanguage(language_);
}

bool MasterpieceProcessor::loadGlobalDefaults() {
  const auto f = globalSettingsFile();
  if (!f.existsAsFile()) {
    globalsReadOnce_ = true;  // a first start: nothing can have crashed
    applyLanguage(language_);
    return false;
  }

  juce::String body;
  auto sw = graph_.engineSwitch;
  // Settings written before the display mode existed: every one stays shown.
  simpleMode_.store(false);
  cacheMode_ = SampleLibrary::CacheMode::Single;
  // The file holds them all, so a second read must not add them twice.
  eqPresets_.clearOwn();
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
    } else if (key == "updatecheck") {
      updateCheck_ = val.getIntValue() != 0;
    } else if (key == "api") {
      remote_.enabled = val.getIntValue() != 0;
    } else if (key == "apiport") {
      remote_.port = juce::jlimit(1, 65535, val.getIntValue());
    } else if (key == "apiwrite") {
      remote_.allowChanges = val.getIntValue() != 0;
    } else if (key == "apitoken") {
      remote_.token = val.toStdString();
    } else if (key == "apipoweroff") {
      remote_.allowPowerOff = val.getIntValue() != 0;
    } else if (key == "readylamp") {
      const auto parts = juce::StringArray::fromTokens(val, " ", "");
      ReadyLamp lamp;
      if (parts.size() >= 3 && (parts[0] == "note" || parts[0] == "cc")) {
        lamp.kind = parts[0] == "note" ? MidiSourceKind::Note : MidiSourceKind::ControlChange;
        lamp.channel = juce::jlimit(1, 16, parts[1].getIntValue());
        lamp.number = juce::jlimit(0, 127, parts[2].getIntValue());
      }
      lampKind_.store(static_cast<int>(lamp.kind), std::memory_order_relaxed);
      lampChannel_.store(lamp.channel, std::memory_order_relaxed);
      lampNumber_.store(lamp.number, std::memory_order_relaxed);
    } else if (key == "loadmessage") {
      std::vector<uint8_t> bytes;
      if (parseMidiText(val.toStdString(), bytes)) loadMessage_ = std::move(bytes);
    } else if (key == "unloadmessage") {
      std::vector<uint8_t> bytes;
      if (parseMidiText(val.toStdString(), bytes)) unloadMessage_ = std::move(bytes);
    } else if (key == "readylamplit" || key == "readylampdark") {
      std::vector<uint8_t> bytes;
      if (parseMidiText(val.toStdString(), bytes)) (key == "readylamplit" ? lampLit_ : lampDark_) = std::move(bytes);
    } else if (key == "apilocal") {
      remote_.thisComputerOnly = val.getIntValue() != 0;
    } else if (key == "apisysex") {
      remote_.sysex = val.getIntValue() != 0;
    } else if (key == "apisysexunit") {
      remote_.sysexUnit = juce::jlimit(0, 126, val.getIntValue());
    } else if (key == "apiosc") {
      remote_.osc = val.getIntValue() != 0;
    } else if (key == "apioscport") {
      remote_.oscPort = juce::jlimit(1, 65535, val.getIntValue());
    } else if (key == "updatecheckedat") {
      updateCheckedAt_ = val.getLargeIntValue();
    } else if (key == "speakerkeepalive") {
      const float db = val.getFloatValue();
      keepAliveDb_.store(db < 0.0f ? juce::jlimit(-80.0f, -40.0f, db) : 0.0f,
                         std::memory_order_relaxed);
    } else if (key == "memlimit") {
      memoryLimitMB_ = std::max(0, val.getIntValue());
    } else if (key == "polyphony") {
      polyphony_ = std::max(0, val.getIntValue());
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
    } else if (key == "language") {
      language_ = val.toLowerCase();
    } else if (key == "loadticks") {
      loadTicks_.store(val.getIntValue() != 0, std::memory_order_release);
    } else if (key == "cancelresetskeyboards") {
      cancelResetsKeyboards_.store(val.getIntValue() != 0);
    } else if (key == "stopsleaveheldnotes") {
      stopsLeaveHeldNotes_.store(val.getIntValue() != 0);
    } else if (key == "sustainpedal") {
      sustainPedal_.store(val.getIntValue() != 0);
    } else if (key == "pedalcombinationscoupled") {
      pedalCoupled_.store(val.getIntValue() != 0);
    } else if (key == "learnnoisefilter") {
      learnNoiseFilter_.store(val.getIntValue() != 0);
    } else if (key == "archive") {
      archiveOn_.store(val.getIntValue() != 0);
    } else if (key == "archivesilence") {
      archiveSilence_ = juce::jlimit(10, 600, val.getIntValue());
    } else if (key == "archivemb") {
      archiveMegabytes_ = juce::jmax(1, val.getIntValue());
    } else if (key == "archivedays") {
      archiveDays_ = juce::jmax(1, val.getIntValue());
    } else if (key == "cachewhileplaying") {
      cacheWhilePlaying_.store(val.getIntValue() != 0);
    } else if (key == "samplecache" || key == "cache") {
      // "cache" is the line "Save as global defaults" wrote before the
      // choice was the program's; "samplecache" is written after the
      // defaults, so where both are present it is the one that holds.
      const int v = val.getIntValue();
      cacheMode_ = v == 0   ? SampleLibrary::CacheMode::Off
                   : v == 2 ? SampleLibrary::CacheMode::PerOrgan
                            : SampleLibrary::CacheMode::Single;
    } else if (key == "combinationsontop") {
      combinationsOnTop_ = val.getIntValue() != 0;
    } else if (key == "setoffafterstore") {
      setOffAfterStore_.store(val.getIntValue() != 0);
    } else if (key == "pistonsleavebass") {
      leaveBassAndMelody_ = val.getIntValue() != 0;
      player_.setLeaveBassAndMelody(leaveBassAndMelody_);
    } else if (key == "showkeys") {
      showPlayedKeys_.store(val.getIntValue() != 0);
    } else if (key == "displaymode") {
      simpleMode_.store(val.trim() == "simple");
    } else if (key == "smallpagesfill") {
      smallPagesFill_.store(val.getIntValue() != 0);
    } else if (key == "fasterengine") {
      fasterEngine_.store(val.getIntValue() != 0);
    } else if (key == "realtimethreads") {
      realtimeThreads_.store(val.getIntValue() != 0 && realtime::supported());
      if (realtimeThreads_.load()) {
        voices_.setWorkerHook([this] { realtimeHere(); });
        realtimeGeneration_.fetch_add(1);
      }
    } else if (key == "everyprocessor") {
      // The audio is running by the time the file is read at start: the
      // workers it started are started again for the saved choice.
      const bool every = val.getIntValue() != 0;
      if (everyProcessor_.exchange(every) != every) {
        const AudioLock lock(*this);
        startRenderThreads(getBlockSize());
      }
    } else if (key == "midiinputoff") {
      if (val.isNotEmpty()) inputsOff_.addIfNotAlreadyThere(val);
    } else if (key == "scorepage") {
      const auto f = juce::StringArray::fromTokens(val, " ", "");
      if (f.size() == 4) {
        ScorePageMessage m;
        m.kind = juce::jlimit(0, 2, f[0].getIntValue());
        m.channel = juce::jlimit(1, 16, f[1].getIntValue());
        m.forward = juce::jlimit(0, 127, f[2].getIntValue());
        m.back = juce::jlimit(0, 127, f[3].getIntValue());
        scorePage_.store(packScorePage(m), std::memory_order_release);
      }
    } else if (key == "midioutput") {
      midiOutputName_ = val;  // opened by the application at start
    } else if (key == "midifeedback") {
      midiFeedback_ = val.getIntValue() != 0;
    } else if (key == "library") {
      const juce::File dir(val);
      if (val.isNotEmpty() &&
          std::find(libraries_.begin(), libraries_.end(), dir) == libraries_.end())
        libraries_.push_back(dir);
    } else if (key == "lcd") {
      // "lcd <on> <display> <width> <line1..line4> <prefix bytes...>"
      const auto parts = juce::StringArray::fromTokens(val, " ", "");
      if (parts.size() >= 8) {
        LcdSetup s;
        s.on = parts[0].getIntValue() != 0;
        s.displayNumber = parts[1].getIntValue();
        s.width = parts[2].getIntValue();
        for (size_t i = 0; i < s.lines.size(); ++i) s.lines[i] = parts[3 + static_cast<int>(i)].getIntValue();
        juce::StringArray prefix;
        for (int i = 7; i < parts.size(); ++i) prefix.add(parts[i]);
        s.prefix = prefix.joinIntoString(" ");
        lcdSetup_ = s;
        applyLcdSetup();
      }
    } else if (key == "cachedir") {
      // A path, taken whole: the sample cache can be gigabytes, and a player
      // with a small fast disk and a large slow one wants to choose which of
      // them holds it.
      cacheDir_ = val.isEmpty() ? juce::File() : juce::File(val);
    } else if (key == "defaulteq") {
      VoicingEqSettings eq;
      if (eq.fromLine(val.toStdString())) defaultEq_ = eq;
    } else if (key == "eqpreset") {
      eqPresets_.readLine(val.toStdString());
    } else if (key == "eqpresetnext") {
      eqPresets_.readNext(val.getIntValue());
    } else if (key == "packagedir") {
      packageDir_ = val.isEmpty() ? juce::File() : juce::File(val);
    } else if (key == "lastorgan") {
      lastOrgan_ = juce::File(val);
    } else if (key == "metronome") {
      const auto parts = juce::StringArray::fromTokens(val, " ", "");
      if (parts.size() >= 3) {
        metronome_.setTempo(parts[0].getDoubleValue());
        metronome_.setBeatsPerBar(parts[1].getIntValue());
        metronome_.setLevel(static_cast<float>(parts[2].getDoubleValue()));
      }
    } else if (key == "limiter") {
      // "limiter <on> <ceilingDb> <releaseMs>". The two settings are read
      // whatever the switch says, so a player who turned the limiter off
      // and on again finds the ceiling and the rate they had chosen.
      const auto parts = juce::StringArray::fromTokens(val, " ", "");
      if (parts.size() >= 3) {
        limiter_.setCeilingDb(static_cast<float>(parts[1].getDoubleValue()));
        limiter_.setReleaseMs(static_cast<float>(parts[2].getDoubleValue()));
        limiter_.setEnabled(parts[0].getIntValue() != 0);
      }
    } else if (key == "compressor") {
      // "compressor <on> <thresholdDb> <ratio> <attackMs> <releaseMs>
      // <makeupDb> <highPassHz>", the settings kept whatever the switch says.
      const auto parts = juce::StringArray::fromTokens(val, " ", "");
      if (parts.size() >= 7) {
        compressor_.setThresholdDb(static_cast<float>(parts[1].getDoubleValue()));
        compressor_.setRatio(static_cast<float>(parts[2].getDoubleValue()));
        compressor_.setAttackMs(static_cast<float>(parts[3].getDoubleValue()));
        compressor_.setReleaseMs(static_cast<float>(parts[4].getDoubleValue()));
        compressor_.setMakeupDb(static_cast<float>(parts[5].getDoubleValue()));
        compressor_.setDetectorHighPassHz(parts[6].getIntValue());
        compressor_.setEnabled(parts[0].getIntValue() != 0);
      }
    } else if (key == "openfolder") {
      openFolder_ = val.isEmpty() ? juce::File() : juce::File(val);
    } else if (key == "dialogfolder") {
      // "dialogfolder <kind> <path...>": the path last, it can hold spaces.
      const auto kind = val.upToFirstOccurrenceOf(" ", false, false).trim();
      const auto path = val.fromFirstOccurrenceOf(" ", false, false).trim();
      if (kind.isNotEmpty() && path.isNotEmpty()) dialogFolders_[kind] = juce::File(path);
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
  samples_.setCacheMode(cacheMode());
  globalBody_ = body;
  // Only the first read speaks the language: the windows already open were
  // built in the one that was chosen when they were made.
  if (!globalsReadOnce_) applyLanguage(language_);
  globalsReadOnce_ = true;
  return true;
}

bool MasterpieceProcessor::saveVoicingEqForOrgan() {
  organOwnsEq_ = true;
  return saveSettings();
}

bool MasterpieceProcessor::saveVoicingEqAsDefault() {
  defaultEq_ = voicingEqSettings_;
  return writeGlobalFile();
}

bool MasterpieceProcessor::recallVoicingEqPreset(int id) {
  const VoicingEqPreset* p = eqPresets_.find(id);
  if (p == nullptr) return false;
  setVoicingEq(p->eq);
  lastEqPreset_ = id;
  juce::Logger::writeToLog("voicing EQ: preset " + juce::String(id) + " (" +
                           juce::String::fromUTF8(p->name.c_str()) + ")");
  return true;
}

bool MasterpieceProcessor::recallVoicingEqPresetNamed(const juce::String& name) {
  const VoicingEqPreset* p = eqPresets_.findNamed(name.toStdString());
  return p != nullptr && recallVoicingEqPreset(p->id);
}

int MasterpieceProcessor::stepVoicingEqPreset(int step) {
  const int now = currentVoicingEqPreset();
  const int to = eqPresets_.step(now != 0 ? now : lastEqPreset_, step);
  return recallVoicingEqPreset(to) ? to : 0;
}

int MasterpieceProcessor::currentVoicingEqPreset() const {
  return voicingEqSettings_.on ? eqPresets_.matching(voicingEqSettings_) : 0;
}

int MasterpieceProcessor::saveVoicingEqPreset(const juce::String& name) {
  const int id = eqPresets_.add(name.trim().toStdString(), voicingEqSettings_);
  lastEqPreset_ = id;
  writeGlobalFile();
  return id;
}

bool MasterpieceProcessor::updateVoicingEqPreset(int id) {
  return eqPresets_.update(id, voicingEqSettings_) && writeGlobalFile();
}

bool MasterpieceProcessor::renameVoicingEqPreset(int id, const juce::String& name) {
  return eqPresets_.rename(id, name.trim().toStdString()) && writeGlobalFile();
}

bool MasterpieceProcessor::deleteVoicingEqPreset(int id) {
  if (!eqPresets_.remove(id)) return false;
  // A mapping to it would now do nothing, and its button says so: the
  // mapping goes with it.
  midiMap_.unbindTarget(MidiTargetKind::VoicingEqPreset, static_cast<Id>(id));
  saveMidiMap();
  return writeGlobalFile();
}

bool MasterpieceProcessor::resetVoicingEqToDefault() {
  setVoicingEq(defaultEq_);
  const bool owned = organOwnsEq_;
  organOwnsEq_ = false;
  // The organ's own line goes from its file, so the next load plays the
  // default too; an organ that had none has nothing to write.
  return !owned || saveSettings();
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

void MasterpieceProcessor::rememberDialogFolder(const juce::String& kind, const juce::File& picked) {
  if (picked.getFullPathName().isEmpty()) return;
  const juce::File folder = picked.isDirectory() ? picked : picked.getParentDirectory();
  auto& kept = dialogFolders_[kind];
  if (kept == folder) return;
  kept = folder;
  writeGlobalFile();
}

void MasterpieceProcessor::getStateInformation(juce::MemoryBlock& destData) {
  juce::XmlElement state("MasterpieceState");
  state.setAttribute("version", 1);
  if (loadedOdf_.getFullPathName().isNotEmpty()) state.setAttribute("organ", loadedOdf_.getFullPathName());
  copyXmlToBinary(state, destData);
}

void MasterpieceProcessor::setStateInformation(const void* data, int sizeInBytes) {
  const auto state = getXmlFromBinary(data, sizeInBytes);
  if (state == nullptr || !state->hasTagName("MasterpieceState")) return;
  const juce::File organ(state->getStringAttribute("organ"));
  if (organ.getFullPathName().isEmpty() || organ == loadedOdf_) return;
  // Hosts call this from any thread; the load starts on the message thread.
  std::weak_ptr<std::atomic<bool>> alive = alive_;
  juce::MessageManager::callAsync([this, alive, organ] {
    const auto token = alive.lock();
    if (token == nullptr || !token->load()) return;
    if (editorLoader && editorLoader(organ)) return;
    juce::Logger::writeToLog("state: the project's organ " + organ.getFullPathName() + " loads");
    loadOrgan(organ);
  });
}

void MasterpieceProcessor::setOpenFolder(const juce::File& folder) {
  if (openFolder_ == folder) return;
  openFolder_ = folder;
  writeGlobalFile();
}

bool MasterpieceProcessor::openMidiOutput(const juce::String& name) {
  std::unique_ptr<juce::MidiOutput> opened;
  if (name.isNotEmpty())
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
      if (d.name == name) {
        opened = juce::MidiOutput::openDevice(d.identifier);
        break;
      }
  {
    // The audio thread sends through it: swapped between blocks.
    const AudioLock audio(*this);
    midiOut_ = opened.get();
    ownedMidiOut_.swap(opened);
  }
  opened.reset();  // the old port closes outside the lock
  if (midiOutputName_ != name) {
    midiOutputName_ = name;
    writeGlobalFile();
  }
  return name.isEmpty() || midiOut_ != nullptr;
}

void MasterpieceProcessor::setLastOrgan(const juce::File& odf) {
  if (lastOrgan_ == odf) return;
  lastOrgan_ = odf;
  writeGlobalFile();
}

void MasterpieceProcessor::setLimiterEnabled(bool on) {
  if (limiter_.enabled() == on) return;
  limiter_.setEnabled(on);
  writeGlobalFile();
}

void MasterpieceProcessor::setCompressorEnabled(bool on) {
  if (compressor_.enabled() == on) return;
  compressor_.setEnabled(on);
  writeGlobalFile();
}

void MasterpieceProcessor::applyOutputDynamicsPreset(int index) {
  if (index < 0 || index >= static_cast<int>(kOutputDynamicsPresets.size())) return;
  const auto& p = kOutputDynamicsPresets[static_cast<size_t>(index)];
  compressor_.setThresholdDb(p.thresholdDb);
  compressor_.setRatio(p.ratio);
  compressor_.setAttackMs(p.attackMs);
  compressor_.setReleaseMs(p.releaseMs);
  compressor_.setMakeupDb(p.makeupDb);
  compressor_.setDetectorHighPassHz(p.highPassHz);
  compressor_.setEnabled(p.compressorOn);
  limiter_.setCeilingDb(p.ceilingDb);
  limiter_.setReleaseMs(p.limiterReleaseMs);
  limiter_.setEnabled(p.limiterOn);
  writeGlobalFile();
}

int MasterpieceProcessor::outputDynamicsPreset() const {
  // Values off by less than a control's step still count as the preset,
  // since the settings file keeps them to one decimal.
  const auto near = [](float a, float b) { return std::abs(a - b) < 0.05f; };
  for (size_t i = 0; i < kOutputDynamicsPresets.size(); ++i) {
    const auto& p = kOutputDynamicsPresets[i];
    if (compressor_.enabled() != p.compressorOn || limiter_.enabled() != p.limiterOn) continue;
    // A setting that is off is not compared: it makes no sound.
    if (p.compressorOn &&
        !(near(compressor_.thresholdDb(), p.thresholdDb) && near(compressor_.ratio(), p.ratio) &&
          near(compressor_.attackMs(), p.attackMs) && near(compressor_.releaseMs(), p.releaseMs) &&
          near(compressor_.makeupDb(), p.makeupDb) &&
          compressor_.detectorHighPassHz() == p.highPassHz))
      continue;
    if (p.limiterOn &&
        !(near(limiter_.ceilingDb(), p.ceilingDb) && near(limiter_.releaseMs(), p.limiterReleaseMs)))
      continue;
    return static_cast<int>(i);
  }
  return -1;
}

void MasterpieceProcessor::setLimiterCeilingDb(float db) {
  if (limiter_.ceilingDb() == db) return;
  limiter_.setCeilingDb(db);
  writeGlobalFile();
}

void MasterpieceProcessor::setLimiterReleaseMs(float ms) {
  if (limiter_.releaseMs() == ms) return;
  limiter_.setReleaseMs(ms);
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
  int64_t mb = std::max(512, juce::SystemStats::getMemorySizeInMegabytes() * 8 / 10);
  // Where the system ends a program at a ceiling of its own (iOS), the limit
  // stays under it, with the reserve kept free.
  if (const int64_t ceiling = processMemoryCeilingBytes(); ceiling > 0)
    mb = std::min<int64_t>(mb, std::max<int64_t>(256, (ceiling - memoryReserveBytes()) / (1024 * 1024)));
  return static_cast<int>(mb);
}

void MasterpieceProcessor::setMemoryLimitMB(int mb) {
  mb = std::max(0, mb);
  if (memoryLimitMB_ == mb) return;
  memoryLimitMB_ = mb;
  writeGlobalFile();
}

void MasterpieceProcessor::setPolyphonyLimit(int voices) {
  // Not below what one key of a large organ sounds.
  voices = voices <= 0 ? 0 : std::clamp(voices, 64, maxPolyphonySetting());
  if (voices == defaultPolyphony()) voices = 0;
  if (polyphony_ == voices) return;
  polyphony_ = voices;
  // A smaller pool applies at once, as a limit on the one there is. A larger
  // one is built now, the organ staying loaded: the player who raised it
  // and played on heard the old pool, with no word that a reload was due
  // (#320). What sounds as it is built stops.
  {
    const AudioLock lock(*this);
    if (voicePool() > voices_.poolSize() && sampleRate_ > 0.0) {
      buildVoicePool(getBlockSize());
    }
    voices_.setPolyphony(voicePool() < voices_.poolSize() ? voicePool() : 0);
  }
  writeGlobalFile();
}

int64_t MasterpieceProcessor::voicePoolBytes(int voices, double ringSeconds) const {
  // A release buffer the player chose is the length the next load builds.
  const double ring = ringSeconds > 0.0 ? ringSeconds
                      : releaseBufferSeconds_ > 0.0 ? releaseBufferSeconds_
                                                    : voices_.streamSeconds();
  return static_cast<int64_t>(std::max(0, voices)) *
         VoiceEngine::bytesPerVoice(sampleRate_ > 0.0 ? sampleRate_ : 48000.0, ring, loadCanStream(),
                                    samples_.codeSamples() || samples_.codedCount() > 0);
}

bool MasterpieceProcessor::loadCanStream() const {
  return samples_.streamReleases() || preloadHead_ > 0 || samples_.streamedCount() > 0;
}

int64_t MasterpieceProcessor::roomForPool() const {
  const int64_t free = memoryProbe ? memoryProbe() : availableMemoryBytes();
  if (free < 0) return -1;
  return free - (memoryProbe ? 0 : memoryReserveBytes()) + voices_.poolBytes();
}

int MasterpieceProcessor::voicesThatFit(int voices, bool rings, bool codedWindows) const {
  int64_t room = roomForPool();
  if (room < 0) return voices;
  // The pool is made under the memory limit too, beside the samples: the
  // load stopped at that limit with the pool counted in.
  room = std::min(room, memoryLimitBytes() - samples_.residentBytes());
  const int64_t perVoice = VoiceEngine::bytesPerVoice(sampleRate_ > 0.0 ? sampleRate_ : 48000.0,
                                                      voices_.streamSeconds(), rings, codedWindows);
  const int64_t fit = perVoice > 0 && room > 0 ? room / perVoice : 0;
  if (fit >= voices) return voices;
  // Never below what one key of a large organ sounds: a pool that small is a
  // few megabytes.
  const int fitted = static_cast<int>(std::max<int64_t>(64, fit));
  juce::Logger::writeToLog("voices: " + juce::String(fitted) + " of the " + juce::String(voices) +
                           " asked for: the memory free allows no more");
  return fitted;
}

void MasterpieceProcessor::armLoadBudget(bool graphicsOnly) {
  // The system's own figure, asked as the samples come in, on every platform.
  loadProgress_.freeMemory = graphicsOnly ? std::function<int64_t()>{}
                             : memoryProbe ? memoryProbe
                                           : std::function<int64_t()>([] { return availableMemoryBytes(); });
  loadProgress_.reserveBytes = memoryProbe ? 0 : memoryReserveBytes();
  // A probe is asked at every sample, so a test can stop a load at one.
  loadProgress_.checkEveryBytes = memoryProbe ? 1 : LoadProgress::kCheckEveryBytes;
  // The samples of this load may take up to the memory ceiling, and no more.
  // The voices are made from the same memory: what the pool will hold, with
  // the shortest ring or the one the player chose, is taken off what the
  // samples may.
  const double ring = releaseBufferSeconds_ > 0.0 ? releaseBufferSeconds_ : kShortestRingSeconds;
  loadProgress_.resetBudget(
      graphicsOnly ? 0 : std::max<int64_t>(0, memoryLimitBytes() - voicePoolBytes(voicePool(), ring)));
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

void MasterpieceProcessor::setUpdateCheckEnabled(bool on) {
  if (updateCheck_ == on) return;
  updateCheck_ = on;
  writeGlobalFile();
}

bool MasterpieceProcessor::updateCheckDue() const {
  const juce::int64 now = juce::Time::currentTimeMillis() / 1000;
  // A clock set back makes the last check look in the future: due as well.
  return updateCheck_ && (now - updateCheckedAt_ >= 24 * 60 * 60 || now < updateCheckedAt_);
}

void MasterpieceProcessor::markUpdateChecked() {
  updateCheckedAt_ = juce::Time::currentTimeMillis() / 1000;
  writeGlobalFile();
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

namespace {
juce::File& testCacheDirectory() {
  static juce::File dir;
  return dir;
}
}  // namespace

void MasterpieceProcessor::setCacheDirectoryForTests(const juce::File& dir) { testCacheDirectory() = dir; }

juce::File MasterpieceProcessor::defaultCacheDirectory() {
  if (testCacheDirectory().getFullPathName().isNotEmpty()) return testCacheDirectory();
  return mp::dataFolder()
      .getChildFile("cache");
}

juce::File MasterpieceProcessor::cacheDirectory() const {
  // Under the test suite, its own folder, whatever folder the player chose.
  if (testCacheDirectory().getFullPathName().isNotEmpty()) {
    testCacheDirectory().createDirectory();
    return testCacheDirectory();
  }
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

void MasterpieceProcessor::setPackageDirectory(const juce::File& dir) {
  if (dir == packageDir_) return;
  packageDir_ = dir;
  writeGlobalFile();
}

void MasterpieceProcessor::setLoadTicks(bool on) {
  if (loadTicks_.load(std::memory_order_acquire) == on) return;
  loadTicks_.store(on, std::memory_order_release);
  // Written at once, like the startup preference above: a general config is
  // not something a player sets per organ.
  writeGlobalFile();
}

void MasterpieceProcessor::setLanguageSetting(const juce::String& code) {
  if (language_ == code) return;
  language_ = code;
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
  if (savesHeld()) return false;
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
  return mp::writeTextFile(f, lines.joinIntoString("\n") + "\n");
}

bool MasterpieceProcessor::saveMasterGainIfDirty() {
  if (!masterGainDirty_.exchange(false, std::memory_order_acq_rel)) return false;
  return saveMasterGain();
}

juce::File MasterpieceProcessor::consoleMidiFile() {
  return mp::dataFolder()
      .getChildFile("midi")
      .getChildFile("console.mpmidi");
}

bool MasterpieceProcessor::saveMidiMap() const {
  if (savesHeld()) return false;
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
  mp::writeTextFile(cf, juce::String(console.toText()));

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
  return mp::writeTextFile(f, juce::String(organ.toText()));
}

void MasterpieceProcessor::applyConsoleMidi() {
  // Nothing saved for the console yet: whatever the organ's own file carried
  // stays, and moves to the console file with the next save.
  const auto cf = consoleMidiFile();
  if (!cf.existsAsFile()) return;
  MidiMap console;
  if (!console.fromText(cf.loadFileAsString().toStdString())) return;
  // An organ's own file can still hold the player's generals and the like,
  // saved before they had a file of their own (#103). They stay beside the
  // console's wherever the console does not use the same button: dropped,
  // the generals mapped on that organ went silent and their notes played
  // the keyboard, while its divisionals still worked (#366). The next save
  // moves them to the console file.
  MidiMap own;
  for (const auto& b : midiMap_.bindings())
    if (MidiMap::isConsoleTarget(b.targetKind)) own.bindFrom(midiMap_, b);
  midiMap_.removeBindings([](const MidiBinding& b) { return MidiMap::isConsoleTarget(b.targetKind); });
  std::unordered_set<MidiSource, MidiSourceHash> taken;
  for (const auto& b : console.bindings())
    if (MidiMap::isConsoleTarget(b.targetKind)) {
      midiMap_.bindFrom(console, b);
      taken.insert(b.source);
    }
  for (const auto& b : own.bindings())
    if (taken.count(b.source) == 0) midiMap_.bindFrom(own, b);
  // The console's lamps and computer keys come with it too. Only its buttons
  // came before: on an organ with no file of its own, or one saved before the
  // lamp was set, the recorder's lamps stayed dark. A target the console has
  // none for keeps what the organ's file gave it, as its buttons do.
  std::set<std::pair<MidiTargetKind, Id>> lit, keyed;
  for (const auto& s : console.sends())
    if (MidiMap::isConsoleTarget(s.targetKind)) lit.insert({s.targetKind, s.targetId});
  for (const auto& [kind, id] : lit) midiMap_.setSendsFor(kind, id, console.sendsFor(kind, id));
  for (const auto& k : console.shortcuts())
    if (MidiMap::isConsoleTarget(k.targetKind)) keyed.insert({k.targetKind, k.targetId});
  for (const auto& [kind, id] : keyed) midiMap_.setShortcutsFor(kind, id, console.shortcutsFor(kind, id));
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
  // Their lamps and computer keys as well.
  for (const auto s : std::vector<MidiSend>(midiMap_.sends())) midiMap_.setSendsFor(s.targetKind, s.targetId, {});
  for (const auto k : std::vector<KeyShortcut>(midiMap_.shortcuts()))
    midiMap_.setShortcutsFor(k.targetKind, k.targetId, {});
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
    if (!backup.existsAsFile()) mp::writeTextFile(backup, text);
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
      // A pipe that plays a neighbour's samples is judged by the pipe they
      // were recorded for: the shared sample is resolved as if it were
      // recorded for the wrong pipe otherwise.
      const Pipe* judge = &pipe;
      if (pipe.borrowedFromPipeId != 0) {
        const auto it = pipeById_.find(pipe.borrowedFromPipeId);
        if (it != pipeById_.end()) judge = it->second;
      }
      // The pitch the pipe is keyed to, untempered: enough to tell a real
      // declaration of the base pitch from a placeholder.
      const double base = model_.basePitchHz > 0.0 ? model_.basePitchHz : 440.0;
      const int harm = judge->basePitch64ftHarmonicNum > 0 ? judge->basePitch64ftHarmonicNum : 8;
      const double pipeHz = rank.isNoise || judge->midiNote < 0
                                ? 0.0
                                : base * std::pow(2.0, (judge->midiNote - 69) / 12.0) * harm / 8.0;
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

double MasterpieceProcessor::pipeTargetHzFor(const Pipe& pipe, const PipeLayer& layer,
                                              bool originalPitch) const {
  // What this pipe must sound at. Two modes: at the original instrument's own
  // pitch (which is why anyone samples a particular organ), or at a tempered
  // pitch derived from the keyboard. A pipe with no declared original pitch
  // falls back to the tempered path rather than going silent.
  double targetHz = 0.0;
  if (originalPitch && pipe.originalOrganPitchHz > 0.0) {
    targetHz = pipe.originalOrganPitchHz;
  } else {
    targetHz = pipeTargetHz(pipe.midiNote, pipe.basePitch64ftHarmonicNum,
                            model_.basePitchHz, pipe.baseTuningDeviationCents,
                            activeTuning(), 0);
  }
  // The player's pitch moves the whole organ, in either mode.
  targetHz *= pitchFactor();
  // The layer's own detune, a celeste's beat (#261).
  if (layer.detuneCents != 0.0) targetHz *= std::pow(2.0, layer.detuneCents / 1200.0);

  // Detuning rides on the target, not on the recorded pitch: it is a change
  // to what this pipe should sound, not a claim about what the file holds.
  // Applied to the original-organ path too — an instrument left out of tune
  // was out of tune at its own pitch as well. Zero under simpleWavOnly.
  if (layer.pitchControlId != 0)
    targetHz = detunedTargetHz(targetHz, detuneControlValue(layer),
                               detuneCentre(layer),
                               layer.pitchSensitivityHzPerUnit);
  return targetHz;
}

const Pipe* MasterpieceProcessor::borrowedNeighbour(const Pipe& pipe) const {
  if (pipe.borrowedFromPipeId == 0) return nullptr;
  const auto it = pipeById_.find(pipe.borrowedFromPipeId);
  return it == pipeById_.end() ? nullptr : it->second;
}

double MasterpieceProcessor::playbackRatioFor(const Pipe& pipe,
                                             const SampleRef& sample,
                                             const PipeLayer& layer) const {
  // A pipe that plays a neighbour's samples, because its own were left out of
  // the load. The samples were recorded for the neighbour.
  const Pipe* neighbour = borrowedNeighbour(pipe);
  // In the original-pitch mode the two pipes are measured the same way: the
  // mode needs both of them to say what they sounded at.
  const bool originalPitch =
      graph_.engineSwitch.playAtOriginalOrganPitch && pipe.originalOrganPitchHz > 0.0 &&
      (neighbour == nullptr || neighbour->originalOrganPitchHz > 0.0);
  double targetHz = pipeTargetHzFor(pipe, layer, originalPitch);

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
    //
    // A pipe that plays a neighbour's samples is the exception: its recording
    // was made for the neighbour, at the neighbour's pitch, so the ratio is
    // the two pipes' targets.
    if (neighbour == nullptr) return 1.0;
    const double neighbourHz = pipeTargetHzFor(*neighbour, layer, originalPitch);
    if (neighbourHz <= 0.0) return 1.0;
    return targetHz / neighbourHz;
  }
  return playbackRatio(targetHz, recordedHz);
}

// A keyboard the player has given a channel of its own, for this console,
// leaves its default channel (#585): a Great moved to channel 12 still played
// from channel 2 as well.
bool MasterpieceProcessor::placedByPlayer(Id keyboardId, int deviceId) const {
  for (const auto& b : midiMap_.keyboardBindings()) {
    if (b.deviceId != MidiDeviceMap::kAnyDevice && b.deviceId != deviceId) continue;
    if (b.keyboardId == keyboardId || couplers_.inputKeyboardFor(b.keyboardId) == keyboardId) return true;
  }
  return false;
}

Id MasterpieceProcessor::defaultChannelTakenBy(Id keyboardId) const {
  for (const auto& b : midiMap_.keyboardBindings())
    if (b.keyboardId == keyboardId) return 0;  // the player's own channel
  const int channel = channelForKeyboard(keyboardId);
  for (const auto& b : midiMap_.keyboardBindings())
    if (b.channel == channel && b.keyboardId != keyboardId) return b.keyboardId;
  return 0;
}

void MasterpieceProcessor::setKeyboardDevice(Id keyboardId, int deviceId) {
  const int channel = channelForKeyboard(keyboardId);
  keepRouting();
  setKeyboardForChannel(channel, keyboardId, deviceId, false);
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
    if (couplers_.assignmentCodeFor(kb) == channel && !placedByPlayer(kb, deviceId)) return kb;

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
  startNoteOnKeyboard(keyboard, noteKey(channel, midiNote), transposed(midiNote), velocity,
                      midiNote);
}

void MasterpieceProcessor::stopNote(int channel, int midiNote, int velocity) {
  keyUp(noteKey(channel, midiNote), velocity);
}

void MasterpieceProcessor::keyUp(int key, int velocity) {
  // The option turned off with the pedal down: let go of what it held.
  if (sustainDown_ && !sustainPedal()) setSustainDown(false);
  if (sustainDown_ && (soundingNotes_.count(key) != 0 || heldKeySwitches_.count(key) != 0)) {
    if (std::find(sustainedKeys_.begin(), sustainedKeys_.end(), key) == sustainedKeys_.end())
      sustainedKeys_.push_back(key);
    return;
  }
  stopNoteByKey(key, velocity);
}

void MasterpieceProcessor::setSustainDown(bool down) {
  sustainDown_ = down;
  if (down) return;
  // Released in the order they were let go; a copy, since releasing can
  // reach back here through the switch network.
  const std::vector<int> keys(sustainedKeys_.begin(), sustainedKeys_.end());
  sustainedKeys_.clear();
  for (int key : keys) stopNoteByKey(key, 0);
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
  const CoupledScope coupled(*this);
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
  // What the couplers opened for this note goes with it.
  releaseCoupledInputs(it->second.id);
  voices_.noteOff(it->second.id, rel);
  setKeyDown(it->second.keyboard, it->second.playedNote, false);
  const Id releasedOn = it->second.keyboard;
  soundingNotes_.erase(it);
  scheduleExtremes(releasedOn);
}

void MasterpieceProcessor::scheduleExtremes(Id keyboard) {
  if (!couplers_.hasExtremeActions()) return;
  const double due = blockTimeMs_ + kExtremeSettleMs;
  for (auto& [kb, when] : extremesDue_)
    if (kb == keyboard) {
      when = due;
      return;
    }
  extremesDue_.emplace_back(keyboard, due);  // reserved at construction
}

void MasterpieceProcessor::settleExtremes() {
  const CoupledScope coupled(*this);
  for (size_t i = 0; i < extremesDue_.size();) {
    if (extremesDue_[i].second > blockTimeMs_) {
      ++i;
      continue;
    }
    const Id keyboard = extremesDue_[i].first;
    extremesDue_[i] = extremesDue_.back();
    extremesDue_.pop_back();
    reflowExtremes(keyboard);
  }
}

int MasterpieceProcessor::extremeFor(Id keyboard, int midiNote) const {
  if (!couplers_.hasExtremeActions()) return 0;
  int lowest = midiNote, highest = midiNote;
  for (const auto& [key, held] : soundingNotes_) {
    (void)key;
    if (held.keyboard != keyboard) continue;
    lowest = std::min(lowest, held.midiNote);
    highest = std::max(highest, held.midiNote);
  }
  return (midiNote <= lowest ? CouplerMatrix::kLowestHeld : 0) |
         (midiNote >= highest ? CouplerMatrix::kHighestHeld : 0);
}

void MasterpieceProcessor::reflowExtremes(Id keyboard) {
  if (!couplers_.hasExtremeActions()) return;
  const auto same = [](const ExpandedNote& a, const ExpandedNote& b) {
    return a.divisionId == b.divisionId && a.midiNote == b.midiNote;
  };
  const auto contains = [&same](const std::vector<ExpandedNote>& v, const ExpandedNote& n) {
    return std::any_of(v.begin(), v.end(), [&](const ExpandedNote& x) { return same(x, n); });
  };
  for (auto& [key, held] : soundingNotes_) {
    (void)key;
    if (held.keyboard != keyboard) continue;
    const int now = extremeFor(keyboard, held.midiNote);
    if (now == held.extreme) continue;
    const float vel = static_cast<float>(held.velocity) / 127.0f;
    reflowBefore_.clear();
    couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote, vel, engagedSwitches_,
                         keyFlow_, reflowBefore_, held.extreme);
    reflowAfter_.clear();
    couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote, vel, engagedSwitches_,
                         keyFlow_, reflowAfter_, now);
    held.extreme = now;
    // Now the top or the bottom of the chord: what the coupler reaches
    // speaks, under the key's own note id.
    for (const ExpandedNote& reached : reflowAfter_) {
      if (contains(reflowBefore_, reached)) continue;
      if (engageCoupledInput(reached, held.id, held.velocity)) continue;
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
    // No longer the top or the bottom: those pipes let go, the key stays down.
    for (const ExpandedNote& reached : reflowBefore_) {
      if (contains(reflowAfter_, reached)) continue;
      releaseCoupledInput(reached, held.id);
      for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                         engagedStops_, &engagedSwitches_))
        voices_.noteOffPipe(held.id, rp.pipeId, NoteRelease{});
    }
  }
}

bool MasterpieceProcessor::startVoicesForKey(Id keyboard, int midiNote,
                                             int velocity, uint64_t noteId,
                                             const std::unordered_set<Id>& stops, int extreme) {
  bool anyStarted = false;
  // Which divisions this key actually reaches, at which pitches. This is where
  // couplers live: a drawn "Great to Pedal" is an edge of the key-flow graph
  // that is only walkable while its switch is engaged.
  expandScratch_.clear();
  couplers_.expandInto(static_cast<int>(keyboard), midiNote,
                       static_cast<float>(velocity) / 127.0f, engagedSwitches_,
                       keyFlow_, expandScratch_, extreme);

  for (const ExpandedNote& reached : expandScratch_) {
    const int divisionId = reached.divisionId;
    // A division the note was carried to by a coupler: no key of that
    // division was pressed, so nothing opened its pipe. The coupler closes
    // the division's own input switch the way a key does, and the organ's
    // wiring opens the pipe from there.
    const bool coupled = engageCoupledInput(reached, noteId, velocity);
    if (coupled) continue;
    resolveScratch_.clear();
    resolvePipesInto(model_, divisionId, reached.midiNote, stops, &engagedSwitches_,
                     resolveScratch_);
    for (const auto& rp : resolveScratch_)
      if (rp.pipe != nullptr &&
          startPipeLayers(*rp.pipe, rp.rankId, reached.midiNote, velocity, noteId))
        anyStarted = true;
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
    // switch off. The EQ, the pan and the width are a rank's and a
    // division's, applied to its group's bus (RankEq.h), not here.
    //
    // The empty() guard is the point of the whole lookup: an organ
    // nobody has voiced must not pay two hash lookups for every pipe of
    // every chord. The Voicing box (Organ settings, Loading) turns it off,
    // to hear the set as recorded against the voicing (#399).
    if (graph_.engineSwitch.enableVoicing && !voicing_.live().empty()) {
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
    // Unevenness: each pipe a little off true, and the wind's slow sway.
    // Neither is looked at while it is off.
    if (const float d = pipeDetuneCents_.load(std::memory_order_relaxed); d > 0.0f)
      vs.ratio *= centsRatio(d * pipeDetuneOffset(pipe.pipeId));
    if (windSwayCents_.load(std::memory_order_relaxed) > 0.0f && swayChannels_ > 0) {
      const auto s = rankSwayIndex_.find(rankId);
      vs.swayIndex = s != rankSwayIndex_.end() ? s->second : swayChannels_ - 1;
    }
    vs.busIndex = busForPipe(pipe.pipeId);
    // A rank with an EQ plays into its group's bus, which adds into this one.
    if (rankEq_.wanted()) {
      const auto eq = rankEqIndex_.find(rankId);
      if (eq != rankEqIndex_.end()) vs.busIndex = rankEq_.busFor(eq->second, vs.busIndex);
    }
    vs.mixBus = mixBusForPipe(rankId, midiNote);
    vs.rankIndex = liveRankIndexOf(rankId);
    // The attack's emphasis, for a pipe's own speech only: a noise and a
    // release start as they were recorded.
    if (const float db = attackDb(); db != 0.0f) {
      vs.attackBoost = juce::Decibels::decibelsToGain(db) - 1.0f;
      vs.attackFrames = static_cast<int>(attackMs() * 0.001 * sampleRate_);
    }
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
          // percentages of the waveform's pitch swing, in semitones
          // (tremulantPitchSwingSemitones).
          vs.tremAmpDepth = static_cast<float>(
              tremulantAmpSwing(tm->second.ampDepthDb + layer.tremAmpDepthAdjustDb));
          vs.tremPitchDepth =
              tremulantPitchSwingSemitones(tm->second.pitchDepthPct) *
              juce::jlimit(0.0, 4.0, layer.tremPitchDepthAdjustPct / 100.0);
        }
      }
    }
    // LoopCrossfadeLengthInSrcSampleMs is stated against the SOURCE
    // sample rate, so convert with the file's rate, not the engine's.
    vs.loopCrossfadeFrames = static_cast<int>(
        layer.loopCrossfadeMs * 0.001 * sampleRate_);
    vs.startOffset = eventOffset_;
    // Two keys reaching this pipe, through a coupler and directly, sound it
    // once (#521).
    vs.joinSpeaking = true;
    if (voices_.startVoice(vs, noteId) >= 0) anyStarted = true;
  }
  return anyStarted;
}

void MasterpieceProcessor::setKeyDown(Id keyboard, int note, bool down) {
  if (note < 0 || note > 127) return;
  auto& word = keysDown_[(static_cast<size_t>(keyboard) & 63u) * 2u + static_cast<size_t>(note >> 6)];
  const uint64_t bit = uint64_t{1} << (note & 63);
  if (down) word.fetch_or(bit, std::memory_order_relaxed);
  else word.fetch_and(~bit, std::memory_order_relaxed);
}

void MasterpieceProcessor::startNoteOnKeyboard(Id keyboard, int noteKeyId,
                                               int midiNote, int velocity,
                                               int playedNote) {
  const CoupledScope coupled(*this);
  if (playedNote < 0) playedNote = midiNote;
  // A second Note On for a key the hand still holds: the key is already
  // down, so nothing changes, as GrandOrgue does (#565). A merge of two
  // controllers or a contact that bounces sends one; striking the pipes
  // again for it sounded like a new key press.
  if (soundingNotes_.count(noteKeyId) != 0 &&
      std::find(sustainedKeys_.begin(), sustainedKeys_.end(), noteKeyId) == sustainedKeys_.end())
    return;
  // Struck again under the pedal: the hand holds it now.
  if (!sustainedKeys_.empty())
    sustainedKeys_.erase(std::remove(sustainedKeys_.begin(), sustainedKeys_.end(), noteKeyId),
                         sustainedKeys_.end());
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
    releaseCoupledInputs(already->second.id);
    voices_.noteOff(already->second.id, NoteRelease{});
    setKeyDown(already->second.keyboard, already->second.playedNote, false);
    soundingNotes_.erase(already);
  }

  const uint64_t noteId = nextNoteId_++;

  // Neither the top nor the bottom yet: that is decided once the chord has
  // settled (settleExtremes), so a melody coupler takes the top note only.
  const int extreme = 0;
  const bool anyStarted =
      !engagedStops_.empty() &&
      startVoicesForKey(keyboard, midiNote, velocity, noteId, engagedStops_, extreme);

  // Held whether or not anything sounds yet. A key pressed before its stop is
  // drawn speaks the moment the stop is drawn, as on a real organ; recorded
  // only when a pipe had started, such a key was unknown to the code that
  // follows stops for held notes, so drawing the stop did nothing (#120: "it
  // isn't always possible to change stops while holding a note").
  soundingNotes_[noteKeyId] =
      HeldNote{noteId, keyboard, midiNote, velocity, noteChannel_, noteDeviceId_, playedNote, extreme};
  setKeyDown(keyboard, playedNote, true);
  // The keys already held on this manual: one of them was the top or the
  // bottom of the chord until now.
  scheduleExtremes(keyboard);

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
  // A coupler moves only the stops the held notes already sound. A stop moved
  // in the same piston press is started or let go at the next block, along
  // the couplers drawn then; a stop pushed in just before the coupler still
  // sounds on the notes the coupler reached, so the coupler lets it go (#589).
  reflowStops_.clear();
  for (Id s : appliedStops_)
    if (engagedStops_.count(s) != 0) reflowStops_.insert(s);
  for (const auto& [key, held] : soundingNotes_) {
    (void)key;
    const float vel = static_cast<float>(held.velocity) / 127.0f;
    reflowBefore_.clear();
    couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote, vel, heldFlowBefore_,
                         keyFlow_, reflowBefore_, held.extreme);
    reflowAfter_.clear();
    couplers_.expandInto(static_cast<int>(held.keyboard), held.midiNote, vel, engagedSwitches_,
                         keyFlow_, reflowAfter_, held.extreme);
    // Newly reached: its pipes speak, under the key's own note id so the
    // note-off still to come releases them with the rest.
    for (const ExpandedNote& reached : reflowAfter_) {
      if (contains(reflowBefore_, reached)) continue;
      if (engageCoupledInput(reached, held.id, held.velocity)) continue;
      for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                         reflowStops_, &engagedSwitches_)) {
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
      releaseCoupledInput(reached, held.id);
      for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                         appliedStops_, &heldFlowBefore_))
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
  // Left alone when the player asks: a held note keeps the pipes it started
  // with until its key is let go.
  if ((soundingNotes_.empty() && divisionInputNotes_.empty()) || stopsLeaveHeldNotes()) {
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
                        stopSetScratch_, held.extreme);
    }
    // And the division notes held through their inputs.
    for (const auto& [switchId, noteId] : divisionInputNotes_) {
      const auto [division, note] = divisionInputBySwitch_.at(switchId);
      divisionInputScratch_.clear();
      resolvePipesInto(model_, division, note, stopSetScratch_, &engagedSwitches_, divisionInputScratch_);
      for (const auto& rp : divisionInputScratch_)
        if (rp.pipe != nullptr) startPipeLayers(*rp.pipe, rp.rankId, note, palletVelocity_, noteId);
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
                           engagedSwitches_, keyFlow_, expandScratch_, held.extreme);
      for (const ExpandedNote& reached : expandScratch_) {
        const auto pipes = resolvePipes(model_, reached.divisionId,
                                        reached.midiNote, stopSetScratch_, &engagedSwitches_);
        // Only this rank's pipes let go; the rest of the note plays on, and
        // the key is still down.
        for (const auto& rp : pipes)
          voices_.noteOffPipe(held.id, rp.pipeId, NoteRelease{});
      }
    }
    for (const auto& [switchId, noteId] : divisionInputNotes_) {
      const auto [division, note] = divisionInputBySwitch_.at(switchId);
      divisionInputScratch_.clear();
      resolvePipesInto(model_, division, note, stopSetScratch_, &engagedSwitches_, divisionInputScratch_);
      for (const auto& rp : divisionInputScratch_) voices_.noteOffPipe(noteId, rp.pipeId, NoteRelease{});
    }
  }

  appliedStops_ = engagedStops_;
}

void MasterpieceProcessor::setStopEngaged(Id stopId, bool engaged) {
  const AudioLock audio(*this);
  if (engaged && lockedStops_.count(stopId) != 0) return;  // held in: holdOff
  if (!applyingPistons_) player_.registrationMoved();
  if (engaged) engagedStops_.insert(stopId);
  else engagedStops_.erase(stopId);
  shownStale_ = true;
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

void MasterpieceProcessor::publishShown() {
  if (!shownStale_) return;
  const std::unique_lock<std::mutex> lock(shownMutex_, std::try_to_lock);
  if (!lock.owns_lock()) return;  // the window is reading: the next block tries again
  shownStops_ = engagedStops_;
  shownSwitches_ = engagedSwitches_;
  shownStale_ = false;
}

// ------------------------------------------------------ load estimates

void MasterpieceProcessor::buildPipeIndex() {
  // The pipes by id, for the sparse loading: a pipe that plays a neighbour's
  // samples has to find that neighbour, both when the samples' pitch is
  // resolved and when the pipe sounds.
  pipeById_.clear();
  pipeById_.reserve(model_.ranks.size() * 8);
  for (const auto& [rankId, rank] : model_.ranks) {
    (void)rankId;
    for (const auto& pipe : rank.pipes) pipeById_[pipe.pipeId] = &pipe;
  }
}

void MasterpieceProcessor::applySparseLoadingToModel() {
  // The ranks left alone: the sampled tremulant ranks. A rank recorded with a
  // tremulant running is played against the switch that runs it, and its pipes
  // are the ones the tremulant reaches, so they keep their own samples.
  std::unordered_set<Id> protectedRanks;
  for (const auto& [stopId, stop] : model_.stops) {
    (void)stopId;
    for (const StopRankEntry& e : stop.ranks)
      if (e.alternateRankId != 0) protectedRanks.insert(e.alternateRankId);
  }
  // Whether a sample can be read: on disk, in the organ's archive, or already
  // held. Asked once per sample: a set asks about thousands of them.
  std::unordered_map<Id, bool> presence;
  const auto present = [&](Id id) {
    const auto it = presence.find(id);
    if (it != presence.end()) return it->second;
    const bool yes = samples_.samplePresent(id, model_, organRootDir_);
    presence.emplace(id, yes);
    return yes;
  };
  const auto plan = mp::planSparseLoading(model_, sparseEvery_, protectedRanks, present);
  const int borrowed = mp::applySparsePlan(model_, plan);
  if (borrowed > 0)
    juce::Logger::writeToLog(
        "load: notes loaded: every " + juce::String(sparseEvery_) + " - " +
        juce::String(borrowed) + " pipe(s) play a neighbour's samples, retuned to their own");
}

namespace {
// The folder an unpacked organ's archives are marked in, and where the organ's
// root sits inside it. Usually that is the root itself. A GrandOrgue
// definition kept in a Hauptwerk package's sample folder has its root there,
// two folders below the marker, and names its samples from that folder (#637).
struct PackagedRoot {
  std::string folder;       // holds the marker; empty for an organ not packaged
  std::string archivePath;  // the first archive, as the marker names it
  std::string prefix;       // the root inside the package, "a/b/"; empty at the top
};

PackagedRoot packagedRootOf(const std::string& organRoot) {
  PackagedRoot out;
  const juce::File root(juce::String::fromUTF8(organRoot.c_str()));
  juce::File dir = root;
  for (int up = 0; up < 3 && dir.isDirectory(); ++up) {
    const std::string marked = readArchiveMarker(dir.getFullPathName().toStdString());
    if (!marked.empty()) {
      out.folder = dir.getFullPathName().toStdString();
      out.archivePath = marked;
      if (dir != root)
        out.prefix = root.getRelativePathFrom(dir).replaceCharacter('\\', '/').toStdString() + "/";
      return out;
    }
    dir = dir.getParentDirectory();
  }
  return out;
}
} // namespace

std::vector<SampleLibrary::ShapeJob> MasterpieceProcessor::sampleShapeJobs() const {
  // A packaged organ's samples are in its archives; their saved index is
  // enough to know each file's size.
  std::unique_ptr<OrganArchive> archive;
  const PackagedRoot packaged = packagedRootOf(organRootDir_);
  if (!packaged.archivePath.empty()) {
    archive = std::make_unique<OrganArchive>();
    const std::string index = juce::File(juce::String::fromUTF8(packaged.folder.c_str()))
                                  .getChildFile("archive-index.txt")
                                  .getFullPathName()
                                  .toStdString();
    if (!archive->loadIndex(index)) archive.reset();
  }
  // A sparse load reads fewer samples than the model names: the pipes that
  // play a neighbour's samples do not read their own. The figure has to show
  // that, so the plan is worked out here as well as at the load.
  std::unique_ptr<mp::SparseSamples> wanted;
  if (sparseEvery_ > 1) {
    std::unordered_set<Id> protectedRanks;
    for (const auto& [stopId, stop] : model_.stops) {
      (void)stopId;
      for (const StopRankEntry& e : stop.ranks)
        if (e.alternateRankId != 0) protectedRanks.insert(e.alternateRankId);
    }
    std::unordered_map<Id, bool> presence;
    const auto present = [&](Id id) {
      const auto it = presence.find(id);
      if (it != presence.end()) return it->second;
      const bool yes = samples_.samplePresent(id, model_, organRootDir_);
      presence.emplace(id, yes);
      return yes;
    };
    const auto plan = mp::planSparseLoading(model_, sparseEvery_, protectedRanks, present);
    wanted = std::make_unique<mp::SparseSamples>(mp::sparseSampleIds(model_, plan));
  }
  return samples_.shapeJobs(model_, organRootDir_, archive.get(), wanted.get());
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

std::string MasterpieceProcessor::cacheChoices() const {
  return std::to_string(static_cast<int>(samples_.storage())) + "/" +
         (samples_.loadMono() ? "m" : "s") + "/" + (samples_.streamReleases() ? "t" : "n") + "/" +
         std::to_string(samples_.streamHeadPercent()) + "/" +
         std::to_string(samples_.streamHeadFrames()) + "/" + std::to_string(preloadHead_) + "/" +
         std::to_string(static_cast<int64_t>(samples_.loadSampleRate())) + "/" +
         std::to_string(static_cast<int>(samples_.cacheMode())) + "/" +
         std::to_string(static_cast<int>(samples_.releaseChoice())) + "/" +
         std::to_string(samples_.releaseMaxMs()) + "/" +
         std::to_string(sparseEvery_) + (samples_.codeSamples() ? "/z" : "");
}

void MasterpieceProcessor::keepLoadingChoiceForSession() {
  if (loadedOdf_ == juce::File()) return;
  LoadingChoice c;
  c.organ = loadedOdf_;
  c.storage = samples_.storage();
  c.mono = samples_.loadMono();
  c.code = samples_.codeSamples();
  c.stream = samples_.streamReleases();
  c.streamHead = samples_.streamHeadFrames();
  c.streamHeadPercent = samples_.streamHeadPercent();
  c.releaseBuffer = releaseBufferSeconds_;
  c.preload = preloadHead_;
  c.sparse = sparseEvery_;
  c.programCouplers = programCouplers_;
  c.engine = graph_.engineSwitch;
  c.rate = samples_.loadSampleRate();
  c.releases = samples_.releaseChoice();
  c.releaseMs = samples_.releaseMaxMs();
  c.releaseCutoffDb = releaseCutoffDb_;
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
      if (e.aligned && e.releaseRate > 0.0) {
        char al[48];
        std::snprintf(al, sizeof al, ", in phase +%.2f ms", static_cast<double>(e.alignFrames) * 1000.0 / e.releaseRate);
        l += al;
      }
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

void MasterpieceProcessor::setPipeDetuneCents(float cents) {
  pipeDetuneCents_.store(juce::jlimit(0.0f, kMaxUnevennessCents, cents), std::memory_order_relaxed);
  markSettingsDirty();
}

void MasterpieceProcessor::setWindSwayCents(float cents) {
  windSwayCents_.store(juce::jlimit(0.0f, kMaxUnevennessCents, cents), std::memory_order_relaxed);
  markSettingsDirty();
}

// A pipe's place, -1 to 1, from its id alone: the same on every run and
// every computer, so a pipe that is sharp stays sharp.
double MasterpieceProcessor::pipeDetuneOffset(Id pipeId) {
  uint64_t x = static_cast<uint64_t>(pipeId) * 0x9E3779B97F4A7C15ull;
  x ^= x >> 31;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 29;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 32;
  return static_cast<double>(x >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

void MasterpieceProcessor::advanceSway(int numFrames) {
  const float cents = windSwayCents_.load(std::memory_order_relaxed);
  if (cents <= 0.0f || swayChannels_ <= 0 || sampleRate_ <= 0.0) {
    voices_.setSwayRatios(nullptr, 0);
    return;
  }
  // Two slow waves per division at rates of their own, between about one in
  // ten seconds and one in three, so the divisions drift apart and the
  // movement never quite repeats.
  constexpr double kTwoPi = 6.283185307179586;
  const double dt = static_cast<double>(numFrames) / sampleRate_;
  for (int i = 0; i < swayChannels_; ++i) {
    const auto k = static_cast<size_t>(i);
    const double rate = 0.11 + 0.17 * (pipeDetuneOffset(Id(3000 + i)) * 0.5 + 0.5);
    swayPhaseA_[k] = std::fmod(swayPhaseA_[k] + kTwoPi * rate * dt, kTwoPi);
    swayPhaseB_[k] = std::fmod(swayPhaseB_[k] + kTwoPi * rate * 1.73 * dt, kTwoPi);
    const double wave = 0.65 * std::sin(swayPhaseA_[k]) + 0.35 * std::sin(swayPhaseB_[k]);
    swayRatios_[k] = centsRatio(static_cast<double>(cents) * wave);
  }
  voices_.setSwayRatios(swayRatios_.data(), swayChannels_);
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
  storeTarget_ = {StoreTarget::Kind::General, 0, n};
  if (captureMode()) {
    if (player_.captureGeneral(n, read))
      combinationsDirty_.store(true, std::memory_order_release);
    afterPistonStore();
    return;
  }
  // Never captured: nothing to recall, but it is chosen, for Set to fill.
  if (!player_.generalSet(n)) return;
  playerScratch_.clear();
  player_.recallGeneral(n, playerScratch_);
  applyPlayerChanges();
  player_.lightGeneral(n);
}

MasterpieceProcessor::StoreTarget MasterpieceProcessor::storeTarget() const {
  const AudioLock audio(const_cast<MasterpieceProcessor&>(*this));
  return storeTarget_;
}

std::string MasterpieceProcessor::storeTargetName() const {
  const StoreTarget t = storeTarget();
  switch (t.kind) {
    case StoreTarget::Kind::General: return "General " + std::to_string(t.n);
    case StoreTarget::Kind::Step: return "Step " + std::to_string(t.n);
    case StoreTarget::Kind::Divisional: {
      const auto d = model_.divisions.find(t.divisionId);
      const std::string div = d != model_.divisions.end() && !d->second.name.empty()
                                  ? d->second.name
                                  : "Division " + std::to_string(t.divisionId);
      return div + " divisional " + std::to_string(t.n);
    }
    case StoreTarget::Kind::None: break;
  }
  return {};
}

bool MasterpieceProcessor::storeIntoTarget() {
  const AudioLock audio(*this);
  const auto read = [this](const PlayerCombinations::Element& e) {
    return playerElementEngaged(e);
  };
  bool stored = false;
  switch (storeTarget_.kind) {
    case StoreTarget::Kind::General:
      stored = player_.captureGeneral(storeTarget_.n, read);
      if (stored) player_.lightGeneral(storeTarget_.n);
      break;
    case StoreTarget::Kind::Divisional:
      stored = player_.captureDivisional(storeTarget_.divisionId, storeTarget_.n, read);
      if (stored) player_.lightDivisional(storeTarget_.divisionId, storeTarget_.n);
      if (const Id pedal = coupledPedalFor(storeTarget_.divisionId);
          stored && pedal != 0 && player_.captureDivisional(pedal, storeTarget_.n, read))
        player_.lightDivisional(pedal, storeTarget_.n);
      break;
    case StoreTarget::Kind::Step:
      // Into the step the sequencer is on; the frames after it stay as they are.
      playerScratch_.clear();
      stored = player_.gotoFrame(storeTarget_.n, true, read, playerScratch_);
      break;
    case StoreTarget::Kind::None: break;
  }
  if (stored) combinationsDirty_.store(true, std::memory_order_release);
  return stored;
}

void MasterpieceProcessor::pressDivisional(Id divisionId, int n) {
  const AudioLock audio(*this);
  const auto read = [this](const PlayerCombinations::Element& e) {
    return playerElementEngaged(e);
  };
  storeTarget_ = {StoreTarget::Kind::Divisional, divisionId, n};
  // While the Pedal combinations are coupled, the Pedal's piston of the same
  // number goes with this one, stored and recalled together.
  const Id pedal = coupledPedalFor(divisionId);
  if (captureMode()) {
    bool stored = player_.captureDivisional(divisionId, n, read);
    if (pedal != 0) stored = player_.captureDivisional(pedal, n, read) || stored;
    if (stored) combinationsDirty_.store(true, std::memory_order_release);
    afterPistonStore();
    return;
  }
  const bool own = player_.divisionalSet(divisionId, n);
  const bool coupled = pedal != 0 && player_.divisionalSet(pedal, n);
  if (!own && !coupled) return;
  playerScratch_.clear();
  if (own) player_.recallDivisional(divisionId, n, playerScratch_);
  if (coupled) player_.recallDivisional(pedal, n, playerScratch_);
  applyPlayerChanges();
  if (own) player_.lightDivisional(divisionId, n);
  if (coupled) player_.lightDivisional(pedal, n);
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
  // A console draws its manuals as keyboards of their own and plays through
  // the organ's internal ones, so one manual can go by two ids: the drawn
  // keyboard the channel plays, and the internal keyboard a piston is learned
  // on. Both are settled to the keyboard a player plays before they are
  // compared, or the piston reads the manual it is already on as a different
  // one and takes two presses to move (#591).
  const auto played = [this](Id keyboard) {
    const Id input = couplers_.inputKeyboardFor(keyboard);
    return input != 0 ? input : keyboard;
  };
  const auto& order = couplers_.inputKeyboards();
  const auto rank = [&order, &played](Id target) {
    const auto it = std::find(order.begin(), order.end(), played(routedKeyboard(target)));
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
      const Id now = played(keyboardForChannel(channel, MidiDeviceMap::kAnyDevice));
      Id next = routedKeyboard(routeScratch_[i]);
      for (size_t k = i; k < end; ++k)
        if (played(routedKeyboard(routeScratch_[k])) == now) {
          next = routedKeyboard(routeScratch_[k + 1 < end ? k + 1 : i]);
          break;
        }
      if (next != 0 && played(next) != now) {
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
          startNoteOnKeyboard(next, newKey, held.midiNote, held.velocity, held.playedNote);
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
  const Id pedal = coupledPedalFor(divisionId);
  playerScratch_.clear();
  player_.divisionalCancel(divisionId, playerScratch_);
  if (pedal != 0) player_.divisionalCancel(pedal, playerScratch_);
  applyPlayerChanges();
  player_.lightDivisional(divisionId, 0);
  if (pedal != 0) player_.lightDivisional(pedal, 0);
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
  storeTarget_ = {StoreTarget::Kind::Step, 0, player_.frame()};
  if (capturing) {
    combinationsDirty_.store(true, std::memory_order_release);
    afterPistonStore();
  } else {
    applyPlayerChanges();
  }
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
  storeTarget_ = {StoreTarget::Kind::Step, 0, player_.frame()};
  if (capturing) {
    combinationsDirty_.store(true, std::memory_order_release);
    afterPistonStore();
  } else {
    applyPlayerChanges();
  }
  return true;
}

bool MasterpieceProcessor::stepperFirst() {
  if (player_.frame() == 1) return false;
  if (player_.lastUsedFrame() < 1 && !captureMode()) return false;
  return stepperGoto(1);
}

bool MasterpieceProcessor::stepperLast() {
  const int last = player_.lastUsedFrame();
  if (last < 1 || player_.frame() == last) return false;
  return stepperGoto(last);
}

bool MasterpieceProcessor::stepperJump(int frames) {
  const int limit = captureMode() ? PlayerCombinations::kMaxFrames : player_.lastUsedFrame();
  if (limit < 1) return false;
  const int to = std::clamp(player_.frame() + frames, 1, limit);
  if (to == player_.frame()) return false;
  return stepperGoto(to);
}

bool MasterpieceProcessor::stepperDigit(int digit) {
  if (digit < 0 || digit > 9) return false;
  // As GrandOrgue's setter: a digit picks one of the ten frames the current
  // one is among, by the last figure of its number as shown (from 000).
  const int shown = PlayerCombinations::shownNumber(std::max(player_.frame(), 1));
  const int to = shown - shown % 10 + digit + 1;
  if (to > PlayerCombinations::kMaxFrames) return false;
  const int last = player_.lastUsedFrame();
  if (!captureMode() && (last < 1 || to > last)) return false;
  // The digit of the frame shown fires it again, as any piston does and as
  // GrandOrgue's setter does: with Set off it recalls the frame, so General
  // Cancel then 0 brings frame 000 back (#692); with Set on it stores into
  // it, which is how an inserted frame is filled.
  return stepperGoto(to);
}

bool MasterpieceProcessor::stepperGoto(int frame) {
  const AudioLock audio(*this);
  const bool capturing = captureMode();
  playerScratch_.clear();
  const bool moved = player_.gotoFrame(
      frame, capturing, [this](const auto& e) { return playerElementEngaged(e); },
      playerScratch_);
  if (!moved) return false;
  storeTarget_ = {StoreTarget::Kind::Step, 0, player_.frame()};
  if (capturing) {
    combinationsDirty_.store(true, std::memory_order_release);
    afterPistonStore();
  } else {
    applyPlayerChanges();
  }
  return true;
}

bool MasterpieceProcessor::stepperInsertFrame() {
  const AudioLock audio(*this);
  if (!player_.insertFrame()) return false;
  storeTarget_ = {StoreTarget::Kind::Step, 0, player_.frame()};
  combinationsDirty_.store(true, std::memory_order_release);
  return true;
}

bool MasterpieceProcessor::stepperDeleteFrame() {
  const AudioLock audio(*this);
  if (!player_.deleteFrame()) return false;
  storeTarget_ = {StoreTarget::Kind::Step, 0, player_.frame()};
  combinationsDirty_.store(true, std::memory_order_release);
  return true;
}

// What the organ offers a player to register, worked out once the console's
// wiring is known: which switch a player moves is what gets captured.
bool MasterpieceProcessor::hasBassOrMelodyCoupler() const {
  for (const auto& e : player_.elements())
    if (e.bassOrMelody) return true;
  return false;
}

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
  // one the wiring leads to, or the one matched by name (stopKnob_). The
  // program couplers have their own buttons, in the stop list and on the
  // panels, and are stored and cancelled with the rest (#637).
  auto programCoupler = [this](Id sw) {
    const auto it = model_.switches.find(sw);
    return it != model_.switches.end() && it->second.programProvided;
  };
  std::function<bool(const PlayerCombinations::Element&)> onConsole;
  if (drawnConsole)
    onConsole = [this, drawn, programCoupler](const PlayerCombinations::Element& e) {
      if (e.kind == PlayerCombinations::ElementKind::Switch) return drawn(e.id) || programCoupler(e.id);
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
  coupledDivisions_ = mp::coupledDivisions(model_, divisions);
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
  s.transpose = transpose();
  s.combinationSet = combinationSet_.empty() ? std::string("(default)") : combinationSet_;
  return s;
}

void MasterpieceProcessor::applyLcdSetup() {
  lcd_.clear();
  if (!lcdSetup_.on) return;
  // The prefix, as hex bytes. Anything unparseable falls back to the reserved
  // non-commercial id: a typo leaves something on the wire to see.
  std::vector<uint8_t> header;
  for (const auto& tok : juce::StringArray::fromTokens(lcdSetup_.prefix, " ", ""))
    if (tok.isNotEmpty()) header.push_back(static_cast<uint8_t>(tok.getHexValue32() & 0xFF));
  if (!lcd_.setHeader(header)) lcd_.setHeader({0x7D});
  LcdPanel p;
  p.hardwareId = juce::jlimit(0, 15, lcdSetup_.displayNumber);
  p.lineWidth = lcdSetup_.width;
  const LcdField byItem[] = {LcdField::Literal,     LcdField::OrganName,  LcdField::Temperament,
                             LcdField::PitchHz,     LcdField::Transpose,  LcdField::StopsDrawn,
                             LcdField::CrescendoStep, LcdField::CombinationSet};
  for (int item : lcdSetup_.lines) p.lines.push_back({byItem[juce::jlimit(1, 8, item) - 1], ""});
  lcd_.addPanel(std::move(p));
}

void MasterpieceProcessor::setLcdSetup(const LcdSetup& setup) {
  lcdSetup_ = setup;
  if (lcdSetup_.prefix.trim().isEmpty()) lcdSetup_.prefix = "7D";
  applyLcdSetup();
  refreshLcdPanels();
  writeGlobalFile();
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

void MasterpieceProcessor::sendSysexOut(std::vector<uint8_t> bytes) {
  if (midiOut_ == nullptr) return;
  std::lock_guard<std::mutex> lk(lcdQueueLock_);
  lcdQueue_.push_back(std::move(bytes));
}

// The page turns asked for since the last block, into the outgoing stream:
// the one sender to the port, as the lamps and the LCD lines are.
void MasterpieceProcessor::emitScorePageTurns() {
  const int forward = scorePagesForward_.exchange(0, std::memory_order_acq_rel);
  const int back = scorePagesBack_.exchange(0, std::memory_order_acq_rel);
  if (forward == 0 && back == 0) return;
  scorePagesTurned_.fetch_add(forward + back, std::memory_order_acq_rel);
  if (midiOut_ == nullptr) return;
  const ScorePageMessage m = scorePageMessage();
  RawMidi raw[2];
  for (int i = 0; i < forward + back; ++i) {
    const int n = scorePageTurn(m, i < forward, raw);
    for (int k = 0; k < n; ++k) outgoing_.addEvent(raw[k].bytes, raw[k].size, 0);
  }
}

void MasterpieceProcessor::setScorePageMessage(const ScorePageMessage& m) {
  ScorePageMessage held = m;
  held.kind = juce::jlimit(0, 2, held.kind);
  held.channel = juce::jlimit(1, 16, held.channel);
  held.forward = juce::jlimit(0, 127, held.forward);
  held.back = juce::jlimit(0, 127, held.back);
  scorePage_.store(packScorePage(held), std::memory_order_release);
  writeGlobalFile();
}

void MasterpieceProcessor::setLiveLevelFromMidi(MidiTargetKind kind, Id targetId, int value) {
  switch (kind) {
    case MidiTargetKind::RankLevel: liveLevels_.setRank(liveRankIndexOf(targetId), value); break;
    case MidiTargetKind::DivisionLevel: liveLevels_.setDivision(liveDivisionIndexOf(targetId), value); break;
    case MidiTargetKind::PairLevel: liveLevels_.setPair(static_cast<int>(targetId) - 1, value); break;
    default: break;
  }
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

Id MasterpieceProcessor::stopKnobFor(Id stopId) const {
  if (const auto knob = stopKnob_.find(stopId); knob != stopKnob_.end()) return knob->second;
  const auto s = model_.stops.find(stopId);
  if (s == model_.stops.end() || s->second.controllingSwitchId == 0) return 0;
  return playerSwitchFor(s->second.controllingSwitchId);
}

bool MasterpieceProcessor::firePiston(Id switchId) {
  const Id comboId = combinations_.combinationForSwitch(switchId);
  if (comboId == 0) return false;
  // GrandOrgue's own General Cancel is its setter's, the same control as the
  // Combinations window's GC (#248): the same cancel, the keyboards put back
  // when that option is on. An organ that defines its own GC keeps it.
  if (switchId == static_cast<Id>(kGrandOrgueGeneralCancelSwitch))
    if (const auto c = model_.combinations.find(comboId);
        c != model_.combinations.end() && c->second.name == "General cancel") {
      pressGeneralCancel();
      return true;
    }
  // Capture reads the RESOLVED state, because that is what the player can see
  // and hear; the base state would miss a stop pulled by a coupler or by
  // another piston.
  const bool storing = combinations_.captureMode();
  fireCombination(comboId);
  if (storing) afterPistonStore();
  return true;
}

void MasterpieceProcessor::afterPistonStore() {
  // Set lets go after a piston has stored, when the player asked for it
  // (#130), wherever the piston was pressed: the console, MIDI, the control
  // API, the combinations window or the player's stepper (#591).
  if (setOffAfterStore_.load(std::memory_order_relaxed)) setCaptureMode(false);
}

void MasterpieceProcessor::setSwitchEngaged(Id switchId, bool engaged) {
  const AudioLock audio(*this);
  // The couplers' inputs this move reaches are moved after it, not inside it.
  const CoupledScope coupled(*this);
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
  // The setter and the pistons the wiring moved. Klais Szikszo draws its own
  // Set, GC and cancels, each linked to the organ's real switch (Set 12, GC
  // 100, the cancels 200-500): pressing the drawn one moves the real one
  // through the network, and only the switch pressed was ever looked at, so
  // they did nothing (#248). Collected now, fired below: firing changes
  // switches, and the list is the network's own.
  std::array<Id, 8> wiredPistons{};
  size_t wiredCount = 0;
  for (const auto& [movedId, nowEngaged] : switches_.lastChanges()) {
    if (movedId == switchId) continue;
    if (movedId == setterSwitchId_ && setterSwitchId_ != 0)
      combinations_.setCaptureMode(nowEngaged);
    else if (nowEngaged && wiredCount < wiredPistons.size() &&
             combinations_.combinationForSwitch(movedId) != 0)
      wiredPistons[wiredCount++] = movedId;
  }
  const bool swapsRanks = !alternateStopsBySwitch_.empty();
  if (swapsRanks) previousSwitches_ = engagedSwitches_;
  const bool keysHeld = !soundingNotes_.empty();
  if (keysHeld) heldFlowBefore_ = engagedSwitches_;
  engagedSwitches_ = switches_.engagedSwitches();
  shownStale_ = true;
  // A coupler drawn or pushed in while keys are down reaches, or stops
  // reaching, the divisions it couples at once, as on a console with
  // electric action (#131). Only what the switch changed moves. Unless the
  // player has asked for held notes to be left alone.
  if (keysHeld && !stopsLeaveHeldNotes()) reflowHeldNotes();
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
                             previousSwitches_, keyFlow_, expandScratch_, held.extreme);
        for (const ExpandedNote& reached : expandScratch_)
          for (const auto& rp : resolvePipes(model_, reached.divisionId, reached.midiNote,
                                             stopSetScratch_, &previousSwitches_))
            voices_.noteOffPipe(held.id, rp.pipeId, NoteRelease{});
        startVoicesForKey(held.keyboard, held.midiNote, held.velocity, held.id,
                          stopSetScratch_, held.extreme);
      }
    }

  // Every switch whose RESOLVED state moved — which on a wired console is
  // usually more than the one clicked.
  for (const auto& [movedId, nowEngaged] : switches_.lastChanges()) {
    (void)nowEngaged;
    // Every stop the moved switch controls, and every stop it STANDS for
    // whether or not the wiring reaches it: on Friesach the knob and the
    // stop's own switch are separate chains, so the knob moves alone and
    // this is the only thing that draws the stop with it. Each is drawn while
    // any of its switches is engaged.
    auto follow = [this](Id stopId) {
      if (stopSourcesEngaged(stopId)) engagedStops_.insert(stopId);
      else engagedStops_.erase(stopId);
      shownStale_ = true;
    };
    if (const auto stopIt = stopBySwitch_.find(movedId); stopIt != stopBySwitch_.end()) {
      for (Id stopId : stopIt->second) follow(stopId);
      stopsChanged_.store(true, std::memory_order_release);
    }
    if (const auto stoodFor = stopsBySwitch_.find(movedId);
        stoodFor != stopsBySwitch_.end())
      for (Id stopId : stoodFor->second) follow(stopId);

    // Reflect the change on the physical console, if the player wants that.
    // A switch with sends of its own is lit by those, every block.
    if (midiFeedback_ && (midiOut_ != nullptr || outgoingProbe) &&
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
    divisionInputMoved(movedId, nowEngaged);
  }

  // A piston fires on the way in, never on the way out, and then lets itself
  // out again: it is a button, not a drawstop, and leaving it latched would
  // make the second press do nothing.
  if (engaged && firePiston(switchId)) {
    const auto sw = model_.switches.find(switchId);
    const bool momentary = sw == model_.switches.end() || !sw->second.latching;
    if (momentary) setSwitchEngaged(switchId, false);
  }
  // A piston reached through the wiring fires as if pressed; the button that
  // drives it lets it out again when released.
  for (size_t i = 0; i < wiredCount; ++i) firePiston(wiredPistons[i]);
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
  if (savesHeld()) return false;
  const std::string clean = sanitiseSetName(name);
  if (clean == sanitiseSetName(combinationSet_)) return false;  // itself
  const auto f = organFileForSaving("combinations", setExtension(clean));
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return mp::writeTextFile(f, juce::String(combinationFileText()));
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
  if (savesHeld()) return false;
  const auto f = organFileForSaving("combinations", setExtension(combinationSet_));
  if (f.getFullPathName().isEmpty()) return false;
  f.getParentDirectory().createDirectory();
  return mp::writeTextFile(f, juce::String(combinationFileText()));
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
  divisionInputNotes_.clear();

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

void MasterpieceProcessor::buildCouplerInputs() {
  coupledInput_.clear();
  coupledHeld_.clear();
  coupledPending_.clear();
  coupledDepth_ = 0;
  coupledFlushing_ = false;
  // The engine's own switches, well above the ids an organ uses, so they
  // never clash with the organ's own.
  nextCoupledSwitch_ = kCoupledSwitchBase;
  if (model_.divisionInputs.empty()) return;

  // The divisions with a stop that names a rank: there the stop-to-rank walk
  // reaches the pipes, as it always has.
  std::unordered_set<Id> ranked;
  for (const auto& [stopId, stop] : model_.stops)
    if (!stop.ranks.empty()) ranked.insert(stop.divisionId);

  std::vector<Id> reached;
  for (const KeyAction& a : model_.keyActions) {
    if (a.conditionSwitchId == 0) continue;
    // A division the coupler reaches: the division's own input switch for
    // that note is what carries the note into its pipework. A coupler to a
    // keyboard reaches the division that keyboard plays, and the ones its
    // plain key flow carries on to: on an organ wired through its switches
    // that keyboard's keys are not pressed, so its inputs are the way in
    // there as well (#565).
    reached.clear();
    if (a.destIsKeyboard) {
      if (a.destKeyboard != 0) reached = couplers_.divisionsThrough(static_cast<Id>(a.destKeyboard));
    } else if (a.destDivision != 0) {
      reached.push_back(static_cast<Id>(a.destDivision));
    }
    for (const Id division : reached)
      addCoupledInputs(static_cast<Id>(a.conditionSwitchId), division, ranked.count(division) == 0);
  }
  // Ten notes held on every coupled division at once is far beyond a player;
  // the audio thread adds to these and must not allocate.
  coupledHeld_.reserve(1024);
  coupledPending_.reserve(1024);
}

void MasterpieceProcessor::addCoupledInputs(Id coupler, Id division, bool rankless) {
  for (const DivisionInput& in : model_.divisionInputs) {
    if (in.switchId == 0 || in.divisionId != division) continue;
    const CoupledInputKey key{coupler, division, in.midiNote};
    // Several key actions on the same coupler and division (one per range of
    // notes) share one switch per note.
    if (coupledInput_.count(key) != 0) continue;
    const Id mine = nextCoupledSwitch_++;
    Switch sw;
    sw.switchId = mine;
    sw.name = "coupled input";
    sw.latching = false;
    sw.clickable = false;
    model_.switches[mine] = sw;
    // Wired the way a key is: engaged while it is held, released when it is
    // let go, and nothing else in the way.
    SwitchLinkage l;
    l.sourceSwitchId = mine;
    l.destSwitchId = in.switchId;
    l.sourceWhenEngaged = true;
    l.conditionWhenEngaged = true;
    l.engageAction = 1;
    l.disengageAction = 2;
    model_.switchLinkages.push_back(l);
    coupledInput_[key] = {mine, in.switchId, rankless};
  }
}

bool MasterpieceProcessor::engageCoupledInput(const ExpandedNote& reached, uint64_t noteId,
                                              int velocity) {
  if (reached.couplerSwitch == 0 || coupledInput_.empty()) return false;
  const auto it = coupledInput_.find(
      CoupledInputKey{reached.couplerSwitch, static_cast<Id>(reached.divisionId),
                      reached.midiNote});
  if (it == coupledInput_.end()) return false;
  // Into a division its own keys play through these inputs
  // (buildDivisionInputs), where the input is the way in for the key and the
  // coupler alike; or into one whose stops name no rank, where the wiring is
  // the only way in. A division with ranked stops and no such keys keeps the
  // stop-to-rank walk, which has always reached it.
  if (!it->second.rankless && divisionInputBySwitch_.count(it->second.input) == 0) return false;
  const Id sw = it->second.own;
  bool held = false;
  for (const auto& [note, s] : coupledHeld_) {
    // The same note can reach the coupler twice - the note itself, and a
    // stop drawn while it is held - and it is released once.
    if (s == sw && note == noteId) return true;
    if (s == sw) held = true;
  }
  coupledHeld_.emplace_back(noteId, sw);
  if (!held) coupledPending_.push_back({sw, true, velocity});
  return true;
}

void MasterpieceProcessor::releaseCoupledSwitch(Id sw) {
  for (const auto& h : coupledHeld_)
    if (h.second == sw) return;  // another note still holds it
  coupledPending_.push_back({sw, false, palletVelocity_});
}

void MasterpieceProcessor::releaseCoupledInput(const ExpandedNote& reached, uint64_t noteId) {
  if (reached.couplerSwitch == 0 || coupledHeld_.empty()) return;
  const auto it = coupledInput_.find(
      CoupledInputKey{reached.couplerSwitch, static_cast<Id>(reached.divisionId),
                      reached.midiNote});
  if (it == coupledInput_.end()) return;
  const Id sw = it->second.own;
  for (size_t i = 0; i < coupledHeld_.size(); ++i)
    if (coupledHeld_[i].first == noteId && coupledHeld_[i].second == sw) {
      coupledHeld_.erase(coupledHeld_.begin() + static_cast<std::ptrdiff_t>(i));
      releaseCoupledSwitch(sw);
      return;
    }
}

void MasterpieceProcessor::releaseCoupledInputs(uint64_t noteId) {
  for (size_t i = 0; i < coupledHeld_.size();) {
    if (coupledHeld_[i].first != noteId) {
      ++i;
      continue;
    }
    const Id sw = coupledHeld_[i].second;
    coupledHeld_.erase(coupledHeld_.begin() + static_cast<std::ptrdiff_t>(i));
    releaseCoupledSwitch(sw);
  }
}

void MasterpieceProcessor::releaseAllCoupledInputs() {
  if (coupledHeld_.empty()) return;
  const CoupledScope scope(*this);
  while (!coupledHeld_.empty()) {
    const Id sw = coupledHeld_.back().second;
    coupledHeld_.pop_back();
    releaseCoupledSwitch(sw);
  }
}

void MasterpieceProcessor::flushCoupledInputs() {
  // A move made here reflows the held notes, which can ask for more moves:
  // they join the queue and this loop makes them.
  if (coupledFlushing_ || coupledPending_.empty()) return;
  coupledFlushing_ = true;
  for (size_t i = 0; i < coupledPending_.size(); ++i) {
    const CoupledMove move = coupledPending_[i];
    palletVelocity_ = move.velocity;
    setSwitchEngaged(move.switchId, move.engaged);
  }
  coupledPending_.clear();
  coupledFlushing_ = false;
}

void MasterpieceProcessor::findPalletStops() {
  palletStops_.clear();
  std::unordered_set<Id> pallets;
  for (const auto& [rankId, rank] : model_.ranks) {
    if (unshippedRanks_.count(rankId) != 0) continue;
    for (const Pipe& pipe : rank.pipes)
      if (pipe.palletSwitchId != 0) pallets.insert(pipe.palletSwitchId);
  }
  if (pallets.empty()) return;
  // Each wire both ways: what moves a switch, as the source of a linkage or
  // its condition, and what that switch moves in turn.
  std::unordered_map<Id, std::vector<Id>> feeders, fed;
  for (const auto& l : model_.switchLinkages)
    for (Id from : {l.sourceSwitchId, l.conditionSwitchId})
      if (from != 0 && from != l.destSwitchId) {
        feeders[l.destSwitchId].push_back(from);
        fed[from].push_back(l.destSwitchId);
      }
  // Only a pallet a key opens holds pipework. A demo keeps every stop's
  // action noise, a pipe on a pallet the stop's own switch opens with no key,
  // and counting those took Nowa Ruda's 35 locked stops for playing ones
  // (#390). An organ that names no key switches keeps every pallet.
  std::unordered_set<Id> keys;
  for (const auto& [switchId, key] : model_.keyboardKeys) keys.insert(switchId);
  for (const auto& input : model_.divisionInputs)
    if (input.switchId != 0) keys.insert(input.switchId);
  if (!keys.empty()) {
    std::unordered_set<Id> opened(keys.begin(), keys.end());
    std::vector<Id> down(keys.begin(), keys.end());
    while (!down.empty()) {
      const Id at = down.back();
      down.pop_back();
      if (const auto it = fed.find(at); it != fed.end())
        for (Id next : it->second)
          if (opened.insert(next).second) down.push_back(next);
    }
    for (auto it = pallets.begin(); it != pallets.end();)
      it = opened.count(*it) != 0 ? std::next(it) : pallets.erase(it);
  }
  std::unordered_set<Id> reached(pallets.begin(), pallets.end());
  std::vector<Id> todo(pallets.begin(), pallets.end());
  while (!todo.empty()) {
    const Id at = todo.back();
    todo.pop_back();
    const auto it = feeders.find(at);
    if (it == feeders.end()) continue;
    for (Id up : it->second)
      if (reached.insert(up).second) todo.push_back(up);
  }
  for (const auto& [stopId, stop] : model_.stops)
    if (stop.controllingSwitchId != 0 && reached.count(stop.controllingSwitchId) != 0)
      palletStops_.insert(stopId);
}

void MasterpieceProcessor::findControlStops() {
  controlStops_.clear();
  // What a stop with no ranks of its own can work: a coupler or a tremulant.
  std::unordered_set<Id> works;
  // And what a tremulant stop works whatever ranks it has: a tremulant, or
  // tremmed ranks it swaps in that the set ships. St David's gives each
  // tremulant stop a rank of its own for the motor's noise, and with that
  // noise missing the stop was locked as not available while its tremmed
  // ranks were loaded (#552).
  std::unordered_set<Id> trems;
  for (const auto& ka : model_.keyActions)
    if (ka.conditionSwitchId != 0) works.insert(ka.conditionSwitchId);
  for (const auto& [tremulantId, tremulant] : model_.tremulants)
    if (tremulant.controllingSwitchId != 0) {
      works.insert(tremulant.controllingSwitchId);
      trems.insert(tremulant.controllingSwitchId);
    }
  for (const auto& [stopId, stop] : model_.stops)
    for (const auto& entry : stop.ranks) {
      if (entry.alternateSwitchId == 0) continue;
      works.insert(entry.alternateSwitchId);
      const auto alt = model_.ranks.find(entry.alternateRankId);
      if (alt != model_.ranks.end() && !alt->second.pipes.empty() && unshippedRanks_.count(alt->first) == 0)
        trems.insert(entry.alternateSwitchId);
    }
  if (works.empty()) return;
  for (const auto& [stopId, stop] : model_.stops)
    if (stop.ranks.empty() ? works.count(stop.controllingSwitchId) != 0
                           : trems.count(stop.controllingSwitchId) != 0)
      controlStops_.insert(stopId);
}

// A division the organ plays from its keys through switches: each key's
// switch drives the input for its note, and a unison off is the condition on
// that link (Hill, Burton-Berlin, #345). For each keyboard, the key switches
// are followed down the linkages; a keyboard whose keys reach its own
// division's inputs plays that division through them, and the key flow stops
// sounding it directly. A division whose inputs no key reaches is left as it
// was, so a set that declares inputs and plays its keys some other way is
// unchanged.
void MasterpieceProcessor::buildProgramCouplers() {
  programCouplersList_.clear();
  couplers_.setProgramCouplers({});
  if (!programCouplers_) return;

  // The divisions a player can play: the ones the playable keyboards reach.
  std::vector<Id> divisions;
  for (Id kb : couplers_.inputKeyboards()) {
    const Id div = couplers_.primaryDivisionFor(kb);
    if (div == 0) continue;
    if (std::find(divisions.begin(), divisions.end(), div) == divisions.end())
      divisions.push_back(div);
  }
  if (divisions.empty()) return;

  // The ids: from the next multiple of 100000 above the organ's own, in a
  // fixed order, so that the same organ gives the same ids at every load and
  // a MIDI mapping saved for one keeps working.
  Id highest = 0;
  // The organ's own switches only: the engine numbers switches of its own
  // from 0x40000000, and counting those would move these ids.
  for (const auto& [id, s] : model_.switches)
    if (id < 0x40000000 && !s.programProvided) highest = std::max(highest, id);
  Id nextId = ((highest / 100000) + 1) * 100000;

  // The name a player knows a division by.
  const auto nameOf = [&](Id div) {
    const auto it = model_.divisions.find(div);
    return it != model_.divisions.end() ? it->second.name : std::string();
  };

  // For every ordered pair of different divisions: 16', 8' and 4'.
  for (Id dest : divisions)
    for (Id src : divisions) {
      if (dest == src) continue;
      for (const int shift : {-12, 0, 12}) {
        const char* pitch = shift == -12 ? "16'" : shift == 0 ? "8'" : "4'";
        Switch sw;
        sw.switchId = nextId++;
        sw.name = nameOf(src) + " to " + nameOf(dest) + " " + pitch;
        sw.isCoupler = true;
        sw.programProvided = true;
        model_.switches[sw.switchId] = sw;
        programCouplersList_.push_back({sw.switchId, dest, src, shift});
      }
    }

  // For every division with itself: 16' and 4'. A manual coupled to itself
  // an octave down or up, which is what a sub and a super coupler are.
  for (Id div : divisions)
    for (const int shift : {-12, 12}) {
      const char* pitch = shift == -12 ? "16'" : "4'";
      Switch sw;
      sw.switchId = nextId++;
      sw.name = nameOf(div) + " " + pitch;
      sw.isCoupler = true;
      sw.programProvided = true;
      model_.switches[sw.switchId] = sw;
      programCouplersList_.push_back({sw.switchId, div, div, shift});
    }

  // A division whose stops name no rank is reached only through its inputs,
  // on an organ wired through its switches: a program coupler closes them the
  // way the organ's own couplers do (#565). A division with ranked stops keeps
  // the stop-to-rank walk.
  if (!model_.divisionInputs.empty()) {
    std::unordered_set<Id> ranked;
    for (const auto& [stopId, stop] : model_.stops)
      if (!stop.ranks.empty()) ranked.insert(stop.divisionId);
    for (const auto& c : programCouplersList_)
      if (ranked.count(c.srcDivision) == 0) addCoupledInputs(c.switchId, c.srcDivision, true);
  }

  // The organ's own generals store the program couplers, and its general
  // cancels clear them (#637). By the type code: 2 a general, 6 a general
  // cancel, 101 to 199 the numbered generals and 100 their cancel, and 1, which
  // a GrandOrgue set's generals carry. Divisionals and the crescendo's stages
  // are left as they are.
  auto general = [](const Combination& c) {
    return c.divisionId == 0 &&
           (c.type == 1 || c.type == 2 || c.type == 6 || (c.type >= 100 && c.type <= 199));
  };
  for (auto& [comboId, combo] : model_.combinations) {
    if (!general(combo) || combo.elements.empty()) continue;
    for (const auto& pc : programCouplersList_) {
      CombinationElement e;
      e.combinationId = comboId;
      e.controlledSwitchId = pc.switchId;
      e.capturedSwitchId = pc.switchId;
      combo.elements.push_back(e);
    }
  }

  couplers_.setProgramCouplers(programCouplersList_);
}

void MasterpieceProcessor::buildDivisionInputs() {
  divisionInputBySwitch_.clear();
  divisionInputNotes_.clear();
  std::unordered_set<Id> fedKeyboards;
  if (!model_.divisionInputs.empty() && !model_.keyboardKeys.empty()) {
    std::unordered_map<Id, std::unordered_set<Id>> inputsOf;
    for (const auto& in : model_.divisionInputs) inputsOf[in.divisionId].insert(in.switchId);
    std::unordered_map<Id, std::vector<Id>> feeds;
    for (const auto& l : model_.switchLinkages)
      if (l.sourceSwitchId != l.destSwitchId) feeds[l.sourceSwitchId].push_back(l.destSwitchId);
    std::unordered_map<Id, std::vector<Id>> keysOf;
    for (const auto& [switchId, key] : model_.keyboardKeys) keysOf[key.keyboardId].push_back(switchId);

    std::unordered_set<Id> fedDivisions;
    for (const auto& [keyboardId, keys] : keysOf) {
      Id division = 0;
      if (const auto kb = model_.keyboards.find(keyboardId); kb != model_.keyboards.end())
        division = kb->second.primaryDivisionHint;
      if (division == 0) division = couplers_.primaryDivisionFor(keyboardId);
      const auto inputs = inputsOf.find(division);
      if (division == 0 || inputs == inputsOf.end()) continue;
      std::unordered_set<Id> seen(keys.begin(), keys.end());
      std::vector<Id> todo(keys.begin(), keys.end());
      bool reaches = false;
      while (!todo.empty() && !reaches && seen.size() < 200000) {
        const Id at = todo.back();
        todo.pop_back();
        const auto next = feeds.find(at);
        if (next == feeds.end()) continue;
        for (Id to : next->second) {
          if (inputs->second.count(to) != 0) { reaches = true; break; }
          if (seen.insert(to).second) todo.push_back(to);
        }
      }
      if (!reaches) continue;
      fedKeyboards.insert(keyboardId);
      fedDivisions.insert(division);
    }
    for (const auto& in : model_.divisionInputs)
      if (fedDivisions.count(in.divisionId) != 0)
        divisionInputBySwitch_[in.switchId] = {static_cast<int>(in.divisionId), in.midiNote};
    if (!divisionInputBySwitch_.empty()) {
      divisionInputNotes_.reserve(divisionInputBySwitch_.size());
      divisionInputScratch_.reserve(64);
      // The keys have to move their switches for the inputs to hear them.
      if (keySwitchByKey_.empty()) {
        keySwitchIds_.clear();
        for (const auto& [switchId, key] : model_.keyboardKeys) {
          keySwitchByKey_[static_cast<int>(key.keyboardId) * 256 + key.midiNote] = switchId;
          keySwitchIds_.insert(switchId);
        }
        heldKeySwitches_.reserve(256);
      }
      juce::Logger::writeToLog("load: " + juce::String(static_cast<int>(fedDivisions.size())) +
                               " division(s) played from the keys through their division inputs");
    }
  }
  couplers_.setSwitchFedKeyboards(std::move(fedKeyboards));
}

void MasterpieceProcessor::divisionInputMoved(Id switchId, bool engaged) {
  if (divisionInputBySwitch_.empty() || !palletsLive_.load(std::memory_order_acquire)) return;
  const auto it = divisionInputBySwitch_.find(switchId);
  if (it == divisionInputBySwitch_.end()) return;
  const auto open = divisionInputNotes_.find(switchId);
  if (!engaged) {
    if (open == divisionInputNotes_.end()) return;
    NoteRelease rel;
    rel.velocity = palletVelocity_;
    voices_.noteOff(open->second, rel);
    divisionInputNotes_.erase(open);
    return;
  }
  if (open != divisionInputNotes_.end()) return;  // already sounding
  const auto [division, note] = it->second;
  const uint64_t noteId = nextNoteId_++;
  divisionInputScratch_.clear();
  resolvePipesInto(model_, division, note, engagedStops_, &engagedSwitches_, divisionInputScratch_);
  for (const auto& rp : divisionInputScratch_)
    if (rp.pipe != nullptr) startPipeLayers(*rp.pipe, rp.rankId, note, palletVelocity_, noteId);
  // Kept whether or not a pipe spoke: a stop drawn while the key is down
  // speaks under this id (applyStopChangeToHeldNotes).
  divisionInputNotes_[switchId] = noteId;
}

std::set<std::string> MasterpieceProcessor::joinPackagesFromElsewhere(
    std::shared_ptr<OrganArchive>& archive, const std::string& root, const std::string& archivePath) {
  samples_.setPackageRoots({});
  samples_.setPackagesFromElsewhere({});
  auto dirName = [](Id id) {
    const std::string d = std::to_string(id);
    return std::string(d.size() < 6 ? 6 - d.size() : 0, '0') + d;
  };
  auto file = [](const std::string& path) { return juce::File(juce::String::fromUTF8(path.c_str())); };
  // A package is here when any of its samples is: under the organ's root, in
  // its archive, or in the folder it was found in. One sample is not enough:
  // a demo's definition names the whole organ's samples and ships a few
  // stops' worth, and the Nancy demo was asked for its own package.
  std::map<Id, std::vector<std::string>> samplesOf;
  for (const auto& [id, s] : model_.samples)
    if (s.installationPackageId > 0 && !s.fileName.empty() && !s.encrypted) {
      std::string rel = s.fileName;
      std::replace(rel.begin(), rel.end(), '\\', '/');
      samplesOf[s.installationPackageId].push_back(std::move(rel));
    }
  std::unordered_map<Id, std::string> roots;
  auto packageRel = [&](Id package) { return "OrganInstallationPackages/" + dirName(package) + "/"; };
  // Matched as the samples are read, ignoring case: a package unpacked on a
  // case-sensitive disk with "pipes/GO/04-Rea" for the definition's
  // "pipes/go/04-rea" plays, and was asked for at every load all the same,
  // the folder picked for it never kept (#591, Burton Berlin on Linux).
  auto installedIn = [&](const juce::File& dir, Id package) {
    const juce::File pkg = dir.getChildFile(juce::String::fromUTF8(packageRel(package).c_str()));
    if (!pkg.isDirectory()) return false;
    std::error_code ec;
    for (const auto& rel : samplesOf.at(package)) {
      const juce::File wanted = pkg.getChildFile(juce::String::fromUTF8(rel.c_str()));
      if (wanted.existsAsFile() ||
          std::filesystem::is_regular_file(
              mp::resolvePathIgnoringCase(mp::pathOf(wanted.getFullPathName().toStdString())), ec))
        return true;
    }
    return false;
  };
  auto inArchive = [&](Id package) {
    if (archive == nullptr) return false;
    for (const auto& rel : samplesOf.at(package))
      if (archive->find(packageRel(package) + rel) != nullptr) return true;
    return false;
  };
  // Found stays found: the roots and the archive only grow.
  std::set<Id> present;
  auto here = [&](Id package) {
    if (present.count(package) != 0) return true;
    const auto at = roots.find(package);
    if (!installedIn(file(at != roots.end() ? at->second : root), package) && !inArchive(package)) return false;
    present.insert(package);
    return true;
  };
  std::set<Id> missing;
  for (const auto& [package, samples] : samplesOf)
    if (!here(package)) missing.insert(package);
  std::set<std::string> found;
  if (missing.empty() || root.empty()) return found;

  const juce::File folder = file(root);
  const juce::File home = !archivePath.empty() ? file(archivePath).getParentDirectory() : folder;

  // What the player pointed to before, and what was found before, kept beside
  // the organ: a folder per package, and the archives found or chosen.
  const juce::File folderRecord = folder.getChildFile("package-folders.txt");
  const juce::File archiveRecord = folder.getChildFile("joined-packages.txt");
  juce::File lastPicked;
  for (const auto& line : juce::StringArray::fromLines(folderRecord.loadFileAsString())) {
    const Id package = static_cast<Id>(line.upToFirstOccurrenceOf("\t", false, false).getLargeIntValue());
    const juce::File dir(line.fromFirstOccurrenceOf("\t", false, false));
    if (dir.isDirectory()) lastPicked = dir;
    if (missing.count(package) != 0 && installedIn(dir, package)) roots[package] = dir.getFullPathName().toStdString();
  }
  auto keepFolder = [&](Id package, const juce::File& dir) {
    roots[package] = dir.getFullPathName().toStdString();
    folderRecord.appendText(juce::String(static_cast<juce::int64>(package)) + "\t" + dir.getFullPathName() + "\n");
  };
  auto needed = [&] {
    std::vector<Id> out;
    for (Id package : missing)
      if (!here(package)) out.push_back(package);
    return out;
  };

  // Where the player keeps the organ: the folder of its archive, or its own
  // folder. A set downloaded as several packages is often unpacked one
  // package to a folder, side by side, so the search takes in the organ's
  // folder and two levels below it, the folders beside it and the folders in
  // those, and the package folder and two levels below it. A folder of
  // packages or of definitions is not walked into: what it holds are
  // packages, not the folders that hold them. The walk stops at a few hundred
  // folders, which a folder of everything reaches.
  constexpr size_t kMostFolders = 600;
  auto walk = [](const juce::File& top, int depth, size_t most) {
    std::vector<juce::File> out{top};
    std::vector<juce::File> level{top};
    for (int i = 0; i < depth && !level.empty() && out.size() < most; ++i) {
      std::vector<juce::File> next;
      for (const auto& d : level)
        for (const auto& c : d.findChildFiles(juce::File::findDirectories, false)) {
          const juce::String name = c.getFileName();
          if (name.equalsIgnoreCase("OrganInstallationPackages") || name.equalsIgnoreCase("OrganDefinitions") ||
              name.startsWithChar('.'))
            continue;
          if (out.size() >= most) break;
          out.push_back(c);
          next.push_back(c);
        }
      level = std::move(next);
    }
    return out;
  };
  std::vector<juce::File> dirs;
  auto addTree = [&](const juce::File& top, int depth) {
    if (top.getFullPathName().isEmpty() || !top.isDirectory() || dirs.size() >= kMostFolders) return;
    for (const auto& d : walk(top, depth, kMostFolders - dirs.size()))
      if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
  };
  const juce::File beside = home.getParentDirectory();
  const bool packageFolder = packageDir_.getFullPathName().isNotEmpty() && packageDir_.isDirectory();
  if (!needed().empty()) {
    addTree(home, 2);
    // Beside a drive's root folder are the drive's other folders: the ones
    // directly there, and no further.
    if (beside != home) addTree(beside, beside.getParentDirectory() == beside ? 1 : 2);
    if (packageFolder) addTree(packageDir_, 2);
    juce::Logger::writeToLog("load: " + juce::String((int)needed().size()) + " package(s) not with the organ; looking in " +
                             juce::String((int)dirs.size()) + " folders: " + home.getFullPathName() +
                             " and the folders in it, the folders beside it in " + beside.getFullPathName() +
                             (packageFolder ? ", and the package folder " + packageDir_.getFullPathName() : juce::String()));
  }

  // Installed folders: OrganInstallationPackages/<id> with the samples, in
  // those folders or in a Hauptwerk library. A folder found is kept, so the
  // next load goes to it first.
  auto lookIn = [&](const std::vector<juce::File>& where) {
    for (const auto& dir : where) {
      if (dir == folder || !dir.getChildFile("OrganInstallationPackages").isDirectory()) continue;
      for (Id package : needed())
        if (installedIn(dir, package)) keepFolder(package, dir);
    }
  };
  {
    std::vector<juce::File> installed = dirs;
    for (const auto& lib : libraries_) installed.push_back(lib);
    lookIn(installed);
  }

  // An archive joined to the organ's: its index kept beside the organ, or the
  // one kept when that organ was opened itself, so it is walked once.
  const juce::File packagedRoot = mp::dataFolder()
                                      .getChildFile("Packaged");
  auto joinSource = [&](const std::string& source) {
    if (archive != nullptr)
      for (const auto& group : archive->archives())
        for (const auto& volume : group)
          if (volume == source) return;  // joined as a part of a set before it
    OrganArchive extra;
    std::string error;
    if (!extra.discover(source, error)) return;
    const std::string cache =
        folder.getChildFile("joined-" + juce::String(extra.identity()) + ".txt").getFullPathName().toStdString();
    const std::string itsOwn = packagedRoot.getChildFile(juce::String(extra.identity()))
                                   .getChildFile("archive-index.txt")
                                   .getFullPathName()
                                   .toStdString();
    const double openStart = juce::Time::getMillisecondCounterHiRes();
    if (!extra.loadIndexFor(cache) && !extra.loadIndexFor(itsOwn)) {
      if (!extra.inspect(error) || !extra.index(error)) {
        juce::Logger::writeToLog("load: " + juce::String(source) + " cannot be read: " + juce::String(error));
        return;
      }
      extra.saveIndex(cache);
    }
    if (archive == nullptr) archive = std::make_shared<OrganArchive>();
    archive->join(extra);
    juce::Logger::writeToLog("load: samples from " + juce::String((int)extra.archives().size()) +
                             " archive(s) of another organ, beginning " + juce::String(source) + ", in " +
                             juce::String((juce::Time::getMillisecondCounterHiRes() - openStart) / 1000.0, 1) +
                             " s");
  };
  auto stillMissing = [&] {
    std::set<std::string> out;
    for (Id package : missing)
      if (!here(package)) out.insert(dirName(package));
    return out;
  };

  // Archives for what is still missing: where they were found last time, else
  // the folders above.
  if (const auto wanted = stillMissing(); !wanted.empty()) {
    std::vector<std::string> sources;
    for (const auto& line : juce::StringArray::fromLines(archiveRecord.loadFileAsString()))
      if (line.isNotEmpty() && juce::File(line).existsAsFile()) sources.push_back(line.toStdString());
    if (sources.empty()) {
      std::vector<std::string> folders;
      for (const auto& d : dirs) folders.push_back(d.getFullPathName().toStdString());
      sources = OrganArchive::carryingIn(folders, wanted);
      juce::String text;
      for (const auto& s : sources) text << juce::String::fromUTF8(s.c_str()) << "\n";
      if (text.isNotEmpty()) mp::writeTextFile(archiveRecord, text);
    }
    for (const auto& source : sources) joinSource(source);
  }

  // Anything not found: the player may point to its archive, or the folder
  // that holds it, and the answer is kept for the next load. Hauptwerk's own
  // standard components (package 1) come with no sample set and have
  // stand-ins of their own, so they are not asked for.
  //
  // The folder picked may hold OrganInstallationPackages, be that folder, be
  // the package's own folder or a folder in it, or be a folder above them:
  // three levels up and two down are looked at. The other packages still
  // missing are looked for there too.
  if (packageAsker)
    for (Id package : missing) {
      if (package == 1 || here(package)) continue;
      const auto named = model_.packages.find(package);
      // The chooser opens where the player last pointed for this organ, or
      // in the folder that holds the organ's.
      const juce::File start = lastPicked.isDirectory() ? lastPicked : beside;
      const std::string chosen =
          packageAsker(dirName(package), named != model_.packages.end() ? named->second.name : std::string(),
                       start.getFullPathName().toStdString());
      if (chosen.empty()) continue;
      const juce::File pick = file(chosen);
      if (pick.isDirectory()) {
        lastPicked = pick;
        std::vector<juce::File> near;
        for (juce::File up = pick.getParentDirectory(); near.size() < 3 && up != up.getParentDirectory();
             up = up.getParentDirectory())
          near.push_back(up);
        for (const auto& d : walk(pick, 2, kMostFolders)) near.push_back(d);
        lookIn(near);
        if (!here(package)) {
          std::set<std::string> wanted;
          for (Id p : needed()) wanted.insert(dirName(p));
          std::vector<std::string> folders;
          for (const auto& d : walk(pick, 2, kMostFolders)) folders.push_back(d.getFullPathName().toStdString());
          for (const auto& s : OrganArchive::carryingIn(folders, wanted)) {
            joinSource(s);
            archiveRecord.appendText(juce::String::fromUTF8(s.c_str()) + "\n");
          }
        }
      } else if (pick.existsAsFile()) {
        joinSource(chosen);
        archiveRecord.appendText(pick.getFullPathName() + "\n");
      }
    }

  for (Id package : missing) {
    const auto named = model_.packages.find(package);
    const juce::String name = named != model_.packages.end() ? juce::String(named->second.name) : juce::String();
    const juce::String id(dirName(package));
    if (const auto at = roots.find(package); at != roots.end() && installedIn(file(at->second), package)) {
      found.insert(dirName(package));
      juce::Logger::writeToLog("load: package " + id + " (" + name + ") from " + juce::String(at->second));
    } else if (here(package)) {
      found.insert(dirName(package));
      juce::Logger::writeToLog("load: package " + id + " (" + name + ") from another organ's archives");
    } else {
      juce::Logger::writeToLog("load: package " + id + " (" + name + ") is not in " + home.getFullPathName() +
                               ", the folders in it or beside it, nor the package folder: its pipes are silent");
    }
  }
  samples_.setPackageRoots(std::move(roots));
  samples_.setPackagesFromElsewhere(found);
  return found;
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
      vs.startOffset = eventOffset_;
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

bool MasterpieceProcessor::stopSourcesEngaged(Id stopId) const {
  const auto it = stopSources_.find(stopId);
  if (it == stopSources_.end()) return false;
  for (Id sw : it->second)
    if (switches_.engaged(sw)) return true;
  return false;
}

void MasterpieceProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
  juce::ScopedNoDenormals noDenormals;
  buffer.clear();

  // Real-time render threads turned on or off, or a new device: the audio
  // thread follows, once (it does anything only on Linux).
  if (const uint32_t g = realtimeGeneration_.load(std::memory_order_relaxed); g != audioThreadGeneration_) {
    audioThreadGeneration_ = g;
    realtime::setAudioThread(realtimeThreads_.load(), blockSeconds_.load());
  }

  // Counted in before looking at the flag, counted out on every way out.
  inAudioCallback_.fetch_add(1);
  struct Leave {
    std::atomic<int>& count;
    ~Leave() { count.fetch_sub(1); }
  } leave{inAudioCallback_};
  // What this block changed that the window draws, handed over on every way
  // out.
  struct Publish {
    MasterpieceProcessor& p;
    ~Publish() { p.publishShown(); }
  } publish{*this};

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
  // The ready lamp, every block: it blinks through a load, when nothing
  // else runs.
  pumpReadyLamp(buffer.getNumSamples());
  // An organ is being loaded: the engine is being rebuilt under us. Silence,
  // and the notes that arrive meanwhile are dropped -- the organ they were
  // played on is the one being replaced.
  if (engineSuspended_.load()) {
    midi.clear();
    blocksSkippedForLoad_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The first block of a new organ: the load message from Settings, then the
  // system exclusive sends, go before anything else, so a controller's mode message reaches it before any stop
  // moves and lights a lamp. The other sends follow with the lamps below.
  if (lampsAfterLoad_.load(std::memory_order_acquire)) {
    if (!loadMessage_.empty() && (midiOut_ != nullptr || outgoingProbe)) addMessages(outgoing_, loadMessage_);
    sentValues_.clear();
    emitSends(true);
  }

  // The runtime DSP switch is also a plain parameter, so a host can drop to
  // the simple-WAV path without a rebuild (ADR-005). The organ's own switch
  // is the one that is saved and loaded, so the parameter moves it only when
  // the parameter itself has moved. Copying it in on every block undid the
  // Organ settings tick within one block, and the next save wrote it as off.
  const bool simpleParam = *apvts_.getRawParameterValue("simpleWavOnly") > 0.5f;
  if (simpleParam != simpleParamSeen_) {
    simpleParamSeen_ = simpleParam;
    graph_.engineSwitch.simpleWavOnly = simpleParam;
  }

  const bool logBlocks = !blockLog_.empty() && blockLogCount_ < blockLog_.size();
  const auto tick = [] { return juce::Time::getHighResolutionTicks(); };
  const auto us = [](juce::int64 a, juce::int64 b) {
    return static_cast<float>(juce::Time::highResolutionTicksToSeconds(b - a) * 1.0e6);
  };
  const juce::int64 t0 = logBlocks ? tick() : 0;
  const int activeBefore = logBlocks ? voices_.stats().activeVoices : 0;
  const int stolenBefore = logBlocks ? voices_.stats().stolenVoices : 0;
  int ons = 0, offs = 0;

  // Merge the on-screen keyboard into the same buffer a device would fill, so
  // mouse and MIDI take one identical path.
  keyboardState_.processNextMidiBuffer(midi, 0, buffer.getNumSamples(), true);

  // Recording captures what arrived; playback hands its events back, each with
  // the console it came from, so a take drives exactly the path a live console
  // does. The console is part of what says which manual a note plays, and a
  // take that lost it played on whatever the channel means in the organ's own
  // assignment -- the pedal, whose code there is 1.
  recorder_.process(midi, buffer.getNumSamples(), recordedPlayback_);
  // The score panel's playing merges into the buffer. One atomic load while idle.
  scorePlayer_.process(midi, buffer.getNumSamples());

  // Then fold the buffer's events, the score panel's among them, into the
  // keyboard state.
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

  // The recorded events are not in the buffer the host handed us, so the
  // keyboard state is told about them here: the drawn manuals light up
  // through a recorded performance, and Panic has something to release.
  for (const auto& e : recordedPlayback_)
    keyboardState_.processNextMidiEvent(e.message);

  // Panic. Every key on every channel is released, as note-offs, so they take
  // the ordinary path: a voice stopped this way still gets its release sample
  // and the room's own decay, rather than being cut dead.
  //
  // The releases are kept apart from the buffer and sent after the recorded
  // events, so a note a take puts down in this block is released with the
  // rest. Done after the recorder so a stuck note cannot be re-triggered by
  // an event already queued this block.
  panicNotes_.clear();
  const bool panic = releaseAll_.exchange(false, std::memory_order_acq_rel);
  if (panic) {
    for (int ch = 1; ch <= 16; ++ch) {
      for (int note = 0; note < 128; ++note)
        if (keyboardState_.isNoteOn(ch, note))
          panicNotes_.push_back(juce::MidiMessage::noteOff(ch, note));
      keyboardState_.allNotesOff(ch);
    }
  }

  // A stop drawn or pushed in since the last block reaches the notes already
  // sounding, before this block's own keys are dealt with.
  if (stopsChanged_.exchange(false, std::memory_order_acq_rel))
    applyStopChangeToHeldNotes();

  // outgoing_ already holds what the window queued since the last block: a
  // stop clicked on screen or moved by the remote lights its lamp from here.
  juce::int64 t1 = 0, t2 = 0;
  if (logBlocks) {
    for (const auto meta : midi) {
      const auto m = meta.getMessage();
      if (m.isNoteOn()) ++ons;
      else if (m.isNoteOff()) ++offs;
    }
    t1 = tick();
  }
  handleMidi(midi);
  emitScorePageTurns();
  if (archiveOn_.load(std::memory_order_relaxed)) archive_.advance(buffer.getNumSamples());
  if (logBlocks) t2 = tick();
  // And every voice still sounding, once the note-offs above have been dealt
  // with: a note stuck because its note-off was lost, or arrived where the
  // keyboard state did not have it, is in no keyboard state to release, and
  // Panic did nothing for it (#276).
  // A note a drawn switch holds -- a motor or blower noise -- is not stuck,
  // and sounds on.
  if (panic) {
    voices_.releaseAll([this](uint64_t noteId) {
      for (const auto& [sw, id] : palletNotes_)
        if (id == noteId) return true;
      return false;
    });
    soundingNotes_.clear();
    releaseAllCoupledInputs();
    sustainedKeys_.clear();
  }
  controls_.propagate(0, &engagedSwitches_);
  if (stagesReady_.load(std::memory_order_acquire)) fireMovedStages();
  // A pallet switch that was engaged before the organ went live never moved
  // while pallets listened, so its pipes are opened here, once.
  if (palletsOpenEngaged_.exchange(false, std::memory_order_acq_rel))
    for (const auto& [switchId, pipes] : palletPipes_)
      if (switches_.engaged(switchId)) palletMoved(switchId, true);
  if (lampsAfterLoad_.exchange(false, std::memory_order_acq_rel)) sendLampsAfterLoad();
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
  if (outgoingProbe && !outgoing_.isEmpty()) outgoingProbe(outgoing_);
  outgoing_.clear();

  // Each enclosure renders its own voices and filters only those, so an
  // unenclosed Great stays unenclosed while the Swell shades move.
  const juce::int64 t3 = logBlocks ? tick() : 0;
  renderBuses(buffer);
  const juce::int64 t4 = logBlocks ? tick() : 0;

  // The player's voicing EQ for this organ, on the organ itself: before the
  // room and the master fader. Off, or under "no DSP", it does nothing.
  if (!graph_.engineSwitch.simpleWavOnly)
    voicingEq_.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                       buffer.getNumSamples());

  // Room before level: the convolver is part of the instrument's sound, and
  // the master fader is the last thing in the chain.
  //
  // Skipped entirely under simpleWavOnly. An FFT convolution is the most
  // expensive thing in this callback by a wide margin, and the switch is
  // called "no DSP" — a machine that needs it needs this gone more than it
  // needs anything else gone.
  // The room of the whole organ, the metronome and the taps belong to the
  // first pair: the one a stereo device has, and the one the buses without
  // device channels of their own are summed into.
  juce::AudioBuffer<float> firstPair(buffer.getArrayOfWritePointers(),
                                     juce::jmin(2, buffer.getNumChannels()), buffer.getNumSamples());
  if (!graph_.engineSwitch.simpleWavOnly) convolver_.process(firstPair);
  if (logBlocks) {
    const juce::int64 t5 = tick();
    BlockRecord& r = blockLog_[blockLogCount_++];
    r.totalUs = us(t0, t5);
    r.budgetUs = getSampleRate() > 0.0
                     ? static_cast<float>(1.0e6 * buffer.getNumSamples() / getSampleRate())
                     : 0.0f;
    r.preUs = us(t0, t1);
    r.midiUs = us(t1, t2);
    r.controlsUs = us(t2, t3);
    r.windUs = static_cast<float>(blockWindUs_);
    r.renderUs = us(t3, t4) - r.windUs;
    r.dspUs = us(t4, t5);
    r.noteOns = ons;
    r.noteOffs = offs;
    r.activeBefore = activeBefore;
    r.activeAfter = voices_.stats().activeVoices;
    r.stolen = voices_.stats().stolenVoices - stolenBefore;
  }

  // The organ's own output trim, before the player's fader: it is part of how
  // this set is meant to sound, not a setting. Folded into the same multiply
  // so it costs nothing, and deliberately NOT gated behind the DSP switch — a
  // constant gain is not an effect, and a slow machine should still hear the
  // set at the level its producer intended.
  buffer.applyGain(organTrimGain_ *
                   *apvts_.getRawParameterValue("masterGain"));

  // The compressor, after the master fader and ahead of the limiter, so
  // its make-up gain is still held under the limiter's ceiling. Off by
  // default and not called while off; a plugin leaves it to the host.
  if (compressor_.enabled() && !hostOwnsOutput_)
    compressor_.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                        buffer.getNumSamples());

  // The peak limiter, on the final output: after the master fader and
  // before everything that leaves the program. Off by default, and while it
  // is off the call is not made at all, so the output is bit for bit what
  // the engine made. A plugin leaves it to the host's effects.
  if (limiter_.enabled() && !hostOwnsOutput_)
    limiter_.process(buffer.getArrayOfWritePointers(), buffer.getNumChannels(),
                     buffer.getNumSamples());

  // Capture before the metronome. A click track belongs to the practice room,
  // not to the recording.
  // A wider output is folded down to a pair for the recording, each pair's
  // left into the left and its right into the right, so the take holds the
  // whole organ.
  if (buffer.getNumChannels() > 2 && audioRecorder_.isRecording()) {
    const int n = buffer.getNumSamples();
    if (recordScratch_.getNumSamples() < n) recordScratch_.setSize(2, n, false, false, true);
    juce::AudioBuffer<float> folded(recordScratch_.getArrayOfWritePointers(), 2, n);
    folded.clear();
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) folded.addFrom(ch & 1, 0, buffer, ch, 0, n);
    audioRecorder_.write(folded);
  } else {
    audioRecorder_.write(buffer);
  }

  // The metronome is a monitoring aid, not part of the instrument, so it sits
  // after the master fader and is not affected by it.
  metronome_.process(firstPair);

  // Load-progress taps, if asked for. Audible, post-recording, beside the
  // metronome: the same kind of thing for the same reason.
  maybeLoadTick(firstPair);

  // Under everything and after the recorder, so a recording never carries it.
  addKeepAlive(firstPair);

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
  soundingVoices_.store(voices_.stats().activeVoices, std::memory_order_relaxed);
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
  // A load runs on a thread of its own, where an exception would end the
  // program. Whatever throws -- the system refusing memory, a definition the
  // parser chokes on -- the load fails and says so, and the program goes on.
  // The engine's suspension and the loading flags undo themselves on the way
  // out; the stage table and the pallets stay off from the load's start, so
  // nothing half-built sounds.
  auto failed = [&](const std::string& why) {
    juce::Logger::writeToLog("load: FAILED: " + juce::String(why));
    try {
      voices_.reset();
      samples_.clear();
    } catch (...) {
    }
    LoadResult r;
    r.memoryLow = why.rfind("out of memory", 0) == 0;
    r.outOfMemory = r.memoryLow;
    r.error = why;
    return r;
  };
  try {
    return loadOrganUnguarded(odfFile, maxFramesPerSample, graphicsOnly);
  } catch (const std::bad_alloc&) {
    return failed("out of memory: the system refused memory while loading this organ");
  } catch (const std::exception& e) {
    return failed(std::string("the organ could not be loaded: ") + e.what());
  } catch (...) {
    return failed("the organ could not be loaded");
  }
}

MasterpieceProcessor::LoadResult MasterpieceProcessor::loadOrganUnguarded(
    const juce::File& odfFile, int64_t maxFramesPerSample, bool graphicsOnly) {
  // For anything that reads the model from another thread (the control API):
  // hands off while this runs, and a new session once it has.
  struct Loading {
    MasterpieceProcessor& p;
    explicit Loading(MasterpieceProcessor& proc) : p(proc) { p.organLoading_.store(true, std::memory_order_release); }
    ~Loading() {
      p.organReady_.store(p.loadedOdf_ != juce::File(), std::memory_order_release);
      p.organLoads_.fetch_add(1, std::memory_order_acq_rel);
      p.organLoading_.store(false, std::memory_order_release);
    }
  } loading(*this);
  LoadResult result;
  LoadPhases phases;
  // The organ being left keeps what the player just set on it: the file is
  // named after the organ that is loaded, which is about to change.
  saveRememberedStateIfPending();
  // The organ being left unloads: its message goes out first (#565).
  sendUnloadMessage();
  // Settings restored from an archive are what this load reads; from here
  // on the program saves again.
  releaseSaves();
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
  // An archive's stages, when there were any, came before this point and
  // keep their times; the rest start from nothing.
  for (auto p : {LoadProgress::Phase::ReadingDefinition, LoadProgress::Phase::BuildingWind,
                 LoadProgress::Phase::WiringConsole, LoadProgress::Phase::FindingSamples,
                 LoadProgress::Phase::ReadingCache, LoadProgress::Phase::LoadingSamples,
                 LoadProgress::Phase::Preparing})
    loadProgress_.phaseMs[static_cast<size_t>(p)].store(0, std::memory_order_relaxed);
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
  // The voices are made from the same memory: what the pool will hold is
  // taken off what the samples may.
  armLoadBudget(graphicsOnly);
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
    samples_.setCodeSamples(c.code);
    samples_.setStreamReleases(c.stream);
    samples_.setStreamHeadFrames(c.streamHead);
    samples_.setStreamHeadPercent(c.streamHeadPercent);
    releaseBufferSeconds_ = c.releaseBuffer;
    preloadHead_ = c.preload;
    sparseEvery_ = c.sparse;
    samples_.setSparseEvery(c.sparse);
    programCouplers_ = c.programCouplers;
    graph_.engineSwitch = c.engine;
    samples_.setLoadSampleRate(c.rate);
    samples_.setReleaseChoice(c.releases);
    samples_.setReleaseMaxMs(c.releaseMs);
    setReleaseCutoffDb(c.releaseCutoffDb);
    juce::Logger::writeToLog("load: using the Loading choices kept for this session");
  } else {
    sessionLoading_.reset();
  }
  // The switch is as this organ's files left it; a host sees the same.
  mirrorSimpleWavParameter();

  OdfLoader loader;
  OdfLoader::Options opts;
  opts.organRootDir = root.getFullPathName().toStdString();
  opts.reportUnreadFields = reportUnreadFields_;
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
  if (!result.diagnostics.latchedDrawstops.empty())
    juce::Logger::writeToLog("load: " + juce::String(static_cast<int>(result.diagnostics.latchedDrawstops.size())) +
                             " drawstop switch(es) the definition marks momentary latch");

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
  // The pipes by id, for the sparse loading. Rebuilt here because the pipes
  // are new: the old ones belong to the organ being left.
  buildPipeIndex();
  // The releases the player chose to load, before anything reads them.
  if (samples_.releaseChoice() != SampleLibrary::ReleaseChoice::All) {
    SampleLibrary::pruneReleases(model_, samples_.releaseChoice());
    juce::Logger::writeToLog(juce::String("load: releases: ") +
                             (samples_.releaseChoice() == SampleLibrary::ReleaseChoice::Longest  ? "the longest only"
                              : samples_.releaseChoice() == SampleLibrary::ReleaseChoice::Shortest ? "the shortest only"
                                                                                                    : "none"));
  }
  if (samples_.releaseMaxMs() > 0)
    juce::Logger::writeToLog("load: releases kept up to " + juce::String(samples_.releaseMaxMs()) + " ms");
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
      stopBySwitch_[stop.controllingSwitchId].push_back(stopId);

  // The organ's switch wiring, solved once from the declared defaults. An
  // organ that ships with its blower running or a unison coupler drawn comes
  // up that way rather than needing the player to find a switch nobody told
  // them about.
  phases.mark("model: stop map");
  // The key flow is walked before the switches are wired, because the
  // program couplers are switches of the organ's own, and the coupler
  // matrix has to know which divisions a player can play before it can
  // offer the couplers between them. The coupled inputs follow it: a coupler
  // to a keyboard reaches the divisions the matrix says that keyboard plays.
  couplers_.reset(model_);
  buildCouplerInputs();
  buildProgramCouplers();
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
  // The state the player saved with saveCurrentState(): applied whatever the
  // organ marks as remembered, since the player asked for exactly this.
  for (const auto& [id, on] : snapshotStates_) {
    const auto it = model_.switches.find(id);
    if (it != model_.switches.end() && it->second.latching) switches_.set(id, on);
  }
  for (Id id : snapshotStops_) engagedStops_.insert(id);
  engagedSwitches_ = switches_.engagedSwitches();
  shownStale_ = true;
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
  // The wind sway's waves: one per division, in id order, each rank under the
  // division of the first stop (by id) that draws it, and the last wave for
  // ranks in none. Each starts at a phase of its own.
  {
    rankSwayIndex_.clear();
    std::vector<Id> divisions, stops;
    for (const auto& [id, d] : model_.divisions) divisions.push_back(id);
    for (const auto& [id, s] : model_.stops) stops.push_back(id);
    std::sort(divisions.begin(), divisions.end());
    std::sort(stops.begin(), stops.end());
    std::unordered_map<Id, int> divisionIndex;
    for (size_t i = 0; i < divisions.size(); ++i) divisionIndex[divisions[i]] = static_cast<int>(i);
    for (Id stopId : stops) {
      const Stop& stop = model_.stops.at(stopId);
      const auto d = divisionIndex.find(stop.divisionId);
      if (d == divisionIndex.end()) continue;
      for (const auto& e : stop.ranks) rankSwayIndex_.emplace(e.rankId, d->second);
    }
    swayChannels_ = static_cast<int>(divisions.size()) + 1;
    swayPhaseA_.assign(static_cast<size_t>(swayChannels_), 0.0);
    swayPhaseB_.assign(static_cast<size_t>(swayChannels_), 0.0);
    for (int i = 0; i < swayChannels_; ++i) {
      swayPhaseA_[static_cast<size_t>(i)] = 6.283185307179586 * (pipeDetuneOffset(Id(1000 + i)) * 0.5 + 0.5);
      swayPhaseB_[static_cast<size_t>(i)] = 6.283185307179586 * (pipeDetuneOffset(Id(2000 + i)) * 0.5 + 0.5);
    }
    swayRatios_.assign(static_cast<size_t>(swayChannels_), 1.0);
  }
  for (const auto& [rankId, rank] : model_.ranks) {
    (void)rankId;
    for (const Pipe& pipe : rank.pipes) {
      const auto it = windIndexOf_.find(wind_.compartmentForPipe(pipe));
      if (it != windIndexOf_.end()) pipeWindIndex_[pipe.pipeId] = it->second;
    }
  }
  // The live levels: a fader per rank and per division, in id order as the
  // control API lists them, each rank under the division of the first stop
  // (by id) that draws it. All at full.
  {
    liveRankIndex_.clear();
    liveDivisionIndex_.clear();
    std::vector<Id> divisions, ranks, stops;
    for (const auto& [id, d] : model_.divisions) divisions.push_back(id);
    for (const auto& [id, r] : model_.ranks) ranks.push_back(id);
    for (const auto& [id, s] : model_.stops) stops.push_back(id);
    std::sort(divisions.begin(), divisions.end());
    std::sort(ranks.begin(), ranks.end());
    std::sort(stops.begin(), stops.end());
    for (size_t i = 0; i < divisions.size(); ++i) liveDivisionIndex_[divisions[i]] = static_cast<int>(i);
    std::unordered_map<Id, int> rankDivision;
    for (Id stopId : stops) {
      const Stop& stop = model_.stops.at(stopId);
      const auto d = liveDivisionIndex_.find(stop.divisionId);
      if (d == liveDivisionIndex_.end()) continue;
      for (const auto& e : stop.ranks) rankDivision.emplace(e.rankId, d->second);
    }
    std::vector<int> divisionOfRank;
    for (size_t i = 0; i < ranks.size(); ++i) {
      liveRankIndex_[ranks[i]] = static_cast<int>(i);
      const auto d = rankDivision.find(ranks[i]);
      divisionOfRank.push_back(d == rankDivision.end() ? -1 : d->second);
    }
    liveLevels_.configure(std::move(divisionOfRank), static_cast<int>(divisions.size()));
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
    // The organ's own start signal. Sets built on one template (Nancy,
    // Friesach, Oloron) declare a hidden pair, "Hidden OffToOn" at 0 and
    // "Hidden OnToOff" at 127, that moves once when the organ loads; every
    // start-up delay follows them, and at the top of its ramp fires what has
    // to happen then: Oloron's "Start blower on organ load" and "Enable
    // Appels d'anches on load" act through that pulse (#263). Moved at once:
    // the delays they feed are not timed here. Put at rest first: the delays
    // can come out of the settle already at the top, and a step fires only on
    // a move past it.
    for (const bool start : {false, true})
      for (const auto& [id, c] : model_.continuousControls) {
        if (c.imageSetInstanceId != 0) continue;
        const int low = std::min(c.maxValue, c.minValue), high = std::max(c.maxValue, c.minValue);
        if (c.name.rfind("Hidden OffToOn", 0) == 0) setControlValue(id, start ? high : low);
        else if (c.name.rfind("Hidden OnToOff", 0) == 0) setControlValue(id, start ? low : high);
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
  // The switches a linkage waits on, for the second look below.
  std::unordered_map<Id, std::vector<Id>> conditions;
  for (const auto& l : model_.switchLinkages) {
    if (l.sourceSwitchId != l.destSwitchId && l.sourceWhenEngaged)
      feeders[l.destSwitchId].push_back(l.sourceSwitchId);
    if (l.conditionSwitchId != 0 && l.conditionSwitchId != l.destSwitchId && l.conditionWhenEngaged)
      conditions[l.destSwitchId].push_back(l.conditionSwitchId);
  }

  // A switch wired straight into the nodes of several stops is a registration
  // aid -- a Tutti, a reversible, a general -- and the knob of none of them.
  // Coral Pipes' sets wire their Tutti into every stop node beside the stop's
  // own switch, one level closer than the stop's knob: taken for that knob,
  // drawing any stop pulled the Tutti (#211).
  // Each node counts once per switch: Portsea wires every knob into its
  // stop's node twice, engaged and disengaged, around its stop delay, and
  // counted per linkage every knob there passed for an aid (#366).
  std::unordered_map<Id, int> controlsOf;
  for (const auto& [sid, stop] : model_.stops)
    if (stop.controllingSwitchId != 0) ++controlsOf[stop.controllingSwitchId];
  std::unordered_map<Id, int> stopsFed;
  std::set<std::pair<Id, Id>> wired;
  for (const auto& l : model_.switchLinkages)
    if (l.sourceSwitchId != l.destSwitchId && controlsOf.count(l.destSwitchId) != 0 &&
        wired.emplace(l.sourceSwitchId, l.destSwitchId).second)
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
  //
  // The switches a linkage waits on come in only when the sources lead to no
  // knob. A GrandOrgue "And" of an undrawn switch (a blower, a ventil) and a
  // drawstop becomes a linkage from the first named into the stop's switch,
  // waiting on the second; named in that order, the walk ended on the undrawn
  // one and the stop had no knob, so the console listed the gate itself and a
  // piston drew the stop without its drawstop (#570). Last, so that a stop
  // whose own knob is upstream never takes a condition's knob for it.
  playerSwitch_.clear();
  std::vector<Id> queue;
  std::unordered_set<Id> seen;
  auto walk = [&](Id id, bool withConditions) {
    queue.clear();
    seen.clear();
    queue.push_back(id);
    seen.insert(id);
    for (size_t head = 0; head < queue.size() && head < 512; ++head) {
      for (const auto* edges : {&feeders, withConditions ? &conditions : nullptr}) {
        if (edges == nullptr) continue;
        const auto fit = edges->find(queue[head]);
        if (fit == edges->end()) continue;
        for (Id up : fit->second) {
          if (!seen.insert(up).second) continue;
          if (isAid(up)) continue;
          if (isKnob(up)) return up;
          queue.push_back(up);
        }
      }
    }
    return id;
  };
  for (const auto& [id, sw] : model_.switches) {
    (void)sw;
    if (isKnob(id)) { playerSwitch_[id] = id; continue; }
    Id found = walk(id, false);
    if (found == id && !conditions.empty()) found = walk(id, true);
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
  stopSources_.clear();
  {
    auto standsFor = [&](Id switchId, Id stopId) {
      if (switchId == 0) return;
      auto& v = stopsBySwitch_[switchId];
      if (std::find(v.begin(), v.end(), stopId) == v.end())
        v.push_back(stopId);
      auto& sources = stopSources_[stopId];
      if (std::find(sources.begin(), sources.end(), switchId) == sources.end())
        sources.push_back(switchId);
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
  for (const auto& [stopId, sources] : stopSources_)
    if (stopSourcesEngaged(stopId)) engagedStops_.insert(stopId);
  shownStale_ = true;
  // A new organ starts a new registration, so what the audio thread believes
  // is drawn has to be replaced rather than compared with the last organ's.
  stopsChanged_.store(true, std::memory_order_release);

  // Key flow. This is what makes couplers work at all: without it every
  // division sounds on every key, and drawing "Great to Pedal" changes
  // nothing because everything is already coupled to everything.
  buildDivisionInputs();
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
  loadProgress_.beginPhase(LoadProgress::Phase::FindingSamples);
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
    // The player's own choice, when no test flag has made one: the
    // perspectives, stops and single ranks left out (LoadPlan.h), worked out
    // as the memory figure in Organ settings works it out.
    perspectivesLeftOut_.clear();
    ranksLeftOut_.clear();
    bool readsNothing = false;
    if (onlyRanks.empty()) {
      LoadPlan plan = planLoad(model_, excludedPerspectives_, excludedStops_, excludedRanks_);
      readsNothing = plan.readsNothing;
      onlyRanks = std::move(plan.onlyRanks);
      perspectivesLeftOut_ = std::move(plan.perspectivesLeftOut);
      ranksLeftOut_ = std::move(plan.ranksLeftOut);
      unloadedStops_ = std::move(plan.unloadedStops);
      if (ranksLeftOut_.empty() && std::any_of(excludedRanks_.begin(), excludedRanks_.end(),
                                               [this](Id r) { return model_.ranks.count(r) != 0; }))
        juce::Logger::writeToLog("load: the single ranks left out would leave out every rank; they load");
      if (readsNothing) juce::Logger::writeToLog("load: the stops left out take every rank; no rank is read");
    }
    if (!perspectivesLeftOut_.empty()) {
      juce::String names;
      for (const auto& p : perspectivesLeftOut_) names << (names.isEmpty() ? "" : ", ") << juce::String(p);
      juce::Logger::writeToLog("load: perspectives left out: " + names);
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
    samples_.setCacheMode(keepPortable_ ? SampleLibrary::CacheMode::PerOrgan : cacheMode());
    samples_.setCacheIdentity(organKey(), odfStamp);
    loadedStamp_ = odfStamp;

    // An organ unpacked from its packages keeps its samples there: the
    // folder says which archives, and its saved index says where in them.
    {
      std::shared_ptr<OrganArchive> packaged;
      const PackagedRoot marked = packagedRootOf(opts.organRootDir);
      const std::string& archivePath = marked.archivePath;
      if (!archivePath.empty()) {
        auto archive = std::make_shared<OrganArchive>();
        std::string error;
        const std::string index = juce::File(juce::String::fromUTF8(marked.folder.c_str()))
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
      // A package found beside the organ is installed as far as the player is
      // concerned: the loader's warning that it is not goes.
      // A search that fails is logged, and the organ loads without them.
      std::set<std::string> joined;
      try {
        joined = joinPackagesFromElsewhere(packaged, opts.organRootDir, archivePath);
      } catch (const std::exception& e) {
        juce::Logger::writeToLog("load: the search for the packages the organ plays stopped: " +
                                 juce::String::fromUTF8(e.what()));
      }
      auto& warnings = result.diagnostics.warnings;
      for (const auto& package : joined)
        warnings.erase(std::remove_if(warnings.begin(), warnings.end(),
                                      [&package](const std::string& w) {
                                        return w.find("from package " + package + " (") != std::string::npos;
                                      }),
                       warnings.end());
      samples_.setArchive(packaged, marked.prefix);
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

    samples_.setCacheWhilePlaying(cacheWhilePlaying_.load());
    skipCacheSave_.store(false);
    // Sparse loading: the pipes that are not kept play a neighbour's samples,
    // so the samples they would have played are never read. The model is
    // changed first, and the load follows it.
    samples_.setSparseEvery(sparseEvery_);
    if (sparseEvery_ > 1 && !graphicsOnly) applySparseLoadingToModel();
    // Again, now that this organ's own loading choices are in force: whether
    // it streams and holds samples compressed decides what its pool takes.
    armLoadBudget(graphicsOnly);
    result.samples = samples_.loadAll(model_, opts.organRootDir, head,
                                      LoopSelection::Longest, &loadProgress_,
                                      onlyRanks.empty() && !readsNothing ? nullptr : &onlyRanks);

    // Now that the files have been opened, and only now, each sample can be
    // asked what pitch it holds. This has to happen after loading because one
    // of the routes the format offers is "it is in the file".
    resolveSamplePitches();
  }

  // A load stopped while its samples were being read keeps what was read:
  // at the memory limit, when the system ran low, and when the player
  // pressed Cancel. The stops whose samples arrived play; the rest are
  // silent, and the organ says it is incomplete, so the player can leave out
  // perspectives or stops until it fits, or load it again in full.
  if (loadProgress_.isCancelled() && result.samples.loaded > 0) {
    const bool overBudget = loadProgress_.overBudget.load(std::memory_order_acquire);
    const bool low = loadProgress_.memoryLow.load(std::memory_order_relaxed);
    juce::Logger::writeToLog(juce::String("load: ") +
                             (!overBudget ? "cancelled by the player"
                              : low       ? "stopped: the system has little memory left"
                                          : "stopped at the memory limit") +
                             "; keeping the " + juce::String(result.samples.loaded) + " of " +
                             juce::String(result.samples.wanted) +
                             " samples read -- the organ is INCOMPLETE");
    result.incomplete = true;
    result.outOfMemory = overBudget;
    result.memoryLow = low;
    result.stoppedByPlayer = !overBudget;
    loadProgress_.cancelled.store(false, std::memory_order_release);
  }
  if (loadProgress_.isCancelled()) {
    const bool outOfMemory = loadProgress_.overBudget.load(std::memory_order_acquire);
    juce::Logger::writeToLog(outOfMemory
                                 ? "load: stopped at the memory limit, discarding partial organ"
                                 : "load: cancelled, discarding partial organ");
    samples_.clear();
    model_ = OrganModel{};
    pipeById_.clear();
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
    result.memoryLow = loadProgress_.memoryLow.load(std::memory_order_relaxed);
    result.error = result.memoryLow
                       ? std::string("out of memory: the system has too little memory free for this "
                                     "organ's samples")
                   : outOfMemory
                       ? "out of memory: this organ's samples need more than the " +
                             std::to_string(memoryLimitBytes() / (1024 * 1024)) +
                             " MB memory limit"
                       : "cancelled";
    return result;
  }
  // The samples are in; what follows takes seconds on a large set, and the
  // window says so, step by step, where it used to sit at the last sample.
  if (!graphicsOnly) {
    phases.mark("samples read");
    loadProgress_.beginPhase(LoadProgress::Phase::Preparing);
    loadProgress_.beginStep(LoadProgress::Step::LettingGo);
  }
  // Still suspended, with no voice sounding: nothing else needs keeping.
  samples_.retireOldGenerations();
  // Rings for streamed releases as long as the disk this organ is on needs.
  loadProgress_.beginStep(LoadProgress::Step::Voices, voicePool());
  const double ringBefore = voices_.streamSeconds();
  sizeStreamRings();
  const bool ringChanged = voices_.streamSeconds() != ringBefore && samples_.streamedCount() > 0;
  // Samples held compressed are read through windows the voice pool makes
  // when it is built. A pool built for the other kind is built again, here,
  // while nothing sounds; and one whose rings are the wrong length.
  if (const size_t coded = samples_.codedCount();
      ((coded > 0) != voices_.codedWindows() || ringChanged) && sampleRate_ > 0.0) {
    buildVoicePool(getBlockSize());
    juce::Logger::writeToLog("load: the voice pool made again " +
                             juce::String(coded > 0 ? "with" : "without") +
                             " windows for compressed samples, rings of " +
                             juce::String(voices_.streamSeconds(), 2) + " s");
  }
  if (const size_t coded = samples_.codedCount(); coded > 0)
    juce::Logger::writeToLog("load: " + juce::String(static_cast<int>(coded)) +
                             " samples held compressed, " +
                             juce::String(samples_.residentBytes() / (1024 * 1024)) + " MB");
  voices_.setSampleProvider(samples_.provider());
  if (!graphicsOnly) phases.mark("voices");
  loadProgress_.beginStep(LoadProgress::Step::Releases);
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

  // The cache for the next load, written now, before the organ can be played,
  // unless the player chose to have it written while the organ plays. The
  // engine is still suspended: nothing sounds, and nothing competes with the
  // write. The load window shows its progress, and can skip it.
  if (!graphicsOnly && !cacheWhilePlaying_.load() && samples_.cacheWriting()) {
    loadProgress_.beginPhase(LoadProgress::Phase::SavingCache);
    while (samples_.cacheWriting()) {
      if (skipCacheSave_.load()) {
        samples_.finishCacheWrite(true);
        juce::Logger::writeToLog("cache: skipped by the player; the next load reads the samples");
        break;
      }
      loadProgress_.archiveFraction.store(samples_.cacheWriteFraction(), std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    phases.mark("cache");
  }
  cacheChoicesAtLoad_ = graphicsOnly ? std::string() : cacheChoices();

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
  loadProgress_.beginStep(LoadProgress::Step::Audio, voicePool());
  if (sampleRate_ > 0.0)
    prepareToPlay(sampleRate_, juce::jmax(1, maxBlock_));
  phases.mark("audio");

  // A mapping and a set of combinations saved for this organ come back with
  // it. Missing is normal: it means the player has not saved any yet.
  loadMidiMap();
  split_.organLoaded(organFile(loadedOdf_, "split", ".mpsplit"), &model_, couplers_.inputKeyboards());
  unloadedSwitches_.clear();
  unavailable_.clear();
  for (Id stopId : unloadedStops_) {
    const auto it = model_.stops.find(stopId);
    if (it != model_.stops.end() && it->second.controllingSwitchId != 0) {
      const Id sw = playerSwitchFor(it->second.controllingSwitchId);
      unloadedSwitches_.insert(sw);
      unavailable_.emplace(sw, Unavailable{stopId, false});
    }
    const auto knob = stopKnob_.find(stopId);
    if (knob != stopKnob_.end()) {
      unloadedSwitches_.insert(knob->second);
      unavailable_.emplace(knob->second, Unavailable{stopId, false});
    }
  }
  // A stop whose ranks ship no pipes (a demo set's locked stops) sounds no
  // more than one left out. It and its copies on other pages are drawn as
  // the set draws them, with no veil: a click on one is refused, and a
  // tooltip names it as not available.
  // Only a load that reads samples can tell: a graphics-only one has no
  // archive and holds nothing, and every rank would look unshipped.
  loadProgress_.beginStep(LoadProgress::Step::Ranks);
  unshippedRanks_.clear();
  if (!graphicsOnly)
    for (const auto& [rankId, rank] : model_.ranks)
      if (!rank.pipes.empty() && !samples_.rankShipped(rank, model_, organRootDir_)) unshippedRanks_.insert(rankId);
  if (!unshippedRanks_.empty())
    juce::Logger::writeToLog("load: " + juce::String((int)unshippedRanks_.size()) +
                             " rank(s) list pipes and ship no samples; their stops are not available");
  phases.mark("ranks");
  findPalletStops();
  findControlStops();
  std::unordered_set<Id> silent;
  lockedStops_.clear();
  for (const auto& e : stopList()) {
    if (e.playable) continue;
    lockedStops_.insert(e.stopId);
    const auto it = model_.stops.find(e.stopId);
    if (it != model_.stops.end() && it->second.controllingSwitchId != 0) {
      const Id sw = playerSwitchFor(it->second.controllingSwitchId);
      silent.insert(sw);
      unavailable_.emplace(sw, Unavailable{e.stopId, true});
    }
    if (const auto knob = stopKnob_.find(e.stopId); knob != stopKnob_.end()) {
      silent.insert(knob->second);
      unavailable_.emplace(knob->second, Unavailable{e.stopId, true});
    }
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
          // A copy is veiled when the stop was left out of the load. A stop
          // with no pipes is left as drawn: a click on it is refused and a
          // tooltip says why (ConsoleView).
          const auto why = unavailable_.find(at);
          if (why == unavailable_.end()) continue;
          unavailable_.emplace(it->second, why->second);
          if (!why->second.noPipes) unloadedSwitches_.insert(it->second);
        }
    }
  }
  // And held in, whatever would draw them. Only the click was refused: a
  // general, the crescendo, a tutti, a MIDI message or the organ's own wiring
  // still drew a locked stop, which then showed drawn and sounded nothing,
  // where the set's own player keeps it in. A switch that also draws a stop
  // with pipes stays free, and an organ with no pipes at all is no demo:
  // its stops are left as they are.
  if (lockedStops_.size() == model_.stops.size()) lockedStops_.clear();
  if (!lockedStops_.empty()) {
    auto drawsPlayable = [&](Id sw) {
      for (const auto* map : {&stopBySwitch_, &stopsBySwitch_})
        if (const auto it = map->find(sw); it != map->end())
          for (Id stopId : it->second)
            if (lockedStops_.count(stopId) == 0) return true;
      return false;
    };
    // Nor a switch the organ starts engaged: the Alessandria demo controls its
    // locked Tuba with the switch that runs the organ's noises.
    std::unordered_set<Id> held;
    auto hold = [&](Id sw) {
      if (sw == 0 || drawsPlayable(sw)) return;
      if (const auto s = model_.switches.find(sw); s != model_.switches.end() && s->second.defaultEngaged) return;
      held.insert(sw);
    };
    for (const auto& [sw, why] : unavailable_)
      if (why.noPipes) hold(sw);
    for (Id stopId : lockedStops_)
      if (const auto it = model_.stops.find(stopId); it != model_.stops.end())
        hold(it->second.controllingSwitchId);
    switches_.holdOff(std::move(held));
    for (Id stopId : lockedStops_) engagedStops_.erase(stopId);
    engagedSwitches_ = switches_.engagedSwitches();
    shownStale_ = true;
    stopsChanged_.store(true, std::memory_order_release);
  }
  loadProgress_.beginStep(LoadProgress::Step::Combinations);
  resetPlayerCombinations();
  loadCombinations();

  phases.mark("prepare");
  juce::Logger::writeToLog("load: TOTAL         " +
                           juce::String(phases.sinceStart(), 1) + " ms  (" +
                           odfFile.getFileName() + ")");
  // The same load as the dialog showed it: the time under each of its labels.
  {
    const int64_t now = LoadProgress::nowMs();
    const int64_t began = loadProgress_.phaseBeganMs.load(std::memory_order_acquire);
    for (auto p : {LoadProgress::Phase::OpeningArchive, LoadProgress::Phase::ReadingArchive,
                   LoadProgress::Phase::ExtractingArchive, LoadProgress::Phase::ReadingDefinition,
                   LoadProgress::Phase::BuildingWind, LoadProgress::Phase::WiringConsole,
                   LoadProgress::Phase::FindingSamples, LoadProgress::Phase::ReadingCache,
                   LoadProgress::Phase::LoadingSamples, LoadProgress::Phase::Preparing}) {
      int64_t ms = loadProgress_.timeIn(p);
      // The stage still running is the last one, charged up to now.
      if (p == loadProgress_.phase.load(std::memory_order_acquire) && began > 0) ms += now - began;
      if (ms > 0)
        juce::Logger::writeToLog("stage: " + juce::String(LoadProgress::phaseName(p)) + "  " +
                                 juce::String(static_cast<double>(ms) / 1000.0, 1) + " s");
    }
  }

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
      juce::Logger::writeToLog("cache: creating the sample cache, in the background");
    juce::Logger::writeToLog(
        "memory: resident " + mb(samples_.residentBytes()) + " MB" +
        ", streamed " + mb(samples_.streamedBytesSaved()) + " MB not held" +
        ", storage=" +
        (samples_.storage() == SampleStorage::Int16   ? "int16"
         : samples_.storage() == SampleStorage::Int20 ? "int20"
         : samples_.storage() == SampleStorage::Int24 ? "int24"
                                                      : "float32") +
        ", mono=" + (samples_.loadMono() ? "on" : "off") +
        ", compressed=" + (samples_.codeSamples() ? "on" : "off") +
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
  lampsAfterLoad_.store(true, std::memory_order_release);
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
  return mp::dataFolder()
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
  if (odf.isAChildOf(root) || !packagedRootOf(organRootDir_).folder.empty()) return;
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
  mp::writeTextFile(stamp, juce::String(loadedStamp_));

  // The index: this original now has a copy.
  juce::StringArray lines;
  lines.addLines(root.getChildFile("index.txt").loadFileAsString());
  lines.removeEmptyStrings();
  for (int i = lines.size(); --i >= 0;)
    if (lines[i].upToFirstOccurrenceOf("\t", false, false) == odf.getFullPathName()) lines.remove(i);
  lines.add(odf.getFullPathName() + "\t" + copy.getFullPathName());
  mp::writeTextFile(root.getChildFile("index.txt"), lines.joinIntoString("\n") + "\n");
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
    // and one that was never shipped is actively misleading. A stop wired to
    // its pipes' pallets plays them too, and one that works a coupler or a
    // tremulant is a control in its own right. A tremulant with ranks of its
    // own keeps them to leave out in Organ settings.
    e.control = controlStops_.count(id) != 0 && stop.ranks.empty();
    e.playable = palletStops_.count(id) != 0 || controlStops_.count(id) != 0;
    for (const auto& entry : stop.ranks) {
      const auto rit = model_.ranks.find(entry.rankId);
      if (rit != model_.ranks.end() && !rit->second.pipes.empty() && unshippedRanks_.count(entry.rankId) == 0) {
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
// The spellings a package carries: matching is case-sensitive on Linux, and
// GrandOrgue sets inside Hauptwerk packages are often "Name.Organ".
// The Hauptwerk.xml pair is version 3's spelling of the first two, dots for underscores.
constexpr const char* kDefinitionPatterns =
    "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.organ;*.Organ;*.ORGAN;"
    "*.Organ.Hauptwerk.xml;*.CustomOrgan.Hauptwerk.xml";
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
  mp::writeTextFile(package.getChildFile(kChosenDefinition), odf.getRelativePathFrom(package));
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
  const juce::File dir = mp::dataFolder()
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
      loadProgress_.waitWhileHeld();
      return !loadProgress_.isCancelled();
    };
    loadProgress_.beginPhase(LoadProgress::Phase::ReadingArchive);
    if (!archive.index(why, progress, dir.getFullPathName().toStdString())) {
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
                     kDefinitionPatterns);
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

// ------------------------------------------------- ready lamp and power

MasterpieceProcessor::ReadyLamp MasterpieceProcessor::readyLamp() const {
  ReadyLamp lamp;
  lamp.kind = static_cast<MidiSourceKind>(lampKind_.load(std::memory_order_relaxed));
  lamp.channel = lampChannel_.load(std::memory_order_relaxed);
  lamp.number = lampNumber_.load(std::memory_order_relaxed);
  return lamp;
}

void MasterpieceProcessor::setReadyLamp(const ReadyLamp& lamp) {
  const bool usable = lamp.kind == MidiSourceKind::Note || lamp.kind == MidiSourceKind::ControlChange;
  lampKind_.store(static_cast<int>(usable ? lamp.kind : MidiSourceKind::None), std::memory_order_relaxed);
  lampChannel_.store(juce::jlimit(1, 16, lamp.channel), std::memory_order_relaxed);
  lampNumber_.store(juce::jlimit(0, 127, lamp.number), std::memory_order_relaxed);
  writeGlobalFile();
}

bool MasterpieceProcessor::setLoadMessage(const juce::String& hex) {
  std::vector<uint8_t> bytes;
  if (!parseMidiText(hex.toStdString(), bytes)) return false;
  if (bytes == loadMessage_) return true;
  {
    const AudioLock audio(*this);
    loadMessage_.swap(bytes);
  }
  writeGlobalFile();
  return true;
}

juce::String MasterpieceProcessor::loadMessage() const { return juce::String(midiText(loadMessage_)); }

bool MasterpieceProcessor::setUnloadMessage(const juce::String& hex) {
  std::vector<uint8_t> bytes;
  if (!parseMidiText(hex.toStdString(), bytes)) return false;
  if (bytes == unloadMessage_) return true;
  {
    const AudioLock audio(*this);
    unloadMessage_.swap(bytes);
  }
  writeGlobalFile();
  return true;
}

juce::String MasterpieceProcessor::unloadMessage() const { return juce::String(midiText(unloadMessage_)); }

void MasterpieceProcessor::sendUnloadMessage() {
  if (unloadMessage_.empty() || loadedOdf_ == juce::File()) return;
  // Under the lock: the audio thread sends on the same output.
  const AudioLock audio(*this);
  if (midiOut_ == nullptr && !outgoingProbe) return;
  juce::MidiBuffer out;
  addMessages(out, unloadMessage_);
  if (midiOut_ != nullptr) midiOut_->sendBlockOfMessagesNow(out);
  if (outgoingProbe) outgoingProbe(out);
}

bool MasterpieceProcessor::setReadyLampMessages(const juce::String& litHex, const juce::String& darkHex) {
  std::vector<uint8_t> lit, dark;
  if (!parseMidiText(litHex.toStdString(), lit) || !parseMidiText(darkHex.toStdString(), dark)) return false;
  if (lit == lampLit_ && dark == lampDark_) return true;
  {
    const AudioLock audio(*this);
    lampLit_.swap(lit);
    lampDark_.swap(dark);
    // Sent again in the new form at the next block.
    lampSent_ = -1;
  }
  writeGlobalFile();
  return true;
}

juce::String MasterpieceProcessor::readyLampLitMessages() const { return juce::String(midiText(lampLit_)); }
juce::String MasterpieceProcessor::readyLampDarkMessages() const { return juce::String(midiText(lampDark_)); }

void MasterpieceProcessor::addMessages(juce::MidiBuffer& buffer, const std::vector<uint8_t>& bytes) {
  for (size_t at = 0; at < bytes.size();) {
    const size_t n = midiMessageLength(bytes, at);
    if (n == 0) break;
    buffer.addEvent(bytes.data() + at, static_cast<int>(n), 0);
    at += n;
  }
}

bool MasterpieceProcessor::readyLampMessage(const ReadyLamp& lamp, bool lit, RawMidi& out) {
  const int ch = juce::jlimit(1, 16, lamp.channel) - 1;
  const auto n = static_cast<uint8_t>(juce::jlimit(0, 127, lamp.number));
  if (lamp.kind == MidiSourceKind::Note) {
    out.bytes[0] = static_cast<uint8_t>((lit ? 0x90 : 0x80) | ch);
    out.bytes[1] = n;
    out.bytes[2] = lit ? 127 : 0;
  } else if (lamp.kind == MidiSourceKind::ControlChange) {
    out.bytes[0] = static_cast<uint8_t>(0xB0 | ch);
    out.bytes[1] = n;
    out.bytes[2] = lit ? 127 : 0;
  } else {
    return false;
  }
  out.size = 3;
  return true;
}

// Lit with an organ ready, dark with none; blinking once a second while one
// loads, and four times a second while a quit or power-off button is held,
// so a player who lets go in time knows nothing will happen.
void MasterpieceProcessor::pumpReadyLamp(int numFrames) {
  const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
  lampFrames_ += static_cast<uint64_t>(juce::jmax(0, numFrames));
  const double t = static_cast<double>(lampFrames_) / rate;
  int lit;
  if (powerHeld_ != MidiTargetKind::None)
    lit = static_cast<int>(t * 8.0) % 2 == 0 ? 1 : 0;
  else if (organLoading())
    lit = static_cast<int>(t * 2.0) % 2 == 0 ? 1 : 0;
  else
    lit = organReady_.load(std::memory_order_acquire) ? 1 : 0;
  lampState_.store(lit, std::memory_order_relaxed);

  // The quit and power-off buttons count their time here too, so a held one
  // fires even in a block with no MIDI.
  powerClockFrames_ += static_cast<uint64_t>(juce::jmax(0, numFrames));
  if (powerHeld_ != MidiTargetKind::None &&
      static_cast<double>(powerClockFrames_ - powerHeldSince_) >= kPowerHoldSeconds * rate) {
    pendingPower_.store(powerHeld_ == MidiTargetKind::PowerOffComputer ? 2 : 1, std::memory_order_release);
    powerHeld_ = MidiTargetKind::None;
  }

  const ReadyLamp lamp = readyLamp();
  const bool own = !lampLit_.empty() || !lampDark_.empty();
  if ((lamp.kind == MidiSourceKind::None && !own) || (midiOut_ == nullptr && !outgoingProbe)) {
    lampSent_ = -1;
    return;
  }
  if (lit == lampSent_) return;
  // The block after a load opens with the load message; the lamp follows in
  // the next one.
  if (lampsAfterLoad_.load(std::memory_order_acquire)) return;
  lampSent_ = lit;
  // Into a block sized once, so a long message allocates nothing here.
  lampOut_.clear();
  RawMidi raw;
  if (readyLampMessage(lamp, lit == 1, raw)) lampOut_.addEvent(raw.bytes, raw.size, 0);
  addMessages(lampOut_, lit == 1 ? lampLit_ : lampDark_);
  if (lampOut_.isEmpty()) return;
  if (midiOut_ != nullptr) midiOut_->sendBlockOfMessagesNow(lampOut_);
  if (outgoingProbe) outgoingProbe(lampOut_);
}

void MasterpieceProcessor::powerButton(MidiTargetKind kind, bool pressed, bool hasRelease) {
  const double rate = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
  const double ran = static_cast<double>(powerClockFrames_) / rate;
  if (ran < kPowerArmSeconds) {
    // Before it is armed nothing counts, not even the start of a hold.
    powerHeld_ = MidiTargetKind::None;
    powerLastPress_ = MidiTargetKind::None;
    if (pressed)
      powerNotArmedSeconds_.store(juce::jmax(1, static_cast<int>(kPowerArmSeconds - ran + 0.999)),
                                  std::memory_order_release);
    return;
  }
  if (!hasRelease) {
    // A program change or a system exclusive message says nothing when the
    // button is let go, so it is pressed twice.
    if (!pressed) return;
    if (powerLastPress_ == kind &&
        static_cast<double>(powerClockFrames_ - powerLastPressAt_) <= kPowerTwiceSeconds * rate) {
      pendingPower_.store(kind == MidiTargetKind::PowerOffComputer ? 2 : 1, std::memory_order_release);
      powerLastPress_ = MidiTargetKind::None;
      return;
    }
    powerLastPress_ = kind;
    powerLastPressAt_ = powerClockFrames_;
    return;
  }
  if (pressed) {
    if (powerHeld_ != kind) {
      powerHeld_ = kind;
      powerHeldSince_ = powerClockFrames_;
    }
  } else if (powerHeld_ == kind) {
    powerHeld_ = MidiTargetKind::None;  // let go in time: nothing happens
  }
}

void MasterpieceProcessor::serviceMidiRequests() {
  if (const int left = powerNotArmedSeconds_.exchange(0, std::memory_order_acq_rel); left > 0)
    juce::Logger::writeToLog("midi: quit/power-off button ignored, armed in " + juce::String(left) + " s");
  if (const int64_t t = pendingTemperament_.exchange(0, std::memory_order_acq_rel); t > 0) {
    const Id target = static_cast<Id>(t - 1);
    if (target == 0) {
      setTemperament("");
    } else {
      for (const auto& lib : temperamentLibrary())
        if (temperamentTarget(lib.name) == target) {
          setTemperament(lib.name);
          break;
        }
    }
  }
  if (const int p = pendingPower_.exchange(0, std::memory_order_acq_rel); p != 0) {
    // Shutting the computer down needs the same permission the control API's
    // powerOff does (General settings, Remote): a learned button alone is
    // not enough. Quitting saves first and is always allowed.
    if (p == 2 && !remote_.allowPowerOff) {
      juce::Logger::writeToLog("midi: power-off button held, refused: shutting the computer down is not "
                               "allowed (General settings, Remote)");
      return;
    }
    juce::Logger::writeToLog(p == 2 ? "midi: power-off button held, shutting down"
                                    : "midi: quit button held, quitting");
    if (onPowerRequest) onPowerRequest(p == 2);
  }
}

bool MasterpieceProcessor::stopLoad(int timeoutMs) {
  if (!organLoading()) return true;
  cancelLoad();
  const auto until = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (organLoading()) {
    if (juce::Time::getMillisecondCounter() > until) {
      juce::Logger::writeToLog("load: still running " + juce::String(timeoutMs) + " ms after the cancel");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  juce::Logger::writeToLog("load: stopped for the program to close");
  return true;
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

bool MasterpieceProcessor::saveTakeForSequencer(const juce::File& file, bool markers) const {
  const auto events = recorder_.takeEvents();
  // A track per keyboard, in the organ's keyboard order, then "Other" for
  // notes that reach none and "Pedals" for the swell and crescendo pedals.
  std::vector<Id> keyboards;
  for (const auto& [id, k] : model_.keyboards) keyboards.push_back(id);
  std::sort(keyboards.begin(), keyboards.end());
  const size_t other = keyboards.size(), pedals = keyboards.size() + 1;
  std::vector<juce::MidiMessageSequence> tracks(keyboards.size() + 2);
  std::map<Id, size_t> trackOf;
  juce::MidiMessageSequence markerSeq;  // the first track's, with the tempo
  for (size_t i = 0; i < keyboards.size(); ++i) trackOf[keyboards[i]] = i;

  // What a switch is called: the stop it works, or the switch's own name.
  auto nameOf = [this](Id switchId) {
    for (const auto& [stopId, stop] : model_.stops)
      if (stop.controllingSwitchId == switchId && !stop.name.empty()) return juce::String::fromUTF8(stop.name.c_str());
    const auto it = model_.switches.find(switchId);
    return it != model_.switches.end() && !it->second.name.empty() ? juce::String::fromUTF8(it->second.name.c_str())
                                                                     : juce::String(static_cast<int>(switchId));
  };

  // Times from the first event kept: 120 bpm at 960 ticks a quarter, so a
  // second is 1920 ticks.
  constexpr double kTicksPerSecond = 1920.0;
  double start = -1.0, last = 0.0;
  auto ticks = [&](double seconds) { return std::round((seconds - start) * kTicksPerSecond); };
  // Keys down, per track, channel and note: a second note-on gets a note-off
  // first, a note-off for a key not down is dropped, and every key still down
  // at the end is let go there.
  std::set<std::tuple<size_t, int, int>> down;

  for (const auto& e : events) {
    const juce::MidiMessage& m = e.message;
    if (m.isSysEx() || m.isActiveSense() || m.isMidiClock() || m.isMidiStart() || m.isMidiStop() ||
        m.isMidiContinue() || m.isMetaEvent())
      continue;
    MidiSource source;
    int value = 0;
    if (m.isNoteOnOrOff()) {
      source.kind = MidiSourceKind::Note;
      source.number = m.getNoteNumber();
      value = m.isNoteOn() ? m.getVelocity() : 0;
    } else if (m.isController()) {
      source.kind = MidiSourceKind::ControlChange;
      source.number = m.getControllerNumber();
      value = m.getControllerValue();
    } else if (m.isProgramChange()) {
      source.kind = MidiSourceKind::ProgramChange;
      source.number = m.getProgramChangeNumber();
      value = 127;
    }
    source.channel = m.getChannel();
    source.deviceId = e.deviceId;
    // What the message did on the console: the binding a press finds.
    const MidiAction action = source.kind == MidiSourceKind::None
                                  ? MidiAction{}
                                  : midiMap_.actionFor(source, value > 0 ? value : 127);
    if (action.valid() && action.kind != MidiTargetKind::Keyboard) {
      if (action.kind == MidiTargetKind::Switch) {
        // A stop or a coupler: a marker with its name, on the press.
        if (markers && value > 0) {
          if (start < 0.0) start = e.seconds;
          juce::String text = nameOf(action.targetId) + (action.toggled ? juce::String() : (action.engage ? " on" : " off"));
          // And every other stop the same button works.
          if (action.alsoDrives != nullptr)
            for (const auto& b : *action.alsoDrives)
              text << ", " << nameOf(b.targetId)
                   << (b.trigger == MidiTrigger::Toggle          ? ""
                       : b.trigger == MidiTrigger::DisengageOnly ? " off"
                                                                 : " on");
          auto marker = juce::MidiMessage::textMetaEvent(6, text);
          marker.setTimeStamp(ticks(e.seconds));
          markerSeq.addEvent(marker);
          last = std::max(last, e.seconds);
        }
      } else if (action.kind == MidiTargetKind::ContinuousControl) {
        if (start < 0.0) start = e.seconds;
        auto cc = juce::MidiMessage::controllerEvent(1, 11, juce::jlimit(0, 127, value));
        cc.setTimeStamp(ticks(e.seconds));
        tracks[pedals].addEvent(cc);
        last = std::max(last, e.seconds);
      }
      // Every other mapped message works the program: left out.
      continue;
    }
    if (!m.isNoteOnOrOff()) continue;
    const Id keyboard = keyboardForChannel(m.getChannel(), e.deviceId);
    const auto at = trackOf.find(keyboard);
    const size_t track = at != trackOf.end() ? at->second : other;
    const auto key = std::make_tuple(track, m.getChannel(), m.getNoteNumber());
    if (start < 0.0) start = e.seconds;
    const double t = ticks(e.seconds);
    if (m.isNoteOn()) {
      if (down.count(key) != 0) {
        auto off = juce::MidiMessage::noteOff(m.getChannel(), m.getNoteNumber());
        off.setTimeStamp(t);
        tracks[track].addEvent(off);
      }
      auto on = juce::MidiMessage::noteOn(m.getChannel(), m.getNoteNumber(), m.getVelocity());
      on.setTimeStamp(t);
      tracks[track].addEvent(on);
      down.insert(key);
    } else {
      if (down.erase(key) == 0) continue;
      auto off = juce::MidiMessage::noteOff(m.getChannel(), m.getNoteNumber());
      off.setTimeStamp(t);
      tracks[track].addEvent(off);
    }
    last = std::max(last, e.seconds);
  }
  if (start < 0.0) start = 0.0;
  // Keys still down are let go where the take ends, so none is left with no
  // length.
  const double end = std::max(last, recorder_.endSeconds());
  for (const auto& [track, channel, note] : down) {
    auto off = juce::MidiMessage::noteOff(channel, note);
    off.setTimeStamp(ticks(end));
    tracks[track].addEvent(off);
  }

  juce::MidiFile out;
  out.setTicksPerQuarterNote(960);
  juce::MidiMessageSequence first;
  first.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0.0);
  first.addEvent(juce::MidiMessage::textMetaEvent(1, juce::String::fromUTF8(model_.organName.c_str())), 0.0);
  first.addSequence(markerSeq, 0.0);
  out.addTrack(first);
  auto named = [](const juce::String& name, juce::MidiMessageSequence& seq) {
    seq.addEvent(juce::MidiMessage::textMetaEvent(3, name), 0.0);
    seq.sort();
    seq.updateMatchedPairs();
    return seq;
  };
  for (size_t i = 0; i < keyboards.size(); ++i)
    out.addTrack(named(juce::String::fromUTF8(model_.keyboards.at(keyboards[i]).name.c_str()), tracks[i]));
  if (tracks[other].getNumEvents() > 0) out.addTrack(named("Other", tracks[other]));
  if (tracks[pedals].getNumEvents() > 0) out.addTrack(named("Pedals", tracks[pedals]));

  file.getParentDirectory().createDirectory();
  file.deleteFile();
  std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
  return stream != nullptr && out.writeTo(*stream, 1);
}

bool MasterpieceProcessor::saveTake(const juce::File& file) {
  return recorder_.saveToFile(file, [this](int id) { return midiMap_.devices().nameFor(id); });
}

bool MasterpieceProcessor::loadTake(const juce::File& file) {
  return recorder_.loadFromFile(file, [this](const std::string& name) { return midiMap_.devices().idFor(name); });
}

void MasterpieceProcessor::setArchiveSettings(const ArchiveSettings& s, bool save) {
  archiveSilence_ = juce::jlimit(10, 600, s.silenceSeconds);
  archiveMegabytes_ = juce::jmax(1, s.maxMegabytes);
  archiveDays_ = juce::jmax(1, s.maxDays);
  archiveOn_.store(s.on, std::memory_order_release);
  if (!s.on) serviceArchive(true);  // switched off: the take in progress is kept
  if (save) writeGlobalFile();
}

juce::File MasterpieceProcessor::archiveFolder() const {
  return archiveFolder_ != juce::File() ? archiveFolder_ : recordingsFolder().getChildFile("Archive");
}

juce::File MasterpieceProcessor::serviceArchive(bool now) {
  std::vector<MidiRecorder::TakeEvent> take;
  double startedAgo = 0.0;
  if (!archive_.takeEnded(static_cast<double>(archiveSilence_), now, take, startedAgo)) return {};
  const juce::File folder = archiveFolder();
  const juce::Time started = juce::Time::getCurrentTime() - juce::RelativeTime::seconds(startedAgo);
  const juce::File file = MidiArchive::save(folder, take, juce::String::fromUTF8(model_.organName.c_str()), started,
                                            [this](int id) { return midiMap_.devices().nameFor(id); });
  if (file == juce::File()) {
    juce::Logger::writeToLog("archive: the take could not be written in " + folder.getFullPathName());
    return {};
  }
  const int gone = MidiArchive::prune(folder, static_cast<int64_t>(archiveMegabytes_) * 1024 * 1024, archiveDays_,
                                      juce::Time::getCurrentTime());
  juce::Logger::writeToLog("archive: " + juce::String(static_cast<int>(take.size())) + " events saved as " +
                           file.getFullPathName() +
                           (gone > 0 ? "; " + juce::String(gone) + " older file(s) deleted" : juce::String()));
  return file;
}

juce::File MasterpieceProcessor::recordingsFolder() {
  return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("Recordings");
}

juce::File MasterpieceProcessor::pressRecorderButton(bool record, const juce::File& folder) {
  if (!record) {
    if (recorder_.isPlaying()) recorder_.stopPlayback();
    else if (!recorder_.isRecording() && !recorder_.empty()) recorder_.startPlayback();
    return {};
  }
  if (recorder_.isRecording()) {
    recorder_.stopRecording();
    return {};
  }
  if (recorder_.isPlaying()) recorder_.stopPlayback();
  juce::File saved;
  if (recorder_.hasUnsavedTake()) {
    // Named after the organ and the time, with what a file name cannot hold
    // taken out of the organ's name.
    const juce::String organ = juce::File::createLegalFileName(
        model_.organName.empty() ? juce::String("Masterpiece") : juce::String::fromUTF8(model_.organName.c_str()));
    folder.createDirectory();
    saved = folder.getChildFile(organ + " " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S") + ".mid")
                .getNonexistentSibling();
    if (!saveTake(saved)) {
      // Kept in memory and not replaced: a take is never thrown away.
      juce::Logger::writeToLog("recorder: the take could not be saved in " + folder.getFullPathName() +
                               "; it is kept and no new one begins");
      return {};
    }
    lastButtonSave_ = saved;
    juce::Logger::writeToLog("recorder: the take was saved as " + saved.getFullPathName());
  }
  recorder_.startRecording();
  return saved;
}

// A console's lamps after a load: the lamps of the organ before stayed as
// they were, and a stop the new organ starts with drawn was never lit, since
// a lamp was sent only when its switch moved (#565). Every lamp is sent once,
// as the new organ stands: the explicit sends start over, and with stop
// changes sent on the output, each stop learned on a note or a controller.
void MasterpieceProcessor::sendLampsAfterLoad() {
  // The explicit sends start over: the system exclusive ones went at the top
  // of this block, and the others go in emitSends() after the lamps.
  if (!midiFeedback_ || (midiOut_ == nullptr && !outgoingProbe)) return;
  for (const auto& b : midiMap_.bindings()) {
    if (b.targetKind != MidiTargetKind::Switch || midiMap_.hasSends(MidiTargetKind::Switch, b.targetId)) continue;
    const bool on = switches_.engaged(b.targetId);
    const int ch = b.source.channel > 0 ? b.source.channel : 1;
    if (b.source.kind == MidiSourceKind::Note)
      outgoing_.addEvent(on ? juce::MidiMessage::noteOn(ch, b.source.number, 1.0f)
                            : juce::MidiMessage::noteOff(ch, b.source.number),
                         0);
    else if (b.source.kind == MidiSourceKind::ControlChange)
      outgoing_.addEvent(juce::MidiMessage::controllerEvent(ch, b.source.number, on ? 127 : 0), 0);
  }
}

void MasterpieceProcessor::emitSends(bool sysExOnly) {
  const auto& sends = midiMap_.sends();
  if ((midiOut_ == nullptr && !outgoingProbe) || sends.empty()) return;
  // Sized to the list; a list that changed shape starts over, which sends
  // every lamp's current state -- what a console needs when it is set up.
  if (sentValues_.size() != sends.size()) sentValues_.assign(sends.size(), -1);
  RawMidi raw;
  for (size_t i = 0; i < sends.size(); ++i) {
    const MidiSend& s = sends[i];
    if (sysExOnly && s.kind != MidiSourceKind::SysEx) continue;
    int now;
    if (s.targetKind == MidiTargetKind::Switch)
      now = switches_.engaged(s.targetId) ? 1 : 0;
    // The recorder's lamps: lit while it records, and while it plays.
    else if (s.targetKind == MidiTargetKind::RecorderRecord)
      now = recorder_.isRecording() ? 1 : 0;
    else if (s.targetKind == MidiTargetKind::RecorderPlay)
      now = recorder_.isPlaying() ? 1 : 0;
    else if (s.targetKind == MidiTargetKind::ContinuousControl)
      now = controls_.value(s.targetId);
    else
      continue;
    if (now == sentValues_[i]) continue;
    sentValues_[i] = now;
    if (s.kind == MidiSourceKind::SysEx) {
      // A whole message of its own for each state; a control has none.
      if (s.targetKind == MidiTargetKind::ContinuousControl) continue;
      addMessages(outgoing_, now != 0 ? s.sysExOn : s.sysExOff);
      continue;
    }
    const bool made = s.targetKind != MidiTargetKind::ContinuousControl
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
