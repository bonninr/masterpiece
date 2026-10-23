// Masterpiece -- the codecs that ship with the program: the predictive delta
// codec and plain storage.
#include "SampleCodecBuiltins.h"
#include "SplitRice.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace mp {
namespace {

// The predictive delta codec.
//
// The payload is the differences, frame by frame and channel by channel in
// the order of the interleaved samples, each in a code of its own. The unit
// of a code is one byte for samples of up to 16 bits and two bytes for wider
// ones; the low bits of the first unit say how long the code is. All
// little-endian.
//
// Byte units (up to 16 bits):
//
//   first byte    length  the difference d
//   xxxxxxx1      1       d = first >> 1                       (-64..63)
//   xxxxxx10      2       d = (first & ~3) << 6 | byte1        (-8192..8191)
//   xxxxx100      3       d = (first & ~7) << 13 | word(1)     (-2^20..2^20-1)
//   xxxxx000      4       d = (first & ~7) << 21 | byte1 << 16 | word(2)
//
// Two-byte units (wider samples):
//
//   first word    length  the difference d
//   ...1          2       d = first >> 1                       (-2^14..2^14-1)
//   ...10         3       d = (first & ~3) << 6 | byte2        (-2^21..2^21-1)
//   ...00         4       d = (first & ~3) << 14 | word(2)
//
// The first unit is read as signed, so the sign comes from its top bit.
constexpr int kDeltaMaxChannels = 8;

struct DeltaState {
  uint64_t frame = 0;   // frames decoded
  size_t offset = 0;    // the next code's byte in the payload
  int32_t value[kDeltaMaxChannels] = {};
  int32_t prev[kDeltaMaxChannels] = {};
};

bool wideUnits(int bitDepth) { return bitDepth > 16; }

int32_t predict(int32_t prev, int32_t last) { return prev + (prev - last) / 2; }

void put16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFFu));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

uint32_t get16(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8);
}

// Appends the code for one difference. False when it does not fit any code,
// which a sample within the codec's widths never reaches.
bool putDifference(std::vector<uint8_t>& out, int32_t d, bool wide) {
  if (!wide) {
    if (d >= -64 && d <= 63) {
      out.push_back(static_cast<uint8_t>((d << 1) | 1));
    } else if (d >= -8192 && d <= 8191) {
      out.push_back(static_cast<uint8_t>(((d >> 8) << 2) | 2));
      out.push_back(static_cast<uint8_t>(d & 0xFF));
    } else if (d >= -(1 << 20) && d <= (1 << 20) - 1) {
      out.push_back(static_cast<uint8_t>(((d >> 16) << 3) | 4));
      put16(out, static_cast<uint32_t>(d) & 0xFFFFu);
    } else if (d >= -(1 << 28) && d <= (1 << 28) - 1) {
      out.push_back(static_cast<uint8_t>((d >> 24) << 3));
      out.push_back(static_cast<uint8_t>((d >> 16) & 0xFF));
      put16(out, static_cast<uint32_t>(d) & 0xFFFFu);
    } else {
      return false;
    }
  } else {
    if (d >= -(1 << 14) && d <= (1 << 14) - 1) {
      put16(out, static_cast<uint32_t>((d << 1) | 1) & 0xFFFFu);
    } else if (d >= -(1 << 21) && d <= (1 << 21) - 1) {
      put16(out, static_cast<uint32_t>(((d >> 8) << 2) | 2) & 0xFFFFu);
      out.push_back(static_cast<uint8_t>(d & 0xFF));
    } else if (d >= -(1 << 29) && d <= (1 << 29) - 1) {
      put16(out, static_cast<uint32_t>((d >> 16) << 2) & 0xFFFFu);
      put16(out, static_cast<uint32_t>(d) & 0xFFFFu);
    } else {
      return false;
    }
  }
  return true;
}

