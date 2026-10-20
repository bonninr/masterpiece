// Masterpiece -- lossless block codec. See LosslessBlocks.h for the format.
//
// The work is deliberately plain: fixed predictors, Rice codes, no entropy
// coder, no LPC. It is a measurement of what a simple scheme buys, so that
// the engine design can be chosen from numbers rather than from taste.
#include "LosslessBlocks.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace mp {
namespace {

constexpr uint32_t kMagic = 0x424C504Du; // "MPLB"
constexpr size_t kHeaderBytes = 20;
constexpr size_t kCheckOffset = 16;
constexpr int kRiceParamBits = 5;
constexpr int kMaxRiceParam = 30;
// The flags word: bit 0 says the two channels are stored as left and side.
constexpr uint8_t kFlagLeftSide = 1u;

// ------------------------------------------------------------------ CRC-32

struct Crc32Table {
  uint32_t v[256];
  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      v[i] = c;
    }
  }
};

const Crc32Table& crcTable() {
  static const Crc32Table table;
  return table;
}

uint32_t crc32Of(const unsigned char* data, size_t count) {
  const Crc32Table& t = crcTable();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < count; ++i)
    crc = t.v[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

// -------------------------------------------------------------- bit output

// Writes most significant bit first, into a byte vector.
class BitWriter {
public:
  explicit BitWriter(std::vector<unsigned char>& out) : out_(out) {}

  void write(uint32_t value, int bits) {
    if (bits <= 0)
      return;
    acc_ = (acc_ << bits) | (value & ((1ull << bits) - 1ull));
    nbits_ += bits;
    while (nbits_ >= 8) {
      nbits_ -= 8;
      out_.push_back(static_cast<unsigned char>((acc_ >> nbits_) & 0xFFu));
    }
    acc_ &= (1ull << nbits_) - 1ull;
  }

  // The unary part of a Rice code: `count` ones and a zero.
  void writeUnary(uint32_t count) {
    while (count >= 32) {
      write(0xFFFFFFFFu, 32);
      count -= 32;
    }
    if (count > 0)
      write((1u << count) - 1u, static_cast<int>(count));
    write(0u, 1);
  }

  void writeRice(uint32_t value, int k) {
    writeUnary(value >> k);
    if (k > 0)
      write(value & ((1u << k) - 1u), k);
  }

  // Pads to a byte boundary with zeros.
  void flush() {
    if (nbits_ > 0) {
      out_.push_back(static_cast<unsigned char>((acc_ << (8 - nbits_)) & 0xFFu));
      nbits_ = 0;
      acc_ = 0;
    }
  }

private:
  std::vector<unsigned char>& out_;
  uint64_t acc_ = 0;
  int nbits_ = 0;
};

// --------------------------------------------------------------- bit input

// Reads most significant bit first, from a byte range. Every read reports
// whether it had the bits; a reader that runs past the end fails rather than
// reading memory it does not own.
class BitReader {
public:
  BitReader(const unsigned char* data, size_t size)
      : data_(data), size_(size) {}

  bool read(uint32_t& value, int bits) {
    if (bits <= 0) {
      value = 0;
      return true;
    }
    if (!fill(bits))
      return false;
    nbits_ -= bits;
    value = static_cast<uint32_t>((acc_ >> nbits_) & ((1ull << bits) - 1ull));
    acc_ &= (1ull << nbits_) - 1ull;
    return true;
  }

  // The unary part of a Rice code: the ones before the zero.
  bool readUnary(uint32_t& count) {
    count = 0;
    for (;;) {
      if (nbits_ == 0) {
        if (pos_ >= size_)
          return false;
        acc_ = data_[pos_++];
        nbits_ = 8;
      }
      if (nbits_ >= 8) {
        const uint32_t byte =
            static_cast<uint32_t>((acc_ >> (nbits_ - 8)) & 0xFFu);
        if (byte == 0xFFu) {
          count += 8;
          nbits_ -= 8;
          acc_ &= (1ull << nbits_) - 1ull;
          continue;
        }
        int ones = 0;
        while (ones < 8 && (byte & (0x80u >> ones)) != 0)
          ++ones;
        count += static_cast<uint32_t>(ones);
        nbits_ -= ones + 1;
        acc_ &= (1ull << nbits_) - 1ull;
        return true;
      }
      if ((acc_ & (1ull << (nbits_ - 1))) != 0) {
        ++count;
        --nbits_;
      } else {
        --nbits_;
        acc_ &= (1ull << nbits_) - 1ull;
        return true;
      }
      acc_ &= (1ull << nbits_) - 1ull;
    }
  }

  bool readRice(uint32_t& value, int k) {
    uint32_t count = 0;
    if (!readUnary(count))
      return false;
    if (count > (0xFFFFFFFFu >> k))
      return false; // the code does not fit the value it claims to hold
    uint32_t low = 0;
    if (k > 0 && !read(low, k))
      return false;
    value = (count << k) | low;
    return true;
  }

  // The bytes fully consumed. The writer pads every stream to a byte
  // boundary, so this is where the next stream starts.
  size_t bytesConsumed() const { return pos_ - nbits_ / 8; }

private:
  bool fill(int want) {
    while (nbits_ < want) {
      if (pos_ >= size_)
        return false;
      // The newest byte goes below the bits already held: the stream is read
      // most significant bit first, so the earliest bits stay on top.
      acc_ = (acc_ << 8) | data_[pos_++];
      nbits_ += 8;
    }
    return true;
  }

  const unsigned char* data_;
  size_t size_;
  size_t pos_ = 0;
  uint64_t acc_ = 0;
  int nbits_ = 0;
};

// ------------------------------------------------------------------ values

inline uint32_t zigzag(int32_t value) {
  const uint32_t u = static_cast<uint32_t>(value);
  return (u << 1) ^ (value < 0 ? 0xFFFFFFFFu : 0u);
}

inline int32_t unzigzag(uint32_t value) {
  return static_cast<int32_t>((value >> 1) ^ (0u - (value & 1u)));
}

// A value of `bits` bits, signed.
inline int64_t signExtend(uint32_t value, int bits) {
  const uint32_t mask = (1u << bits) - 1u;
  value &= mask;
  const uint32_t sign = 1u << (bits - 1);
  if ((value & sign) != 0)
    return static_cast<int64_t>(value) - static_cast<int64_t>(sign) * 2;
  return static_cast<int64_t>(value);
}

inline int64_t minSample(int bitDepth) {
  return -(static_cast<int64_t>(1) << (bitDepth - 1));
}

inline int64_t maxSample(int bitDepth) {
  return (static_cast<int64_t>(1) << (bitDepth - 1)) - 1;
}

inline int floorLog2(uint64_t value) {
  int result = 0;
  while ((value >>= 1) != 0)
    ++result;
  return result;
}

// The residual of order `order` at index j.
inline int32_t residualAt(const int32_t* x, size_t j, int order) {
  if (order == 0)
    return x[j];
  if (order == 1)
    return static_cast<int32_t>(static_cast<int64_t>(x[j]) - x[j - 1]);
  return static_cast<int32_t>(static_cast<int64_t>(x[j]) - 2ll * x[j - 1] +
                              x[j - 2]);
}

// The value the predictor of order `order` expects at index j.
inline int64_t predictedAt(const int32_t* x, size_t j, int order) {
  if (order == 0)
    return 0;
  if (order == 1)
    return x[j - 1];
  return 2ll * x[j - 1] - x[j - 2];
}

// ----------------------------------------------------------------- picking

// What one channel of one block would cost, and with which predictor.
struct ChannelCost {
  uint64_t bits = std::numeric_limits<uint64_t>::max();
  int order = 0;
};

// The estimated cost of coding a channel with each fixed predictor, and the
// best of them. The estimate is the number of bits the Rice codes would take
// with the best parameter for the whole block, which is close enough to
// choose between the three and costs a pass each.
ChannelCost estimateChannel(const int32_t* x, size_t frames, int maxOrder,
                             int bitDepth) {
  ChannelCost best;
  for (int order = 0; order <= maxOrder; ++order) {
    const size_t count = frames - static_cast<size_t>(order);
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i)
      sum += zigzag(residualAt(x, order + i, order));
    const uint64_t mean = count > 0 ? sum / count : 0;
    const int k = mean > 0 ? floorLog2(mean) : 0;
    const uint64_t bits = static_cast<uint64_t>(count) * (k + 1) + (sum >> k) +
                          static_cast<uint64_t>(order) * bitDepth;
    if (bits < best.bits) {
      best.bits = bits;
      best.order = order;
    }
  }
  return best;
}

