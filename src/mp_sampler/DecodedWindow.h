// Masterpiece -- a voice's decoded frames of a sample held coded (#447).
//
// A sample held split Rice coded is read through a window: a few thousand of
// its frames, decoded, in the storage the voice reads for a sample held as
// plain counts (16-bit, or 24-bit for anything wider). The voice's own code
// reads the window as it reads a sample held whole, counting frames from the
// frame the window starts at.
//
// A window holds one coded block and, before it, the last frames of the block
// before: the interpolation reads a frame behind the cursor, and the vector
// runs stop a few frames short of the end. As the cursor reaches the end, the
// window moves on: those last frames go to the front and the next block is
// decoded after them, so each block is decoded once as a voice plays through.
// A jump -- a loop's wrap, a release's start -- decodes the block it lands in.
//
// The windows are made when the engine is prepared, two for each voice, and
// never grow: the audio thread allocates nothing. The decoding is done on the
// thread that renders the voice, with that thread's own scratch.
//
// JUCE-free, like the rest of mp_sampler.
#pragma once

#include "SplitRice.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mp {

// The frames a window keeps from the block before when it moves on.
inline constexpr int64_t kWindowMargin = 32;
// The most frames a window holds.
inline constexpr int64_t kWindowFrames = kWindowMargin + kSplitRiceBlockFrames;
// Zeroed frames after the last one held: the vector code reads 32 bits for a
// 16- or 24-bit tap, so up to two bytes past it.
inline constexpr int64_t kWindowPad = 4;
// What one window takes, at the widest: two channels of three bytes.
inline constexpr size_t kWindowBytes =
    static_cast<size_t>(kWindowFrames + kWindowPad) * kSplitRiceMaxChannels * 3;

struct DecodedWindow {
  const SplitRiceStream* stream = nullptr;  // the sample it holds frames of
  uint64_t serial = 0;                      // and its serial, which is what is compared
  int64_t start = 0;                        // the first frame held
  int64_t count = 0;                        // how many
  int64_t block = -1;                       // the block the frames end with
  bool wide = false;                        // three bytes a value; two otherwise
  unsigned char* data = nullptr;            // kWindowBytes, owned by the engine

  bool holds(const SplitRiceStream* s, int64_t first, int64_t last) const {
    return serial != 0 && serial == s->serial && first >= start && last < start + count;
  }
  bool holdsFrame(const SplitRiceStream* s, int64_t frame) const {
    return serial != 0 && serial == s->serial && frame >= start && frame < start + count;
  }
  void clear() {
    stream = nullptr;
    serial = 0;
    count = 0;
    block = -1;
  }
  // The raw count of a frame the window holds.
  int32_t value(int64_t frame, int channel) const {
    const size_t i = static_cast<size_t>((frame - start) * stream->channels + channel);
    if (wide) {
      const unsigned char* p = data + i * 3;
      int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
      if (v & 0x800000) v -= 0x1000000;
      return v;
    }
    int16_t v;
    std::memcpy(&v, data + i * 2, 2);
    return v;
  }
};

// What a render thread decodes with. One per thread, made off the audio thread.
struct DecodeScratch {
  DecodeScratch();
  SplitRiceScratch rice;
  std::vector<int32_t> decoded;  // one block, both channels
  // Two decoded blocks for the reads no window holds: a loop's seam, a
  // release's level, where a release starts in phase. Taken in turn.
  struct Block {
    uint64_t serial = 0;  // the stream's; 0 for none
    int64_t block = -1;
    std::vector<int32_t> values;
  };
  Block blocks[2];
  int next = 0;
  std::atomic<uint64_t> decodes{0};  // blocks decoded, read by the diagnostics
};

// Makes the window hold frames `first` to `last` of the sample, both clamped
// to the sample, decoding what it lacks. `last - first` stays under
// kWindowMargin. False when the sample's bytes do not decode.
bool holdFrames(DecodedWindow& w, const SplitRiceStream& s, int64_t first, int64_t last,
                DecodeScratch& scratch);

// The raw count of one frame of the sample: from a window that holds it,
// else from the scratch's blocks, decoding the block into one of them. 0
// outside the sample, and when it does not decode.
int32_t codedValue(const SplitRiceStream& s, const DecodedWindow* a, const DecodedWindow* b,
                   int64_t frame, int channel, DecodeScratch& scratch);

}  // namespace mp
