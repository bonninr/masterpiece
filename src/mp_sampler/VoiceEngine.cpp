#include "VoiceEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>  // _mm_pause
#endif

namespace mp {
namespace {

// Hermite (Catmull-Rom) interpolation. Organ pipes are held tones with strong
// partials well above the fundamental, and linear interpolation audibly dulls
// them once a rank is transposed by temperament; four-point Hermite costs a
// handful of flops per sample and keeps the reed edge.
inline float hermite(float xm1, float x0, float x1, float x2, float t) {
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b = w + a;
  return ((((a * t) - b) * t + c) * t + x0);
}

// Reading is parameterised on the storage type rather than branching per
// sample: the inner loop runs ~25 million times a second, and a resident
// format test inside it would cost more than the format saves.
//
// Both readers return RAW storage counts. For Int16 the scale is folded into
// the voice gain once per frame instead, which is exact — interpolation is
// linear in its inputs, so scaling the result equals scaling every tap.
template <typename T>
const T* storageOf(const SampleBuffer& buf);
template <>
inline const float* storageOf<float>(const SampleBuffer& buf) {
  return buf.frames.data();
}
template <>
inline const int16_t* storageOf<int16_t>(const SampleBuffer& buf) {
  return buf.pcm16.data();
}
template <>
inline const Pcm24* storageOf<Pcm24>(const SampleBuffer& buf) {
  return buf.pcm24.data();
}

inline float storageScale(const SampleBuffer& buf) {
  return buf.compact() ? buf.pcmScale : 1.0f;
}

template <typename T>
inline float rawSample(const SampleBuffer& buf, int64_t frame, int channel) {
  if (frame < 0 || frame >= buf.numFrames) return 0.0f;
  const int ch = channel < buf.numChannels ? channel : buf.numChannels - 1;
  return static_cast<float>(
      storageOf<T>(buf)[static_cast<size_t>(frame * buf.numChannels + ch)]);
}

template <typename T>
inline float readInterpolated(const SampleBuffer& buf, double pos, int channel) {
  const auto i = static_cast<int64_t>(pos);
  const auto t = static_cast<float>(pos - static_cast<double>(i));
  return hermite(rawSample<T>(buf, i - 1, channel), rawSample<T>(buf, i, channel),
                 rawSample<T>(buf, i + 1, channel),
                 rawSample<T>(buf, i + 2, channel), t);
}

// How long a rank with no release sample takes to fade, in frames. Short
// enough not to smear the room tail, long enough that cutting a voice never
// clicks. Applies ONLY when no release sample matched: a real tail plays to
// its end, and fading it here is what ate every reverb tail in the program.
constexpr double kReleaseFadeSeconds = 0.12;
// How much of a real tail's end is faded, so a file cut dead at its last
// frame still never clicks.
constexpr int64_t kReleaseEndFadeFrames = 256;

// The shortest straight run worth handing to the vector code: below this the
// setup costs more than the frames save.
constexpr int kMinSimdRun = 8;

// A release crossfade is run through the vector code this many frames at a
// time, into scratch of this many channels on the stack.
constexpr int kXfadeChunk = 256;
constexpr int kXfadeChannels = 2;
// A run under a moving tremulant is worked out this many frames at a time.
constexpr int kTremChunk = 256;

} // namespace

void VoiceEngine::prepare(double sampleRate, int maxVoices, int numChannels,
                          int maxBlockFrames) {
  stopWorkers(); // pool sizes depend on everything below
  isa_ = simdWanted_ ? simd::detect() : simd::Isa::None;
  simdFrames_.store(0, std::memory_order_relaxed);
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  numChannels_ = numChannels > 0 ? numChannels : 2;
  maxFrames_ = maxBlockFrames > 0 ? maxBlockFrames : 2048;
  voices_.assign(static_cast<size_t>(std::max(1, maxVoices)), Voice{});
  jobList_.clear();
  jobList_.reserve(voices_.size());
  for (auto& list : busLists_) {
    list.clear();
    list.reserve(voices_.size());
  }
  stealList_.clear();
  stealList_.reserve(voices_.size());
  stealStage_ = 0;
  freeHint_ = 0;
  liveCount_ = 0;
  scanEnd_ = 0;
  blockCounter_ = 0;
  stats_ = EngineStats{};

  // One ring per voice, allocated here and never again: the audio thread must
  // not allocate, and a voice can start streaming at any note-off. Only when
  // something can stream: an organ loaded whole never reads a ring, and the
  // rings were most of the pool's memory.
  stopStreamer();
  const auto ringFrames =
      static_cast<size_t>(std::max(1.0, streamSeconds_ * sampleRate_));
  streams_.clear();
  streams_.reserve(voices_.size());
  for (size_t i = 0; i < voices_.size(); ++i) {
    auto st = std::make_unique<VoiceStream>();
    // Stereo worst case; a mono sample simply uses half of it.
    if (streamRings_) st->ring.assign(ringFrames * 2, 0.0f);
    streams_.push_back(std::move(st));
  }
  underruns_.store(0, std::memory_order_relaxed);
  streaming_.store(0, std::memory_order_relaxed);
  startStreamer();
}

void VoiceEngine::setStreamSeconds(double seconds) {
  // Takes effect at the next prepare(), because the rings are sized there and
  // resizing them under a sounding voice is not something to do at all.
  streamSeconds_ = std::clamp(seconds, 0.25, 30.0);
}

void VoiceEngine::startStreamer() {
  {
    std::lock_guard<std::mutex> lock(streamMutex_);
    streamQuit_ = false;
  }
  streamer_ = std::thread([this] { streamerLoop(); });
}

void VoiceEngine::stopStreamer() {
  {
    std::lock_guard<std::mutex> lock(streamMutex_);
    streamQuit_ = true;
  }
  streamCv_.notify_all();
  if (streamer_.joinable()) streamer_.join();
}

int64_t VoiceEngine::bytesPerVoice(double sampleRate, double streamSeconds, bool rings) {
  const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
  const auto ringFrames = static_cast<int64_t>(std::max(1.0, streamSeconds * rate));
  // The voice, its stream record, and its places in the render and steal
  // lists; the ring, stereo, when there is one.
  return static_cast<int64_t>(sizeof(Voice) + sizeof(VoiceStream) + 2 * sizeof(uint32_t)) +
         (rings ? ringFrames * 2 * static_cast<int64_t>(sizeof(float)) : 0);
}

void VoiceEngine::armStream(size_t voiceIndex, const SampleBuffer& buf,
                            int64_t fromFrame) {
  if (voiceIndex >= streams_.size()) return;
  VoiceStream& st = *streams_[voiceIndex];
  // A pool prepared without rings has nowhere to put a tail: the voice plays
  // what is resident and goes quiet at its end.
  if (st.ring.empty()) return;
  // Bump the generation FIRST: a fill still in flight for the previous voice
  // must not publish its frames into this one's ring.
  st.generation.fetch_add(1, std::memory_order_acq_rel);
  // A slot taken over while its last voice was still streaming was counted
  // twice, and the count is what lets the streamer sleep.
  if (st.armed.exchange(false, std::memory_order_acq_rel))
    streaming_.fetch_sub(1, std::memory_order_relaxed);
  st.filled.store(0, std::memory_order_release);
  st.starved.store(false, std::memory_order_relaxed);
  st.base = fromFrame;
  st.channels = buf.numChannels;
  st.tail = buf.tail;
  st.armed.store(true, std::memory_order_release);
  streaming_.fetch_add(1, std::memory_order_relaxed);
  streamCv_.notify_one();
}

void VoiceEngine::disarmStream(size_t voiceIndex) {
  if (voiceIndex >= streams_.size()) return;
  VoiceStream& st = *streams_[voiceIndex];
  if (!st.armed.exchange(false, std::memory_order_acq_rel)) return;
  st.generation.fetch_add(1, std::memory_order_acq_rel);
  streaming_.fetch_sub(1, std::memory_order_relaxed);
}

float VoiceEngine::streamedSample(size_t voiceIndex, const SampleBuffer& buf,
                                  int64_t frame, int channel) const {
  if (voiceIndex >= streams_.size()) return 0.0f;
  const VoiceStream& st = *streams_[voiceIndex];
  if (!st.armed.load(std::memory_order_acquire)) return 0.0f;
  const int64_t offset = frame - st.base;
  if (offset < 0) return 0.0f;
  // Past the end of the file: the interpolation's last taps reach beyond the
  // final frame of every release. That is the sample's end, and silence;
  // counted as a shortfall, every release that played out added to the
  // count, which reached the hundreds of millions in a session.
  if (frame >= buf.totalFrames()) return 0.0f;
  // Acquire: everything the streamer wrote before publishing `filled` is
  // visible here. This one load is the whole synchronisation.
  const int64_t filled = st.filled.load(std::memory_order_acquire);
  if (offset >= filled) {
    // The disk did not keep up. Silence for this frame, and a count, because
    // a release that goes quiet halfway is exactly the fault a player would
    // hear and have no way to diagnose. Counted once per shortfall: this
    // runs for every frame, channel and interpolation tap.
    if (!st.starved.exchange(true, std::memory_order_relaxed))
      underruns_.fetch_add(1, std::memory_order_relaxed);
    return 0.0f;
  }
  if (st.starved.load(std::memory_order_relaxed))
    st.starved.store(false, std::memory_order_relaxed);
  const int ch = channel < st.channels ? channel : st.channels - 1;
  const auto idx = static_cast<size_t>(offset) * static_cast<size_t>(st.channels) +
                   static_cast<size_t>(ch);
  return idx < st.ring.size() ? st.ring[idx] : 0.0f;
}

void VoiceEngine::streamerLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(streamMutex_);
      // With nothing streaming there is nothing to top up: sleep until a
      // voice is armed. An organ loaded whole never streams, and this thread
      // used to go over every voice a hundred times a second regardless.
      // The audio thread signals without taking the lock, so a signal can
      // fall between the test and the sleep; the timeout bounds what that
      // costs, and the resident head of a sample is far longer.
      if (streaming_.load(std::memory_order_acquire) == 0)
        streamCv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
          return streamQuit_ || streaming_.load(std::memory_order_acquire) > 0;
        });
      // While something streams, often enough to keep every ring topped up.
      // A voice consumes one frame per output frame, so even a full ring
      // lasts seconds; ten milliseconds is generous. The first pass after a
      // sleep goes at once.
      else
        streamCv_.wait_for(lock, std::chrono::milliseconds(10),
                           [this] { return streamQuit_; });
      if (streamQuit_) return;
    }
    if (streaming_.load(std::memory_order_acquire) == 0) continue;
    streamPasses_.fetch_add(1, std::memory_order_relaxed);

    for (size_t i = 0; i < streams_.size(); ++i) {
      VoiceStream& st = *streams_[i];
      if (!st.armed.load(std::memory_order_acquire)) continue;
      const auto tail = st.tail;
      if (!tail || !tail->read) continue;

      const uint64_t gen = st.generation.load(std::memory_order_acquire);
      const int64_t filled = st.filled.load(std::memory_order_acquire);
      const int64_t capacity =
          static_cast<int64_t>(st.ring.size() / static_cast<size_t>(std::max(1, st.channels)));
      if (filled >= capacity) continue; // full
      const int64_t remaining = tail->totalFrames - (st.base + filled);
      if (remaining <= 0) continue; // end of the file

      // A chunk at a time, so one slow read cannot starve every other voice.
      constexpr int64_t kChunk = 16384;
      const int64_t want = std::min({kChunk, capacity - filled, remaining});
      float* dest = st.ring.data() +
                    static_cast<size_t>(filled) * static_cast<size_t>(st.channels);
      const int64_t got = tail->read(st.base + filled, static_cast<int>(want),
                                     dest, st.channels);
      if (got <= 0) continue;

      // Publish only if this slot is still the same voice. Re-armed underneath
      // us means the frames just written belong to nobody, and the new voice's
      // own fill will overwrite them.
      if (st.generation.load(std::memory_order_acquire) != gen) continue;
      st.filled.store(filled + got, std::memory_order_release);
    }
  }
}

