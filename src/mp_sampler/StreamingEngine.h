// Streaming sampler — HISE architecture, organ-adapted (ADR-010).
// - WAV: MemoryMappedAudioFormatReader (OS page cache, direct mmap playback).
// - WavPack (.wv): libwavpack decode on background pool into voice buffers (ADR-011).
// - Per-VOICE ring buffers (not per-sound): same note retriggered needs independent cursors.
// - Preload attack head + loop start on startNote; background pool refills loop/release tails.
// - Release samples begin within one audio block of note-off (hard RT requirement).
// - Voice rendering shards across worker threads; disk tier scales heads (ADR-012).
#pragma once
#include "../mp_core/OrganModel.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace mp {

// Storage tiers, slowest-first. Probed at organ load (DiskProbe) so SATA SSD
// and HDD machines stream correctly instead of assuming NVMe (ADR-012).
enum class DiskTier { Unknown, Hdd, SataSsd, Nvme, RamOnly };

struct ParallelConfig {
  int renderThreads = 0; // 0 = auto (hardware_concurrency - 1, audio thread excluded)
  int minVoicesPerThread = 32; // below renderThreads*this total voices, render inline
  bool pinThreads = false; // affinity pinning; off — OS scheduler usually wins
};

struct DiskBudget {
  DiskTier tier = DiskTier::Unknown; // Unknown = probe at organ load
  double maxReadMBps = 0.0; // 0 = probed value for tier; >0 overrides
  int headScalePercent = 0; // 0 = headScaleForTier(tier): Nvme 100 / Sata 200 / Hdd 400
};

struct StreamConfig {
  SampleLoadMode loadMode = SampleLoadMode::Auto;
  int preloadHeadFrames = 32768;   // attack + loop start resident (scaled by DiskBudget)
  int ringFramesPerVoice = 131072; // ~2.7s @48k mono float; tuned in M2 bench
  int maxVoices = 1536;            // organ tutti + couplers (500-1500+ sounding)
  int numBackgroundThreads = 4;
  int streamingPriorityReleases = 1; // releases jump the queue
  ParallelConfig parallel; // voice-render sharding (ADR-012)
  DiskBudget disk; // storage tier budget, incl. SATA SSD support (ADR-012)
};

class SampleHandle {
public:
  SampleHandle() = default;
  // Opened lazily: memory-map on first use, share across voices (one Sound, many Voices).
  bool open(const std::string& wavPath, std::string& error);
  bool isOpen() const { return numFrames > 0; }
  int numChannels = 0;
  double sampleRate = 48000.0;
  int64_t numFrames = 0;
  // Synchronous head read (for preload); tail reads go via background pool.
  int readHead(float* const* dest, int numCh, int64_t startFrame, int numFramesWanted) const;
private:
  struct Impl;
  std::shared_ptr<Impl> impl;
};

// One live pipe instance: attack->loop sustain->release glue.
// Pairing: release variant coupled to attack variant that was played (per-voice state).
struct PipeVoiceState {
  const Pipe* pipe = nullptr;
  const PipeLayer* layer = nullptr;
  int attackIndex = 0;
  int attackId = 0; // UniqueID of the played attack (release coupling key)
  int releaseIndex = -1;
  int64_t posInAttack = 0;
  int64_t posInLoop = 0;
  bool inRelease = false;
  int64_t posInRelease = 0;
  double playbackRatio = 1.0; // temperament * wind * trem pitch mods
  float amp = 1.0f;           // enclosure * wind * trem amp mods * voicing
  uint64_t noteOnId = 0;      // retrigger tracking (no orphaned reads)
};

// M2.1 selection matrices (first-match in file order, HW ceiling semantics).
// A note sounds when velocity <= velHigh, timeSinceCloseMs >=
// minTimeSinceCloseMs and ctsValue <= ctsHigh. Returns the attack index or
// -1 when nothing matches (layer stays silent for this note).
struct NoteStrike {
  int velocity = 64; // key velocity 0..127
  int64_t timeSinceCloseMs = INT64_MAX; // since this pipe last closed
  int ctsValue = 127; // attack continuous-control value 0..127
};

int selectAttack(const PipeLayer& layer, const NoteStrike& strike);

struct NoteRelease {
  int attackIndex = 0; // index played at note-on
  int attackId = 0; // UniqueID played at note-on (coupling key)
  int attackVelocity = 64; // velocity of the played attack
  int attackCts = 127; // cts value of the played attack
  int velocity = 64; // key-off velocity 0..127
  int64_t holdTimeMs = 0; // key hold duration
  int ctsValue = 127; // release continuous-control value 0..127
};

// Release choice: candidates must match velocity/hold/cts ceilings AND the
// attack context; releases naming this attackId via
// ReleaseSelCriteria_PreferThisRelForAttackID win, else file order.
int selectRelease(const PipeLayer& layer, const NoteRelease& rel);

} // namespace mp