// The Rice parameter of one partition: the one whose codes are shortest.
// The search runs around the parameter the mean suggests, and every
// candidate is measured exactly.
int chooseRiceParameter(const uint32_t* values, size_t count) {
  if (count == 0)
    return 0;
  uint64_t sum = 0;
  for (size_t i = 0; i < count; ++i)
    sum += values[i];
  const uint64_t mean = sum / count;
  const int guess = mean > 0 ? floorLog2(mean) : 0;
  const int low = std::max(0, guess - 3);
  const int high = std::min(kMaxRiceParam, guess + 3);
  // The span is at most seven: the guess moves three on each side.
  uint64_t quotients[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < count; ++i) {
    const uint32_t value = values[i];
    for (int k = low; k <= high; ++k)
      quotients[k - low] += (value >> k);
  }
  int best = low;
  uint64_t bestBits = std::numeric_limits<uint64_t>::max();
  for (int k = low; k <= high; ++k) {
    const uint64_t bits = static_cast<uint64_t>(count) * (k + 1) +
                          quotients[k - low];
    if (bits < bestBits) {
      bestBits = bits;
      best = k;
    }
  }
  return best;
}

// ---------------------------------------------------------------- encoding

// The raw samples of the warmup, packed least significant bit first.
void packRaw(const int32_t* x, size_t count, int bitDepth,
             std::vector<unsigned char>& out) {
  if (count == 0)
    return;
  const uint64_t mask = (1ull << bitDepth) - 1ull;
  uint64_t acc = 0;
  for (size_t i = 0; i < count; ++i) {
    // Only the bits of the width: the sign extension of a negative sample
    // would otherwise land in the next sample.
    const uint64_t value =
        static_cast<uint64_t>(static_cast<uint32_t>(x[i])) & mask;
    acc |= value << (i * bitDepth);
  }
  const size_t bytes = (count * static_cast<size_t>(bitDepth) + 7) / 8;
  for (size_t b = 0; b < bytes; ++b)
    out.push_back(static_cast<unsigned char>((acc >> (8 * b)) & 0xFFu));
}