VoiceEngine::~VoiceEngine() {
  stopWorkers();
  stopStreamer();
}

void VoiceEngine::quiesce() {
  stopWorkers();
  stopStreamer();
  reset();
}

void VoiceEngine::reset() {
  for (size_t i = 0; i < voices_.size(); ++i) disarmStream(i);
  for (auto& v : voices_) v = Voice{};
  stats_ = EngineStats{};
  freeHint_ = 0;
  liveCount_ = 0;
  scanEnd_ = 0;
  stealStage_ = 0;
}

int VoiceEngine::activeVoiceCount() const {
  int n = 0;
  for (size_t i = 0; i < scanEnd_; ++i)
    if (voices_[i].active()) ++n;
  return n;
}

int VoiceEngine::allocateVoice() {
  // 1. A free slot is always best, while the player's limit leaves one.
  //    Every slot below the hint was in use when render() last looked, so the
  //    search starts there: a chord of six hundred pipes used to walk the
  //    pool from its first slot for each of them.
  const int limit = polyphony_ > 0 ? std::min(polyphony_, static_cast<int>(voices_.size()))
                                   : static_cast<int>(voices_.size());
  if (liveCount_ < limit) {
    const size_t n = voices_.size();
    for (size_t k = 0; k < n; ++k) {
      const size_t i = freeHint_ + k < n ? freeHint_ + k : freeHint_ + k - n;
      if (voices_[i].active()) continue;
      freeHint_ = i + 1 < n ? i + 1 : 0;
      ++liveCount_;
      if (i + 1 > scanEnd_) scanEnd_ = i + 1;
      return static_cast<int>(i);
    }
  }

  // 2. Otherwise steal, and steal musically. A voice already releasing is the
  //    cheapest thing to lose — it is decaying anyway — so take the oldest of
  //    those before touching anything still speaking.
  // 3. Nothing releasing: take the oldest quietest sounding voice. Never the
  //    newest — stealing the note the player just pressed is the one failure
  //    they always hear.
  //
  // The order is worked out once and handed out from the front: a chord that
  // arrives with the pool full steals hundreds of voices in one block, and
  // looking over the whole pool twice for each was the dearest moment the
  // engine had, at the moment it had least to spare.
  for (;;) {
    if (stealStage_ == 0 || stealBlock_ != blockCounter_) {
      stealList_.clear();
      for (size_t i = 0; i < voices_.size(); ++i)
        if (voices_[i].phase == VoicePhase::Release) stealList_.push_back(static_cast<uint32_t>(i));
      std::sort(stealList_.begin(), stealList_.end(), [this](uint32_t a, uint32_t b) {
        const Voice& x = voices_[a];
        const Voice& y = voices_[b];
        if (x.startedAtBlock != y.startedAtBlock) return x.startedAtBlock < y.startedAtBlock;
        return a < b;
      });
      stealStage_ = 1;
      stealNext_ = 0;
      stealBlock_ = blockCounter_;
    }
    while (stealNext_ < stealList_.size()) {
      const uint32_t i = stealList_[stealNext_++];
      const Voice& v = voices_[i];
      // Still what it was when the order was made: a slot already taken from
      // the list is a new voice now.
      const bool takeable = stealStage_ == 1 ? v.phase == VoicePhase::Release
                                             : v.active() && v.startedAtBlock != blockCounter_;
      if (!takeable) continue;
      ++stats_.stolenVoices;
      return static_cast<int>(i);
    }
    if (stealStage_ != 1) return -1;
    stealList_.clear();
    for (size_t i = 0; i < voices_.size(); ++i) {
      const Voice& v = voices_[i];
      if (!v.active() || v.startedAtBlock == blockCounter_) continue; // started this block
      stealList_.push_back(static_cast<uint32_t>(i));
    }
    std::sort(stealList_.begin(), stealList_.end(), [this](uint32_t a, uint32_t b) {
      const Voice& x = voices_[a];
      const Voice& y = voices_[b];
      if (x.gain != y.gain) return x.gain < y.gain;
      if (x.startedAtBlock != y.startedAtBlock) return x.startedAtBlock < y.startedAtBlock;
      return a < b;
    });
    stealStage_ = 2;
    stealNext_ = 0;
  }
}

int VoiceEngine::startVoice(const VoiceStart& start, uint64_t noteId) {
  if (start.pipe == nullptr || start.layer == nullptr) return -1;
  if (start.attackIndex < 0 ||
      start.attackIndex >= static_cast<int>(start.layer->attacks.size()))
    return -1;

  const AttackSample& attack =
      start.layer->attacks[static_cast<size_t>(start.attackIndex)];

  const SampleBuffer* buf =
      provider_ ? provider_(attack.sample.sampleId) : nullptr;
  if (buf == nullptr || buf->numFrames <= 0) {
    // Not resident: stay silent rather than block the audio thread on disk.
    ++stats_.samplesMissing;
    return -1;
  }

  const int slot = allocateVoice();
  if (slot < 0) {
    ++stats_.startsDropped;
    return -1;
  }

  Voice& v = voices_[static_cast<size_t>(slot)];
  v = Voice{};
  v.phase = VoicePhase::Attack;
  v.buffer = buf;
  v.pipeId = start.pipe->pipeId;
  v.sampleId = attack.sample.sampleId;
  v.noteId = noteId;
  v.attackId = start.attackId != 0 ? start.attackId : attack.id;
  v.attackIndex = start.attackIndex;
  v.attackVelocity = start.velocity;
  v.cursor = 0.0;
  // The file may have been recorded at a different rate than we are running at;
  // fold that into the same cursor step as the temperament ratio.
  v.ratio = start.ratio * (buf->sampleRate / sampleRate_);
  if (!(v.ratio > 0.0)) v.ratio = 1.0;
  v.gain = start.gain;
  v.releaseGain = 1.0f;
  v.layer = start.layer;
  v.busIndex = start.busIndex;
  v.mixBus = start.mixBus;
  v.windIndex = start.windIndex;
  v.delayFrames = start.startOffset > 0 ? start.startOffset : 0;
  v.windFlow = start.windFlowKgPerSec;
  {
    // A release started as a voice of its own begins from the chest as it
    // is now, and keeps that; a sustaining one follows it from here on.
    const WindMod w = windModFor(v.windIndex);
    v.heldWindAmp = w.ampMul;
    v.heldWindPitch = w.pitchRatio;
  }
  v.tremIndex = start.tremIndex;
  v.tremAmpDepth = start.tremAmpDepth;
  v.tremPitchDepth = start.tremPitchDepth;
  v.startedAtBlock = blockCounter_;
  v.startedAtFrame = framesRendered_;
  // If only this sample's head is resident, tell the streamer to start
  // fetching the rest. It has from now until the voice reaches the end of the
  // head — seconds — which is why a two second ring is ample.
  if (buf->streams()) armStream(static_cast<size_t>(slot), *buf, buf->numFrames);
  else disarmStream(static_cast<size_t>(slot));

  // The ODF wins over the file when it declares a loop, because the sample set
  // author edited the ODF to fix a bad or missing chunk. A declared loop that
  // does not fit the resident audio is ignored rather than clamped — a clamped
  // loop invents a pitch, which is worse than a note that stops.
  v.xfadeLength = start.oneShot ? 0 : std::max(0, start.loopCrossfadeFrames);
  v.xfadeRemaining = 0;
  v.loopStart = start.oneShot ? -1 : buf->loopStart;
  v.loopEnd = start.oneShot ? -1 : buf->loopEnd;
  if (!start.oneShot && start.loopStartOverride >= 0 &&
      start.loopEndOverride > start.loopStartOverride &&
      start.loopEndOverride <= buf->numFrames) {
    v.loopStart = start.loopStartOverride;
    v.loopEnd = start.loopEndOverride;
  }
  return slot;
}

int64_t VoiceEngine::msSincePipeClosed(Id pipeId) const {
  const auto it = pipeClosedAt_.find(pipeId);
  if (it == pipeClosedAt_.end() || sampleRate_ <= 0.0) return INT64_MAX;
  return static_cast<int64_t>(static_cast<double>(framesRendered_ - it->second) * 1000.0 /
                              sampleRate_);
}

void VoiceEngine::noteOffPipe(uint64_t noteId, Id pipeId,
                              const NoteRelease& release) {
  releaseVoices(noteId, pipeId, release);
}

