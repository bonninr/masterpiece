// Sample backing store — the bridge between an OrganModel's Sample rows and
// the JUCE-free VoiceEngine, which only knows about resident SampleBuffers.
//
// This is where JUCE belongs (ADR-010/011): reading audio off a disk is a
// backing-store concern, so the decoders live here rather than in mp_sampler,
// and the voice path stays testable without an audio stack. Per ADR-004 the
// preloaded and streaming modes differ only in how much of a file is resident
// — the engine reads the same SampleBuffer either way.
//
// Loading runs on a background thread. The audio thread reads the sample set
// through a single atomic raw pointer — no mutex, no shared_ptr refcount, no
// allocation — so a note-on during a reload sees either the whole old
// generation or the whole new one, never a torn map. Retired generations are
// kept alive until retireOldGenerations() is called from the message thread
// with audio stopped, because a voice may still hold a pointer into one.
#pragma once
#include "../mp_core/OrganModel.h"
#include "../mp_core/LoadProgress.h"
#include "../mp_sampler/VoiceEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp {

// Which sustain loop to use when a sample carries several, as organ samples
// routinely do. Hauptwerk stores loops in the WAV `smpl` chunk, never in the
// ODF (verified against the GrandOrgue Hauptwerk loader: the only loop-related
// ODF field is LoopCrossfadeLengthInSrcSampleMs, which is a crossfade length,
// not a loop point). The choice among them is a listening preference, so it is
// a load option rather than something baked in.
enum class LoopSelection {
  Longest,      // most sustain material, fewest wraps per second — the default
  Conservative, // shortest loop: least memory when streaming, more wraps
  First,        // the file's own first loop, whatever the author intended
};

struct SampleLoadReport {
  int loaded = 0;
  int missing = 0;    // referenced but not on disk
  int encrypted = 0;  // .hbw/.hbx — ADR-003, we do not decode these
  int failed = 0;     // present but unreadable
  std::vector<std::string> missingFiles;
  std::vector<std::string> failedFiles;
};

class SampleLibrary {
public:
  SampleLibrary();
  ~SampleLibrary();

  // Load the samples the model's PIPES actually reference, beneath
  // `organRootDir`. Not every Sample row is reachable: a demo set lists the
  // whole instrument in its Sample table but ships only some ranks, so Nancy
  // has 61927 sample rows and roughly 13000 files on disk. Walking the table
  // instead of the pipework spent minutes failing to stat files belonging to
  // ranks that have no pipes.
  // `maxFramesPerSample` is the MINIMUM preload head, not a cap: the read is
  // always extended to cover the sustain loop, because a head that stops short
  // of the loop makes the note die rather than merely preloading less.
  // 0 means the whole file (Preloaded mode).
  // `progress`, when given, is updated as files are decoded and is polled for
  // cancellation between them. A cancelled load returns whatever it had
  // managed so far; the caller decides that this is not an organ.
  // `onlyRanks`, when given, loads ONLY those ranks. This is not a smaller
  // organ, it is an incomplete one: every stop outside the set is silent, and
  // nothing in the engine will say so. It exists because a load is the slow
  // part of trying anything -- minutes for a large set -- and a test that
  // needs four stops should not pay for four hundred. Callers say so
  // explicitly; nothing turns it on by itself.
  SampleLoadReport loadAll(const OrganModel& model, const std::string& organRootDir,
                           int64_t maxFramesPerSample = 0,
                           LoopSelection loopSelection = LoopSelection::Longest,
                           LoadProgress* progress = nullptr,
                           const std::unordered_set<Id>* onlyRanks = nullptr);

  // How resident audio is stored. An organ IS its sample data, so this is the
  // largest single lever on how big a set a machine can hold: Int16 halves the
  // footprint against the decoder's native float. Takes effect on the next
  // load — converting a live set would have to be done underneath sounding
  // voices, and there is no reason to.
  // Fold a stereo set down to one channel as it is read. Halves everything
  // that follows and costs the recording's stereo image, which is a real
  // loss on a set recorded in a building -- and the difference between
  // loading and not loading on a small machine. The streamed tail is folded
  // the same way, or the splice from head to tail would step from a downmix
  // to a bare left channel.
  void setLoadMono(bool on) { loadMono_ = on; }
  bool loadMono() const { return loadMono_; }

