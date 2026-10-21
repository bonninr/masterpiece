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
#include "SimdRun.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
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
// Int20 is the same idea one notch narrower: twenty bits a sample, packed so
// that two samples take five bytes. It costs two and a half bytes a sample, a
// sixth less than Int24, and quantises against each file's own peak, so a
// quiet stop keeps every bit it is given.
//
// Float32 remains for a set that is genuinely float, and because processing
// happens in float regardless -- that is where 32 bits belongs.
//
// An 8-bit width was built and withdrawn: it held about 32 dB, which is
// audible hiss under a quiet stop.
//
// The order matters: settings files store a bit WIDTH, not this ordinal, but
// tests pin it so a reordering cannot pass unnoticed. A new width goes at the
// END of the list: the ordinal is written into the sample cache's
// fingerprint, and a width inserted in the middle would give an old cache the
// fingerprint of a different one.
enum class SampleStorage { Float32, Int24, Int16, Int20 };

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

// Twenty bits a sample, packed. The interleaved samples of a buffer form one
// little-endian bit stream: sample i occupies bits [20i, 20i+20), so two
// samples take five bytes and the byte a sample starts at is (i*5)>>1.
//
// NO conversion operator, on purpose. Any code that walks this as an array of
// samples and converts what it finds does not compile, which is what catches
// the places that need the decoder instead.
struct Pcm20 {
  unsigned char byte;
};
static_assert(sizeof(Pcm20) == 1, "20-bit storage is one byte a sample, packed");

// The bytes a buffer of `count` interleaved samples takes: the samples
// themselves, plus four bytes of padding, so the four-byte read of the last
// one never runs past the end. The padding is zeroed.
constexpr size_t pcm20Bytes(size_t count) { return (count * 20 + 7) / 8 + 4; }

// Read sample `i`: four bytes at its byte offset, little-endian and
// unaligned, of which the sample is the twenty bits at shift (i&1)*4.
inline int32_t pcm20At(const Pcm20* data, size_t i) {
  const size_t o = (i * 5) >> 1;
  const uint32_t s = static_cast<uint32_t>((i & 1) * 4);
  uint32_t raw = 0;
  std::memcpy(&raw, data + o, 4);
  const uint32_t v = (raw >> s) & 0xFFFFFu;
  return static_cast<int32_t>((v ^ 0x80000u) - 0x80000u);
}