void VoiceEngine::noteOff(uint64_t noteId, const NoteRelease& release) {
  releaseVoices(noteId, 0, release);
}

// pipeId 0 means every pipe of the note; otherwise only that one.
void VoiceEngine::releaseVoices(uint64_t noteId, Id pipeId,
                                const NoteRelease& release) {
  for (size_t slot = 0; slot < scanEnd_; ++slot) {
    Voice& v = voices_[slot];
    if (!v.active() || v.noteId != noteId) continue;
    if (pipeId != 0 && v.pipeId != pipeId) continue;
    if (v.phase == VoicePhase::Release) continue;
    stealStage_ = 0;  // the voices to take first have changed
    if (v.pipeId != 0) pipeClosedAt_[v.pipeId] = framesRendered_;

    // Pick the release sample that matches the attack this voice actually
    // played — that pairing is what makes a release sound like the same pipe.
    // How long the key was held, from when this voice started. Nothing
    // measured it before, so every release was chosen as if the key had been
    // held for no time at all: Nancy's held notes took the release written
    // for short ones, whose tail is shorter (#120). A caller that knows the
    // hold time better passes it.
    int64_t heldMs = release.holdTimeMs;
    if (heldMs <= 0 && sampleRate_ > 0.0)
      heldMs = static_cast<int64_t>(
          static_cast<double>(framesRendered_ - v.startedAtFrame) * 1000.0 / sampleRate_);
    int chosen = -1;
    if (v.layer != nullptr) {
      NoteRelease rel = release;
      rel.holdTimeMs = heldMs;
      rel.attackIndex = v.attackIndex;
      rel.attackId = v.attackId;
      rel.attackVelocity = v.attackVelocity;
      chosen = selectRelease(*v.layer, rel);
    }

    const SampleBuffer* relBuf = nullptr;
    Id relSampleId = 0;
    double xfadeMs = 0.0;
    if (chosen >= 0 && v.layer != nullptr) {
      const ReleaseSample& r = v.layer->releases[static_cast<size_t>(chosen)];
      relSampleId = r.sample.sampleId;
      xfadeMs = r.releaseCrossfadeMs;
      if (provider_) relBuf = provider_(relSampleId);
    }

    // The attack being left: it keeps sounding through the crossfade, so its
    // buffer, cursor and loop have to survive the swap below.
    const bool logging = logReleases_.load(std::memory_order_acquire);
    ReleaseEvent ev;
    if (logging) {
      ev.pipeId = v.pipeId;
      ev.attackSampleId = v.sampleId;
      ev.heldMs = heldMs;
      ev.velocity = release.velocity;
      ev.releaseCount = v.layer != nullptr ? static_cast<int>(v.layer->releases.size()) : 0;
      ev.chosen = chosen;
      ev.attackFrame = static_cast<int64_t>(v.cursor);
      ev.attackRate = v.buffer != nullptr ? v.buffer->sampleRate : 0.0;
      ev.attackLooping = v.loops();
      ev.crossfadeMs = xfadeMs;
    }
    const SampleBuffer* oldBuf = v.buffer;
    const double oldCursor = v.cursor;
    const int64_t oldLoopStart = v.loopStart;
    const int64_t oldLoopEnd = v.loopEnd;

    v.phase = VoicePhase::Release;
    v.releaseIsSample = (relBuf != nullptr && relBuf->numFrames > 0);
    if (v.releaseIsSample) {
      v.xfadeBuf = oldBuf;
      v.xfadeOldCursor = oldCursor;
      v.xfadeOldLoopStart = oldLoopStart;
      v.xfadeOldLoopEnd = oldLoopEnd;
      v.relXfadeLength = static_cast<int>(std::max(0.0, xfadeMs) * sampleRate_ / 1000.0);
      v.relXfadeLeft = v.relXfadeLength;
      // The render loop ramps this 0 -> 1 over the crossfade; a zero-length
      // crossfade swaps instantly, as authored.
      v.releaseGain = v.relXfadeLength > 0 ? 0.0f : 1.0f;
    } else {
      v.releaseGain = 1.0f;
      v.xfadeBuf = nullptr;
      v.relXfadeLength = 0;
      v.relXfadeLeft = 0;
    }
    if (relBuf != nullptr && relBuf->numFrames > 0) {
      // A real release sample: swap the backing store and play from its head.
      // The playback ratio carries over unchanged — the release is the same
      // pipe at the same pitch, so it must be resampled identically.
      // At its own pitch. The ratio so far tunes the ATTACK's recording to the
      // pipe; a release is its own file, recorded -- or measured -- at a pitch
      // and a rate of its own, and played at the attack's speed it sounded
      // off by the difference (#90: the release "at a different pitch" on a
      // Tromba and a Trumpet). Rescaled by the two recorded pitches and rates;
      // the attack, fading out under it, keeps its speed.
      double scale = 1.0;
      const auto& att = v.layer->attacks;
      const double attackHz =
          v.attackIndex >= 0 && static_cast<size_t>(v.attackIndex) < att.size()
              ? att[static_cast<size_t>(v.attackIndex)].sample.resolvedPitchHz
              : 0.0;
      const double releaseHz = v.layer->releases[static_cast<size_t>(chosen)].sample.resolvedPitchHz;
      if (attackHz > 0.0 && releaseHz > 0.0) scale *= attackHz / releaseHz;
      if (oldBuf != nullptr && oldBuf->sampleRate > 0.0 && relBuf->sampleRate > 0.0)
        scale *= relBuf->sampleRate / oldBuf->sampleRate;
      if (scale > 0.0 && std::isfinite(scale)) {
        v.ratio *= scale;
        v.xfadeRatioScale = 1.0 / scale;
      }
      v.buffer = relBuf;
      v.sampleId = relSampleId;
      // ...unless the set ships attack, loop and release as ONE recording and
      // points the release at a marker inside it. Starting at the head then
      // replays the attack and the whole sustain: the note goes on sounding
      // for as long as the original took to reach its release, and a piece
      // piles up note on note. The organ says so in the release's load range;
      // the file says where, with a cue point.
      const ReleaseSample& row = v.layer->releases[static_cast<size_t>(chosen)];
      const bool fromMarker = row.loadStartValue > 0 || row.loadStartType > 0;
      // With no marker, the release is what follows the loop, as GrandOrgue
      // reads a pipe's own file; a file with nothing there was left out of
      // the choice at load (ReleaseSample::empty).
      v.cursor = !fromMarker ? 0.0
                 : relBuf->releaseCue > 0 ? static_cast<double>(relBuf->releaseCue)
                 : relBuf->loopEnd > 0 && relBuf->loopEnd < relBuf->totalFrames()
                     ? static_cast<double>(relBuf->loopEnd)
                     : 0.0;
      // A release is a one-shot: it must not inherit the attack's loop.
      v.loopStart = -1;
      v.loopEnd = -1;
      // The voice has swapped to a different sample, so its ring has to follow.
      // Releases are the samples worth streaming: several seconds each, played
      // once, straight through, and never looped.
      const size_t slot = static_cast<size_t>(&v - voices_.data());
      if (relBuf->streams()) armStream(slot, *relBuf, relBuf->numFrames);
      else disarmStream(slot);
    }
    // No matching release sample: the voice fades from wherever it is, which
    // is what a rank without releases should do rather than cutting abruptly.
    if (logging) {
      if (v.releaseIsSample) {
        const ReleaseSample& row = v.layer->releases[static_cast<size_t>(chosen)];
        ev.releaseSampleId = relSampleId;
        ev.releaseFrames = relBuf->totalFrames();
        ev.releaseRate = relBuf->sampleRate;
        ev.startFrame = static_cast<int64_t>(v.cursor);
        ev.cueWanted = row.loadStartValue > 0 || row.loadStartType > 0;
        ev.cueFound = relBuf->releaseCue > 0;
        ev.streamed = relBuf->streams();
        ev.residentFrames = relBuf->numFrames;
      }
      const size_t head = releaseHead_.load(std::memory_order_relaxed);
      if (head - releaseTail_.load(std::memory_order_acquire) >= kReleaseRing) {
        releaseDropped_.fetch_add(1, std::memory_order_relaxed);
      } else {
        releaseRing_[head % kReleaseRing] = ev;
        releaseHead_.store(head + 1, std::memory_order_release);
      }
    }
  }
}

int64_t VoiceEngine::takeReleaseEvents(std::vector<ReleaseEvent>& out) {
  const size_t head = releaseHead_.load(std::memory_order_acquire);
  size_t tail = releaseTail_.load(std::memory_order_relaxed);
  for (; tail != head; ++tail) out.push_back(releaseRing_[tail % kReleaseRing]);
  releaseTail_.store(tail, std::memory_order_release);
  return releaseDropped_.exchange(0, std::memory_order_relaxed);
}

// Interior fast path: all four Hermite taps are in range, so the per-tap
// bounds checks and channel clamp in SampleBuffer::sample() are dead weight.
// This is the whole inner loop of the engine — at 2000 voices it runs ~25
// million times a second — so hoisting the checks out of it is worth more
// than vectorising the arithmetic, which is gather-bound rather than
// compute-bound.
namespace {
template <typename T>
inline float readInterior(const T* frames, int channels, int channel, int64_t i,
                          float t) {
  const T* p = frames + (i - 1) * channels + channel;
  return hermite(static_cast<float>(p[0]), static_cast<float>(p[channels]),
                 static_cast<float>(p[2 * channels]),
                 static_cast<float>(p[3 * channels]), t);
}
} // namespace

// The resident format is decided once per voice per block, here, so nothing
// below it pays for the choice.
void VoiceEngine::renderVoice(Voice& v, float* const* out, int numChannels,
                              int numFrames) {
  const size_t index = static_cast<size_t>(&v - voices_.data());
  // A voice started part-way into the block begins there: what comes before
  // its note-on stays as it was.
  if (v.delayFrames > 0) {
    const int skip = std::min(v.delayFrames, numFrames);
    v.delayFrames -= skip;
    if (skip >= numFrames) return;
    constexpr int kMaxChannels = 16;
    float* shifted[kMaxChannels];
    const int channels = std::min(numChannels, kMaxChannels);
    for (int c = 0; c < channels; ++c) shifted[c] = out[c] + skip;
    renderVoice(v, shifted, channels, numFrames - skip);
    return;
  }
  if (v.buffer != nullptr && v.buffer->wide())
    renderVoiceFrom<Pcm24>(v, index, out, numChannels, numFrames);
  else if (v.buffer != nullptr && v.buffer->compact())
    renderVoiceFrom<int16_t>(v, index, out, numChannels, numFrames);
  else
    renderVoiceFrom<float>(v, index, out, numChannels, numFrames);
}