void unpackRaw(const unsigned char* data, size_t count, int bitDepth,
               int32_t* x) {
  if (count == 0)
    return;
  const size_t bytes = (count * static_cast<size_t>(bitDepth) + 7) / 8;
  uint64_t acc = 0;
  for (size_t b = 0; b < bytes; ++b)
    acc |= static_cast<uint64_t>(data[b]) << (8 * b);
  const uint32_t mask = (1u << bitDepth) - 1u;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t value =
        static_cast<uint32_t>((acc >> (i * bitDepth)) & mask);
    x[i] = static_cast<int32_t>(signExtend(value, bitDepth));
  }
}

// One channel of one block: the order byte, the raw warmup, the Rice
// parameters and the residuals. `wide` says the channel is one bit wider
// than the block, which the side channel of a stereo pair needs: it is the
// difference of two channels and reaches twice their width.
void encodeChannel(const int32_t* x, size_t frames, int order, int bitDepth,
                   bool wide, std::vector<unsigned char>& out) {
  out.push_back(static_cast<unsigned char>(order | (wide ? 4 : 0)));
  packRaw(x, static_cast<size_t>(order), bitDepth + (wide ? 1 : 0), out);

  const size_t count = frames - static_cast<size_t>(order);
  std::vector<uint32_t> values(count);
  for (size_t i = 0; i < count; ++i)
    values[i] = zigzag(residualAt(x, order + i, order));

  const size_t partition = kLosslessRicePartition;
  const size_t parts = (count + partition - 1) / partition;
  std::vector<uint8_t> parameters(parts);
  for (size_t p = 0; p < parts; ++p) {
    const size_t from = p * partition;
    const size_t n = std::min(partition, count - from);
    parameters[p] = static_cast<uint8_t>(chooseRiceParameter(&values[from], n));
  }

  out.push_back(static_cast<unsigned char>(parts & 0xFFu));
  out.push_back(static_cast<unsigned char>((parts >> 8) & 0xFFu));
  {
    BitWriter writer(out);
    for (size_t p = 0; p < parts; ++p)
      writer.write(parameters[p], kRiceParamBits);
    writer.flush();
  }
  {
    BitWriter writer(out);
    for (size_t p = 0; p < parts; ++p) {
      const size_t from = p * partition;
      const size_t n = std::min(partition, count - from);
      const int k = parameters[p];
      for (size_t i = 0; i < n; ++i)
        writer.writeRice(values[from + i], k);
    }
    writer.flush();
  }
}

