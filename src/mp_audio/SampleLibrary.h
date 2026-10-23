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
#include "../mp_core/SparseLoading.h"
#include "../mp_sampler/VoiceEngine.h"
#include "../mp_sampler/DiskProbe.h"
#include "../mp_archive/OrganArchive.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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
  int wanted = 0;     // what the pipework asked for, of which `loaded` arrived
  int licensed = 0;   // left out: the publisher's licence is not confirmed
  int failed = 0;     // present but unreadable
  int standIns = 0;   // standard-component noises that were missing, played as generated ones
  std::vector<std::string> missingFiles;
  std::vector<std::string> failedFiles;
};

class SampleLibrary {
public:
  // Raise the process's limit on open files as far as the system allows, and
  // size the pool of streamed tails that may stay open to it. Once, at start.
  // Returns how many tails may be open at a time.
  static size_t raiseOpenFileLimit();
  // How many streamed tails hold their file open right now.
  static size_t openTailFiles();
  static void setOpenTailLimitForTesting(size_t limit);

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

  // Which of each pipe's releases are loaded. Many sets record a short and a
  // long release per pipe, chosen by how long the key was held; one of them
  // loads faster and holds less. None: a note fades from where it is.
  enum class ReleaseChoice { All = 0, Longest = 1, Shortest = 2, None = 3 };
  void setReleaseChoice(ReleaseChoice c) { releaseChoice_ = c; }
  ReleaseChoice releaseChoice() const { return releaseChoice_; }
  // Leaves in each layer only the releases `choice` keeps. The one kept
  // answers a key held for any time, since the others are gone.
  static void pruneReleases(OrganModel& model, ReleaseChoice choice);
  // Each release kept up to this length after its start, in milliseconds,
  // fading out over its last quarter; 0 keeps it whole. Read only that far:
  // a shortened release is held in memory, never streamed.
  void setReleaseMaxMs(int ms) { releaseMaxMs_ = std::max(0, ms); }
  int releaseMaxMs() const { return releaseMaxMs_; }

  // Convert sample data to this rate as it is read. 0 keeps whatever rate
  // each file already has, which is what every set did before this existed.
  //
  // A 96 kHz set played through a 48 kHz device is held at twice the size it
  // needs and resampled once per voice per note; converting at load pays for
  // it once. It is a real conversion, so it is also a real decision: nothing
  // above the target's Nyquist survives it.
  void setLoadSampleRate(double hz) { loadRate_ = hz > 0.0 ? hz : 0.0; }
  double loadSampleRate() const { return loadRate_; }

  void setStorage(SampleStorage s) { storage_ = s; }
  SampleStorage storage() const { return storage_; }

  // Hold the samples compressed (#447): every 16- and 24-bit sample of one
  // or two channels is coded split Rice once read, and the voices decode it
  // as they play. Lossless: what plays is what plain storage plays, to the
  // bit. The sample cache holds them coded too. Takes effect on the next load.
  void setCodeSamples(bool on) { codeSamples_ = on; }
  bool codeSamples() const { return codeSamples_; }
  // The samples loaded that are held coded.
  size_t codedCount() const;