template <typename T>
void VoiceEngine::renderVoiceFrom(Voice& v, size_t voiceIndex,
                                  float* const* out, int numChannels,
                                  int numFrames) {
  const SampleBuffer& buf = *v.buffer;
  // The resident head, and the whole file. They are the same length unless
  // this sample streams, which is the only difference between the two modes:
  // per ADR-004 the voice path below does not otherwise know or care.
  const int64_t resident = buf.numFrames;
  const int64_t total = buf.totalFrames();
  const bool streaming = total > resident;
  const double fadeStep =
      1.0 / std::max(1.0, kReleaseFadeSeconds * sampleRate_);
  const T* const frames = storageOf<T>(buf);
  const int bufChannels = buf.numChannels;
  // The wind, read once per voice per block rather than per sample: the
  // pressure moves on the order of tenths of a second and the inner loop runs
  // twenty-five million times a second.
  WindMod wind = windModFor(v.windIndex);
  if (v.phase == VoicePhase::Release) {
    wind.ampMul = v.heldWindAmp;
    wind.pitchRatio = v.heldWindPitch;
  } else {
    v.heldWindAmp = wind.ampMul;
    v.heldWindPitch = wind.pitchRatio;
  }
  const double windRatio = v.ratio * wind.pitchRatio;
  // The tremulant, as a ramp across the block. Its own value is a signed
  // swing in -1..1; this pipe's depths turn that into a gain and a pitch.
  const TremMod trem = tremModFor(v.tremIndex);
  float tremPhase = trem.ampStart;
  // Whether the tremulant moves anything in this block. A pipe that stands
  // under a tremulant is under it whether it is drawn or not, and most pipes
  // of most organs do: with the tremulant at rest its swing is zero from one
  // end of the block to the other, and the voice sounds exactly as one with
  // no tremulant. Treating it as one keeps it on the straight runs and spares
  // a pow() per frame. Before this, every such voice took the per-frame path
  // for as long as it sounded.
  const bool tremMoving = v.tremIndex >= 0 &&
                          (trem.ampStart != 0.0f || trem.ampStep != 0.0f ||
                           trem.pitchStart != 0.0 || trem.pitchStep != 0.0);
  // The pitch the tremulant gives this pipe, as a ratio that is multiplied on
  // from one frame to the next. Its swing is a straight line across the block
  // in semitones, which is a geometric one in ratio: two pow() for the block
  // give every frame's ratio exactly, where one pow() per frame gave it at an
  // eighth of the engine's time with a tremulant running.
  double tremRatio = 1.0;
  double tremRatioStep = 1.0;
  if (tremMoving) {
    tremRatio = std::pow(2.0, v.tremPitchDepth * trem.pitchStart / 12.0);
    tremRatioStep = std::pow(2.0, v.tremPitchDepth * trem.pitchStep / 12.0);
  }
  // Folded in once rather than applied per tap. For Float32 this is 1.0f and
  // the multiply below is the one the gain needed anyway.
  const float storeScale = storageScale(buf);
  // Whether this voice can have straight runs at all this block, and whether
  // they are the plain kind. A tremulant in motion moves the pitch and the
  // level every frame; its runs are given both frame by frame.
  const bool canRun = isa_ != simd::Isa::None && simd::runSupports(isa_, bufChannels) &&
                      windRatio > 0.0;
  const bool runnable = canRun && !tremMoving;
  const int64_t runEnd = resident - simd::kRunMargin;

  for (int i = 0; i < numFrames; ++i) {
    if (v.phase == VoicePhase::Idle) return;

    // A release crossfade, as two straight runs. For the length the release
    // names -- tens of milliseconds -- the tail fades in over the attack fading
    // out, and both are being read well inside their samples. Every voice of a
    // chord does this at once when the keys are let go, a frame at a time it
    // was the most expensive thing the engine did: a full organ's note-off
    // took two blocks' time on one core. Each side goes to the vector code at
    // full level into a small scratch, and the two are mixed under the ramp.
    if (runnable && numChannels <= kXfadeChannels && v.phase == VoicePhase::Release &&
        v.releaseIsSample && v.xfadeBuf != nullptr && v.relXfadeLeft > 0 &&
        v.relXfadeLength > 0 && v.xfadeRemaining <= 0) {
      const SampleBuffer& ob = *v.xfadeBuf;
      const double oldRatio = windRatio * v.xfadeRatioScale;
      const bool oldLoops = v.xfadeOldLoopEnd > v.xfadeOldLoopStart && v.xfadeOldLoopStart >= 0;
      // Short of each side's own seam, which the per-frame path reads.
      const double newLimit =
          static_cast<double>(std::min(runEnd, total - kReleaseEndFadeFrames));
      double oldLimit = static_cast<double>(ob.numFrames - simd::kRunMargin);
      if (oldLoops) oldLimit = std::min(oldLimit, static_cast<double>(v.xfadeOldLoopEnd - 3));
      if (ob.wide() == buf.wide() && ob.compact() == buf.compact() && oldRatio > 0.0 &&
          simd::runSupports(isa_, ob.numChannels) && v.cursor >= 1.0 && v.cursor < newLimit &&
          v.xfadeOldCursor >= 1.0 && v.xfadeOldCursor < oldLimit) {
        int n = std::min({numFrames - i, v.relXfadeLeft, kXfadeChunk});
        const double newRoom = (newLimit - v.cursor) / windRatio;
        const double oldRoom = (oldLimit - v.xfadeOldCursor) / oldRatio;
        if (newRoom < static_cast<double>(n) + 1.0) n = static_cast<int>(newRoom) - 1;
        if (oldRoom < static_cast<double>(n) + 1.0) n = static_cast<int>(oldRoom) - 1;
        if (n >= kMinSimdRun) {
          float tail[kXfadeChannels][kXfadeChunk];
          float attack[kXfadeChannels][kXfadeChunk];
          float* tailPlanes[kXfadeChannels];
          float* attackPlanes[kXfadeChannels];
          for (int ch = 0; ch < numChannels; ++ch) {
            std::fill(tail[ch], tail[ch] + n, 0.0f);
            std::fill(attack[ch], attack[ch] + n, 0.0f);
            tailPlanes[ch] = tail[ch];
            attackPlanes[ch] = attack[ch];
          }
          // In storage counts, at a level of one, which changes nothing: the
          // levels go on below exactly as the per-frame path puts them on.
          v.cursor = simd::renderRun(isa_, frames, bufChannels, v.cursor, windRatio, 1.0f,
                                     tailPlanes, numChannels, 0, n);
          v.xfadeOldCursor =
              simd::renderRun(isa_, storageOf<T>(ob), ob.numChannels, v.xfadeOldCursor, oldRatio,
                              1.0f, attackPlanes, numChannels, 0, n);
          // The same ramp the per-frame path walks, in the same arithmetic and
          // the same order, so the two give the same samples: the tail's share
          // on each frame is how far through the crossfade that frame is.
          const float level = v.gain * wind.ampMul;
          const float oldScale = storageScale(ob);
          const float length = static_cast<float>(v.relXfadeLength);
          const int left = v.relXfadeLeft;
          for (int ch = 0; ch < numChannels; ++ch) {
            float* dst = out[ch] + i;
            const float* a = attack[ch];
            const float* r = tail[ch];
            for (int k = 0; k < n; ++k) {
              const float t = 1.0f - static_cast<float>(left - k) / length;
              dst[k] += r[k] * ((level * t) * storeScale);
              dst[k] += a[k] * (level * oldScale * (1.0f - t));
            }
          }
          v.releaseGain = 1.0f - static_cast<float>(left - (n - 1)) / length;
          v.relXfadeLeft = left - n;
          if (v.relXfadeLeft <= 0) v.xfadeBuf = nullptr;
          simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
          i += n - 1;
          continue;
        }
      }
    }

    // The same crossfade under a tremulant in motion. Both sides move at the
    // tremulant's pitch and sound at its level, frame by frame, so each
    // frame's step and level are worked out first, as the per-frame path
    // works them out, and both sides go to the vector code with them.
    if (canRun && tremMoving && numChannels <= kXfadeChannels &&
        v.phase == VoicePhase::Release && v.releaseIsSample && v.xfadeBuf != nullptr &&
        v.relXfadeLeft > 0 && v.relXfadeLength > 0 && v.xfadeRemaining <= 0) {
      const SampleBuffer& ob = *v.xfadeBuf;
      const bool oldLoops = v.xfadeOldLoopEnd > v.xfadeOldLoopStart && v.xfadeOldLoopStart >= 0;
      const double newLimit =
          static_cast<double>(std::min(runEnd, total - kReleaseEndFadeFrames));
      double oldLimit = static_cast<double>(ob.numFrames - simd::kRunMargin);
      if (oldLoops) oldLimit = std::min(oldLimit, static_cast<double>(v.xfadeOldLoopEnd - 3));
      if (ob.wide() == buf.wide() && ob.compact() == buf.compact() && v.xfadeRatioScale > 0.0 &&
          simd::runSupports(isa_, ob.numChannels) && v.cursor >= 1.0 && v.cursor < newLimit &&
          v.xfadeOldCursor >= 1.0 && v.xfadeOldCursor < oldLimit) {
        double ratios[kXfadeChunk];
        double oldRatios[kXfadeChunk];
        float levels[kXfadeChunk];
        const int want = std::min({numFrames - i, v.relXfadeLeft, kXfadeChunk});
        const float base = v.gain * wind.ampMul;
        double p = v.cursor;
        double q = v.xfadeOldCursor;
        float phase = tremPhase;
        double swing = tremRatio;
        int n = 0;
        // Every frame of the run reads short of both limits.
        for (; n < want && p < newLimit && q < oldLimit; ++n) {
          float e = base;
          e *= 1.0f + v.tremAmpDepth * phase;
          if (e < 0.0f) e = 0.0f;
          levels[n] = e;
          ratios[n] = windRatio * swing;
          oldRatios[n] = ratios[n] * v.xfadeRatioScale;
          p += ratios[n];
          q += oldRatios[n];
          phase += trem.ampStep;
          swing *= tremRatioStep;
        }
        if (n >= kMinSimdRun) {
          static const std::array<float, kXfadeChunk> ones = [] {
            std::array<float, kXfadeChunk> a;
            a.fill(1.0f);
            return a;
          }();
          float tail[kXfadeChannels][kXfadeChunk];
          float attack[kXfadeChannels][kXfadeChunk];
          float* tailPlanes[kXfadeChannels];
          float* attackPlanes[kXfadeChannels];
          for (int ch = 0; ch < numChannels; ++ch) {
            std::fill(tail[ch], tail[ch] + n, 0.0f);
            std::fill(attack[ch], attack[ch] + n, 0.0f);
            tailPlanes[ch] = tail[ch];
            attackPlanes[ch] = attack[ch];
          }
          v.cursor = simd::renderRunPerFrame(isa_, frames, bufChannels, v.cursor, ratios,
                                             ones.data(), tailPlanes, numChannels, 0, n);
          v.xfadeOldCursor =
              simd::renderRunPerFrame(isa_, storageOf<T>(ob), ob.numChannels, v.xfadeOldCursor,
                                      oldRatios, ones.data(), attackPlanes, numChannels, 0, n);
          const float oldScale = storageScale(ob);
          const float length = static_cast<float>(v.relXfadeLength);
          const int left = v.relXfadeLeft;
          for (int ch = 0; ch < numChannels; ++ch) {
            float* dst = out[ch] + i;
            const float* a = attack[ch];
            const float* r = tail[ch];
            for (int k = 0; k < n; ++k) {
              const float t = 1.0f - static_cast<float>(left - k) / length;
              dst[k] += r[k] * ((levels[k] * t) * storeScale);
              dst[k] += a[k] * (levels[k] * oldScale * (1.0f - t));
            }
          }
          v.releaseGain = 1.0f - static_cast<float>(left - (n - 1)) / length;
          v.relXfadeLeft = left - n;
          if (v.relXfadeLeft <= 0) v.xfadeBuf = nullptr;
          tremPhase = phase;
          tremRatio = swing;
          simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
          i += n - 1;
          continue;
        }
      }
    }

    // A straight run: nothing but interpolating and mixing until the cursor
    // reaches the loop end, the end of the resident head, or the fade at the
    // end of a release. Those frames go to the vector code at once and the
    // per-frame path picks up from where the run stops -- which is why the
    // run always ends short of anything that would need it.
    if (runnable && v.xfadeRemaining <= 0 && v.xfadeBuf == nullptr) {
      const bool release = v.phase == VoicePhase::Release;
      if (!release || v.releaseIsSample) {
        double limit = static_cast<double>(runEnd);
        // Short of the loop's seam, which the per-frame path reads (atSeam).
        if (!release && v.loops()) limit = std::min(limit, static_cast<double>(v.loopEnd - 3));
        if (release)
          limit = std::min(limit, static_cast<double>(total - kReleaseEndFadeFrames));
        if (v.cursor >= 1.0 && v.cursor < limit) {
          // One frame of slack: the positions are summed a frame at a time
          // and must not cross the limit on the last of them.
          const double room = (limit - v.cursor) / windRatio;
          int n = numFrames - i;
          if (room < static_cast<double>(n) + 1.0) n = static_cast<int>(room) - 1;
          if (n >= kMinSimdRun) {
            if (release) {
              // What the per-frame path does on every frame of a release
              // sample that is past its crossfade.
              v.releaseGain = 1.0f;
              v.relXfadeLeft = 0;
            }
            float envReal = v.gain * wind.ampMul;
            if (release) envReal *= v.releaseGain;
            const float env = envReal * storeScale;
            v.cursor = simd::renderRun(isa_, frames, bufChannels, v.cursor, windRatio, env, out,
                                       numChannels, i, n);
            simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            i += n - 1;
            continue;
          }
        }
      }
    }

    // The same straight run under a tremulant in motion: every frame has its
    // own step and its own level, worked out here as the per-frame path works
    // them out, and the vector code is given both.
    if (canRun && tremMoving && v.xfadeRemaining <= 0 && v.xfadeBuf == nullptr) {
      const bool release = v.phase == VoicePhase::Release;
      if (!release || v.releaseIsSample) {
        double limit = static_cast<double>(runEnd);
        if (!release && v.loops()) limit = std::min(limit, static_cast<double>(v.loopEnd - 3));
        if (release)
          limit = std::min(limit, static_cast<double>(total - kReleaseEndFadeFrames));
        if (v.cursor >= 1.0 && v.cursor < limit) {
          double ratios[kTremChunk];
          float envs[kTremChunk];
          const int want = std::min(numFrames - i, kTremChunk);
          const float base = v.gain * wind.ampMul;
          double p = v.cursor;
          float phase = tremPhase;
          double swing = tremRatio;
          int n = 0;
          // Every frame of the run reads short of the limit.
          for (; n < want && p < limit; ++n) {
            float e = base;
            e *= 1.0f + v.tremAmpDepth * phase;
            if (e < 0.0f) e = 0.0f;
            envs[n] = e * storeScale;
            ratios[n] = windRatio * swing;
            p += ratios[n];
            phase += trem.ampStep;
            swing *= tremRatioStep;
          }
          if (n >= kMinSimdRun) {
            if (release) {
              v.releaseGain = 1.0f;
              v.relXfadeLeft = 0;
            }
            v.cursor = simd::renderRunPerFrame(isa_, frames, bufChannels, v.cursor, ratios, envs,
                                               out, numChannels, i, n);
            tremPhase = phase;
            tremRatio = swing;
            simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            i += n - 1;
            continue;
          }
        }
      }
    }

    // A streamed release tail, as straight runs over the voice's ring. Past
    // its resident start a release is read from the ring the streamer fills,
    // and every tap of every frame went through streamedSample() and its
    // checks. In a fast passage nearly every voice is such a tail a moment
    // after its note: on a two-core computer, half of the program's time.
    // The ring is plain samples in order, so the vector code reads it as it
    // reads a resident sample, up to what the streamer has published. What
    // lies past that, and the fade at the file's end, stay frame by frame.
    if (canRun && streaming && v.phase == VoicePhase::Release && v.releaseIsSample &&
        v.xfadeRemaining <= 0 && v.xfadeBuf == nullptr && voiceIndex < streams_.size()) {
      const VoiceStream& st = *streams_[voiceIndex];
      if (st.armed.load(std::memory_order_acquire) && !st.ring.empty() &&
          simd::runSupports(isa_, st.channels)) {
        const int64_t filled = st.filled.load(std::memory_order_acquire);
        const double base = static_cast<double>(st.base);
        // Every tap of every frame inside what is published, and short of
        // the fade at the end of the file.
        const double limit = std::min(base + static_cast<double>(filled - simd::kRunMargin),
                                      static_cast<double>(total - kReleaseEndFadeFrames));
        if (v.cursor >= base + 1.0 && v.cursor < limit) {
          const float* ring = st.ring.data();
          // Read in time: a shortfall counted before is over.
          if (st.starved.load(std::memory_order_relaxed)) st.starved.store(false, std::memory_order_relaxed);
          if (!tremMoving) {
            const double room = (limit - v.cursor) / windRatio;
            int n = numFrames - i;
            if (room < static_cast<double>(n) + 1.0) n = static_cast<int>(room) - 1;
            if (n >= kMinSimdRun) {
              v.releaseGain = 1.0f;
              v.relXfadeLeft = 0;
              // The ring is in real units: the level goes on as it is, with
              // no storage scale, as the per-frame path does for it.
              const float env = v.gain * wind.ampMul * v.releaseGain;
              v.cursor = base + simd::renderRun(isa_, ring, st.channels, v.cursor - base,
                                                windRatio, env, out, numChannels, i, n);
              simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
              i += n - 1;
              continue;
            }
          } else {
            double ratios[kTremChunk];
            float envs[kTremChunk];
            const int want = std::min(numFrames - i, kTremChunk);
            const float level = v.gain * wind.ampMul;
            double p = v.cursor;
            float phase = tremPhase;
            double swing = tremRatio;
            int n = 0;
            for (; n < want && p < limit; ++n) {
              float e = level;
              e *= 1.0f + v.tremAmpDepth * phase;
              if (e < 0.0f) e = 0.0f;
              envs[n] = e;
              ratios[n] = windRatio * swing;
              p += ratios[n];
              phase += trem.ampStep;
              swing *= tremRatioStep;
            }
            if (n >= kMinSimdRun) {
              v.releaseGain = 1.0f;
              v.relXfadeLeft = 0;
              v.cursor = base + simd::renderRunPerFrame(isa_, ring, st.channels, v.cursor - base,
                                                        ratios, envs, out, numChannels, i, n);
              tremPhase = phase;
              tremRatio = swing;
              simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
              i += n - 1;
              continue;
            }
          }
        }
      }
    }

    // A key let go on a pipe with no release sample: the attack fades out
    // over a tenth of a second where it is. And the last frames of a release
    // sample, faded so a file cut dead at its end does not click. Both went
    // frame by frame: in a fast passage, nine tenths of what still did. Each
    // frame's level is worked out here as the per-frame path below works it
    // out, and the vector code is given the levels.
    if (canRun && v.phase == VoicePhase::Release && v.xfadeRemaining <= 0 &&
        v.xfadeBuf == nullptr) {
      const double endFade = static_cast<double>(total - kReleaseEndFadeFrames);
      const bool fading = !v.releaseIsSample;
      const bool ending = v.releaseIsSample && v.cursor >= endFade;
      // Where the frames are read from: the resident sample, or this voice's
      // ring for a streamed tail.
      const VoiceStream* st = nullptr;
      double base = 0.0;
      double limit = static_cast<double>(runEnd);
      if (ending && streaming && v.cursor >= static_cast<double>(runEnd) &&
          voiceIndex < streams_.size()) {
        st = streams_[voiceIndex].get();
        if (st->armed.load(std::memory_order_acquire) && !st->ring.empty() &&
            simd::runSupports(isa_, st->channels)) {
          base = static_cast<double>(st->base);
          limit = base + static_cast<double>(st->filled.load(std::memory_order_acquire) -
                                             simd::kRunMargin);
        } else {
          st = nullptr;
        }
      }
      if ((fading || ending) && v.cursor >= base + 1.0 && v.cursor < limit &&
          (!streaming || st != nullptr || v.cursor < static_cast<double>(runEnd))) {
        double ratios[kTremChunk];
        float envs[kTremChunk];
        const int want = std::min(numFrames - i, kTremChunk);
        const float level = v.gain * wind.ampMul;
        // The ring is in real units; the resident sample in storage counts.
        const float scale = st != nullptr ? 1.0f : storeScale;
        double p = v.cursor;
        float phase = tremPhase;
        double swing = tremRatio;
        float gain = fading ? v.releaseGain : 1.0f;
        bool ends = false;
        int n = 0;
        for (; n < want && p < limit; ++n) {
          float e = level;
          if (tremMoving) {
            e *= 1.0f + v.tremAmpDepth * phase;
            if (e < 0.0f) e = 0.0f;
          }
          if (fading) {
            e *= gain;
            const float next = gain - static_cast<float>(fadeStep);
            // The frame whose fade reaches nothing is not played.
            if (next <= 0.0f) {
              ends = true;
              break;
            }
            gain = next;
          } else {
            const double framesLeft = static_cast<double>(total) - p;
            if (framesLeft < static_cast<double>(kReleaseEndFadeFrames))
              e *= static_cast<float>(
                  std::max(0.0, framesLeft / static_cast<double>(kReleaseEndFadeFrames)));
          }
          envs[n] = e * scale;
          ratios[n] = tremMoving ? windRatio * swing : windRatio;
          p += ratios[n];
          phase += trem.ampStep;
          swing *= tremRatioStep;
        }
        if (n >= kMinSimdRun || (ends && n > 0)) {
          if (st != nullptr)
            v.cursor = base + simd::renderRunPerFrame(isa_, st->ring.data(), st->channels,
                                                      v.cursor - base, ratios, envs, out,
                                                      numChannels, i, n);
          else
            v.cursor = simd::renderRunPerFrame(isa_, frames, bufChannels, v.cursor, ratios, envs,
                                               out, numChannels, i, n);
          if (fading) v.releaseGain = gain;
          else v.releaseGain = 1.0f;
          v.relXfadeLeft = 0;
          tremPhase = phase;
          tremRatio = swing;
          simdFrames_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
          if (ends) {
            v.releaseGain -= static_cast<float>(fadeStep);
            v.phase = VoicePhase::Idle;
            if (streaming) disarmStream(voiceIndex);
            return;
          }
          i += n - 1;
          continue;
        }
      }
    }

    // Loop handling: wrap inside the sustain loop while the key is held. The
    // loop is the voice's, not the buffer's, so a layer override applies.
    if (v.phase != VoicePhase::Release && v.loops()) {
      if (v.cursor >= static_cast<double>(v.loopEnd)) {
        // Start the blend from where we actually are, before wrapping: that
        // read position continues into the material after the loop end, which
        // is the outgoing half of the crossfade.
        if (v.xfadeLength > 0 && v.cursor < static_cast<double>(buf.numFrames)) {
          v.xfadePos = v.cursor;
          v.xfadeRemaining = v.xfadeLength;
        }
        const double loopLen = static_cast<double>(v.loopEnd - v.loopStart);
        if (loopLen > 0.0) {
          v.cursor -= loopLen * std::floor(
              (v.cursor - static_cast<double>(v.loopStart)) / loopLen);
        }
        v.phase = VoicePhase::Loop;
      }
    }

    if (v.cursor >= static_cast<double>(total)) {
      v.phase = VoicePhase::Idle; // ran off the end of a one-shot
      if (streaming) disarmStream(voiceIndex);
      return;
    }

    // Two envelopes: one for taps that are still in their storage units (the
    // resident head, which may be 16-bit counts) and one for taps already in
    // real units (the streamed ring, which is always float).
    float envReal = v.gain * wind.ampMul;
    // A tremulant swings the level and the pitch together, which is what makes
    // it sound like wind rather than like a volume pedal. Shared by both
    // sides of a release crossfade: the wind does not jump at note-off.
    if (tremMoving) {
      envReal *= 1.0f + v.tremAmpDepth * tremPhase;
      if (envReal < 0.0f) envReal = 0.0f;
    }
    // The attack side of a release crossfade sounds through this level while
    // the tail sounds through envReal below; the two meet in the middle.
    const float xfadeEnv = envReal;
    float xfadeOld = 0.0f;
    if (v.phase == VoicePhase::Release) {
      if (v.releaseIsSample) {
        // Overlap crossfade over the release's own authored length: the old
        // attack fades out while the tail fades in from the matched level,
        // so a tail recorded louder than the sustain loop never steps.
        // There is deliberately no 120 ms fade on this path.
        if (v.relXfadeLeft > 0 && v.relXfadeLength > 0 &&
            v.xfadeBuf != nullptr) {
          const float t =
              1.0f - static_cast<float>(v.relXfadeLeft) /
                         static_cast<float>(v.relXfadeLength);
          v.releaseGain = t;
          xfadeOld = 1.0f - t;
        } else {
          v.releaseGain = 1.0f;
          v.xfadeBuf = nullptr;
          v.relXfadeLeft = 0;
        }
        envReal *= v.releaseGain;
        // A file cut dead at its end still must not click.
        const double framesLeft = static_cast<double>(total) - v.cursor;
        if (framesLeft < static_cast<double>(kReleaseEndFadeFrames))
          envReal *= static_cast<float>(
              std::max(0.0, framesLeft /
                                static_cast<double>(kReleaseEndFadeFrames)));
      } else {
        envReal *= v.releaseGain;
        v.releaseGain -= static_cast<float>(fadeStep);
        if (v.releaseGain <= 0.0f) {
          v.phase = VoicePhase::Idle;
          if (streaming) disarmStream(voiceIndex);
          return;
        }
      }
    }
    const float env = envReal * storeScale;
    const double ratio = tremMoving ? windRatio * tremRatio : windRatio;

    // One range test for all four taps and every channel, instead of one per
    // tap per channel. The edges fall back to the checked reader.
    const auto pos = static_cast<int64_t>(v.cursor);
    const float frac = static_cast<float>(v.cursor - static_cast<double>(pos));

    // The crossfade branch is hoisted out of the per-channel loop and out of
    // the common case entirely: a voice is blending for a few milliseconds
    // after a wrap and playing straight the rest of the time, so the ordinary
    // path must not pay for it. (Folding the interior test into the channel
    // loop cost ~30% of the engine when it was written that way.)
    // At the loop's seam the interpolation reaches past the loop end. What
    // follows the end, as the voice plays, is the loop's start: read from
    // there. Reading on into the file worked only where the file went on;
    // one that ends on its loop's last frame (St David's rear samples, and
    // many more) read silence there, and every wrap clicked.
    const bool atSeam = v.phase != VoicePhase::Release && v.loops() &&
                        pos + 2 >= v.loopEnd && pos >= v.loopStart + 1 &&
                        v.loopEnd - v.loopStart > 4 && v.loopEnd <= resident;
    if ((v.xfadeRemaining <= 0 || v.xfadeLength <= 0) && atSeam) {
      const int64_t loopLen = v.loopEnd - v.loopStart;
      const auto tap = [&](int64_t f, int srcCh) {
        return rawSample<T>(buf, f >= v.loopEnd ? f - loopLen : f, srcCh) * storeScale;
      };
      for (int ch = 0; ch < numChannels; ++ch) {
        const int srcCh = ch < bufChannels ? ch : bufChannels - 1;
        out[ch][i] += hermite(tap(pos - 1, srcCh), tap(pos, srcCh), tap(pos + 1, srcCh),
                              tap(pos + 2, srcCh), frac) *
                      envReal;
      }
    } else if (v.xfadeRemaining <= 0 || v.xfadeLength <= 0) {
      if (pos >= 1 && pos + 2 < resident) {
        for (int ch = 0; ch < numChannels; ++ch) {
          const int srcCh = ch < bufChannels ? ch : bufChannels - 1;
          out[ch][i] += readInterior<T>(frames, bufChannels, srcCh, pos, frac) * env;
        }
      } else if (streaming && pos + 2 >= resident) {
        // At and past the end of the resident head. Each of the four taps
        // comes from wherever that FRAME lives — the head, or this voice's
        // ring — and not from wherever the cursor happens to be.
        //
        // Reading all four from the ring is the obvious version and it is
        // wrong: the ring starts where the head ends, so on the last three
        // frames before the seam it answers with silence, and every streamed
        // sample gets a three-frame hole punched in it exactly once. Small
        // enough to pass a render comparison, loud enough to be a click.
        const auto tap = [&](int64_t f, int srcCh) {
          return f < resident
                     ? rawSample<T>(buf, f, srcCh) * storeScale
                     : streamedSample(voiceIndex, buf, f, srcCh);
        };
        for (int ch = 0; ch < numChannels; ++ch) {
          const int srcCh = ch < bufChannels ? ch : bufChannels - 1;
          out[ch][i] += hermite(tap(pos - 1, srcCh), tap(pos, srcCh),
                                tap(pos + 1, srcCh), tap(pos + 2, srcCh),
                                frac) *
                        envReal;
        }
      } else {
        for (int ch = 0; ch < numChannels; ++ch)
          out[ch][i] += readInterpolated<T>(buf, v.cursor, ch) * env;
      }
    } else {
      // Blending. Weight 1 is fully the outgoing (pre-wrap) stream, 0 fully
      // the incoming one. Equal-gain rather than equal-power: the two streams
      // are the same pipe a loop-length apart and correlate strongly, so
      // equal-power would bulge the level at every wrap.
      const float xfade = static_cast<float>(v.xfadeRemaining) /
                          static_cast<float>(v.xfadeLength);
      const bool interior = pos >= 1 && pos + 2 < resident;
      for (int ch = 0; ch < numChannels; ++ch) {
        const int srcCh = ch < bufChannels ? ch : bufChannels - 1;
        const float incoming =
            interior ? readInterior<T>(frames, bufChannels, srcCh, pos, frac)
                     : readInterpolated<T>(buf, v.cursor, ch);
        const float outgoing = readInterpolated<T>(buf, v.xfadePos, ch);
        out[ch][i] += (outgoing * xfade + incoming * (1.0f - xfade)) * env;
      }
    }

    if (v.xfadeRemaining > 0) {
      v.xfadePos += ratio;
      // Run out of file: nothing left to fade from, so stop blending rather
      // than mixing in silence, which would dip the level at every wrap.
      if (--v.xfadeRemaining <= 0 ||
          v.xfadePos >= static_cast<double>(resident))
        v.xfadeRemaining = 0;
    }

    // The attack side of a release crossfade: the old sustain keeps sounding
    // while fading out, against the tail fading in above. It reads inside
    // its own sustain loop with the same wrap rule, and that loop is covered
    // by the preload head by construction; rawSample clamps anything past
    // the resident end to silence rather than faulting.
    if (xfadeOld > 0.0f && v.xfadeBuf != nullptr) {
      if (v.xfadeOldLoopEnd > v.xfadeOldLoopStart &&
          v.xfadeOldLoopStart >= 0 &&
          v.xfadeOldCursor >= static_cast<double>(v.xfadeOldLoopEnd)) {
        const double loopLen =
            static_cast<double>(v.xfadeOldLoopEnd - v.xfadeOldLoopStart);
        if (loopLen > 0.0)
          v.xfadeOldCursor -=
              loopLen * std::floor((v.xfadeOldCursor -
                                    static_cast<double>(v.xfadeOldLoopStart)) /
                                   loopLen);
      }
      const auto opos = static_cast<int64_t>(v.xfadeOldCursor);
      const float ofrac =
          static_cast<float>(v.xfadeOldCursor - static_cast<double>(opos));
      const SampleBuffer& ob = *v.xfadeBuf;
      // At the attack's own storage scale: 16-bit samples are each scaled by
      // their own peak, and the release's scale was used here for both.
      const float oldEnv = xfadeEnv * storageScale(ob) * xfadeOld;
      // Past the loop end comes the loop start, as on the attack's own path
      // (atSeam): a key let go just after a wrap clicked otherwise.
      const int64_t oldLoopLen = v.xfadeOldLoopEnd - v.xfadeOldLoopStart;
      const bool oldWraps = v.xfadeOldLoopStart >= 0 && oldLoopLen > 4;
      const auto otap = [&](int64_t f, int srcCh) {
        return rawSample<T>(ob, oldWraps && f >= v.xfadeOldLoopEnd ? f - oldLoopLen : f, srcCh);
      };
      for (int ch = 0; ch < numChannels; ++ch) {
        const int srcCh = ch < ob.numChannels ? ch : ob.numChannels - 1;
        out[ch][i] += hermite(otap(opos - 1, srcCh), otap(opos, srcCh), otap(opos + 1, srcCh),
                              otap(opos + 2, srcCh), ofrac) *
                      oldEnv;
      }
      v.xfadeOldCursor += ratio * v.xfadeRatioScale;
      if (--v.relXfadeLeft <= 0) v.xfadeBuf = nullptr;
    }

    v.cursor += ratio;
    tremPhase += trem.ampStep;
    tremRatio *= tremRatioStep;
  }
}