// Write sample `i`: the inverse, a read-modify-write of the three bytes the
// sample touches. The bits of the neighbours in those bytes are kept.
inline void pcm20Put(Pcm20* data, size_t i, int32_t value) {
  const size_t o = (i * 5) >> 1;
  const uint32_t s = static_cast<uint32_t>((i & 1) * 4);
  uint32_t raw = 0;
  std::memcpy(&raw, data + o, 4);
  const uint32_t v = static_cast<uint32_t>(value) & 0xFFFFFu;
  const uint32_t merged = (raw & ~(0xFFFFFu << s)) | (v << s);
  data[o + 0].byte = static_cast<unsigned char>(merged & 0xFF);
  data[o + 1].byte = static_cast<unsigned char>((merged >> 8) & 0xFF);
  data[o + 2].byte = static_cast<unsigned char>((merged >> 16) & 0xFF);
}

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
  std::vector<Pcm20> pcm20;    // Int20 storage: packed, two and a half bytes a sample
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
  bool compact() const { return !pcm16.empty() || !pcm24.empty() || !pcm20.empty(); }
  bool wide() const { return !pcm24.empty(); }
  // True when the samples are packed twenty bits: neither a plain integer
  // count nor a 24-bit one, so the engine asks for it on its own.
  bool packed20() const { return !pcm20.empty(); }
  // What one sample of one channel costs, resident. Asking compact() alone
  // cannot answer this any more: it is true of both integer widths. Twenty
  // bits is two and a half bytes, so the answer is not a whole number.
  double bytesPerSampleExact() const {
    if (packed20()) return 2.5;
    return wide() ? 3.0 : compact() ? 2.0 : 4.0;
  }
  int64_t residentBytes() const {
    return static_cast<int64_t>(frames.size() * sizeof(float) +
                                pcm16.size() * sizeof(int16_t) +
                                pcm24.size() * sizeof(Pcm24) +
                                pcm20.size() * sizeof(Pcm20));
  }

  bool loops() const {
    return loopStart >= 0 && loopEnd > loopStart && loopEnd <= numFrames;
  }
  // As a release read from its file's marker: nothing to play when the file
  // has no marker and its loop runs to its last frame (a GrandOrgue attack
  // file offered as its own release). A file with no loop at all is a
  // recorded release and holds all of it: Sonus Paradisi's separate release
  // files are that, and 0.7.6 left all 3847 of Friesach's out (#218).
  bool releaseHoldsNothing() const {
    return releaseCue <= 0 && loopEnd > 0 && loopEnd >= totalFrames();
  }
  float sample(int64_t frame, int channel) const {
    if (frame < 0 || frame >= numFrames) return 0.0f;
    const int ch = channel < numChannels ? channel : numChannels - 1;
    const auto i = static_cast<size_t>(frame * numChannels + ch);
    if (!pcm20.empty()) return static_cast<float>(pcm20At(pcm20.data(), i)) * pcmScale;
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
  // The attack's speed relative to the release's, while it fades out under
  // it: the release plays at its own recorded pitch, the attack at its own.
  double xfadeRatioScale = 1.0;
  int64_t xfadeOldLoopStart = -1, xfadeOldLoopEnd = -1;
  int relXfadeLeft = 0;
  int relXfadeLength = 0;

  uint64_t startedAtBlock = 0; // for stealing (oldest first)
  // The block its note-off came in. Releasing voices are stolen by this, the
  // longest decayed first: by startedAtBlock, a pedal note held through a
  // whole passage became the first one taken the moment it was let go, and
  // its release was cut on every note-off while the pool was full (#320).
  uint64_t releasedAtBlock = 0;
  // The frame the key went down, for how long it was held at the release:
  // the release a set recorded after a short note is not the one it recorded
  // after a held one.
  int64_t startedAtFrame = 0;
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
  // Frames of the next rendered block to leave before the voice begins; see
  // VoiceStart::startOffset.
  int delayFrames = 0;
  int windIndex = -1;
  // What this pipe draws from its chest while it is speaking, in kilograms per
  // second. Carried on the voice because the engine is what knows which pipes
  // are actually sounding, and the wind model is what knows what that costs.
  float windFlow = 0.0f;
  // The wind this pipe last spoke with. Once its valve closes the chest no
  // longer reaches it -- what sounds is the release and the room -- so a
  // releasing voice keeps this instead of following the pressure as it
  // recovers, which slid every tail in pitch the moment a chord let go.
  float heldWindAmp = 1.0f;
  double heldWindPitch = 1.0;
  // Which tremulant reaches this pipe, as an index into the engine's table,
  // and how far it moves THIS pipe. Hauptwerk states the depth per pipe: a
  // tremulant belongs to a chest, and the stops on it wobble by different
  // amounts.
  int tremIndex = -1;
  float tremAmpDepth = 0.0f;   // linear gain swing, already out of decibels
  double tremPitchDepth = 0.0; // semitones
  // Which wind sway moves this pipe's pitch, as an index into the table
  // setSwayRatios() hands over; -1 for none.
  int swayIndex = -1;
  // Which rank's live level reaches this pipe, as an index into the table
  // setLiveGains() hands over; -1 for none (a noise). See LiveLevels.
  int rankIndex = -1;
  // The attack's emphasis: a level that starts at 1 + attackBoost and falls
  // in a straight line to 1 over attackTotal frames, attackLeft of them to
  // go. 0 left is no emphasis, and then nothing is done for it.
  float attackBoost = 0.0f;
  int attackTotal = 0;
  int attackLeft = 0;

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
  // Frames in a row this release has been heard below the engine's cutoff.
  int quietFrames = 0;

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
  // How many frames into the next rendered block the voice begins: the place
  // of its note-on in that block, when the note came with one.
  int startOffset = 0;
  // Force a one-shot: ignore the file's own loop as well as any override.
  // Noises, percussive ranks and releases must not sustain, and "-1 means use
  // the file's loop" cannot express that.
  bool oneShot = false;
  int busIndex = 0;  // enclosure
  int mixBus = 0;    // mixer output
  int swayIndex = -1;  // wind sway; see Voice::swayIndex
  int rankIndex = -1;  // live level; see Voice::rankIndex
  // The attack's emphasis; see Voice::attackBoost. 0 frames is none.
  float attackBoost = 0.0f;
  int attackFrames = 0;
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
  // The engine at rest: the render workers and the streaming thread stopped
  // and joined, every voice silent. For a device that is about to change, so
  // nothing is half way through a block when prepare() rebuilds the pools
  // (#267); prepare() starts both again.
  void quiesce();

  // Start one voice. Returns its index, or -1 when nothing could be started
  // (no free voice and nothing stealable, or the audio is not resident).
  int startVoice(const VoiceStart& start, uint64_t noteId);

  // The vector unit the straight runs use, found when the engine is prepared
  // (see SimdRun.h). Off renders every frame on the per-frame path, which is
  // what a comparison or a suspicion of the vector code wants.
  // Off unless asked for: the vector path is new, and the player turns it on
  // (Settings, "Faster audio engine") until it has proved itself.
  // A release tail ends once it has stayed below this output level -- a
  // linear amplitude, 0 for never -- for a twentieth of a second. A tail
  // only fades, so one that is that far down stays there; and a passage on
  // every stop leaves hundreds of them sounding at once, each costing what a
  // note costs. Read at each block from sixteen of the frames the voice is
  // about to play.
  void setReleaseCutoff(float level) { releaseCutoff_ = level > 0.0f ? level : 0.0f; }
  float releaseCutoff() const { return releaseCutoff_; }
  // Release tails ended below the cutoff, since the engine was made.
  uint64_t tailsEnded() const { return tailsEnded_.load(std::memory_order_relaxed); }

  void setSimd(bool on) {
    simdWanted_ = on;
    isa_ = on ? simd::detect() : simd::Isa::None;
  }
  // A particular unit, for the equivalence test: it must be one this
  // machine can run (simd::available()).
  void setSimdIsa(simd::Isa isa) {
    simdWanted_ = isa != simd::Isa::None;
    isa_ = isa;
  }
  simd::Isa simd() const { return isa_; }
  // Voice-frames rendered by the vector code since prepare(). Counted once
  // per run, not per frame, so it costs nothing to keep.
  uint64_t simdFrames() const { return simdFrames_.load(std::memory_order_relaxed); }

  // Move every voice of this key press into release. `strike` carries the
  // key-off context the release matrix selects on.
  void noteOff(uint64_t noteId, const NoteRelease& release);
  // Every voice still sounding goes into its release, whatever note it
  // belongs to: Panic's last word, for a voice whose note-off was lost and
  // that no key the program knows of is holding (#276). A note for which
  // `keep(noteId)` is true sounds on: one a drawn switch holds, such as a
  // motor noise, which would otherwise stay silent until the switch moved.
  template <class Keep>
  void releaseAll(Keep keep) {
    for (size_t i = 0; i < scanEnd_; ++i) {
      const Voice& v = voices_[i];
      if (!v.active() || v.phase == VoicePhase::Release || keep(v.noteId)) continue;
      releaseVoices(v.noteId, 0, NoteRelease{});
    }
  }
  void releaseAll() {
    releaseAll([](uint64_t) { return false; });
  }
  // Release only the voices of ONE pipe of a held note, leaving the rest of
  // the note sounding. This is a stop pushed in while a key is down: that
  // rank stops speaking, the others carry on, and the key is still held.
  void noteOffPipe(uint64_t noteId, Id pipeId, const NoteRelease& release);

private:
  simd::Isa isa_ = simd::Isa::None;
  bool simdWanted_ = false;
  std::atomic<uint64_t> simdFrames_{0};
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
  // Every enclosure bus of a block at once: busOut[b] is numChannels planar
  // buffers for bus b, added into. One pass over the pool and one hand-off
  // to the threads for the whole block, where render() per bus made one of
  // each per bus. The voices, their shares and the order things are summed
  // in are those of render() per bus, so the output is the same.
  // More buses than setBusCount() prepared go bus by bus.
  void renderBuses(float* const* const* busOut, int numBuses, int numChannels, int numFrames,
                   int mixBus = -1);
  // How many buses renderBuses() is given, so each thread has room for a
  // share of every one. Off the audio thread; restarts the threads.
  void setBusCount(int buses);
  // Advance the block counter once per audio block. render() does this itself
  // when called for all voices; a caller rendering bus by bus must call it
  // exactly once instead, or voice ages drift by the number of buses.
  // A caller that counts blocks this way owns the count from then on: render()
  // stops counting its own whole-block calls. Both counting doubled the clock
  // on every organ rendered without enclosures, and every key's hold time
  // with it: a note held 2 s chose the release written for 4 s.
  void beginBlock(int numFrames = 0) {
    callerCountsBlocks_ = true;
    ++blockCounter_;
    framesRendered_ += numFrames;
  }

  // Milliseconds since a pipe last let go -- its voice went into release --
  // or INT64_MAX if it has not sounded. An attack can be chosen by it: a
  // quick repetition of a note speaks differently from one after a rest.
  int64_t msSincePipeClosed(Id pipeId) const;

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
  // The wind sway for the next block: a pitch ratio per sway, multiplied on
  // once per voice per block like the wind's. Null while the sway is off.
  void setSwayRatios(const double* ratios, int count) {
    swayRatios_ = ratios;
    swayCount_ = count;
  }
  // The live levels for the next block: a gain per rank and per output pair,
  // multiplied on once per voice per block. Null while every fader is at
  // full, and then no voice does anything for them.
  void setLiveGains(const float* rankGains, int rankCount, const float* pairGains, int pairCount) {
    liveRankGains_ = rankGains;
    liveRankCount_ = rankCount;
    livePairGains_ = pairGains;
    livePairCount_ = pairCount;
  }
  float liveGainFor(const Voice& v) const {
    float g = 1.0f;
    if (v.rankIndex >= 0 && v.rankIndex < liveRankCount_) g = liveRankGains_[v.rankIndex];
    if (v.mixBus >= 0 && v.mixBus < livePairCount_) g *= livePairGains_[v.mixBus];
    return g;
  }
  // --- streaming --------------------------------------------------------
  // How far ahead of each voice the streamer keeps, in seconds. Generous: a
  // voice consumes one frame per output frame, so a couple of seconds of slack
  // is an enormous margin against a disk that stalls, and the memory is one
  // ring per VOICE rather than per sample.
  void setStreamSeconds(double seconds);
  // Whether the next prepare() gives each voice a ring for a streamed
  // release tail. Without streaming nothing ever reads one, and at 48 kHz a
  // ring is three quarters of a megabyte a voice.
  void setStreamRings(bool on) { streamRings_ = on; }
  // What one voice of the pool holds in memory, with or without its ring.
  static int64_t bytesPerVoice(double sampleRate, double streamSeconds, bool rings);
  double streamSeconds() const { return streamSeconds_; }
  // Underruns since prepare(): a voice reached material the streamer had not
  // fetched yet and played silence. Should be zero; anything else is a disk
  // that cannot keep up, and the player needs to be told rather than left
  // wondering why the releases sound clipped.
  // What became of each key release, for --log-releases: which of the pipe's
  // releases was chosen, how long it is, where it starts and how it joins the
  // attack. Written on the audio thread into a fixed ring and taken on the
  // message thread; when the reader falls behind, events are dropped and
  // counted, never waited for.
  struct ReleaseEvent {
    Id pipeId = 0;
    Id attackSampleId = 0;
    Id releaseSampleId = 0;    // 0: no release sample, the voice just fades
    int64_t heldMs = 0;
    int velocity = 64;
    int releaseCount = 0;      // releases the pipe offers
    int chosen = -1;           // which of them, -1 for none
    int64_t attackFrame = 0;   // where the attack was when the key let go
    double attackRate = 0.0;
    bool attackLooping = false;
    int64_t releaseFrames = 0; // the whole release file
    double releaseRate = 0.0;
    int64_t startFrame = 0;    // where in it playback starts
    bool cueWanted = false;    // the organ says to start at the file's marker
    bool cueFound = false;     // and the file has one
    double crossfadeMs = 0.0;
    bool streamed = false;
    int64_t residentFrames = 0;
  };
  void setReleaseLogging(bool on) { logReleases_.store(on, std::memory_order_release); }
  // Moves the events recorded since the last call into `out`, and returns how
  // many were dropped meanwhile.
  int64_t takeReleaseEvents(std::vector<ReleaseEvent>& out);

  int64_t streamUnderruns() const {
    return underruns_.load(std::memory_order_relaxed);
  }
  int64_t streamingVoices() const {
    return streaming_.load(std::memory_order_relaxed);
  }
  // How many times the streamer has gone over the voices looking for a ring
  // to fill. It stands still while nothing streams.
  int64_t streamerPasses() const {
    return streamPasses_.load(std::memory_order_relaxed);
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

  // Run on each render worker, on its own thread, before the next share it
  // renders: what the platform asks of a thread that works to the audio
  // deadline (on Apple, joining the audio device's workgroup, so the
  // scheduler keeps it on a performance core). Set again to run it again, on
  // every worker; empty does nothing. The engine knows nothing of what it
  // does. One atomic load per share while nothing changes.
  void setWorkerHook(std::function<void()> hook);

  const EngineStats& stats() const { return stats_; }
  // How many voices sound through one bus, releases included.
  int voicesOnBus(int bus) const {
    int n = 0;
    for (size_t i = 0; i < scanEnd_ && i < voices_.size(); ++i)
      if (voices_[i].active() && voices_[i].busIndex == bus) ++n;
    return n;
  }
  int activeVoiceCount() const;
  const Voice& voice(int index) const { return voices_[static_cast<size_t>(index)]; }
  int poolSize() const { return static_cast<int>(voices_.size()); }

  // The most voices that sound at once, at or below the pool's size; 0 means
  // the whole pool. A voice started at the limit takes the place of one dying
  // away, or of the quietest, as it does when the pool itself is full. Set
  // from the audio thread or with it stopped.
  void setPolyphony(int voices) { polyphony_ = voices > 0 ? voices : 0; }
  int polyphony() const { return polyphony_; }

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

  // Render the voices at these pool indices into `out`. This is the unit of
  // work a thread takes; the shares never overlap, so no voice is touched
  // twice and no locking is needed on the voices themselves.
  void renderList(const uint32_t* list, size_t count, float* const* out, int numChannels,
                  int numFrames);
  void stopWorkers();

  std::vector<Voice> voices_;
  // Finding a slot for a voice. Every slot below the hint was in use when
  // render() last looked; liveCount_ is the voices in use then plus the slots
  // given out since.
  size_t freeHint_ = 0;
  int liveCount_ = 0;
  // One past the highest slot that may hold a voice. Voices are given the
  // lowest free slots, so they sit together at the front of the pool, and
  // every pass that looks for them -- each render, each note-off, the wind --
  // stops here. They walked the whole pool: with ten thousand voices allowed,
  // tens of thousands of empty slots a block for a few hundred sounding.
  size_t scanEnd_ = 0;
  float releaseCutoff_ = 0.0f;
  std::atomic<uint64_t> tailsEnded_{0};
  int polyphony_ = 0;
  // The voices to take when there is no free slot, in the order to take
  // them, made once a block: stage 1 is the voices dying away, 2 the rest.
  std::vector<uint32_t> stealList_;
  size_t stealNext_ = 0;
  int stealStage_ = 0;
  uint64_t stealBlock_ = 0;
  SampleProvider provider_;
  double sampleRate_ = 48000.0;
  int numChannels_ = 2;
  uint64_t blockCounter_ = 0;
  // Frames rendered so far, the clock for msSincePipeClosed; and when each
  // pipe last let go, in those frames.
  int64_t framesRendered_ = 0;
  bool callerCountsBlocks_ = false;  // set by beginBlock(); see there
  std::unordered_map<Id, int64_t> pipeClosedAt_;
  // Borrowed, not owned: the caller keeps the table alive across the block.
  const WindMod* windMods_ = nullptr;
  int windCount_ = 0;
  const TremMod* tremMods_ = nullptr;
  int tremCount_ = 0;
  const double* swayRatios_ = nullptr;
  int swayCount_ = 0;
  const float* liveRankGains_ = nullptr;
  int liveRankCount_ = 0;
  const float* livePairGains_ = nullptr;
  int livePairCount_ = 0;

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
    // Whether the last read found the ring short, so one shortfall counts
    // once however many frames and taps it lasts. Set from the const read
    // path, hence mutable.
    mutable std::atomic<bool> starved{false};
  };
  std::vector<std::unique_ptr<VoiceStream>> streams_;
  std::thread streamer_;
  std::mutex streamMutex_;
  std::condition_variable streamCv_;
  bool streamQuit_ = false;
  double streamSeconds_ = 2.0;
  bool streamRings_ = true;
  // Counted from the read path, which is const because reading a sample does
  // not change the engine — but an underrun is exactly the thing a player must
  // be told about, so it is recorded rather than swallowed.
  mutable std::atomic<int64_t> underruns_{0};
  static constexpr size_t kReleaseRing = 1024;
  std::array<ReleaseEvent, kReleaseRing> releaseRing_{};
  std::atomic<size_t> releaseHead_{0}; // written by the audio thread
  std::atomic<size_t> releaseTail_{0}; // written by the reader
  std::atomic<int64_t> releaseDropped_{0};
  std::atomic<bool> logReleases_{false};
  std::atomic<int64_t> streaming_{0};
  std::atomic<int64_t> streamPasses_{0};

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
  // The voices one render() call plays, by pool index, found in one pass and
  // shared out in equal parts. Each worker takes one part and one scratch
  // buffer; the audio thread sums the scratches after the barrier. Summing on
  // the audio thread, rather than having workers accumulate into a shared
  // buffer, makes the result deterministic: fixed shares summed in a fixed
  // order give the same samples every run. It is NOT bit-identical to inline
  // rendering, because parallel partial sums group float additions
  // differently.
  std::vector<uint32_t> jobList_;
  struct Worker {
    std::thread thread;
    std::vector<float> scratch; // buses * numChannels * maxFrames, planar
    std::vector<float*> planes;
    size_t begin = 0, end = 0;  // its share of jobList_
    // Its share of each bus's list, for renderBuses().
    std::vector<size_t> busBegin, busEnd;
    // Moved on by the audio thread to hand this worker a share, and only
    // this worker: a render that needs two threads wakes one.
    std::atomic<uint32_t> ticket{0};
    std::condition_variable wake;
  };
  std::vector<std::unique_ptr<Worker>> workers_;
  // setWorkerHook(): the hook, and how many times it was set. A worker takes
  // the lock only when the count moved.
  std::mutex workerHookMutex_;
  std::function<void()> workerHook_;
  std::atomic<uint32_t> workerHookGeneration_{0};
  int minVoicesPerThread_ = 32;
  int maxFrames_ = 0;

  // Job handshake. A worker looks at its ticket for a few microseconds before
  // it sleeps on its condition variable, and the audio thread looks at the
  // count of shares outstanding the same way: the renders of one block follow
  // each other more closely than a thread can be put to sleep and woken.
  std::mutex jobMutex_;
  std::condition_variable doneCv_;
  std::atomic<int> jobsOutstanding_{0};
  std::atomic<bool> shuttingDown_{false};
  int jobChannels_ = 0;
  int jobFrames_ = 0;
  // A renderBuses() job: how many buses, each with its own list.
  int jobBuses_ = 0;
  std::vector<std::vector<uint32_t>> busLists_;
  int busCount_ = 1;
  int lastThreads_ = 1;
  // The audio thread's wait for its workers, shared by both kinds of job.
  void waitForWorkers();
  // Whether the threads look before they sleep, and for how many more
  // renders they do not: see kSpinHoldoffRenders.
  std::atomic<bool> spinning_{true};
  int spinHoldoff_ = 0;
  int spinMisses_ = 0;
};

} // namespace mp
