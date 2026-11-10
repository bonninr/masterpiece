// What reading a streamed release costs on the disk the organ is on, timed on
// the organ's own files at load, and the ring length that cost asks for.
// Pure C++20, no platform APIs, no JUCE; runs on the load thread, never on
// the audio thread.
#pragma once
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

} // namespace mp