void VoiceEngine::gatherWindDemand(float* out, int count) const {
  if (out == nullptr || count <= 0) return;
  for (int i = 0; i < count; ++i) out[i] = 0.0f;
  for (size_t slot = 0; slot < scanEnd_; ++slot) {
    const Voice& v = voices_[slot];
    if (!v.active() || v.phase == VoicePhase::Release) continue;
    if (v.windIndex < 0 || v.windIndex >= count) continue;
    out[v.windIndex] += v.windFlow;
  }
}

namespace {
// What a thread does while it waits a few microseconds for another: tell the
// processor so, where there is a way to.
inline void spinPause() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#elif (defined(__aarch64__) || defined(__arm__)) && (defined(__GNUC__) || defined(__clang__))
  __asm__ __volatile__("yield");
#endif
}

// Spin until `done` says so or `microseconds` have gone by; whether it did.
template <typename Done>
inline bool spinFor(int microseconds, Done done) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(microseconds);
  for (;;) {
    for (int i = 0; i < 32; ++i) {
      if (done()) return true;
      spinPause();
    }
    if (std::chrono::steady_clock::now() >= until) return done();
  }
}

// A worker that has just finished a share looks for the next one this long
// before it sleeps: a block with enclosures is several renders a few
// microseconds apart, and going to sleep between them cost more than the
// renders. The audio thread looks this long for its workers to finish.
constexpr int kWorkerSpinMicros = 40;
constexpr int kBarrierSpinMicros = 150;
// Looking instead of sleeping pays only while there is a core for every
// thread. When the workers were not done within the look -- fewer cores than
// threads, or a machine busy with something else -- nobody looks for this many
// renders, and the threads sleep and are woken as they always were.
constexpr int kSpinHoldoffRenders = 256;
constexpr int kSpinMissesBeforeHoldoff = 4;
} // namespace