// Reads the code at `offset`. False when the payload ends inside it.
bool getDifference(const uint8_t* payload, size_t bytes, size_t& offset,
                   bool wide, int32_t& d) {
  const uint8_t* p = payload + offset;
  const size_t left = bytes - offset;
  if (!wide) {
    if (left < 1) return false;
    const int32_t first = static_cast<int8_t>(p[0]);
    if (first & 1) {
      d = first >> 1;
      offset += 1;
    } else if (first & 2) {
      if (left < 2) return false;
      d = ((first & ~3) << 6) | p[1];
      offset += 2;
    } else if (first & 4) {
      if (left < 3) return false;
      d = ((first & ~7) << 13) | static_cast<int32_t>(get16(p + 1));
      offset += 3;
    } else {
      if (left < 4) return false;
      d = ((first & ~7) << 21) | (static_cast<int32_t>(p[1]) << 16) |
          static_cast<int32_t>(get16(p + 2));
      offset += 4;
    }
  } else {
    if (left < 2) return false;
    const int32_t first = static_cast<int16_t>(get16(p));
    if (first & 1) {
      d = first >> 1;
      offset += 2;
    } else if (first & 2) {
      if (left < 3) return false;
      d = ((first & ~3) << 6) | p[2];
      offset += 3;
    } else {
      if (left < 4) return false;
      d = ((first & ~3) << 14) | static_cast<int32_t>(get16(p + 2));
      offset += 4;
    }
  }
  return true;
}

// Decodes the next `frames` frames into `out` (may be null to skip them).
// Every value is checked against the width, so a damaged payload is refused
// instead of decoding into something else.
CodecError decodeFrames(const uint8_t* payload, size_t bytes,
                        const CodedHeader& header, DeltaState& s,
                        uint64_t frames, int32_t* out) {
  const bool wide = wideUnits(header.bitDepth);
  const int64_t low = codecSampleMin(header.bitDepth);
  const int64_t high = codecSampleMax(header.bitDepth);
  const int channels = header.channels;
  for (uint64_t f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      int32_t d = 0;
      if (!getDifference(payload, bytes, s.offset, wide, d)) return CodecError::Truncated;
      const int64_t v = static_cast<int64_t>(predict(s.value[c], s.prev[c])) + d;
      if (v < low || v > high) return CodecError::CodecFailed;
      s.prev[c] = s.value[c];
      s.value[c] = static_cast<int32_t>(v);
      if (out != nullptr) out[f * static_cast<uint64_t>(channels) + static_cast<uint64_t>(c)] = s.value[c];
    }
    ++s.frame;
  }
  // The last frame has to end exactly where the payload does.
  if (s.frame == header.frames && s.offset != bytes) return CodecError::CodecFailed;
  return CodecError::None;
}

CodecError checkHeader(const CodedHeader& header, const CodecInfo& info) {
  if (header.channels <= 0 || header.channels > kDeltaMaxChannels) return CodecError::BadHeader;
  if (header.bitDepth < info.minBitDepth || header.bitDepth > info.maxBitDepth)
    return CodecError::BadWidth;
  return CodecError::None;
}

const CodecInfo& deltaInfo() {
  static const CodecInfo info = [] {
    CodecInfo i;
    i.id = codecId("PDLT");
    i.name = "predictive delta";
    i.access = CodecAccess::Sequential;
    i.minBitDepth = 8;
    i.maxBitDepth = 24;
    i.blockFrames = 0;
    i.lossless = true;
    return i;
  }();
  return info;
}

class DeltaDecoder : public SequentialDecoder {
public:
  bool start(const uint8_t* payload, size_t payloadBytes,
             const CodedHeader& header, CodecError* error) override {
    const CodecError why = payload == nullptr ? CodecError::BadHeader : checkHeader(header, deltaInfo());
    if (error) *error = why;
    if (why != CodecError::None) return false;
    payload_ = payload;
    bytes_ = payloadBytes;
    header_ = header;
    state_ = {};
    return true;
  }

  bool decodeNext(uint64_t frames, int32_t* out, CodecError* error) override {
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (payload_ == nullptr) return fail(CodecError::BadHeader);
    if (state_.frame + frames > header_.frames) return fail(CodecError::BadRange);
    const CodecError why = decodeFrames(payload_, bytes_, header_, state_, frames, out);
    if (why != CodecError::None) return fail(why);
    if (error) *error = CodecError::None;
    return true;
  }

  uint64_t framesDecoded() const override { return state_.frame; }

  // The state: the frame, the byte offset, then value and prev for every
  // channel, all little-endian.
  std::vector<uint8_t> saveState() const override {
    std::vector<uint8_t> out;
    auto put = [&out](uint64_t v, int bytes) {
      for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
    };
    put(state_.frame, 8);
    put(state_.offset, 8);
    for (int c = 0; c < header_.channels; ++c) {
      put(static_cast<uint32_t>(state_.value[c]), 4);
      put(static_cast<uint32_t>(state_.prev[c]), 4);
    }
    return out;
  }