// One channel of one block, read back.
bool decodeChannelStream(const unsigned char* data, size_t size, size_t& pos,
                         int bitDepth, size_t frames, int32_t* x,
                         LosslessError* error) {
  if (pos >= size) {
    *error = LosslessError::Truncated;
    return false;
  }
  const unsigned char orderByte = data[pos++];
  if (orderByte > 7) {
    *error = LosslessError::CorruptData;
    return false;
  }
  const int order = orderByte & 3;
  const int width = bitDepth + ((orderByte & 4) != 0 ? 1 : 0);
  if (static_cast<size_t>(order) > frames) {
    *error = LosslessError::CorruptData;
    return false;
  }

  const size_t warmBytes = (static_cast<size_t>(order) * width + 7) / 8;
  if (size - pos < warmBytes) {
    *error = LosslessError::Truncated;
    return false;
  }
  unpackRaw(data + pos, static_cast<size_t>(order), width, x);
  pos += warmBytes;

  if (size - pos < 2) {
    *error = LosslessError::Truncated;
    return false;
  }
  const size_t parts = static_cast<size_t>(data[pos]) |
                       (static_cast<size_t>(data[pos + 1]) << 8);
  pos += 2;

  const size_t count = frames - static_cast<size_t>(order);
  const size_t expected =
      (count + kLosslessRicePartition - 1) / kLosslessRicePartition;
  if (parts != expected) {
    *error = LosslessError::CorruptData;
    return false;
  }

  const size_t paramBytes = (parts * kRiceParamBits + 7) / 8;
  if (size - pos < paramBytes) {
    *error = LosslessError::Truncated;
    return false;
  }
  std::vector<uint8_t> parameters(parts);
  {
    BitReader reader(data + pos, paramBytes);
    for (size_t p = 0; p < parts; ++p) {
      uint32_t value = 0;
      if (!reader.read(value, kRiceParamBits)) {
        *error = LosslessError::CorruptData;
        return false;
      }
      parameters[p] = static_cast<uint8_t>(value);
    }
  }
  pos += paramBytes;

  {
    BitReader reader(data + pos, size - pos);
    for (size_t i = 0; i < count; ++i) {
      const size_t p = i / kLosslessRicePartition;
      uint32_t value = 0;
      if (!reader.readRice(value, parameters[p])) {
        *error = LosslessError::CorruptData;
        return false;
      }
      const size_t j = static_cast<size_t>(order) + i;
      const int64_t value64 =
          static_cast<int64_t>(unzigzag(value)) + predictedAt(x, j, order);
      if (value64 < minSample(width) || value64 > maxSample(width)) {
        *error = LosslessError::CorruptData;
        return false;
      }
      x[j] = static_cast<int32_t>(value64);
    }
    pos += reader.bytesConsumed();
  }
  return true;
}

