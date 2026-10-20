// Masterpiece -- the codecs that ship with the program: the lossless block
// codec and plain storage.
#include "SampleCodecBuiltins.h"

#include "LosslessBlocks.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace mp {
namespace {

// The lossless block codec.
//
// The payload is the block table and then the blocks:
//
//   offset  size  what
//   0       4     the number of blocks
//   4       8*n   the byte offset of every block, from the start of the
//                 block data
//   ...     ...   the block data
//
// The block codec's own blocks carry their own length and their own check,
// so a damaged block is reported by the block codec itself.
class BlockCodec : public SampleCodec {
public:
  BlockCodec() {
    info_.id = codecId("MPLB");
    info_.name = "lossless blocks";
    info_.access = CodecAccess::RandomAccess;
    info_.minBitDepth = kLosslessMinBitDepth;
    info_.maxBitDepth = kLosslessMaxBitDepth;
    info_.blockFrames = kLosslessBlockFrames;
    info_.lossless = true;
  }

  const CodecInfo& info() const override { return info_; }

  bool encode(const int32_t* samples, uint64_t frames, int channels,
              int bitDepth, uint32_t blockFrames, std::vector<uint8_t>& payload,
              CodecError* error) override {
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (samples == nullptr || frames == 0 || channels <= 0)
      return fail(CodecError::BadHeader);
    if (bitDepth < kLosslessMinBitDepth || bitDepth > kLosslessMaxBitDepth)
      return fail(CodecError::BadWidth);
    if (blockFrames == 0) blockFrames = kLosslessBlockFrames;

    LosslessStream stream;
    LosslessError why = LosslessError::None;
    if (!encodeLosslessBlocks(samples, frames, channels, bitDepth, stream,
                              &why, blockFrames))
      return fail(CodecError::CodecFailed);

    payload.assign(4 + stream.blockOffsets.size() * 8, 0);
    for (int i = 0; i < 4; ++i)
      payload[static_cast<size_t>(i)] =
          static_cast<uint8_t>((stream.blockOffsets.size() >> (8 * i)) & 0xFFu);
    for (size_t b = 0; b < stream.blockOffsets.size(); ++b) {
      const uint64_t at = stream.blockOffsets[b];
      for (int i = 0; i < 8; ++i)
        payload[4 + b * 8 + static_cast<size_t>(i)] =
            static_cast<uint8_t>((at >> (8 * i)) & 0xFFu);
    }
    payload.insert(payload.end(), stream.data.begin(), stream.data.end());
    if (error) *error = CodecError::None;
    return true;
  }

  bool decodeRange(const uint8_t* payload, size_t payloadBytes,
                   const CodedHeader& header, uint64_t firstFrame,
                   uint64_t count, int32_t* out, CodecError* error) override {
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (payload == nullptr || out == nullptr) return fail(CodecError::BadHeader);
    if (payloadBytes < 4) return fail(CodecError::Truncated);
    const uint32_t blocks = static_cast<uint32_t>(payload[0]) |
                            (static_cast<uint32_t>(payload[1]) << 8) |
                            (static_cast<uint32_t>(payload[2]) << 16) |
                            (static_cast<uint32_t>(payload[3]) << 24);
    const size_t tableBytes = 4 + static_cast<size_t>(blocks) * 8;
    if (payloadBytes < tableBytes) return fail(CodecError::Truncated);

    LosslessStream stream;
    stream.blockOffsets.resize(blocks);
    for (uint32_t b = 0; b < blocks; ++b) {
      uint64_t at = 0;
      for (int i = 0; i < 8; ++i)
        at |= static_cast<uint64_t>(payload[4 + static_cast<size_t>(b) * 8 +
                                     static_cast<size_t>(i)])
             << (8 * i);
      stream.blockOffsets[b] = at;
    }
    stream.data.assign(payload + tableBytes, payload + payloadBytes);
    stream.numFrames = header.frames;
    stream.channels = header.channels;
    stream.bitDepth = header.bitDepth;
    stream.blockFrames = header.blockFrames != 0 ? header.blockFrames
                                                 : kLosslessBlockFrames;

    LosslessError why = LosslessError::None;
    if (!decodeLosslessFrames(stream, firstFrame, count, out, &why))
      return fail(why == LosslessError::OutOfRange ? CodecError::BadRange
                                                   : CodecError::CodecFailed);
    if (error) *error = CodecError::None;
    return true;
  }

private:
  CodecInfo info_;
};

// Plain storage: the samples as they are.
//
//   "PCMF"   32-bit floats, carrying 24-bit counts
//   "PCM3"   24-bit integers
//   "PCM2"   16-bit integers
//
// The 32-bit float form carries 24-bit counts: a float holds every
// integer up to 2^24 exactly, and the count is divided by 2^23, so the
// round trip gives back the same numbers.
class PlainCodec : public SampleCodec {
public:
  // `storage` is how the samples are held: 32 means 32-bit floats, and
  // the counts they carry are 24-bit.
  explicit PlainCodec(int storage) : storage_(storage) {
    const int width = storage == 32 ? 24 : storage;
    info_.id = codecId(storage == 32 ? "PCMF" : storage == 24 ? "PCM3"
                             : storage == 16 ? "PCM2"
                                              : "PCMX");
    info_.name = storage == 32   ? "plain 32-bit float"
                   : storage == 24 ? "plain 24-bit"
                   : storage == 16 ? "plain 16-bit"
                                    : "plain";
    info_.access = CodecAccess::RandomAccess;
    info_.minBitDepth = width;
    info_.maxBitDepth = width;
    info_.blockFrames = 0;
    info_.lossless = true;
  }

