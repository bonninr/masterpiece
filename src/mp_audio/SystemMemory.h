#pragma once
// What memory the program may still take before the system refuses it or
// ends the program, read from the system each time it is asked.
#include <cstdint>

namespace mp {

// Bytes this process can still allocate before the system stops it: on iOS
// the figure the system itself kills by; on Android and Linux the memory the
// kernel reports available; on Windows the physical memory free and the
// commit still open, the smaller; on macOS the free and reclaimable pages.
// -1 when the system gives no figure.
int64_t availableMemoryBytes();

// The most this process may hold at all, where the system sets a ceiling of
// its own below the computer's memory (iOS); -1 elsewhere.
int64_t processMemoryCeilingBytes();

// What a load leaves free for everything else the program and the system
// need while it plays: the larger of 256 MB and 1/20 of the memory.
int64_t memoryReserveBytes();

}  // namespace mp
