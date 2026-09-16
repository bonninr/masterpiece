#include "VoiceEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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

} // namespace

void VoiceEngine::prepare(double sampleRate, int maxVoices, int numChannels,
                          int maxBlockFrames) {
  stopWorkers(); // pool sizes depend on everything below
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
  numChannels_ = numChannels > 0 ? numChannels : 2;
  maxFrames_ = maxBlockFrames > 0 ? maxBlockFrames : 2048;
  voices_.assign(static_cast<size_t>(std::max(1, maxVoices)), Voice{});
  blockCounter_ = 0;
  stats_ = EngineStats{};

  // One ring per voice, allocated here and never again: the audio thread must
  // not allocate, and a voice can start streaming at any note-off.
  stopStreamer();
  const auto ringFrames =
      static_cast<size_t>(std::max(1.0, streamSeconds_ * sampleRate_));
  streams_.clear();
  streams_.reserve(voices_.size());
  for (size_t i = 0; i < voices_.size(); ++i) {
    auto st = std::make_unique<VoiceStream>();
    // Stereo worst case; a mono sample simply uses half of it.
    st->ring.assign(ringFrames * 2, 0.0f);
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

void VoiceEngine::armStream(size_t voiceIndex, const SampleBuffer& buf,
                            int64_t fromFrame) {
  if (voiceIndex >= streams_.size()) return;
  VoiceStream& st = *streams_[voiceIndex];
  // Bump the generation FIRST: a fill still in flight for the previous voice
  // must not publish its frames into this one's ring.
  st.generation.fetch_add(1, std::memory_order_acq_rel);
  st.armed.store(false, std::memory_order_release);
  st.filled.store(0, std::memory_order_release);
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
  // Acquire: everything the streamer wrote before publishing `filled` is
  // visible here. This one load is the whole synchronisation.
  const int64_t filled = st.filled.load(std::memory_order_acquire);
  if (offset >= filled) {
    // The disk did not keep up. Silence for this frame, and a count, because
    // a release that goes quiet halfway is exactly the fault a player would
    // hear and have no way to diagnose.
    underruns_.fetch_add(1, std::memory_order_relaxed);
    return 0.0f;
  }
  const int ch = channel < st.channels ? channel : st.channels - 1;
  const auto idx = static_cast<size_t>(offset) * static_cast<size_t>(st.channels) +
                   static_cast<size_t>(ch);
  return idx < st.ring.size() ? st.ring[idx] : 0.0f;
}

void VoiceEngine::streamerLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(streamMutex_);
      // Woken by a new streaming voice, and otherwise often enough to keep
      // every ring topped up. A voice consumes one frame per output frame, so
      // even a full ring lasts seconds; ten milliseconds is generous.
      streamCv_.wait_for(lock, std::chrono::milliseconds(10),
                         [this] { return streamQuit_; });
      if (streamQuit_) return;
    }

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

void VoiceEngine::reset() {
  for (auto& v : voices_) v = Voice{};
  stats_ = EngineStats{};
}

int VoiceEngine::activeVoiceCount() const {
  int n = 0;
  for (const auto& v : voices_)
    if (v.active()) ++n;
  return n;
}

int VoiceEngine::allocateVoice() {
  // 1. A free slot is always best.
  for (size_t i = 0; i < voices_.size(); ++i)
    if (!voices_[i].active()) return static_cast<int>(i);

  // 2. Otherwise steal, and steal musically. A voice already releasing is the
  //    cheapest thing to lose — it is decaying anyway — so take the oldest of
  //    those before touching anything still speaking.
  int best = -1;
  uint64_t oldest = UINT64_MAX;
  for (size_t i = 0; i < voices_.size(); ++i) {
    const Voice& v = voices_[i];
    if (v.phase != VoicePhase::Release) continue;
    if (v.startedAtBlock < oldest) {
      oldest = v.startedAtBlock;
      best = static_cast<int>(i);
    }
  }

  // 3. Nothing releasing: take the oldest quietest sounding voice. Never the
  //    newest — stealing the note the player just pressed is the one failure
  //    they always hear.
  if (best < 0) {
    float quietest = 0.0f;
    oldest = UINT64_MAX;
    for (size_t i = 0; i < voices_.size(); ++i) {
      const Voice& v = voices_[i];
      if (v.startedAtBlock == blockCounter_) continue; // started this block
      if (best < 0 || v.gain < quietest ||
          (v.gain == quietest && v.startedAtBlock < oldest)) {
        quietest = v.gain;
        oldest = v.startedAtBlock;
        best = static_cast<int>(i);
      }
    }
  }

  if (best >= 0) ++stats_.stolenVoices;
  return best;
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
  v.windFlow = start.windFlowKgPerSec;
  v.tremIndex = start.tremIndex;
  v.tremAmpDepth = start.tremAmpDepth;
  v.tremPitchDepth = start.tremPitchDepth;
  v.startedAtBlock = blockCounter_;
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

void VoiceEngine::noteOff(uint64_t noteId, const NoteRelease& release) {
  for (auto& v : voices_) {
    if (!v.active() || v.noteId != noteId) continue;
    if (v.phase == VoicePhase::Release) continue;

    // Pick the release sample that matches the attack this voice actually
    // played — that pairing is what makes a release sound like the same pipe.
    int chosen = -1;
    if (v.layer != nullptr) {
      NoteRelease rel = release;
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
      v.cursor = (fromMarker && relBuf->releaseCue > 0)
                     ? static_cast<double>(relBuf->releaseCue)
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
  }
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
  const WindMod wind = windModFor(v.windIndex);
  const double windRatio = v.ratio * wind.pitchRatio;
  // The tremulant, as a ramp across the block. Its own value is a signed
  // swing in -1..1; this pipe's depths turn that into a gain and a pitch.
  const TremMod trem = tremModFor(v.tremIndex);
  float tremPhase = trem.ampStart;
  double tremPitchPhase = trem.pitchStart;
  // Folded in once rather than applied per tap. For Float32 this is 1.0f and
  // the multiply below is the one the gain needed anyway.
  const float storeScale = storageScale(buf);

  for (int i = 0; i < numFrames; ++i) {
    if (v.phase == VoicePhase::Idle) return;

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
    if (v.tremIndex >= 0) {
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
    const double ratio =
        v.tremIndex >= 0
            ? windRatio * std::pow(2.0, v.tremPitchDepth * tremPitchPhase / 12.0)
            : windRatio;

    // One range test for all four taps and every channel, instead of one per
    // tap per channel. The edges fall back to the checked reader.
    const auto pos = static_cast<int64_t>(v.cursor);
    const float frac = static_cast<float>(v.cursor - static_cast<double>(pos));

    // The crossfade branch is hoisted out of the per-channel loop and out of
    // the common case entirely: a voice is blending for a few milliseconds
    // after a wrap and playing straight the rest of the time, so the ordinary
    // path must not pay for it. (Folding the interior test into the channel
    // loop cost ~30% of the engine when it was written that way.)
    if (v.xfadeRemaining <= 0 || v.xfadeLength <= 0) {
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
      const float oldEnv = xfadeEnv * storeScale * xfadeOld;
      for (int ch = 0; ch < numChannels; ++ch) {
        const int srcCh = ch < ob.numChannels ? ch : ob.numChannels - 1;
        out[ch][i] += hermite(rawSample<T>(ob, opos - 1, srcCh),
                              rawSample<T>(ob, opos, srcCh),
                              rawSample<T>(ob, opos + 1, srcCh),
                              rawSample<T>(ob, opos + 2, srcCh), ofrac) *
                      oldEnv;
      }
      v.xfadeOldCursor += ratio;
      if (--v.relXfadeLeft <= 0) v.xfadeBuf = nullptr;
    }

    v.cursor += ratio;
    tremPhase += trem.ampStep;
    tremPitchPhase += trem.pitchStep;
  }
}

void VoiceEngine::gatherWindDemand(float* out, int count) const {
  if (out == nullptr || count <= 0) return;
  for (int i = 0; i < count; ++i) out[i] = 0.0f;
  for (const Voice& v : voices_) {
    if (!v.active() || v.phase == VoicePhase::Release) continue;
    if (v.windIndex < 0 || v.windIndex >= count) continue;
    out[v.windIndex] += v.windFlow;
  }
}

void VoiceEngine::renderRange(size_t begin, size_t end, float* const* out,
                              int numChannels, int numFrames, int busIndex,
                              int mixBus) {
  for (size_t i = begin; i < end && i < voices_.size(); ++i) {
    Voice& v = voices_[i];
    if (!v.active() || v.buffer == nullptr) continue;
    if (busIndex >= 0 && v.busIndex != busIndex) continue;
    if (mixBus >= 0 && v.mixBus != mixBus) continue;
    renderVoice(v, out, numChannels, numFrames);
  }
}

void VoiceEngine::stopWorkers() {
  if (workers_.empty()) return;
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    shuttingDown_ = true;
    ++jobGeneration_;
  }
  jobCv_.notify_all();
  for (auto& w : workers_)
    if (w.thread.joinable()) w.thread.join();
  workers_.clear();
  shuttingDown_ = false;
}

void VoiceEngine::setRenderThreads(int numThreads, int minVoicesPerThread) {
  stopWorkers();
  minVoicesPerThread_ = minVoicesPerThread > 0 ? minVoicesPerThread : 1;

  // numThreads counts the audio thread, which always renders a share itself.
  const int extra = numThreads > 1 ? numThreads - 1 : 0;
  if (extra <= 0 || maxFrames_ <= 0) return;

  workers_.resize(static_cast<size_t>(extra));
  for (size_t w = 0; w < workers_.size(); ++w) {
    auto& worker = workers_[w];
    worker.scratch.assign(
        static_cast<size_t>(numChannels_) * static_cast<size_t>(maxFrames_), 0.0f);
    worker.planes.resize(static_cast<size_t>(numChannels_));
    for (int c = 0; c < numChannels_; ++c)
      worker.planes[static_cast<size_t>(c)] =
          worker.scratch.data() + static_cast<size_t>(c) * static_cast<size_t>(maxFrames_);

    worker.thread = std::thread([this, w]() {
      uint64_t seen = 0;
      for (;;) {
        std::unique_lock<std::mutex> lock(jobMutex_);
        jobCv_.wait(lock, [&] { return jobGeneration_ != seen || shuttingDown_; });
        if (shuttingDown_) return;
        seen = jobGeneration_;
        const auto begin = workers_[w].begin;
        const auto end = workers_[w].end;
        const int channels = jobChannels_;
        const int frames = jobFrames_;
        const int bus = jobBus_;
        const int mixBus = jobMixBus_;
        auto* planes = workers_[w].planes.data();
        lock.unlock();

        std::fill(workers_[w].scratch.begin(),
                  workers_[w].scratch.begin() +
                      static_cast<ptrdiff_t>(channels) * frames, 0.0f);
        renderRange(begin, end, planes, channels, frames, bus, mixBus);

        {
          std::lock_guard<std::mutex> done(jobMutex_);
          if (--jobsOutstanding_ == 0) doneCv_.notify_one();
        }
      }
    });
  }
}

void VoiceEngine::render(float* const* out, int numChannels, int numFrames,
                         int busIndex, int mixBus) {
  // Rendering everything is one block by definition. Per-bus rendering makes
  // several calls per block, so the caller owns the counter via beginBlock().
  if (busIndex < 0 && mixBus < 0) ++blockCounter_;
  if (out == nullptr || numFrames <= 0 || numChannels <= 0) return;

  const int active = activeVoiceCount();
  const int threads = renderThreads();
  const bool worthSplitting =
      !workers_.empty() && numFrames <= maxFrames_ &&
      numChannels <= numChannels_ &&
      active >= minVoicesPerThread_ * threads;

  if (!worthSplitting) {
    // Inline: below the threshold, thread handoff costs more than it saves.
    renderRange(0, voices_.size(), out, numChannels, numFrames, busIndex,
                mixBus);
    stats_.activeVoices = active;
    return;
  }

  // Split the POOL, not the active voices: ranges must be fixed before the
  // workers start, and scanning for active ones first would cost a pass.
  const size_t total = voices_.size();
  const size_t share = (total + static_cast<size_t>(threads) - 1) /
                       static_cast<size_t>(threads);
  {
    std::lock_guard<std::mutex> lock(jobMutex_);
    for (size_t w = 0; w < workers_.size(); ++w) {
      workers_[w].begin = std::min(total, (w + 1) * share);
      workers_[w].end = std::min(total, (w + 2) * share);
    }
    jobOut_ = out;
    jobChannels_ = numChannels;
    jobFrames_ = numFrames;
    jobBus_ = busIndex;
    jobMixBus_ = mixBus;
    jobsOutstanding_ = static_cast<int>(workers_.size());
    ++jobGeneration_;
  }
  jobCv_.notify_all();

  // The audio thread takes the first share rather than blocking idle.
  renderRange(0, std::min(total, share), out, numChannels, numFrames, busIndex,
              mixBus);

  {
    std::unique_lock<std::mutex> lock(jobMutex_);
    doneCv_.wait(lock, [&] { return jobsOutstanding_ == 0; });
  }

  // Sum the workers' scratches in a fixed order. This does NOT reproduce the
  // inline path bit for bit — float addition is not associative, and parallel
  // partial sums group the terms differently, so the two differ in the last
  // ulp or so. What the fixed order does guarantee is that the threaded path
  // is deterministic: the same voices and the same partition give the same
  // samples on every run, whatever order the workers happen to finish in.
  // That is the property worth having, because it makes renders reproducible
  // and bugs repeatable; exact equality with inline rendering is not
  // achievable without giving up the parallelism.
  for (auto& w : workers_) {
    for (int c = 0; c < numChannels; ++c) {
      const float* src = w.planes[static_cast<size_t>(c)];
      float* dst = out[c];
      for (int i = 0; i < numFrames; ++i) dst[i] += src[i];
    }
  }

  stats_.activeVoices = active;
}

} // namespace mp
