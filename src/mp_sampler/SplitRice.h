// Masterpiece -- split Rice coding: the lossless codec for samples held in
// memory and decoded as they play (#447).
//
// A sample is cut into blocks of 4096 frames. A block stands alone: it
// carries the samples before it that its predictor needs, so a voice can
// start decoding at any block, which is what a loop's jump back needs.
//
// Per block and channel the encoder takes the best fixed polynomial
// predictor of order 0 to 3 and keeps the residuals, zigzagged to unsigned
// values. A stereo block is held as left and right, or as left and side
// (left minus right), whichever is smaller. The residuals go in groups of
// 256 that share one Rice parameter k, and each value is split in two:
//
//   the low k bits, packed k bits a value, least significant bit first;
//   the rest, q = value >> k, as a unary code: q zero bits, then a one.
//
// That is the same number of bits as a Rice code, held in two streams. The
// packed bits are unpacked four values at a time, and the unary stream is
// turned into the positions of its ones a byte at a time, so the decoder
// has no per-value branch and no value waits for the one before it. On the
// two-core i3 the project measures on, that decodes a tenth of the Friesach
// set at 224 million frames a second, against 202 million for the delta
// coding GrandOrgue's sample cache uses; the bytes are a third of plain
// 24-bit storage.
//
// Block layout, little-endian:
//
//   u8      stereo mode: 0 left and right, 1 left and side, 2 side and
//           right, 3 mid and side; 0 for mono
//   per channel: u8 predictor order (0 to 8), then that many i32: the
//           samples before the block, newest first
//   per channel but the last: u32 the bytes of its body
//   per channel, its body:
//     the k of each group, 5 bits each, least significant bit first,
//       padded to a byte
//     the low bits, group after group, each group 32 * k bytes
//     the unary stream, padded to a multiple of 8 bytes
//
// The stream ends with 16 zero bytes, so the decoder's 16-byte reads never
// leave it.
//
// JUCE-free, like the rest of mp_sampler.
#pragma once

#include "SampleCodec.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mp {

inline constexpr uint32_t kSplitRiceBlockFrames = 4096;
inline constexpr int kSplitRiceMaxChannels = 2;
inline constexpr int kSplitRiceMinBitDepth = 8;
inline constexpr int kSplitRiceMaxBitDepth = 24;

// A number no other stream made in this run has. A decoder that keeps
// decoded blocks knows a stream by it: once a stream is freed, its address
// can come back for another one.
uint64_t newSplitRiceSerial();

// One sample, coded.
struct SplitRiceStream {
  uint64_t serial = newSplitRiceSerial();
  // The blocks one after another, then 16 zero bytes.
  std::vector<uint8_t> data;
  // Where each block starts in `data`.
  std::vector<uint32_t> blockOffsets;
  uint64_t numFrames = 0;
  int channels = 0;
  int bitDepth = 0;

  size_t blockCount() const { return blockOffsets.size(); }
  // The frames in a block: all of them but the last are full.
  uint32_t framesInBlock(size_t index) const;
  // What the sample costs in memory: what is held, used or not.
  size_t bytes() const {
    return data.capacity() + blockOffsets.capacity() * sizeof(uint32_t);
  }
};

// The decoder's working memory, made once and kept: a voice decodes on the
// audio thread, which must not allocate.
struct SplitRiceScratch {
  SplitRiceScratch();
  std::vector<int32_t> marks;  // the unary stream's ones, per channel
  std::vector<int32_t> work;   // the residuals and each channel's values
};

// Codes interleaved samples (frames * channels values, frame-major). One or
// two channels, 8 to 24 bits; every sample has to fit the width.
bool encodeSplitRice(const int32_t* samples, uint64_t frames, int channels, int bitDepth,
                     SplitRiceStream& out, CodecError* error = nullptr);

// Decodes block `index`. `out` holds kSplitRiceBlockFrames * channels
// values: the block's frames come back interleaved, and a short last block
// leaves the rest as working space.
bool decodeSplitRiceBlock(const SplitRiceStream& stream, size_t index, int32_t* out,
                          SplitRiceScratch& scratch, CodecError* error = nullptr);

// Decodes `count` frames from `firstFrame`, the blocks that hold them only.
bool decodeSplitRiceFrames(const SplitRiceStream& stream, uint64_t firstFrame, uint64_t count,
                           int32_t* out, CodecError* error = nullptr);

// The codec in the registry, "SRIC". Its payload is the block count, the
// block offsets (u32 each) and the stream's data.
std::unique_ptr<SampleCodec> makeSplitRiceCodec();

// Whether the decoder runs its SSE4.1 kernels on this machine. The tests
// switch them off to compare the two; the switch does nothing where the
// machine has none.
bool splitRiceVectorKernels();
void setSplitRiceVectorKernels(bool on);

} // namespace mp