  bool restoreState(const std::vector<uint8_t>& saved, CodecError* error) override {
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (payload_ == nullptr) return fail(CodecError::BadHeader);
    if (saved.size() != 16 + 8 * static_cast<size_t>(header_.channels)) return fail(CodecError::BadHeader);
    size_t at = 0;
    auto get = [&saved, &at](int bytes) {
      uint64_t v = 0;
      for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(saved[at++]) << (8 * i);
      return v;
    };
    DeltaState s;
    s.frame = get(8);
    s.offset = static_cast<size_t>(get(8));
    for (int c = 0; c < header_.channels; ++c) {
      s.value[c] = static_cast<int32_t>(static_cast<uint32_t>(get(4)));
      s.prev[c] = static_cast<int32_t>(static_cast<uint32_t>(get(4)));
    }
    if (s.frame > header_.frames || s.offset > bytes_) return fail(CodecError::BadRange);
    state_ = s;
    if (error) *error = CodecError::None;
    return true;
  }

private:
  const uint8_t* payload_ = nullptr;
  size_t bytes_ = 0;
  CodedHeader header_;
  DeltaState state_;
};

class DeltaCodec : public SampleCodec {
public:
  const CodecInfo& info() const override { return deltaInfo(); }

  bool encode(const int32_t* samples, uint64_t frames, int channels,
              int bitDepth, uint32_t blockFrames, std::vector<uint8_t>& payload,
              CodecError* error) override {
    (void)blockFrames;
    auto fail = [&](CodecError why) {
      if (error) *error = why;
      return false;
    };
    if (samples == nullptr || frames == 0 || channels <= 0 || channels > kDeltaMaxChannels)
      return fail(CodecError::BadHeader);
    if (bitDepth < deltaInfo().minBitDepth || bitDepth > deltaInfo().maxBitDepth)
      return fail(CodecError::BadWidth);
    const bool wide = wideUnits(bitDepth);
    const int64_t low = codecSampleMin(bitDepth);
    const int64_t high = codecSampleMax(bitDepth);
    payload.clear();
    payload.reserve(static_cast<size_t>(frames) * static_cast<size_t>(channels) * (wide ? 2 : 1));
    int32_t value[kDeltaMaxChannels] = {};
    int32_t prev[kDeltaMaxChannels] = {};
    for (uint64_t f = 0; f < frames; ++f) {
      for (int c = 0; c < channels; ++c) {
        const int32_t v = samples[f * static_cast<uint64_t>(channels) + static_cast<uint64_t>(c)];
        if (v < low || v > high) return fail(CodecError::BadWidth);
        const int32_t d = v - predict(value[c], prev[c]);
        if (!putDifference(payload, d, wide)) return fail(CodecError::CodecFailed);
        prev[c] = value[c];
        value[c] = v;
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
    if (const CodecError why = checkHeader(header, info()); why != CodecError::None) return fail(why);
    if (count == 0 || firstFrame + count > header.frames) return fail(CodecError::BadRange);
    // Forward from the first frame: the frames before the range are decoded
    // and dropped.
    DeltaState s;
    CodecError why = decodeFrames(payload, payloadBytes, header, s, firstFrame, nullptr);
    if (why == CodecError::None) why = decodeFrames(payload, payloadBytes, header, s, count, out);
    if (why != CodecError::None) return fail(why);
    if (error) *error = CodecError::None;
    return true;
  }

  std::unique_ptr<SequentialDecoder> createDecoder() const override {
    return std::make_unique<DeltaDecoder>();
  }
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

std::unique_ptr<SampleCodec> makeDeltaCodec() {
  return std::make_unique<DeltaCodec>();
}

std::unique_ptr<SampleCodec> makePlainCodec(int bitDepth) {
  return std::make_unique<PlainCodec>(bitDepth);
}

void registerBuiltinCodecs(CodecRegistry& registry) {
  registry.add(makeDeltaCodec());
  registry.add(makeSplitRiceCodec());
  registry.add(makePlainCodec(32));
  registry.add(makePlainCodec(24));
  registry.add(makePlainCodec(16));
}

} // namespace mp