void VoiceEngine::renderList(const uint32_t* list, size_t count, float* const* out,
                             int numChannels, int numFrames) {
  for (size_t i = 0; i < count; ++i)
    renderVoice(voices_[list[i]], out, numChannels, numFrames);
}

void VoiceEngine::stopWorkers() {
  if (workers_.empty()) return;
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    shuttingDown_.store(true, std::memory_order_release);
  }
  for (auto& w : workers_) w->wake.notify_all();
  for (auto& w : workers_)
    if (w->thread.joinable()) w->thread.join();
  workers_.clear();
  shuttingDown_.store(false, std::memory_order_release);
}

void VoiceEngine::setBusCount(int buses) {
  buses = std::max(1, buses);
  if (buses == busCount_ && static_cast<int>(busLists_.size()) == buses) return;
  busCount_ = buses;
  busLists_.assign(static_cast<size_t>(buses), {});
  for (auto& list : busLists_) list.reserve(voices_.size());
  if (!workers_.empty()) setRenderThreads(lastThreads_, minVoicesPerThread_);
}

void VoiceEngine::setRenderThreads(int numThreads, int minVoicesPerThread) {
  stopWorkers();
  minVoicesPerThread_ = minVoicesPerThread > 0 ? minVoicesPerThread : 1;
  lastThreads_ = numThreads;

  // numThreads counts the audio thread, which always renders a share itself.
  const int extra = numThreads > 1 ? numThreads - 1 : 0;
  if (extra <= 0 || maxFrames_ <= 0) return;

  // Every worker starts with a ticket of its own at zero, so a pool made new
  // cannot take the last job of the one before it: a worker that did rendered
  // it at once, at the old block size, into scratch sized for the new one --
  // past its end whenever the buffer got smaller (#267, found with
  // AddressSanitizer).
  jobsOutstanding_.store(0, std::memory_order_release);
  const int scratchFrames = maxFrames_;
  const size_t planesPerWorker =
      static_cast<size_t>(numChannels_) * static_cast<size_t>(std::max(1, busCount_));
  workers_.reserve(static_cast<size_t>(extra));
  for (int n = 0; n < extra; ++n) {
    auto worker = std::make_unique<Worker>();
    worker->scratch.assign(planesPerWorker * static_cast<size_t>(maxFrames_), 0.0f);
    worker->planes.resize(planesPerWorker);
    for (size_t c = 0; c < planesPerWorker; ++c)
      worker->planes[c] = worker->scratch.data() + c * static_cast<size_t>(maxFrames_);
    worker->busBegin.assign(static_cast<size_t>(std::max(1, busCount_)), 0);
    worker->busEnd.assign(static_cast<size_t>(std::max(1, busCount_)), 0);
    workers_.push_back(std::move(worker));
  }

  for (auto& owned : workers_) {
    Worker* const wk = owned.get();
    wk->thread = std::thread([this, wk, scratchFrames]() {
      uint32_t seen = 0;
      const auto called = [&] {
        return wk->ticket.load(std::memory_order_acquire) != seen ||
               shuttingDown_.load(std::memory_order_acquire);
      };
      for (;;) {
        if (!spinning_.load(std::memory_order_relaxed) ||
            !spinFor(kWorkerSpinMicros, called)) {
          std::unique_lock<std::mutex> lock(jobMutex_);
          wk->wake.wait(lock, called);
        }
        if (shuttingDown_.load(std::memory_order_acquire)) return;
        seen = wk->ticket.load(std::memory_order_acquire);
        // Never more than the scratch holds, whatever the job says.
        const int channels = std::min(jobChannels_, numChannels_);
        const int frames = std::min(jobFrames_, scratchFrames);

        // A share of every bus, each into its own planes.
        if (jobBuses_ > 0) {
          const int buses = std::min(jobBuses_, static_cast<int>(wk->busBegin.size()));
          for (int b = 0; b < buses; ++b) {
            const size_t from = wk->busBegin[static_cast<size_t>(b)];
            const size_t to = wk->busEnd[static_cast<size_t>(b)];
            if (from >= to) continue;
            float* const* planes = wk->planes.data() + static_cast<size_t>(b) * static_cast<size_t>(numChannels_);
            for (int c = 0; c < channels; ++c) std::fill(planes[c], planes[c] + frames, 0.0f);
            renderList(busLists_[static_cast<size_t>(b)].data() + from, to - from, planes, channels,
                       frames);
          }
          if (jobsOutstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> done(jobMutex_);
            doneCv_.notify_one();
          }
          continue;
        }

        // Each channel's own plane, for the frames of this block. The planes
        // stand maxFrames_ apart; clearing channels * frames from the start
        // of the scratch left the second channel of any block shorter than
        // the largest holding the block before it, and summed it in again.
        for (int c = 0; c < channels; ++c)
          std::fill(wk->planes[static_cast<size_t>(c)],
                    wk->planes[static_cast<size_t>(c)] + frames, 0.0f);
        renderList(jobList_.data() + wk->begin, wk->end - wk->begin, wk->planes.data(), channels,
                   frames);

        if (jobsOutstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          // Under the lock, so the audio thread cannot miss it between its
          // test and its sleep.
          std::lock_guard<std::mutex> done(jobMutex_);
          doneCv_.notify_one();
        }
      }
    });
  }
}

