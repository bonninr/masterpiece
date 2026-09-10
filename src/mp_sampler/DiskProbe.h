// Portable storage-tier probe (ADR-012): measure sequential throughput of
// the drive holding the organ, classify NVMe / SATA SSD / HDD, and scale
// streaming heads + voice budget accordingly. Pure C++20, no platform APIs,
// no JUCE — runs at organ load on a background thread, never on audio.
// Full-on engine (user directive): probe + tiering ships in v1, tuned later.
#pragma once
#include "StreamingEngine.h"
#include <string>

namespace mp {

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