  // How many notes of a rank are loaded: 1 for every note, 2 for every
  // second, 3 for every third. A note that is not loaded plays its nearest
  // loaded neighbour's samples, retuned to its own pitch, so the samples it
  // would have played are never read.
  //
  // It is part of the cache fingerprint: a cache of a full load and a cache of
  // a sparse one hold different samples and must never be mistaken for each
  // other.
  void setSparseEvery(int every) { sparseEvery_ = every < 1 ? 1 : every; }
  int sparseEvery() const { return sparseEvery_; }

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
  // Whether samples whose publisher requires a licence are loaded: only once
  // the player has confirmed holding it, for this organ.
  void setLicenceConfirmed(bool on) { licenceConfirmed_ = on; }
  bool streamReleases() const { return streamReleases_; }
  // How much of a streamed release stays resident. It has to cover the time
  // between a note-off and the streamer's first fill, with room to spare.
  void setStreamHeadFrames(int64_t frames) { streamHead_ = std::max<int64_t>(4096, frames); }
  int64_t streamHeadFrames() const { return streamHead_; }
  // Or a share of each release, in percent of its length (#120: a disk that
  // cannot keep up is given more of every release before it has to). The
  // fixed head is the floor either way; 0 holds only that.
  void setStreamHeadPercent(int percent) { streamHeadPercent_ = std::clamp(percent, 0, 99); }
  int streamHeadPercent() const { return streamHeadPercent_; }
  int64_t streamHeadFor(int64_t frames) const {
    return std::max(streamHead_, frames * streamHeadPercent_ / 100);
  }
  // How many samples are held only in part, and how many frames that saved.
  size_t streamedCount() const;
  int64_t streamedBytesSaved() const;
  // The streamed samples' tails, to time reads of: valid while this library
  // holds the samples it holds now.
  std::vector<TailToTime> streamedTails() const;


  // ---- the sample cache ------------------------------------------------
  //
  // Loading decodes every file and converts it; the result depends only on
  // the files and the settings, so it is worth keeping. A cache turns the
  // second load of the same organ into a sequential read.
  enum class CacheMode { Off, Single, PerOrgan };

  // Where cache files live. Empty disables caching however CacheMode is set.
  void setCacheDir(const std::string& dir) { cacheDir_ = dir; }
  const std::string& cacheDir() const { return cacheDir_; }

  // Off, one cache replaced as organs change, or one per organ. Single is the
  // default: these files are gigabytes, and a cache per organ per settings
  // combination fills a disk without ever being asked to.
  void setCacheMode(CacheMode m) { cacheMode_ = m; }
  CacheMode cacheMode() const { return cacheMode_; }

  // Identifies WHICH organ, for the filename, and whether the definition has
  // changed since the cache was written. Both come from the caller because the
  // library is handed a model, not a path.
  void setCacheIdentity(const std::string& organId, const std::string& odfStamp) {
    cacheOrganId_ = organId;
    cacheOdfStamp_ = odfStamp;
  }

  // Bytes read from a cache on the last load, and written to one. Zero for
  // both means the load did it the long way.
  int64_t cacheBytesRead() const { return cacheRead_; }
  int64_t cacheBytesWritten() const { return cacheWritten_.load(); }
  // The cache is written after the organ goes live, on a thread of its own:
  // writing it first kept a loaded organ silent for as long as a disk takes
  // to write gigabytes, which looked like a hang (#120). True while it runs.
  bool cacheWriting() const { return cacheWriting_.load(); }
  // How far the write has got, 0..1, or -1 before it knows its size.
  double cacheWriteFraction() const {
    const int64_t total = cacheTotal_.load();
    return total > 0 ? static_cast<double>(cacheDone_.load()) / static_cast<double>(total) : -1.0;
  }
  // The bytes of samples the cache write has written, and has to write.
  int64_t cacheBytesDone() const { return cacheDone_.load(); }
  int64_t cacheBytesTotal() const { return cacheTotal_.load(); }
  // How the last cache write of this load ended, for the status line: a
  // write that fails removes its part file, and the next load reads the
  // samples again (#519).
  enum class CacheFailure { None, Failed, DiskFull };
  CacheFailure cacheWriteFailure() const { return cacheFailure_.load(); }
  // Whether the next cache write runs while the organ plays: then at the
  // system's lowest priority, and only while `busy` says nothing sounds. Off,
  // the load waits for it at full speed, with the organ not yet playable.
  void setCacheWhilePlaying(bool on) { cacheWhilePlaying_ = on; }
  void setCacheBusy(std::function<bool()> busy) { cacheBusy_ = std::move(busy); }
  // Waits for a cache write still running, so a load never reads one half
  // written. Asks it to stop first when `abandon` (quitting); the old cache,
  // if any, is then left as it was.
  void finishCacheWrite(bool abandon);