// ------------------------------------------------------------------ header

void writeU16(unsigned char* p, uint16_t value) {
  p[0] = static_cast<unsigned char>(value & 0xFFu);
  p[1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
}

void writeU32(unsigned char* p, uint32_t value) {
  p[0] = static_cast<unsigned char>(value & 0xFFu);
  p[1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
  p[2] = static_cast<unsigned char>((value >> 16) & 0xFFu);
  p[3] = static_cast<unsigned char>((value >> 24) & 0xFFu);
}

uint16_t readU16(const unsigned char* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// ----------------------------------------------------------------- a block

// Codes one block: the header and the payload, appended to `out`.
void encodeBlockInto(const int32_t* src, size_t frames, int channels,
                     int bitDepth, std::vector<unsigned char>& out) {
  // The channels, taken apart.
  std::vector<std::vector<int32_t>> lane(channels);
  for (int c = 0; c < channels; ++c)
    lane[c].resize(frames);
  for (size_t f = 0; f < frames; ++f)
    for (int c = 0; c < channels; ++c)
      lane[c][f] = src[f * static_cast<size_t>(channels) + c];

  // The side channel of a stereo pair: the difference.
  const int maxOrder =
      frames > 2 ? 2 : (frames > 1 ? 1 : 0);
  std::vector<ChannelCost> cost(channels);
  for (int c = 0; c < channels; ++c)
    cost[c] = estimateChannel(lane[c].data(), frames, maxOrder, bitDepth);

  bool leftSide = false;
  if (channels == 2) {
    std::vector<int32_t> side(frames);
    for (size_t f = 0; f < frames; ++f)
      side[f] = static_cast<int32_t>(static_cast<int64_t>(lane[1][f]) -
                                     lane[0][f]);
    const ChannelCost sideCost =
        estimateChannel(side.data(), frames, maxOrder, bitDepth);
    leftSide = sideCost.bits < cost[1].bits;
    if (leftSide) {
      lane[1] = side;
      cost[1] = sideCost;
    }
  }

  std::vector<unsigned char> payload;
  for (int c = 0; c < channels; ++c) {
    // The side channel is the difference of two channels: it is stored one
    // bit wider than the block.
    const bool wide = leftSide && c == 1;
    encodeChannel(lane[c].data(), frames, cost[c].order, bitDepth, wide,
                  payload);
  }

  const size_t start = out.size();
  out.resize(start + kHeaderBytes, 0);
  unsigned char* header = out.data() + start;
  writeU32(header, kMagic);
  writeU16(header + 4, static_cast<uint16_t>(frames));
  header[6] = static_cast<unsigned char>(channels);
  header[7] = static_cast<unsigned char>(bitDepth);
  header[8] = leftSide ? kFlagLeftSide : 0u;
  header[9] = 0;
  writeU16(header + 10, static_cast<uint16_t>(kLosslessRicePartition));
  writeU32(header + 12, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());

  uint32_t crc = crc32Of(out.data() + start, kCheckOffset);
  crc = crc32Of(out.data() + start + kHeaderBytes, payload.size()) ^ crc;
  writeU32(out.data() + start + kCheckOffset, crc);
}

// Reads one block from a byte range. `out` receives frames * channels
// interleaved values.
bool decodeBlockAt(const unsigned char* data, size_t size, size_t offset,
                   int32_t* out, uint16_t* framesOut, int* channelsOut,
                   int* bitDepthOut, LosslessError* error) {
  if (offset > size || size - offset < kHeaderBytes) {
    *error = LosslessError::Truncated;
    return false;
  }
  const unsigned char* header = data + offset;
  if (readU32(header) != kMagic) {
    *error = LosslessError::BadHeader;
    return false;
  }
  const uint16_t frames = readU16(header + 4);
  const int channels = header[6];
  const int bitDepth = header[7];
  const uint8_t flags = header[8];
  const uint16_t partition = readU16(header + 10);
  const uint32_t payloadBytes = readU32(header + 12);
  const uint32_t check = readU32(header + kCheckOffset);

  if (frames == 0) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if (channels < 1 || channels > kLosslessMaxChannels) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if (bitDepth < kLosslessMinBitDepth || bitDepth > kLosslessMaxBitDepth) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if ((flags & ~kFlagLeftSide) != 0) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if (partition != kLosslessRicePartition) {
    *error = LosslessError::BadHeader;
    return false;
  }
  const size_t payloadStart = offset + kHeaderBytes;
  if (payloadStart > size || size - payloadStart < payloadBytes) {
    *error = LosslessError::Truncated;
    return false;
  }

  uint32_t crc = crc32Of(data + offset, kCheckOffset);
  crc = crc32Of(data + payloadStart, payloadBytes) ^ crc;
  if (crc != check) {
    *error = LosslessError::CheckFailed;
    return false;
  }

  const bool leftSide = (flags & kFlagLeftSide) != 0;
  if (leftSide && channels != 2) {
    *error = LosslessError::BadHeader;
    return false;
  }

  std::vector<std::vector<int32_t>> lane(channels);
  for (int c = 0; c < channels; ++c)
    lane[c].resize(frames);
  size_t pos = 0;
  for (int c = 0; c < channels; ++c) {
    if (!decodeChannelStream(data + payloadStart, payloadBytes, pos, bitDepth,
                              frames, lane[c].data(), error))
      return false;
  }
  if (pos != payloadBytes) {
    *error = LosslessError::CorruptData;
    return false;
  }

  for (size_t f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      int64_t value = lane[c][f];
      if (leftSide && c == 1)
        value += lane[0][f];
      if (value < minSample(bitDepth) || value > maxSample(bitDepth)) {
        *error = LosslessError::CorruptData;
        return false;
      }
      out[f * static_cast<size_t>(channels) + c] = static_cast<int32_t>(value);
    }
  }

  if (framesOut != nullptr)
    *framesOut = frames;
  if (channelsOut != nullptr)
    *channelsOut = channels;
  if (bitDepthOut != nullptr)
    *bitDepthOut = bitDepth;
  return true;
}

} // namespace

// ---------------------------------------------------------------- the API

const char* losslessErrorText(LosslessError error) {
  switch (error) {
  case LosslessError::None:
    return "no error";
  case LosslessError::BadHeader:
    return "the block header is not one this codec wrote";
  case LosslessError::Truncated:
    return "the block ends before its payload does";
  case LosslessError::CheckFailed:
    return "the block failed its check";
  case LosslessError::BadStream:
    return "the block table does not match the data";
  case LosslessError::OutOfRange:
    return "the frames asked for lie outside the stream";
  case LosslessError::SamplesOutOfRange:
    return "a sample does not fit the width";
  case LosslessError::CorruptData:
    return "the payload does not decode";
  }
  return "unknown error";
}

uint64_t LosslessStream::framesInBlock(size_t index) const {
  if (index >= blockOffsets.size())
    return 0;
  const uint64_t start = static_cast<uint64_t>(index) * blockFrames;
  if (start >= numFrames)
    return 0;
  const uint64_t left = numFrames - start;
  return left < blockFrames ? left : blockFrames;
}

void LosslessStream::clear() {
  data.clear();
  blockOffsets.clear();
  numFrames = 0;
  channels = 0;
  bitDepth = 0;
  blockFrames = kLosslessBlockFrames;
}

bool encodeLosslessBlocks(const int32_t* samples, uint64_t numFrames,
                           int channels, int bitDepth, LosslessStream& out,
                           LosslessError* error, uint32_t blockFrames) {
  LosslessError local = LosslessError::None;
  if (error == nullptr)
    error = &local;
  out.clear();
  *error = LosslessError::None;

  if (channels < 1 || channels > kLosslessMaxChannels) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if (bitDepth < kLosslessMinBitDepth || bitDepth > kLosslessMaxBitDepth) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if (blockFrames == 0 || blockFrames > 65535) {
    *error = LosslessError::BadHeader;
    return false;
  }
  if (numFrames > 0 && samples == nullptr) {
    *error = LosslessError::OutOfRange;
    return false;
  }
  const int64_t low = minSample(bitDepth);
  const int64_t high = maxSample(bitDepth);
  const uint64_t count = numFrames * static_cast<uint64_t>(channels);
  for (uint64_t i = 0; i < count; ++i) {
    const int32_t value = samples[i];
    if (static_cast<int64_t>(value) < low || static_cast<int64_t>(value) > high) {
      *error = LosslessError::SamplesOutOfRange;
      return false;
    }
  }

  out.numFrames = numFrames;
  out.channels = channels;
  out.bitDepth = bitDepth;
  out.blockFrames = blockFrames;
  const uint64_t blocks =
      (numFrames + blockFrames - 1) / blockFrames;
  out.blockOffsets.reserve(static_cast<size_t>(blocks));
  for (uint64_t b = 0; b < blocks; ++b) {
    const uint64_t start = b * blockFrames;
    const size_t frames = static_cast<size_t>(
        std::min<uint64_t>(blockFrames, numFrames - start));
    out.blockOffsets.push_back(out.data.size());
    encodeBlockInto(samples + start * static_cast<uint64_t>(channels), frames,
                    channels, bitDepth, out.data);
  }
  return true;
}

bool decodeLosslessBlock(const LosslessStream& stream, size_t blockIndex,
                          int32_t* out, uint16_t* frames, LosslessError* error) {
  LosslessError local = LosslessError::None;
  if (error == nullptr)
    error = &local;
  *error = LosslessError::None;
  if (blockIndex >= stream.blockOffsets.size()) {
    *error = LosslessError::OutOfRange;
    return false;
  }
  const uint64_t offset = stream.blockOffsets[blockIndex];
  if (offset > stream.data.size()) {
    *error = LosslessError::BadStream;
    return false;
  }
  return decodeBlockAt(stream.data.data(), stream.data.size(),
                        static_cast<size_t>(offset), out, frames, nullptr,
                        nullptr, error);
}

bool decodeLosslessFrames(const LosslessStream& stream, uint64_t firstFrame,
                           uint64_t count, int32_t* out, LosslessError* error) {
  LosslessError local = LosslessError::None;
  if (error == nullptr)
    error = &local;
  *error = LosslessError::None;

  if (stream.channels < 1 || stream.blockFrames == 0) {
    *error = LosslessError::BadStream;
    return false;
  }
  if (count == 0)
    return true;
  if (firstFrame > stream.numFrames ||
      count > stream.numFrames - firstFrame) {
    *error = LosslessError::OutOfRange;
    return false;
  }
  if (out == nullptr) {
    *error = LosslessError::OutOfRange;
    return false;
  }

  const uint64_t first = firstFrame / stream.blockFrames;
  const uint64_t last = (firstFrame + count - 1) / stream.blockFrames;
  if (last >= stream.blockOffsets.size()) {
    *error = LosslessError::BadStream;
    return false;
  }

  const size_t channels = static_cast<size_t>(stream.channels);
  std::vector<int32_t> scratch(
      static_cast<size_t>(stream.blockFrames) * channels);
  for (uint64_t b = first; b <= last; ++b) {
    uint16_t frames = 0;
    if (!decodeLosslessBlock(stream, static_cast<size_t>(b), scratch.data(),
                             &frames, error))
      return false;
    if (frames > stream.blockFrames) {
      *error = LosslessError::BadStream;
      return false;
    }
    const uint64_t blockStart = b * stream.blockFrames;
    const uint64_t from = std::max(firstFrame, blockStart);
    const uint64_t to = std::min(firstFrame + count,
                                 blockStart + stream.blockFrames);
    for (uint64_t f = from; f < to; ++f) {
      const size_t src = static_cast<size_t>(f - blockStart) * channels;
      const size_t dst = static_cast<size_t>(f - firstFrame) * channels;
      std::memcpy(out + dst, scratch.data() + src, channels * sizeof(int32_t));
    }
  }
  return true;
}

} // namespace mp