  void setStorage(SampleStorage s) { storage_ = s; }
  SampleStorage storage() const { return storage_; }

  // Stream the tails of RELEASE samples from disk instead of holding them.
  //
  // Releases are the samples worth streaming and the only ones worth it. They
  // run several seconds, are played once from the start, straight through, and
  // are never looped — so a ring that runs ahead of the voice is exactly the
  // right structure. An attack cannot be treated the same way: its sustain
  // loop has to be resident or the note does not sustain at all, and the loop
  // is most of the file.
  //
  // Off by default. It changes the audio path, and nobody has listened to it
  // yet; the preloaded path is the one the measurements in docs/ were taken on.
  void setStreamReleases(bool on) { streamReleases_ = on; }
  bool streamReleases() const { return streamReleases_; }
  // How much of a streamed release stays resident. It has to cover the time
  // between a note-off and the streamer's first fill, with room to spare.
  void setStreamHeadFrames(int64_t frames) { streamHead_ = std::max<int64_t>(4096, frames); }
  int64_t streamHeadFrames() const { return streamHead_; }
  // How many samples are held only in part, and how many frames that saved.
  size_t streamedCount() const;
  int64_t streamedBytesSaved() const;

  // A provider to hand VoiceEngine::setSampleProvider. Lock-free and
  // allocation-free: safe to call from the audio thread.
  SampleProvider provider() const;

  // How many worker threads decode in parallel. Loading is I/O bound, so more
  // threads than cores still helps; 0 picks a sensible default.
  void setLoadThreads(int n) { loadThreads_ = n; }

  size_t residentCount() const;
  int64_t residentBytes() const;

  // Drop every generation but the live one. Message thread only, and only with
  // audio stopped: sounding voices hold raw pointers into retired generations.
  void retireOldGenerations();
  void clear();

private:
  // One immutable generation of the sample set. Swapping the pointer is how a
  // reload becomes visible without ever mutating what the audio thread reads.
  using Store = std::unordered_map<Id, std::shared_ptr<SampleBuffer>>;

  void publish(std::shared_ptr<const Store> next);

  // Static, so everything it depends on arrives as an argument: `loadMono`
  // is the member of the same name, passed rather than read.
  static bool readInto(juce::AudioFormatReader& reader, SampleBuffer& out,
                       int64_t maxFrames, LoopSelection selection,
                       SampleStorage storage, bool loadMono);
  // Attach a tail that reads the rest of `path` on demand. The reader is
  // opened lazily, on the streaming thread, and kept for the buffer's life.
  void attachTail(SampleBuffer& out, const std::string& path,
                  int64_t totalFrames) const;
  // Sustain loop from the WAV 'smpl' chunk, applied to what is resident.
  static void readLoopPoints(const juce::AudioFormatReader& reader,
                             SampleBuffer& out, LoopSelection selection);
  // Choose one loop from the metadata, judged against `limitFrames`.
  static void pickLoop(const juce::AudioFormatReader& reader,
                       int64_t limitFrames, LoopSelection selection,
                       int64_t& outStart, int64_t& outEnd);
  // Where the chosen loop ends in the FULL file — this is what decides how
  // much has to be preloaded for a note to sustain.
  static int64_t selectLoopEnd(const juce::AudioFormatReader& reader,
                               int64_t totalFrames, LoopSelection selection);

  juce::AudioFormatManager formats_;
  int loadThreads_ = 0;
  SampleStorage storage_ = SampleStorage::Float32;
  bool loadMono_ = false;
  bool streamReleases_ = false;
  int64_t streamHead_ = 48000; // one second
  // Files a streamed buffer may need to reopen. Kept here so a tail outlives
  // the load that created it.
  mutable std::mutex readerMutex_;
  // What the audio thread reads. Always points at one of `generations_`.
  std::atomic<const Store*> live_{nullptr};
  // Ownership of every generation ever published, newest last. Only the
  // message thread touches this, under the mutex.
  mutable std::mutex publishMutex_;
  std::vector<std::shared_ptr<const Store>> generations_;
};

} // namespace mp
