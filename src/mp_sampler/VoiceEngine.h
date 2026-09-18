// Voice engine — the thing that actually makes sound (M2 acceptance gate).
//
// Deliberately JUCE-free. Everything musical lives here — voice allocation,
// stealing, cursors, attack->loop->release, resampling, mixing — so it is
// testable in seconds by the WSL fast loop with a synthesised sample instead
// of a 40 GB organ on disk. Reading real audio off a disk is a *backing store*
// concern and lives behind SampleProvider; per ADR-004 preloaded and streaming
// share this voice code and differ only in how frames arrive.
//
// Hard constraints (perf budget, docs/architecture/02):
//   - render() allocates nothing, locks nothing, touches no disk.
//   - Voice pool is fixed at prepare() time; 500-2000 concurrent voices.
//   - Per-voice cursors, so the same pipe retriggered keeps independent state.
//   - Stealing is musical, never random: sounding releases go first, then the
//     oldest quietest attack, and a note that just started is never stolen.
#pragma once
#include "../mp_core/OrganModel.h"
#include "StreamingEngine.h" // NoteStrike / NoteRelease + the selection matrices

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace mp {

// How resident audio is stored. This is the single biggest lever on how large
// an organ a machine can hold: an organ is nothing but sample data, so the
// resident format IS the memory footprint.
//
//   Float32 — what the decoder produces, no conversion, no loss. Four bytes a
//             frame per channel.
//   Int16   — half that, and what Hauptwerk loads by default. Each file is
//             scaled by its own peak before quantising, so a quiet sample
//             keeps the full sixteen bits rather than only the top few: this
//             is meaningfully better than a plain truncation, and it costs
//             nothing at run time because the scale folds into the voice gain.
// What a resident frame costs.
//
// Int24 is the default because it is what the material is: organ sample sets
// are 24-bit, and holding them as 32-bit float is exact but a third larger
// for nothing. It stores the source integers unchanged, so the audio is
// bit-for-bit what the file held.
//
// Int16 quantises against the file's OWN peak, so a stop recorded 20 dB down
// still uses every bit it is given; on real samples that is about 78 dB of
// signal to quantisation noise, which is inaudible, for two thirds of Int24.
//
// Float32 remains for a set that is genuinely float, and because processing
// happens in float regardless -- that is where 32 bits belongs.
//
// An 8-bit width was built and withdrawn: it held about 32 dB, which is
// audible hiss under a quiet stop.
//
// The order matters: settings files store a bit WIDTH, not this ordinal, but
// tests pin it so a reordering cannot pass unnoticed.
enum class SampleStorage { Float32, Int24, Int16 };

// Three bytes, little-endian, signed -- the layout a 24-bit WAV already uses.
// Converts to a raw count, like the other integer storage: the scale is folded
// into the voice gain once, not applied per tap.
struct Pcm24 {
  unsigned char b[3];
  operator float() const {
    int v = b[0] | (b[1] << 8) | (b[2] << 16);
    if (v & 0x800000) v -= 0x1000000;
    return static_cast<float>(v);
  }
};
static_assert(sizeof(Pcm24) == 3, "24-bit storage must not be padded");

// Where the rest of a sample lives when only its head is resident.
//
// The engine never opens a file. It is handed a reader and calls it from a
// BACKGROUND thread, well ahead of where the voice is playing, so the audio
// thread only ever touches memory. Per ADR-004 there is still one voice path:
// preloaded and streaming differ only in how the frames got there.
struct SampleTail {
  // Read `numFrames` starting at `startFrame` of the whole file into `dest`,
  // interleaved, `channels` per frame. Returns how many frames it managed.
  // Called on a background thread; may block on a disk.
  std::function<int64_t(int64_t startFrame, int numFrames, float* dest,
                        int channels)>
      read;
  int64_t totalFrames = 0; // the whole file, not just what is resident
};

// Resident audio for one attack or release sample. Interleaved, frame-major,
// which keeps a stereo voice's two channels on the same cache line.
//
// Exactly one of `frames` and `pcm16` is populated. Reading goes through
// sample(), which returns the true value either way; the voice engine's inner
// loop bypasses it and folds `pcmScale` into the voice gain instead, which is
// exact because interpolation is linear in its inputs.
struct SampleBuffer {
  std::vector<float> frames;   // Float32 storage
  std::vector<int16_t> pcm16;  // Int16 storage
  std::vector<Pcm24> pcm24;    // Int24 storage
  // What an int16 count is worth as a float. Carries both the 1/32767 and the
  // per-file peak, so a sample that peaked at -20 dBFS still uses every bit.
  float pcmScale = 1.0f;
  int numChannels = 1;
  double sampleRate = 48000.0;
  int64_t numFrames = 0;
  // Where the release begins inside THIS file, in frames; -1 when the file
  // holds no such marker. Some sets ship one recording per pipe holding the
  // attack, the sustain loop AND the release, and mark the release with a cue
  // point: the organ definition then names the same sample for both, and the
  // release is that file played from the marker rather than from its start.
  int64_t releaseCue = -1;
  // What the FILE says it sounds at: the smpl chunk's MIDIUnityNote plus its
  // MIDIPitchFraction, as a fractional concert-pitch MIDI note. Negative when
  // the file declares none. Sets whose organ definition says "the pitch is in
  // the sample" carry it nowhere else, and for a mixture -- where one
  // recording serves several pipes -- it is the only thing that puts the
  // upper pipes on the right note.
  double fileMidiNote = -1.0;
  // Loop points in frames; -1 disables looping (percussive ranks, releases).
  int64_t loopStart = -1;
  int64_t loopEnd = -1;

  // Where the streamed remainder lives, and the two rates needed to read it
  // back. Only meaningful when `tail` is set. Held as data rather than only
  // inside the reader's closure so a cache can rebuild the tail later without
  // opening the file now.
  std::string tailPath;
  double tailSrcRate = 0.0;
  double tailDstRate = 0.0;

  // The rest of the file, when only the head is resident. Null means what is
  // here is all there is, which is what every preloaded sample looks like.
  std::shared_ptr<const SampleTail> tail;
  // Length of the WHOLE file. Equal to numFrames when nothing is streamed.
  int64_t totalFrames() const {
    return tail ? tail->totalFrames : numFrames;
  }
  bool streams() const { return tail != nullptr && tail->totalFrames > numFrames; }

  // True when the audio is held as integer counts and pcmScale means
  // something. Which integer width is a separate question, asked below.
  bool compact() const { return !pcm16.empty() || !pcm24.empty(); }
  bool wide() const { return !pcm24.empty(); }
  // What one sample of one channel costs, resident. Asking compact() alone
  // cannot answer this any more: it is true of both integer widths.
  int bytesPerSample() const { return wide() ? 3 : compact() ? 2 : 4; }
  int64_t residentBytes() const {
    return static_cast<int64_t>(frames.size() * sizeof(float) +
                                pcm16.size() * sizeof(int16_t) +
                                pcm24.size() * sizeof(Pcm24));
  }

  bool loops() const {
    return loopStart >= 0 && loopEnd > loopStart && loopEnd <= numFrames;
  }
  float sample(int64_t frame, int channel) const {
    if (frame < 0 || frame >= numFrames) return 0.0f;
    const int ch = channel < numChannels ? channel : numChannels - 1;
    const auto i = static_cast<size_t>(frame * numChannels + ch);
    if (!pcm24.empty()) return static_cast<float>(pcm24[i]) * pcmScale;
    if (!pcm16.empty()) return static_cast<float>(pcm16[i]) * pcmScale;
    return frames[i];
  }
};

// How the engine gets at audio. Returns nullptr when a sample is not resident
// (still streaming in, or missing on disk) — the voice then stays silent
// rather than the engine blocking or faulting.
using SampleProvider = std::function<const SampleBuffer*(Id sampleId)>;

enum class VoicePhase { Idle, Attack, Loop, Release };

struct Voice {
  VoicePhase phase = VoicePhase::Idle;
  const SampleBuffer* buffer = nullptr;

  Id pipeId = 0;
  Id sampleId = 0;
  uint64_t noteId = 0;   // groups the voices started by one key press
  int attackId = 0;      // which attack played, for release pairing
  int attackIndex = 0;
  int attackVelocity = 64;

  double cursor = 0.0;   // fractional read position, frames
  double ratio = 1.0;    // playback speed: temperament * sample-rate conversion
  float gain = 1.0f;
  float releaseGain = 1.0f; // envelope during release crossfade
  // True once note-off has swapped in a real release sample. The 120 ms fade
  // below then does not apply: it exists to fade the attack itself when the
  // rank ships no release, and applying it to a real tail murders seconds of
  // room in a tenth of a second. A real tail plays to its end instead.
  bool releaseIsSample = false;
  // Attack->release overlap crossfade, GrandOrgue's way: note-off does not
  // cut the attack but keeps it sounding while fading out over the release's
  // own authored crossfade length, and the tail fades in from the matched
  // level over the same window. Without the overlap a tail recorded louder
  // than the sustain loop it follows steps up audibly at the swap.
  // The old attack stays readable because it lives inside its sustain loop,
  // which the preload head always covers — these reads never touch the
  // streaming ring, so one voice and one ring are enough.
  const SampleBuffer* xfadeBuf = nullptr;
  double xfadeOldCursor = 0.0;
  int64_t xfadeOldLoopStart = -1, xfadeOldLoopEnd = -1;
  int relXfadeLeft = 0;
  int relXfadeLength = 0;

  uint64_t startedAtBlock = 0; // for stealing (oldest first)
  const PipeLayer* layer = nullptr;
  // Which ENCLOSURE this voice sits behind. One bus per enclosure plus one for
  // unenclosed pipework, so the shades act only on what they actually cover.
  int busIndex = 0;
  // Which MIXER bus it speaks through — an independent axis. A rank in the
  // Swell is enclosed by the Swell shades whichever output pair the player
  // sends it to, so these two cannot share a field.
  int mixBus = 0;
  // Which windchest this pipe stands on, as an index into the engine's wind
  // table. -1 means the organ models no wind for it, and then nothing is
  // applied. The wind moves every block, so it cannot be baked into `gain`
  // and `ratio` at note-on the way the temperament is.
  int windIndex = -1;
  // What this pipe draws from its chest while it is speaking, in kilograms per
  // second. Carried on the voice because the engine is what knows which pipes
  // are actually sounding, and the wind model is what knows what that costs.
  float windFlow = 0.0f;
  // Which tremulant reaches this pipe, as an index into the engine's table,
  // and how far it moves THIS pipe. Hauptwerk states the depth per pipe: a
  // tremulant belongs to a chest, and the stops on it wobble by different
  // amounts.
  int tremIndex = -1;
  float tremAmpDepth = 0.0f;   // linear gain swing, already out of decibels
  double tremPitchDepth = 0.0; // semitones

  // Loop crossfade. On a wrap the outgoing stream keeps reading PAST the loop
  // end (that material is still in the file) while the incoming stream starts
  // at the loop start; the two are blended for `xfadeLength` frames. This is
  // what removes the click at a loop point when the two ends do not match
  // phase. GrandOrgue does the same blend, but precomputes it into a tail
  // buffer at load time — we cannot, because the length comes from the ODF
  // layer and one sample file may be shared by layers that specify different
  // lengths.
  double xfadePos = 0.0;   // outgoing read position, past the loop end
  int xfadeRemaining = 0;  // frames left in the current blend
  int xfadeLength = 0;     // 0 disables crossfading entirely

  // Loop actually in force for this voice. Normally copied from the backing
  // store (the file's smpl chunk), but a PipeLayer may override it: the same
  // sample can be shared by layers that loop it differently, so the loop
  // cannot live only on the shared buffer.
  int64_t loopStart = -1;
  int64_t loopEnd = -1;

  bool active() const { return phase != VoicePhase::Idle; }
  bool loops() const {
    return loopStart >= 0 && loopEnd > loopStart && buffer != nullptr &&
           loopEnd <= buffer->numFrames;
  }
};

// One key press asking for one pipe to speak.
struct VoiceStart {
  const Pipe* pipe = nullptr;
  const PipeLayer* layer = nullptr;
  int attackIndex = 0;
  int attackId = 0;
  int velocity = 64;
  double ratio = 1.0; // from the temperament solver
  float gain = 1.0f;
  // ODF-declared loop, overriding whatever the audio file carries.
  // -1 (the default) means "use the file's own loop".
  int64_t loopStartOverride = -1;
  int64_t loopEndOverride = -1;
  // Crossfade length at the loop wrap, in frames. 0 = hard wrap.
  int loopCrossfadeFrames = 0;
  // Windchest index and air draw; see Voice::windIndex and Voice::windFlow.
  int windIndex = -1;
  float windFlowKgPerSec = 0.0f;
  // Tremulant index and this pipe's own depths; see Voice::tremIndex.
  int tremIndex = -1;
  float tremAmpDepth = 0.0f;
  double tremPitchDepth = 0.0;
  // Force a one-shot: ignore the file's own loop as well as any override.
  // Noises, percussive ranks and releases must not sustain, and "-1 means use
  // the file's loop" cannot express that.
  bool oneShot = false;
  int busIndex = 0;  // enclosure
  int mixBus = 0;    // mixer output
};

struct EngineStats {
  int activeVoices = 0;
  int stolenVoices = 0;    // cumulative; a rising count means under-provisioned
  int startsDropped = 0;   // note-ons that found no voice at all
  int samplesMissing = 0;  // starts whose audio was not resident
};

class VoiceEngine {
public:
  ~VoiceEngine();
  // Fixed pool. maxVoices is the ceiling the perf budget is written against;
  // maxBlockFrames sizes the worker scratch buffers, so a host that later
  // hands us a bigger block falls back to inline rendering rather than
  // overrunning them.
  void prepare(double sampleRate, int maxVoices, int numChannels,
               int maxBlockFrames = 2048);
  void setSampleProvider(SampleProvider provider) { provider_ = std::move(provider); }
  void reset();

  // Start one voice. Returns its index, or -1 when nothing could be started
  // (no free voice and nothing stealable, or the audio is not resident).
  int startVoice(const VoiceStart& start, uint64_t noteId);

  // Move every voice of this key press into release. `strike` carries the
  // key-off context the release matrix selects on.
  void noteOff(uint64_t noteId, const NoteRelease& release);
  // Release only the voices of ONE pipe of a held note, leaving the rest of
  // the note sounding. This is a stop pushed in while a key is down: that
  // rank stops speaking, the others carry on, and the key is still held.
  void noteOffPipe(uint64_t noteId, Id pipeId, const NoteRelease& release);

private:
  // The body of both: one pipe of a note, or all of them.
  void releaseVoices(uint64_t noteId, Id pipeId, const NoteRelease& release);
public:

  // Mix active voices into `out` (numChannels planar buffers, additive).
  //
  // Both filters are "< 0 means every voice". They are ANDed, so one call
  // renders exactly the voices behind enclosure `busIndex` AND routed to
  // mixer bus `mixBus` — which is what lets a mix bus be assembled from
  // several enclosures, each filtered by its own shades, without a voice
  // being rendered twice or missed.
  // Allocation-free and lock-free; safe on the audio thread.
  void render(float* const* out, int numChannels, int numFrames,
              int busIndex = -1, int mixBus = -1);
  // Advance the block counter once per audio block. render() does this itself
  // when called for all voices; a caller rendering bus by bus must call it
  // exactly once instead, or voice ages drift by the number of buses.
  void beginBlock() { ++blockCounter_; }

  // What the wind is doing to each windchest this block. Indices match
  // Voice::windIndex; anything a voice points past is treated as steady, so a
  // stale index can only ever mean "no wind", never a wild read.
  //
  // Set once per block from the control side. Not baked into the voice at
  // note-on the way the temperament is, because the whole point of a wind
  // model is that it moves under a held note.
  struct WindMod {
    float ampMul = 1.0f;
    double pitchRatio = 1.0;
  };

  // What a tremulant is doing this block, as a RAMP rather than a level.
  //
  // The wind can be a level: it moves over tenths of a second, so one value a
  // block is plenty. A tremulant cannot. It wobbles at six to eight hertz, and
  // at a 256-frame block that is only about thirty updates a cycle — held
  // constant across each block it steps rather than swings, which is audible
  // on the amplitude and worse on the pitch. So the caller gives the value at
  // the start of the block and the increment per frame, and the voice adds.
  // Both fields are the SAME signed swing, in -1..1, sampled at the start of
  // the block and stepped per frame. Zero is neutral for both — they are
  // swings, not multipliers, and the voice turns each into a gain and a pitch
  // using this pipe's own depths. Defaulting one of them to 1.0, as a
  // multiplier would, silently pinned every pitch to the top of its swing.
  struct TremMod {
    float ampStart = 0.0f;
    float ampStep = 0.0f;
    double pitchStart = 0.0;
    double pitchStep = 0.0;
  };
  void setTremMods(const TremMod* mods, int count) {
    tremMods_ = mods;
    tremCount_ = count;
  }
  TremMod tremModFor(int index) const {
    if (tremMods_ == nullptr || index < 0 || index >= tremCount_) return {};
    return tremMods_[index];
  }
  void setWindMods(const WindMod* mods, int count) {
    windMods_ = mods;
    windCount_ = count;
  }
  // --- streaming --------------------------------------------------------
  // How far ahead of each voice the streamer keeps, in seconds. Generous: a
  // voice consumes one frame per output frame, so a couple of seconds of slack
  // is an enormous margin against a disk that stalls, and the memory is one
  // ring per VOICE rather than per sample.
  void setStreamSeconds(double seconds);
  double streamSeconds() const { return streamSeconds_; }
  // Underruns since prepare(): a voice reached material the streamer had not
  // fetched yet and played silence. Should be zero; anything else is a disk
  // that cannot keep up, and the player needs to be told rather than left
  // wondering why the releases sound clipped.
  int64_t streamUnderruns() const {
    return underruns_.load(std::memory_order_relaxed);
  }
  int64_t streamingVoices() const {
    return streaming_.load(std::memory_order_relaxed);
  }

  // How much air every windchest is being asked for right now, in kilograms
  // per second. Writes `count` entries; the caller owns the array.
  //
  // A pipe in its release no longer draws: the pallet has closed and what is
  // still sounding is the room, not the pipe. That is not a detail — it is why
  // the wind recovers the moment the keys come up rather than waiting for the
  // reverb to die.
  void gatherWindDemand(float* out, int count) const;

  WindMod windModFor(int index) const {
    if (windMods_ == nullptr || index < 0 || index >= windCount_) return {};
    return windMods_[index];
  }

  // Voice rendering across worker threads (ADR-012). The audio thread renders
  // one share itself and waits on the rest, so it is never idle. Below
  // `minVoicesPerThread * threads` active voices the split costs more than it
  // saves and rendering stays inline — measured, not assumed.
  //
  // Threads are started here, never inside render(): starting a thread on the
  // audio thread would be exactly the kind of stall this is meant to avoid.
  void setRenderThreads(int numThreads, int minVoicesPerThread = 32);
  int renderThreads() const { return static_cast<int>(workers_.size()) + 1; }

  const EngineStats& stats() const { return stats_; }
  int activeVoiceCount() const;
  const Voice& voice(int index) const { return voices_[static_cast<size_t>(index)]; }
  int poolSize() const { return static_cast<int>(voices_.size()); }

private:
  // Free slot, else the least musically costly voice to cut short.
  int allocateVoice();
  void renderVoice(Voice& v, float* const* out, int numChannels, int numFrames);
  // Instantiated for each resident storage type, so the inner loop never
  // tests which one it is reading. Defined in the .cpp; both instantiations
  // are reached through renderVoice().
  template <typename T>
  void renderVoiceFrom(Voice& v, size_t voiceIndex, float* const* out,
                       int numChannels, int numFrames);

  // Render voices [begin, end) of the pool into `out`. This is the unit of
  // work a thread takes; ranges never overlap, so no voice is touched twice
  // and no locking is needed on the voices themselves.
  void renderRange(size_t begin, size_t end, float* const* out, int numChannels,
                   int numFrames, int busIndex, int mixBus);
  void stopWorkers();

  std::vector<Voice> voices_;
  SampleProvider provider_;
  double sampleRate_ = 48000.0;
  int numChannels_ = 2;
  uint64_t blockCounter_ = 0;
  // Borrowed, not owned: the caller keeps the table alive across the block.
  const WindMod* windMods_ = nullptr;
  int windCount_ = 0;
  const TremMod* tremMods_ = nullptr;
  int tremCount_ = 0;

  // --- streaming --------------------------------------------------------
  // One ring per voice, parallel to `voices_`. Per voice and not per sample
  // because two voices can be playing the same release at different points —
  // the same reason the voices have their own cursors.
  //
  // Single producer, single consumer: the streamer thread writes and publishes
  // `filled`; the audio thread reads it. Nothing else is shared, so an atomic
  // on that one number is the whole synchronisation.
  struct VoiceStream {
    std::vector<float> ring;   // interleaved; capacity fixed at prepare()
    int64_t base = 0;          // file frame held at ring[0]
    std::atomic<int64_t> filled{0}; // frames valid from `base`
    std::shared_ptr<const SampleTail> tail;
    int channels = 1;
    // Set by the audio thread when it starts a streaming voice, cleared when
    // the voice ends. The streamer only touches armed slots.
    std::atomic<bool> armed{false};
    // Bumped every time the slot is re-armed, so a fill that was in flight for
    // the previous voice cannot publish frames into the new one's ring.
    std::atomic<uint64_t> generation{0};
  };
  std::vector<std::unique_ptr<VoiceStream>> streams_;
  std::thread streamer_;
  std::mutex streamMutex_;
  std::condition_variable streamCv_;
  bool streamQuit_ = false;
  double streamSeconds_ = 2.0;
  // Counted from the read path, which is const because reading a sample does
  // not change the engine — but an underrun is exactly the thing a player must
  // be told about, so it is recorded rather than swallowed.
  mutable std::atomic<int64_t> underruns_{0};
  std::atomic<int64_t> streaming_{0};

  void startStreamer();
  void stopStreamer();
  void streamerLoop();
  // Arm a voice's ring for a streaming sample, or disarm it. Audio thread.
  void armStream(size_t voiceIndex, const SampleBuffer& buf, int64_t fromFrame);
  void disarmStream(size_t voiceIndex);
  // One frame of a streaming voice, or 0 when the streamer has not reached it.
  float streamedSample(size_t voiceIndex, const SampleBuffer& buf,
                       int64_t frame, int channel) const;
  EngineStats stats_;

  // --- worker pool ---
  // Each worker owns one contiguous voice range and one scratch buffer; the
  // audio thread sums the scratches after the barrier. Summing on the audio
  // thread, rather than having workers accumulate into a shared buffer, is
  // what keeps this free of atomics on the hot path and makes the result
  // deterministic: a fixed partition summed in a fixed order gives the same
  // samples every run. It is NOT bit-identical to inline rendering, because
  // parallel partial sums group float additions differently.
  struct Worker {
    std::thread thread;
    std::vector<float> scratch; // numChannels * maxFrames, planar
    std::vector<float*> planes;
    size_t begin = 0, end = 0;
  };
  std::vector<Worker> workers_;
  int minVoicesPerThread_ = 32;
  int maxFrames_ = 0;

  // Job handshake. Workers spin on a generation counter rather than a queue:
  // one integer compare is cheaper than any queue, and the work is always the
  // same shape.
  std::mutex jobMutex_;
  std::condition_variable jobCv_;
  std::condition_variable doneCv_;
  uint64_t jobGeneration_ = 0;
  int jobsOutstanding_ = 0;
  bool shuttingDown_ = false;
  float* const* jobOut_ = nullptr;
  int jobChannels_ = 0;
  int jobFrames_ = 0;
  int jobBus_ = -1;
  int jobMixBus_ = -1;
};

} // namespace mp