  // The archives an organ is played from, when it is not unpacked. A sample
  // that is not on disk under the organ's root is read from them instead:
  // decompressed into memory in one pass per archive, never written out.
  // Such samples are always resident -- there is no file to stream a tail
  // from. Null for an ordinary organ.
  void setArchive(std::shared_ptr<const OrganArchive> archive) { archive_ = std::move(archive); }
  // Packages installed somewhere else: their samples are read from that
  // folder's OrganInstallationPackages (a composite definition, #289). The
  // others stay under the organ's own root.
  void setPackageRoots(std::unordered_map<Id, std::string> roots) { packageRoots_ = std::move(roots); }
  // The packages found with other organs, in their archives or folders: part
  // of what a cache of this organ holds.
  void setPackagesFromElsewhere(std::set<std::string> packages) { packagesFromElsewhere_ = std::move(packages); }

  // Where the generated stand-ins for the standard components' noises are
  // kept. Empty: a folder in the program's data folder.
  void setStandardNoiseDir(const std::string& dir) { standardNoiseDir_ = dir; }

  // Memory figures BEFORE a load, from file sizes and WAV headers, without
  // decoding anything. In three steps so the slow one -- reading thousands of
  // headers, perhaps off a hard disk -- runs on a worker without touching the
  // organ model, and the last one follows the engine settings as they change:
  //
  //   shapeJobs()     what to look at, from the model (message thread, quick)
  //   readShape()     one file's channels, bits, length and rate (any thread)
  //   residentBytes() what that sample holds resident at the current settings
  //
  // A file only in a package has no header to read, so it is taken to be
  // 24-bit stereo; a WavPack file, about 55% of its PCM size.
  struct ShapeJob {
    Id sampleId = 0;
    std::string path;          // on disk, when it is there
    int64_t packageBytes = 0;  // the entry's size, when it is only in a package
    bool wavpack = false;
    bool releaseOnly = false;  // played only as a release: may stream
  };
  struct SampleShape {
    int64_t frames = 0;
    int channels = 0;
    double rate = 0.0;
    bool releaseOnly = false;
  };
  // The samples the pipework plays, and where each is. `wanted` names them
  // instead of the pipework when given: a sparse load reads fewer samples
  // than the model names, and the estimate has to show that.
  std::vector<ShapeJob> shapeJobs(const OrganModel& model, const std::string& organRootDir,
                                  const OrganArchive* archive,
                                  const SparseSamples* wanted = nullptr) const;
  static SampleShape readShape(const ShapeJob& job);
  int64_t residentBytes(const SampleShape& shape) const;
  const std::shared_ptr<const OrganArchive>& archive() const { return archive_; }

  // Whether any pipe of the rank has an attack sample that can be read: held
  // already (this load's, or the sample cache's), on disk under its package's
  // root, or in the organ's archive. A demo lists
  // pipes for stops it ships no samples of (#390). A rank whose pipes name no
  // sample file counts as there. Stops at the first file found, so a shipped
  // rank costs one look.
  bool rankShipped(const Rank& rank, const OrganModel& model, const std::string& organRootDir) const;

  // Whether one sample can be read: held already (this load's, or the sample
  // cache's), on disk under its package's root, or in the organ's archive.
  // The format's standard components are generated when they are missing, so
  // they count as there.
  //
  // Asked before the load, by the sparse loading: a pipe whose neighbour's
  // samples cannot be read keeps its own, so a demo's missing pipes stay
  // missing.
  bool samplePresent(Id sampleId, const OrganModel& model,
                      const std::string& organRootDir) const;

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
  // How many generations are held, the live one included. One after a load.
  size_t generationsHeld() const {
    std::lock_guard<std::mutex> lock(publishMutex_);
    return generations_.size();
  }

private:
  // One immutable generation of the sample set. Swapping the pointer is how a
  // reload becomes visible without ever mutating what the audio thread reads.
  using Store = std::unordered_map<Id, std::shared_ptr<SampleBuffer>>;

  void publish(std::shared_ptr<const Store> next);

