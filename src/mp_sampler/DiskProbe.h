// Portable storage-tier probe (ADR-012): measure sequential throughput of
// the drive holding the organ, classify NVMe / SATA SSD / HDD, and scale
// streaming heads + voice budget accordingly. Pure C++20, no platform APIs,
// no JUCE — runs at organ load on a background thread, never on audio.
// Full-on engine (user directive): probe + tiering ships in v1, tuned later.
#pragma once
#include "StreamingEngine.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mp {

struct SampleTail;

// What a read of a streamed tail costs on the disk the organ is on, measured
// on the organ's own files at load (#548). A read costs a fixed part (the
// seek, the request) and a part that grows with its length.
struct TailReadCost {
  int files = 0;              // tails timed; 0 means nothing was measured
  double latencyMs = 0.0;     // the fixed part of one read
  double framesPerMs = 0.0;   // frames read a millisecond past it; 0 = too fast to tell
  double openMs = 0.0;        // the first read of a file, which opens it
};

// One streamed sample to time: its tail, and how much of it is resident.
struct TailToTime {
  const SampleTail* tail = nullptr;
  int64_t resident = 0;
  int channels = 1;
};

// Times reads of up to `maxFiles` of these tails, past their heads, where
// loading has read nothing: three rounds over the files, so each read is a
// seek to another file, as streaming makes them. A few tenths of a second on
// a hard disk, a few milliseconds on a solid-state one.
TailReadCost measureTailReads(const std::vector<TailToTime>& tails, int maxFiles = 16);

// The shortest ring, in seconds, with which one streamer keeps `voices`
// streaming voices fed on a disk of this cost, using at most half its time.
// Each voice is topped up when half its ring is free, so it costs the disk
// two reads per ring length. Between `minSeconds` and `maxSeconds`; the
// longest when the disk cannot feed that many at any length, and also when
// nothing was measured.
// The reckoning takes every read for a seek; a disk that reads ahead of a
// file read in order does better than it says.
double ringSecondsFor(const TailReadCost& cost, double sampleRate, int voices,
                      double minSeconds = 0.25, double maxSeconds = 2.0);

// Conservative sequential-read thresholds (MB/s). Real-world: NVMe 2000+,
// SATA SSD ~500, HDD ~100-200. Margins keep a slow NVMe in Nvme and a fast
// HDD out of SataSsd.
DiskTier classifyTier(double mbPerSec); // <=0 -> Unknown

// Head-size scale for StreamConfig::preloadHeadFrames (percent).
// Slow tiers preload more so the background pool rarely engages audibly.
int headScaleForTier(DiskTier tier); // Nvme 100 / SataSsd 200 / Hdd 400 / other 200

// Measures sequential read MB/s by writing + re-reading a temp file in dir.
// Returns -1 on any error (caller treats as Unknown). megabytes clamped 8..256.
double probeSequentialMBps(const std::string& dir, int megabytes = 64);

} // namespace mp
