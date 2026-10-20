// Masterpiece -- lossless block codec for resident sample data.
//
// Phase 1 of the in-memory compression work (#447): the codec, its tests and
// a measurement. The render path does not read compressed data and nothing
// here changes the engine. Where decoding happens and how a voice reads it
// is decided from the measurements, later, with the owner.
//
// A stream is a list of blocks, each of kLosslessBlockFrames frames. A block
// carries everything it needs: the first samples of the block are stored raw,
// so a block never depends on the one before it and a decoder may start
// anywhere.
//
// Per block and per channel the encoder picks the best of three fixed
// predictors (order 0, 1 and 2), codes the residuals with Rice codes and
// writes one Rice parameter per 256 residuals. A stereo block is coded either
// as two independent channels or as left and side (R - L), whichever comes
// out smaller.
//
// Every block carries its own length and a CRC over its bytes, so a damaged
// or truncated block is reported instead of being decoded into rubbish.
//
// Block layout, all little-endian:
//
//   offset  size  what
//   0       4     magic "MPLB"
//   4       2     frames in this block
//   6       1     channels (1 to 8)
//   7       1     bit depth (16, 20 or 24)
//   8       1     flags, bit 0: the channels are stored as left and side
//   9       1     reserved, zero
//   10      2     residuals per Rice partition
//   12      4     payload bytes
//   16      4     CRC-32 over bytes 0 to 15 and over the payload
//   20      ...   the payload
//
// The payload holds one stream per channel, in order:
//
//   offset  size  what
//   0       1     the predictor order (0, 1 or 2) in bits 0 and 1, and bit 2
//                 set when the channel is one bit wider than the block
//   1       ...   the first `order` samples, raw, packed
//   ...     2     the number of Rice partitions
//   ...     ...   the Rice parameters, 5 bits each
//   ...     ...   the residuals, Rice coded
//
// The raw samples are packed least significant bit first, so sample i holds
// bits [i * depth, i * depth + depth). The Rice parameters and the residual
// codes are written most significant bit first.
//
// The side channel of a stereo pair is the difference of the two channels,
// which reaches twice the width of a sample, so it is stored one bit wider
// than the block. The left channel keeps the block's width.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mp {

// Frames per block. The default; the encoder takes another size for the
// streaming lab, which compares three.
inline constexpr uint32_t kLosslessBlockFrames = 4096;
// Residuals per Rice partition: one Rice parameter covers this many.
inline constexpr uint32_t kLosslessRicePartition = 256;
// The widths the codec handles, in bits.
inline constexpr int kLosslessMinBitDepth = 16;
inline constexpr int kLosslessMaxBitDepth = 24;
// The most channels a block may hold.
inline constexpr int kLosslessMaxChannels = 8;

// Why a decode failed. A decode either produces every sample it was asked
// for, or reports one of these and produces nothing.
enum class LosslessError {
  None,
  BadHeader,        // the header does not describe a block this codec wrote
  Truncated,        // the block stops before its payload does
  CheckFailed,      // the CRC does not match: the bytes are damaged
  BadStream,        // the block table does not match the data
  OutOfRange,       // the frames asked for lie outside the stream
  SamplesOutOfRange,// a sample does not fit the width the block declares
  CorruptData,      // the payload does not decode to samples of that width
};

const char* losslessErrorText(LosslessError error);

// The coded stream: the blocks one after another, and where each starts.
struct LosslessStream {
  std::vector<unsigned char> data;
  // The byte offset of every block in `data`. A block's own length is in its
  // header, so the table is a lookup, not a length list.
  std::vector<uint64_t> blockOffsets;
  uint64_t numFrames = 0;
  int channels = 0;
  int bitDepth = 0;
  uint32_t blockFrames = kLosslessBlockFrames;

  size_t blockCount() const { return blockOffsets.size(); }
  // The frames in a block: all of them but the last are full.
  uint64_t framesInBlock(size_t index) const;
  void clear();
};

// Encodes interleaved samples (numFrames * channels values, frame-major) into
// blocks. The samples must fit the width: 16 bits means [-32768, 32767],
// 20 bits [-524288, 524287], 24 bits [-8388608, 8388607].
bool encodeLosslessBlocks(const int32_t* samples, uint64_t numFrames,
                           int channels, int bitDepth, LosslessStream& out,
                           LosslessError* error = nullptr,
                           uint32_t blockFrames = kLosslessBlockFrames);

// Decodes one block. `out` receives blockFrames * channels values,
// interleaved and frame-major; the block's own frame count is in `frames`.
bool decodeLosslessBlock(const LosslessStream& stream, size_t blockIndex,
                          int32_t* out, uint16_t* frames = nullptr,
                          LosslessError* error = nullptr);

// Decodes `count` frames starting at `firstFrame`. Only the blocks that hold
// them are decoded.
bool decodeLosslessFrames(const LosslessStream& stream, uint64_t firstFrame,
                           uint64_t count, int32_t* out,
                           LosslessError* error = nullptr);

} // namespace mp