  // Everything that changes the resident bytes, in one string. If it differs,
  // the cache is not ours.
  std::string cacheFingerprint(const std::unordered_set<Id>* onlyRanks,
                               int64_t maxFramesPerSample,
                               LoopSelection loopSelection,
                               size_t licensedLeftOut) const;
  std::string cachePath() const;
  bool writeCache(const Store& store, const std::string& fingerprint,
                  const std::string& path) const;
  bool readCache(Store& out, const std::string& fingerprint,
                 LoadProgress* progress = nullptr) const;

  std::shared_ptr<const OrganArchive> archive_;
  std::unordered_map<Id, std::string> packageRoots_;
  std::set<std::string> packagesFromElsewhere_;
  const std::string& rootFor(const std::string& organRoot, Id package) const {
    const auto it = packageRoots_.find(package);
    return it == packageRoots_.end() ? organRoot : it->second;
  }
  std::string cacheDir_;
  std::string standardNoiseDir_;
  std::string cacheOrganId_;
  std::string cacheOdfStamp_;
  CacheMode cacheMode_ = CacheMode::Single;
  mutable int64_t cacheRead_ = 0;
  mutable std::atomic<int64_t> cacheWritten_{0};
  // Sample bytes the cache write has to do, and has done: its progress.
  mutable std::atomic<int64_t> cacheTotal_{0};
  mutable std::atomic<int64_t> cacheDone_{0};
  std::thread cacheThread_;
  std::atomic<bool> cacheWriting_{false};
  mutable std::atomic<bool> cacheStop_{false};
  mutable std::atomic<CacheFailure> cacheFailure_{CacheFailure::None};
  bool cacheWhilePlaying_ = false;
  std::function<bool()> cacheBusy_;


  // Static, so everything it depends on arrives as an argument: `loadMono`
  // is the member of the same name, passed rather than read.
  static bool readInto(juce::AudioFormatReader& reader, SampleBuffer& out,
                       int64_t maxFrames, LoopSelection selection,
                       SampleStorage storage, bool loadMono,
                       double targetRate, bool releaseOnly = false,
                       int releaseMaxMs = 0);
  // Attach a tail that reads the rest of `path` on demand. The reader is
  // opened lazily, on the streaming thread, and kept for the buffer's life.
  // `totalFrames` counts RESIDENT frames, not frames of the file: when the
  // head has been converted to another rate the tail is measured in the same
  // frames the voice engine asks for. srcRate/dstRate tell it how to get back
  // to the file.
  void attachTail(SampleBuffer& out, const std::string& path,
                  int64_t totalFrames, double srcRate, double dstRate) const;
  // Sustain loop from the WAV 'smpl' chunk, applied to what is resident.
  // Where the release begins inside a file that also holds the attack, in
  // FILE frames, or -1 when the file marks no such point.
  // The last cue point in the file, where a release begins; -1 for none.
  static int64_t lastCueInFile(const juce::AudioFormatReader& reader);
  static int64_t releaseCueInFile(const juce::AudioFormatReader& reader,
                                  int64_t totalFrames, int64_t loopEnd);
  // What the file's own smpl chunk says it sounds at, as a fractional
  // concert-pitch MIDI note. Left negative when the file declares none.
  static void readFileMidiNote(const juce::AudioFormatReader& reader,
                               SampleBuffer& out);
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
  // 24-bit: what the sample sets are. See SampleStorage.
  SampleStorage storage_ = SampleStorage::Int24;
  bool codeSamples_ = false;
  bool loadMono_ = false;
  // How many notes of a rank are loaded: 1 for every note, 2 for every
  // second, 3 for every third.
  int sparseEvery_ = 1;
  ReleaseChoice releaseChoice_ = ReleaseChoice::All;
  int releaseMaxMs_ = 0;
  double loadRate_ = 0.0;   // 0 = keep each file's own rate
  bool streamReleases_ = false;
  bool licenceConfirmed_ = false;
  int64_t streamHead_ = 48000; // one second
  int streamHeadPercent_ = 0;
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