  const CodecInfo& info() const override { return info_; }

  bool encode(const int32_t* samples, uint64_t frames, int channels,
              int bitDepth, uint32_t blockFrames, std::vector<uint8_t>& payload,
              CodecError* error) override {
    (void)blockFrames;
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (samples == nullptr || frames == 0 || channels <= 0)
      return fail(CodecError::BadHeader);
    const int width = storage_ == 32 ? 24 : storage_;
    if (bitDepth != 0 && bitDepth != width) return fail(CodecError::BadWidth);
    const int64_t low = codecSampleMin(width);
    const int64_t high = codecSampleMax(width);
    const size_t count = static_cast<size_t>(frames) * static_cast<size_t>(channels);
    for (size_t i = 0; i < count; ++i) {
      const int32_t v = samples[i];
      if (static_cast<int64_t>(v) < low || static_cast<int64_t>(v) > high)
        return fail(CodecError::BadWidth);
    }

    payload.clear();
    if (storage_ == 32) {
      payload.resize(count * 4);
      for (size_t i = 0; i < count; ++i) {
        const float f = static_cast<float>(samples[i]) / 8388608.0f;
        uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        for (int b = 0; b < 4; ++b)
          payload[i * 4 + static_cast<size_t>(b)] =
              static_cast<uint8_t>((bits >> (8 * b)) & 0xFFu);
      }
    } else if (storage_ == 24) {
      payload.resize(count * 3);
      for (size_t i = 0; i < count; ++i) {
        const uint32_t v = static_cast<uint32_t>(samples[i]) & 0xFFFFFFu;
        payload[i * 3 + 0] = static_cast<uint8_t>(v & 0xFFu);
        payload[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        payload[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
      }
    } else {
      payload.resize(count * 2);
      for (size_t i = 0; i < count; ++i) {
        const uint16_t v = static_cast<uint16_t>(samples[i]);
        payload[i * 2 + 0] = static_cast<uint8_t>(v & 0xFFu);
        payload[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
      }
    }
    if (error) *error = CodecError::None;
    return true;
  }

  bool decodeRange(const uint8_t* payload, size_t payloadBytes,
                   const CodedHeader& header, uint64_t firstFrame,
                   uint64_t count, int32_t* out, CodecError* error) override {
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (payload == nullptr || out == nullptr) return fail(CodecError::BadHeader);
    const int width = storage_ == 32 ? 24 : storage_;
    if (header.bitDepth != 0 && header.bitDepth != width)
      return fail(CodecError::BadWidth);
    const uint64_t channels = static_cast<uint64_t>(header.channels);
    if (channels == 0) return fail(CodecError::BadHeader);
    const uint64_t total = header.frames * channels;
    const uint64_t first = firstFrame * channels;
    if (first + count * channels > total) return fail(CodecError::BadRange);

    const size_t bytesPer = storage_ == 32 ? 4 : storage_ == 24 ? 3 : 2;
    if (payloadBytes < total * bytesPer) return fail(CodecError::Truncated);

    for (uint64_t i = 0; i < count * channels; ++i) {
      const size_t at = static_cast<size_t>((first + i) * bytesPer);
      int32_t v = 0;
      if (storage_ == 32) {
        uint32_t bits = 0;
        for (int b = 0; b < 4; ++b)
          bits |= static_cast<uint32_t>(payload[at + static_cast<size_t>(b)])
                  << (8 * b);
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        v = static_cast<int32_t>(std::lround(static_cast<double>(f) * 8388608.0));
      } else if (storage_ == 24) {
        uint32_t raw = static_cast<uint32_t>(payload[at]) |
                       (static_cast<uint32_t>(payload[at + 1]) << 8) |
                       (static_cast<uint32_t>(payload[at + 2]) << 16);
        if (raw & 0x800000u) raw |= 0xFF000000u;
        v = static_cast<int32_t>(raw);
      } else {
        const uint16_t raw = static_cast<uint16_t>(payload[at]) |
                             static_cast<uint16_t>(payload[at + 1]) << 8;
        v = static_cast<int16_t>(raw);
      }
      out[i] = v;
    }
    if (error) *error = CodecError::None;
    return true;
  }

private:
  int storage_ = 24;
  CodecInfo info_;
};

} // namespace

std::unique_ptr<SampleCodec> makeBlockCodec() {
  return std::make_unique<BlockCodec>();
}

std::unique_ptr<SampleCodec> makePlainCodec(int bitDepth) {
  return std::make_unique<PlainCodec>(bitDepth);
}

void registerBuiltinCodecs(CodecRegistry& registry) {
  registry.add(makeBlockCodec());
  registry.add(makePlainCodec(32));
  registry.add(makePlainCodec(24));
  registry.add(makePlainCodec(16));
}

} // namespace mp