void VoiceEngine::render(float* const* out, int numChannels, int numFrames,
                         int busIndex, int mixBus) {
  // Rendering everything is one block by definition. Per-bus rendering makes
  // several calls per block, so the caller owns the counter via beginBlock().
  if (busIndex < 0 && mixBus < 0 && !callerCountsBlocks_) {
    ++blockCounter_;
    framesRendered_ += numFrames;
  }
  if (out == nullptr || numFrames <= 0 || numChannels <= 0) return;

  // The voices this call renders, found in one pass over the pool. The split
  // below is of these: sharing out the pool by slot gave the first threads
  // every voice of an organ that fills a third of it and the rest nothing,
  // and counting the whole organ's voices woke every thread for an enclosure
  // with five pipes in it.
  int active = 0;
  size_t firstFree = voices_.size();
  size_t lastActive = 0;
  jobList_.clear();
  for (size_t i = 0; i < scanEnd_; ++i) {
    const Voice& v = voices_[i];
    if (!v.active()) {
      if (firstFree == voices_.size()) firstFree = i;
      continue;
    }
    ++active;
    lastActive = i + 1;
    if (v.buffer == nullptr) continue;
    if (busIndex >= 0 && v.busIndex != busIndex) continue;
    if (mixBus >= 0 && v.mixBus != mixBus) continue;
    jobList_.push_back(static_cast<uint32_t>(i));
  }
  stats_.activeVoices = active;
  if (firstFree == voices_.size() && scanEnd_ < voices_.size()) firstFree = scanEnd_;
  scanEnd_ = lastActive;
  // Where the next voice looks for a slot, and how many are in use: voices
  // end inside the render below, and are counted out at the next one.
  freeHint_ = firstFree < voices_.size() ? firstFree : 0;
  liveCount_ = active;

  // As many threads as have minVoicesPerThread voices each to render: below
  // that a thread's handoff costs more than its share saves.
  const size_t count = jobList_.size();
  size_t shares = 1;
  if (!workers_.empty() && numFrames <= maxFrames_ && numChannels <= numChannels_)
    shares = std::min(workers_.size() + 1,
                      count / static_cast<size_t>(std::max(1, minVoicesPerThread_)));
  if (shares <= 1) {
    renderList(jobList_.data(), count, out, numChannels, numFrames);
    return;
  }

  // Equal shares of the list, in order: the same voices and the same thread
  // count give the same shares on every run.
  const size_t helpers = shares - 1;
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    jobChannels_ = numChannels;
    jobFrames_ = numFrames;
    jobBuses_ = 0;
    for (size_t w = 0; w < helpers; ++w) {
      workers_[w]->begin = count * (w + 1) / shares;
      workers_[w]->end = count * (w + 2) / shares;
    }
    jobsOutstanding_.store(static_cast<int>(helpers), std::memory_order_release);
    for (size_t w = 0; w < helpers; ++w)
      workers_[w]->ticket.fetch_add(1, std::memory_order_release);
  }
  for (size_t w = 0; w < helpers; ++w) workers_[w]->wake.notify_one();

  // The audio thread takes the first share rather than blocking idle.
  renderList(jobList_.data(), count / shares, out, numChannels, numFrames);
  waitForWorkers();

  // Sum the scratches of the workers that rendered, in a fixed order. This
  // does NOT reproduce the inline path bit for bit — float addition is not
  // associative, and parallel partial sums group the terms differently, so
  // the two differ in the last ulp or so. What the fixed order does guarantee
  // is that the threaded path is deterministic: the same voices and the same
  // shares give the same samples on every run, whatever order the workers
  // happen to finish in. That is the property worth having, because it makes
  // renders reproducible and bugs repeatable; exact equality with inline
  // rendering is not achievable without giving up the parallelism.
  for (size_t w = 0; w < helpers; ++w) {
    for (int c = 0; c < numChannels; ++c) {
      const float* src = workers_[w]->planes[static_cast<size_t>(c)];
      float* dst = out[c];
      for (int i = 0; i < numFrames; ++i) dst[i] += src[i];
    }
  }
}

