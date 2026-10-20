// Masterpiece -- the sample codec interface: the container, the registry.
//
// The container is the same for every codec: a header that names the
// codec, the width, the block size and the length, a check over the
// payload, and then the payload. A reader that finds a name it does not
// know refuses the sample; it never guesses.
#include "SampleCodec.h"

#include "SampleCodecBuiltins.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace mp {
namespace {

// CRC-32, the same one the lossless block codec uses over its blocks.
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

uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i)
    c = crcTable().v[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void putU32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
  out[at + 0] = static_cast<uint8_t>(value & 0xFFu);
  out[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[at + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[at + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void putU64(std::vector<uint8_t>& out, size_t at, uint64_t value) {
  for (int i = 0; i < 8; ++i)
    out[at + static_cast<size_t>(i)] =
        static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
}

uint32_t getU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t getU64(const uint8_t* data) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(data[i]) << (8 * i);
  return v;
}

constexpr char kMagic[4] = {'M', 'P', 'S', 'C'};

} // namespace

CodecId codecId(const char* four) {
  CodecId id{};
  for (int i = 0; i < 4; ++i) id[static_cast<size_t>(i)] = four[i];
  return id;
}

std::string codecIdText(CodecId id) {
  return std::string(id.data(), id.size());
}

const char* codecErrorText(CodecError error) {
  switch (error) {
  case CodecError::None:
    return "no error";
  case CodecError::UnknownCodec:
    return "unknown codec";
  case CodecError::BadHeader:
    return "the header is not one this codec wrote";
  case CodecError::Truncated:
    return "the data stops short";
  case CodecError::CheckFailed:
    return "the check over the payload does not match";
  case CodecError::BadRange:
    return "the frames asked for lie outside the sample";
  case CodecError::NotSupported:
    return "the codec cannot do that";
  case CodecError::BadWidth:
    return "the samples do not fit the width";
  case CodecError::CodecFailed:
    return "the codec refused";
  }
  return "unknown error";
}

int64_t codecSampleMin(int bitDepth) {
  return -(static_cast<int64_t>(1) << (bitDepth - 1));
}

int64_t codecSampleMax(int bitDepth) {
  return (static_cast<int64_t>(1) << (bitDepth - 1)) - 1;
}

bool writeCodedSample(const SampleCodec& codec, uint64_t frames, int channels,
                      int bitDepth, uint32_t blockFrames,
                      const std::vector<uint8_t>& payload,
                      std::vector<uint8_t>& out, CodecError* error) {
  auto fail = [&](CodecError why) {
    if (error) *error = why;
    return false;
  };
  if (channels <= 0 || frames == 0) return fail(CodecError::BadHeader);
  if (payload.size() > 0xFFFFFFFFull) return fail(CodecError::BadHeader);

  out.assign(kCodedHeaderBytes + payload.size(), 0);
  std::memcpy(out.data(), kMagic, 4);
  const CodecId id = codec.info().id;
  std::memcpy(out.data() + 4, id.data(), id.size());
  putU64(out, 8, frames);
  out[16] = static_cast<uint8_t>(channels & 0xFF);
  out[17] = static_cast<uint8_t>((channels >> 8) & 0xFF);
  out[18] = static_cast<uint8_t>(bitDepth & 0xFF);
  out[19] = 0;
  putU32(out, 20, blockFrames);
  putU32(out, 24, static_cast<uint32_t>(payload.size()));
  putU32(out, 28, crc32(payload.data(), payload.size()));
  if (!payload.empty())
    std::memcpy(out.data() + kCodedHeaderBytes, payload.data(), payload.size());
  if (error) *error = CodecError::None;
  return true;
}

bool readCodedSample(const uint8_t* data, size_t size, CodedHeader& header,
                     std::vector<uint8_t>& payload, CodecError* error) {
  auto fail = [&](CodecError why) {
    if (error) *error = why;
    return false;
  };
  if (data == nullptr || size < kCodedHeaderBytes) return fail(CodecError::Truncated);
  if (std::memcmp(data, kMagic, 4) != 0) return fail(CodecError::BadHeader);

  CodecId id{};
  std::memcpy(id.data(), data + 4, id.size());
  header.id = id;
  header.frames = getU64(data + 8);
  header.channels = static_cast<int>(data[16]) |
                    (static_cast<int>(data[17]) << 8);
  header.bitDepth = data[18];
  header.blockFrames = getU32(data + 20);
  header.payloadBytes = getU32(data + 24);
  const uint32_t wantCheck = getU32(data + 28);

  if (header.channels <= 0 || header.frames == 0) return fail(CodecError::BadHeader);
  if (header.payloadBytes > size - kCodedHeaderBytes) return fail(CodecError::Truncated);

  payload.assign(data + kCodedHeaderBytes, data + kCodedHeaderBytes + header.payloadBytes);
  if (crc32(payload.data(), payload.size()) != wantCheck)
    return fail(CodecError::CheckFailed);

  // A name nobody answers to is refused. The sample is not guessed at.
  if (CodecRegistry::instance().find(header.id) == nullptr)
    return fail(CodecError::UnknownCodec);

  if (error) *error = CodecError::None;
  return true;
}

CodecId codedSampleId(const uint8_t* data, size_t size) {
  CodecId empty{};
  if (data == nullptr || size < 8) return empty;
  if (std::memcmp(data, kMagic, 4) != 0) return empty;
  CodecId id{};
  std::memcpy(id.data(), data + 4, id.size());
  return id;
}

// ---------------------------------------------------------------- registry

struct CodecRegistry::Impl {
  std::vector<std::unique_ptr<SampleCodec>> codecs;
  std::unordered_map<uint64_t, size_t> byId;
};

namespace {
uint64_t idKey(CodecId id) {
  return (static_cast<uint64_t>(static_cast<unsigned char>(id[0])) << 24) |
         (static_cast<uint64_t>(static_cast<unsigned char>(id[1])) << 16) |
         (static_cast<uint64_t>(static_cast<unsigned char>(id[2])) << 8) |
         static_cast<uint64_t>(static_cast<unsigned char>(id[3]));
}
} // namespace

CodecRegistry::CodecRegistry() : impl_(new Impl()) {
  // The codecs that ship with the program are there from the start, so
  // that a reader of a cached sample finds the codec that made it without
  // anyone having to ask for it.
  registerBuiltinCodecs(*this);
}

CodecRegistry& CodecRegistry::instance() {
  static CodecRegistry registry;
  return registry;
}

bool CodecRegistry::add(std::unique_ptr<SampleCodec> codec) {
  if (!codec) return false;
  const uint64_t key = idKey(codec->info().id);
  if (impl_->byId.count(key) != 0) return false;
  impl_->byId[key] = impl_->codecs.size();
  impl_->codecs.push_back(std::move(codec));
  return true;
}

SampleCodec* CodecRegistry::find(CodecId id) const {
  const auto it = impl_->byId.find(idKey(id));
  if (it == impl_->byId.end()) return nullptr;
  return impl_->codecs[it->second].get();
}

SampleCodec* CodecRegistry::find(const char* four) const {
  if (four == nullptr) return nullptr;
  return find(codecId(four));
}

std::vector<SampleCodec*> CodecRegistry::all() const {
  std::vector<SampleCodec*> out;
  out.reserve(impl_->codecs.size());
  for (auto& codec : impl_->codecs) out.push_back(codec.get());
  return out;
}

} // namespace mp
