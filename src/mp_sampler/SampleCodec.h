// Masterpiece -- one interface for every sample codec.
//
// A codec turns interleaved integer samples into bytes and back. The
// interface is JUCE-free and lives in mp_sampler, next to the engine that
// will read coded samples, and it is what a later codec plugs into: the
// sample cache has to know which codec made the bytes it reads, and the
// streaming lab measures codecs side by side. The program ships one codec
// that compresses (the predictive delta codec, SampleCodecBuiltins.h) and
// plain storage.
//
// The name of a codec is four characters, and it is written into every
// coded sample. A cache that finds a name it does not know refuses the
// sample. It never guesses.
//
// The samples themselves are interleaved integers, frame-major: the value
// of frame f, channel c is at f * channels + c. The width they are held at
// is part of the coded sample's header, so one codec can carry several
// widths and the reader knows which one it is looking at.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mp {

// The name of a codec: four characters, as they appear in the header of a
// coded sample.
using CodecId = std::array<char, 4>;

// The name of a codec, written as four characters.
CodecId codecId(const char* four);

// The name as text, for a log line or a row of a table.
std::string codecIdText(CodecId id);

// What a codec can do.
enum class CodecAccess {
  RandomAccess,  // any range of frames can be decoded on its own
  Sequential,    // forward only, from a state that can be saved
};

// What a codec is. The widths and the block size describe the codec
// family; the header of a coded sample says which of them a particular
// sample uses.
struct CodecInfo {
  CodecId id{};
  const char* name = "";
  CodecAccess access = CodecAccess::RandomAccess;
  // The narrowest and the widest sample the codec holds. A codec that
  // holds one width only says so.
  int minBitDepth = 16;
  int maxBitDepth = 24;
  // The frames in one independently decodable block. 0 when the codec has
  // no blocks.
  uint32_t blockFrames = 0;
  // True when the codec gives back exactly the samples it was given.
  bool lossless = true;
};

// Why a call failed. A call either does what it says, or reports one of
// these and produces nothing.
enum class CodecError {
  None,
  UnknownCodec,   // the name in the header is one no registered codec answers to
  BadHeader,      // the header is not one this codec wrote
  Truncated,      // the data stops before the header says it does
  CheckFailed,    // the check over the payload does not match
  BadRange,       // the frames asked for lie outside the sample
  NotSupported,   // the codec cannot do what was asked
  BadWidth,       // the samples do not fit the width the header names
  CodecFailed,    // the codec itself refused
};

const char* codecErrorText(CodecError error);

// The smallest and the largest value a sample of that width can hold.
// A width of 24 means [-8388608, 8388607].
int64_t codecSampleMin(int bitDepth);
int64_t codecSampleMax(int bitDepth);

// The header of a coded sample. Every coded sample carries one, so a
// reader knows the codec, the width, the block size and the length before
// it decodes anything.
//
// Header layout, all little-endian:
//
//   offset  size  what
//   0       4     magic "MPSC"
//   4       4     the codec's name
//   8       8     the frames in the sample
//   16      2     the channels
//   18      1     the width in bits
//   19      1     reserved, zero
//   20      4     the frames in one block
//   24      4     the payload's bytes
//   28      4     CRC-32 over the payload
//   32      ...   the payload
inline constexpr size_t kCodedHeaderBytes = 32;

struct CodedHeader {
  CodecId id{};
  uint64_t frames = 0;
  int channels = 0;
  int bitDepth = 0;
  uint32_t blockFrames = 0;
  uint32_t payloadBytes = 0;
};

// One codec.
//
// The payload is the codec's own bytes. The header around it is written by
// the functions below, so every codec's coded sample looks the same from
// the outside and a cache can read one without knowing the codec.
class SampleCodec {
public:
  virtual ~SampleCodec() = default;

  virtual const CodecInfo& info() const = 0;

  // Encodes interleaved samples (frames * channels values, frame-major)
  // into the payload. The samples must fit the width the header will
  // name; a codec refuses samples that do not. The width and the block
  // size are given here and are written into the header, so one codec
  // carries samples of several widths.
  virtual bool encode(const int32_t* samples, uint64_t frames, int channels,
                      int bitDepth, uint32_t blockFrames,
                      std::vector<uint8_t>& payload, CodecError* error) = 0;

  // Decodes a range of frames into interleaved samples. The header says
  // which width and block size the payload was written with. A codec with
  // random access goes straight to the range; a sequential one decodes
  // forward from the first frame, which is right but slow, and a voice
  // uses its SequentialDecoder instead.
  virtual bool decodeRange(const uint8_t* payload, size_t payloadBytes,
                           const CodedHeader& header, uint64_t firstFrame,
                           uint64_t count, int32_t* out,
                           CodecError* error) = 0;

  // A decoder for a codec that can only go forward. Null for a codec with
  // random access.
  virtual std::unique_ptr<class SequentialDecoder> createDecoder() const {
    return nullptr;
  }
};

// A codec that can only decode forward.
//
// The state is small and can be saved, so a voice can keep it and carry on
// from there: that is what a ring buffer needs when a decoder thread and
// the audio thread share one sample.
class SequentialDecoder {
public:
  virtual ~SequentialDecoder() = default;

  // Starts at the beginning of the payload.
  virtual bool start(const uint8_t* payload, size_t payloadBytes,
                     const CodedHeader& header, CodecError* error) = 0;

  // Decodes the next frames. Fails when the payload ends.
  virtual bool decodeNext(uint64_t frames, int32_t* out, CodecError* error) = 0;

  // How many frames have been decoded.
  virtual uint64_t framesDecoded() const = 0;

  // The state, small enough to keep in a voice.
  virtual std::vector<uint8_t> saveState() const = 0;
  virtual bool restoreState(const std::vector<uint8_t>& state,
                            CodecError* error) = 0;
};

// The codecs this build knows. A codec outside the repository -- the
// streaming lab has several -- is added to the registry before it is used,
// and the registry is the only place that is asked.
class CodecRegistry {
public:
  static CodecRegistry& instance();

  // Adds a codec. Fails when the name is taken or the codec is null.
  bool add(std::unique_ptr<SampleCodec> codec);

  // The codec with that name, or null.
  SampleCodec* find(CodecId id) const;
  SampleCodec* find(const char* four) const;

  // Every codec, in the order they were added.
  std::vector<SampleCodec*> all() const;

private:
  CodecRegistry();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Writes a coded sample: the header and the payload.
//
// The header names the codec, the width, the block size and the length,
// and carries a check over the payload, so a damaged sample is refused
// before anything is decoded.
bool writeCodedSample(const SampleCodec& codec, uint64_t frames, int channels,
                      int bitDepth, uint32_t blockFrames,
                      const std::vector<uint8_t>& payload,
                      std::vector<uint8_t>& out, CodecError* error = nullptr);

// Reads a coded sample: the header and the payload.
//
// Fails when the data is not a coded sample, when it is shorter than its
// header says, when the check over the payload does not match, and when the
// name in the header is one no registered codec answers to.
bool readCodedSample(const uint8_t* data, size_t size, CodedHeader& header,
                     std::vector<uint8_t>& payload,
                     CodecError* error = nullptr);

// The name a coded sample carries, without reading the rest of it. Returns
// an empty name when the data is not a coded sample.
CodecId codedSampleId(const uint8_t* data, size_t size);

} // namespace mp