void VoiceEngine::waitForWorkers() {
  const auto finished = [&] { return jobsOutstanding_.load(std::memory_order_acquire) == 0; };
  if (spinHoldoff_ > 0) {
    if (--spinHoldoff_ == 0) spinning_.store(true, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(jobMutex_);
    doneCv_.wait(lock, finished);
  } else if (spinFor(kBarrierSpinMicros, finished)) {
    spinMisses_ = 0;
  } else {
    // One late worker is a worker the system took away for a moment; several
    // in a row are threads without cores.
    if (++spinMisses_ >= kSpinMissesBeforeHoldoff) {
      spinMisses_ = 0;
      spinHoldoff_ = kSpinHoldoffRenders;
      spinning_.store(false, std::memory_order_relaxed);
    }
    std::unique_lock<std::mutex> lock(jobMutex_);
    doneCv_.wait(lock, finished);
  }
}

void VoiceEngine::renderBuses(float* const* const* busOut, int numBuses, int numChannels,
                              int numFrames, int mixBus) {
  if (busOut == nullptr || numBuses <= 0 || numFrames <= 0 || numChannels <= 0) return;
  // More buses than the threads were given room for, or the lists not made
  // yet: bus by bus, as before.
  if (numBuses > static_cast<int>(busLists_.size()) ||
      (!workers_.empty() && numBuses > static_cast<int>(workers_.front()->busBegin.size()))) {
    for (int b = 0; b < numBuses; ++b) render(busOut[b], numChannels, numFrames, b, mixBus);
    return;
  }

  // One pass over the pool for every bus: the same voices, in the same
  // order, as render() finds for each bus.
  int active = 0;
  size_t firstFree = voices_.size();
  size_t lastActive = 0;
  for (int b = 0; b < numBuses; ++b) busLists_[static_cast<size_t>(b)].clear();
  for (size_t i = 0; i < scanEnd_; ++i) {
    const Voice& v = voices_[i];
    if (!v.active()) {
      if (firstFree == voices_.size()) firstFree = i;
      continue;
    }
    ++active;
    lastActive = i + 1;
    if (v.buffer == nullptr) continue;
    if (mixBus >= 0 && v.mixBus != mixBus) continue;
    if (v.busIndex < 0 || v.busIndex >= numBuses) continue;
    busLists_[static_cast<size_t>(v.busIndex)].push_back(static_cast<uint32_t>(i));
  }
  stats_.activeVoices = active;
  if (firstFree == voices_.size() && scanEnd_ < voices_.size()) firstFree = scanEnd_;
  scanEnd_ = lastActive;
  freeHint_ = firstFree < voices_.size() ? firstFree : 0;
  liveCount_ = active;

  // Each bus shared out exactly as render() shares it.
  const bool threaded =
      !workers_.empty() && numFrames <= maxFrames_ && numChannels <= numChannels_;
  size_t helpers = 0;
  for (int b = 0; b < numBuses; ++b) {
    const size_t count = busLists_[static_cast<size_t>(b)].size();
    size_t shares = 1;
    if (threaded)
      shares = std::min(workers_.size() + 1,
                        count / static_cast<size_t>(std::max(1, minVoicesPerThread_)));
    if (shares < 1) shares = 1;
    for (size_t w = 0; w < workers_.size(); ++w) {
      Worker& wk = *workers_[w];
      if (w + 1 < shares) {
        wk.busBegin[static_cast<size_t>(b)] = count * (w + 1) / shares;
        wk.busEnd[static_cast<size_t>(b)] = count * (w + 2) / shares;
      } else {
        wk.busBegin[static_cast<size_t>(b)] = wk.busEnd[static_cast<size_t>(b)] = 0;
      }
    }
    helpers = std::max(helpers, shares - 1);
  }

  if (helpers > 0) {
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      jobChannels_ = numChannels;
      jobFrames_ = numFrames;
      jobBuses_ = numBuses;
      jobsOutstanding_.store(static_cast<int>(helpers), std::memory_order_release);
      for (size_t w = 0; w < helpers; ++w) workers_[w]->ticket.fetch_add(1, std::memory_order_release);
    }
    for (size_t w = 0; w < helpers; ++w) workers_[w]->wake.notify_one();
  }

  // The audio thread's share of every bus: the first, as render() takes it.
  for (int b = 0; b < numBuses; ++b) {
    const auto& list = busLists_[static_cast<size_t>(b)];
    size_t shares = 1;
    for (size_t w = 0; w < helpers; ++w)
      if (workers_[w]->busBegin[static_cast<size_t>(b)] < workers_[w]->busEnd[static_cast<size_t>(b)])
        shares = w + 2;
    renderList(list.data(), list.size() / shares, busOut[b], numChannels, numFrames);
  }
  if (helpers > 0) waitForWorkers();
  // Voices that ended in this block's render are free for the next note-on,
  // as when each bus counted them out at its own pass.
  for (int b = 0; b < numBuses; ++b)
    for (const uint32_t idx : busLists_[static_cast<size_t>(b)])
      if (!voices_[idx].active()) {
        if (liveCount_ > 0) --liveCount_;
        if (idx < freeHint_) freeHint_ = idx;
      }
  if (helpers == 0) return;

  // Each bus summed as render() sums it: its workers in order.
  for (int b = 0; b < numBuses; ++b)
    for (size_t w = 0; w < helpers; ++w) {
      const Worker& wk = *workers_[w];
      if (wk.busBegin[static_cast<size_t>(b)] >= wk.busEnd[static_cast<size_t>(b)]) continue;
      const size_t first = static_cast<size_t>(b) * static_cast<size_t>(numChannels_);
      for (int c = 0; c < numChannels; ++c) {
        const float* src = wk.planes[first + static_cast<size_t>(c)];
        float* dst = busOut[b][c];
        for (int i = 0; i < numFrames; ++i) dst[i] += src[i];
      }
    }
}

} // namespace mp
