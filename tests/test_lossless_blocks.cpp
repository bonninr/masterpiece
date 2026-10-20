// Masterpiece tests -- the lossless block codec (#447, phase 1).
//
// The codec is JUCE-free and lives in mp_sampler. These tests cover what the
// codec has to be: a bit-exact round trip over the signals and the widths an
// organ sample set holds, a window of frames taken from the middle of a
// stream, and damage that is reported instead of being decoded.
//
// The measurement over a real organ is in test_codec_bench.cpp.
#include "MpTest.h"

#ifdef MP_TEST_HAS_SAMPLER
#include "../src/mp_sampler/LosslessBlocks.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using mp::test::Category;

#ifdef MP_TEST_HAS_SAMPLER

namespace {

// The signals the codec has to survive: the worst case (noise), the empty
// case (silence) and the two that a real organ sample is made of (a square
// wave at full scale and a sine). The last one puts the two channels
// opposite each other, so the difference of a stereo pair reaches twice the
// width of a sample.
enum class Signal { Noise, Silence, Square, Sine, Opposite };

const char* signalName(Signal signal) {
  switch (signal) {
  case Signal::Noise:
    return "noise";
  case Signal::Silence:
    return "silence";
  case Signal::Square:
    return "square";
  case Signal::Sine:
    return "sine";
  case Signal::Opposite:
    return "opposite";
  }
  return "?";
}

int64_t lowestSample(int bitDepth) {
  return -(static_cast<int64_t>(1) << (bitDepth - 1));
}

int64_t highestSample(int bitDepth) {
  return (static_cast<int64_t>(1) << (bitDepth - 1)) - 1;
}

// Interleaved samples of the given signal, at the given width.
std::vector<int32_t> makeSignal(Signal signal, size_t frames, int channels,
                                int bitDepth, uint32_t seed) {
  const int64_t low = lowestSample(bitDepth);
  const int64_t high = highestSample(bitDepth);
  const uint64_t span = static_cast<uint64_t>(high - low + 1);
  std::mt19937 rng(seed);
  std::vector<int32_t> out(frames * static_cast<size_t>(channels));
  for (size_t f = 0; f < frames; ++f) {
    for (int c = 0; c < channels; ++c) {
      int64_t value = 0;
      switch (signal) {
      case Signal::Noise:
        value = low + static_cast<int64_t>(rng() % static_cast<uint32_t>(span));
        break;
      case Signal::Silence:
        value = 0;
        break;
      case Signal::Square:
        value = ((f / 16) % 2 == 0) ? high : low;
        break;
      case Signal::Sine: {
        const double t = static_cast<double>(f) / 48000.0;
        value = static_cast<int64_t>(std::llround(
            0.9 * static_cast<double>(high) *
            std::sin(2.0 * 3.14159265358979323846 * 440.0 * t)));
        break;
      }
      case Signal::Opposite:
        // Both channels reach full scale, and they are of opposite sign, so
        // the difference of the pair reaches twice the width of a sample.
        value = ((f / 16) % 2 == 0) ? high : -high;
        if (c != 0)
          value = -value;
        break;
      }
      out[f * static_cast<size_t>(channels) + c] = static_cast<int32_t>(value);
    }
  }
  return out;
}

// The header of a block, and the first bytes of its payload, as text. A
// failure that prints this says what the encoder wrote.
std::string describeBlock(const mp::LosslessStream& stream, size_t index) {
  const size_t at = static_cast<size_t>(stream.blockOffsets[index]);
  std::string out = "block " + std::to_string(index) + " at " +
                    std::to_string(at);
  if (at + 20 > stream.data.size())
    return out + ": the block table points outside the data";
  const unsigned char* h = stream.data.data() + at;
  const uint32_t payload =
      static_cast<uint32_t>(h[12]) | (static_cast<uint32_t>(h[13]) << 8) |
      (static_cast<uint32_t>(h[14]) << 16) |
      (static_cast<uint32_t>(h[15]) << 24);
  out += ": frames " + std::to_string(h[4] | (h[5] << 8)) + ", channels " +
         std::to_string(h[6]) + ", depth " + std::to_string(h[7]) +
         ", flags " + std::to_string(h[8]) + ", payload " +
         std::to_string(payload) + " bytes, bytes:";
  for (size_t i = 0; i < 24 && at + 20 + i < stream.data.size(); ++i) {
    char byte[4] = {0, 0, 0, 0};
    std::snprintf(byte, sizeof(byte), "%02x", stream.data[at + 20 + i]);
    out += byte;
  }
  return out;
}

// Encodes, decodes and compares. The failure names the case.
void roundTrip(Signal signal, int bitDepth, int channels, size_t frames) {
  const std::vector<int32_t> input =
      makeSignal(signal, frames, channels, bitDepth,
                 static_cast<uint32_t>(frames * 31u + channels * 7u +
                                       bitDepth));
  const std::string what = std::string(signalName(signal)) + " at " +
                           std::to_string(bitDepth) + " bits, " +
                           std::to_string(channels) + " channel(s), " +
                           std::to_string(frames) + " frames";
  mp::LosslessStream stream;
  mp::LosslessError error = mp::LosslessError::None;
  MP_CHECK(mp::encodeLosslessBlocks(input.data(), frames, channels, bitDepth,
                                    stream, &error),
           "encode of " + what + " failed: " + mp::losslessErrorText(error));

  std::fprintf(stderr,
               "[test] stream data=%zu table=%zu frames=%llu ch=%d depth=%d "
               "blockFrames=%u first=%llu\n",
               stream.data.size(), stream.blockOffsets.size(),
               (unsigned long long)stream.numFrames, stream.channels,
               stream.bitDepth, stream.blockFrames,
               stream.blockOffsets.empty()
                   ? 0ULL
                   : (unsigned long long)stream.blockOffsets[0]);

  // The block table covers the stream, and every block holds what it should.
  MP_CHECK(stream.blockCount() == (frames + mp::kLosslessBlockFrames - 1) /
                                     mp::kLosslessBlockFrames,
           "the block count does not match the frame count");
  for (size_t b = 0; b < stream.blockCount(); ++b)
    MP_CHECK(stream.framesInBlock(b) ==
                std::min<uint64_t>(mp::kLosslessBlockFrames,
                                   frames - b * mp::kLosslessBlockFrames),
            "a block holds the wrong number of frames");

  std::vector<int32_t> output(input.size(), 0);
  if (!mp::decodeLosslessFrames(stream, 0, frames, output.data(), &error)) {
    // Name the block that failed, so a report says where the fault is.
    std::string blocks;
    std::vector<int32_t> one(
        static_cast<size_t>(mp::kLosslessBlockFrames) * channels);
    for (size_t b = 0; b < stream.blockCount(); ++b) {
      uint16_t got = 0;
      mp::LosslessError blockError = mp::LosslessError::None;
      if (!mp::decodeLosslessBlock(stream, b, one.data(), &got, &blockError))
        blocks += " block " + std::to_string(b) + ": " +
                  mp::losslessErrorText(blockError) + " [" +
                  describeBlock(stream, b) + "]";
    }
    MP_CHECK(false, "decode of " + what + " failed: " +
                        mp::losslessErrorText(error) + "; per block:" + blocks);
  }
  for (size_t i = 0; i < input.size(); ++i)
    MP_CHECK(output[i] == input[i],
             "the round trip changed sample " + std::to_string(i) + " of " +
                 what);
}

class LosslessRoundTripTest final : public mp::test::Test {
public:
  LosslessRoundTripTest()
    : Test("functional.codec.round-trip", Category::Functional) {}
  void run() override {
    const Signal signals[] = {Signal::Noise, Signal::Silence, Signal::Square,
                              Signal::Sine, Signal::Opposite};
    const int depths[] = {16, 20, 24};
    const size_t lengths[] = {1, 4095, 4096, 4097, 100000};
    for (Signal signal : signals)
      for (int depth : depths)
        for (int channels : {1, 2})
          for (size_t frames : lengths)
            roundTrip(signal, depth, channels, frames);
  }
};

class LosslessWindowTest final : public mp::test::Test {
public:
  LosslessWindowTest()
    : Test("functional.codec.frame-window", Category::Functional) {}
  void run() override {
    const size_t frames = 20000;
    const std::vector<int32_t> input =
        makeSignal(Signal::Sine, frames, 2, 24, 5u);
    mp::LosslessStream stream;
    MP_CHECK(mp::encodeLosslessBlocks(input.data(), frames, 2, 24, stream),
             "encode failed");

    // A window that starts inside a block and ends inside the next one.
    const uint64_t first = 4000;
    const uint64_t count = 5000;
    std::vector<int32_t> output(static_cast<size_t>(count) * 2, 0);
    mp::LosslessError error = mp::LosslessError::None;
    MP_CHECK(mp::decodeLosslessFrames(stream, first, count, output.data(),
                                      &error),
             std::string("the window failed to decode: ") +
                 mp::losslessErrorText(error));
    for (uint64_t f = 0; f < count; ++f)
      for (int c = 0; c < 2; ++c)
        MP_CHECK(output[f * 2 + c] == input[(first + f) * 2 + c],
                 "the window holds the wrong sample");

    // A window of one frame, at the very start and at the very end.
    for (uint64_t at : {uint64_t(0), uint64_t(frames - 1)}) {
      std::vector<int32_t> one(2, 0);
      MP_CHECK(mp::decodeLosslessFrames(stream, at, 1, one.data()),
               "a single frame failed to decode");
      for (int c = 0; c < 2; ++c)
        MP_CHECK(one[c] == input[at * 2 + c],
                 "the single frame holds the wrong sample");
    }

    // Outside the stream is an error, not a read past the end.
    std::vector<int32_t> scratch(2, 0);
    MP_CHECK(!mp::decodeLosslessFrames(stream, frames, 1, scratch.data(),
                                        &error),
             "a frame past the end must be refused");
    MP_CHECK(error == mp::LosslessError::OutOfRange,
             "a frame past the end must be reported as out of range");
    MP_CHECK(!mp::decodeLosslessFrames(stream, frames - 1, 2, scratch.data(),
                                        &error),
             "a window past the end must be refused");
    MP_CHECK(mp::decodeLosslessFrames(stream, 0, 0, nullptr, &error),
             "an empty window is not an error");
  }
};

class LosslessDamageTest final : public mp::test::Test {
public:
  LosslessDamageTest()
    : Test("functional.codec.damage-detected", Category::Functional) {}
  void run() override {
    // One block, so every byte of the stream can be damaged in turn.
    const size_t frames = 500;
    const std::vector<int32_t> input =
        makeSignal(Signal::Noise, frames, 2, 24, 11u);
    mp::LosslessStream stream;
    MP_CHECK(mp::encodeLosslessBlocks(input.data(), frames, 2, 24, stream),
             "encode failed");
    std::vector<int32_t> output(input.size(), 0);
    mp::LosslessError error = mp::LosslessError::None;

    // Every byte of the stream, damaged in turn. A damaged block is either
    // reported or decodes to the same samples; it never decodes to something
    // else, and it never reads outside the buffer.
    size_t reported = 0;
    for (size_t i = 0; i < stream.data.size(); ++i) {
      mp::LosslessStream damaged = stream;
      damaged.data[i] = static_cast<unsigned char>(damaged.data[i] ^ 0xFFu);
      const bool ok = mp::decodeLosslessFrames(damaged, 0, frames,
                                                output.data(), &error);
      if (ok) {
        for (size_t s = 0; s < input.size(); ++s)
          MP_CHECK(output[s] == input[s],
                   "a damaged block decoded to different samples");
      } else {
        ++reported;
        MP_CHECK(error != mp::LosslessError::None,
                 "a damaged block must report why it failed");
      }
    }
    MP_CHECK(reported > 0, "no damage was reported at all");

    // A stream cut short, at every length around a block boundary.
    const size_t longFrames = 9000;
    const std::vector<int32_t> longInput =
        makeSignal(Signal::Noise, longFrames, 2, 24, 13u);
    mp::LosslessStream longStream;
    MP_CHECK(mp::encodeLosslessBlocks(longInput.data(), longFrames, 2, 24,
                                       longStream),
             "encode failed");
    std::vector<int32_t> longOutput(longInput.size(), 0);
    for (size_t cut = 1; cut < 64; ++cut) {
      mp::LosslessStream truncated = longStream;
      truncated.data.resize(truncated.data.size() - cut);
      MP_CHECK(!mp::decodeLosslessFrames(truncated, 0, longFrames,
                                          longOutput.data(), &error),
               "a truncated stream must be refused");
      MP_CHECK(error == mp::LosslessError::Truncated ||
                   error == mp::LosslessError::CheckFailed ||
                   error == mp::LosslessError::BadStream ||
                   error == mp::LosslessError::CorruptData,
               "a truncated stream must report why it failed");
    }

    // A header that is not one of ours.
    mp::LosslessStream wrongMagic = stream;
    wrongMagic.data[0] = static_cast<unsigned char>('X');
    MP_CHECK(!mp::decodeLosslessFrames(wrongMagic, 0, frames, output.data(),
                                        &error),
             "a block with the wrong magic must be refused");
    MP_CHECK(error == mp::LosslessError::BadHeader,
             "a block with the wrong magic must be reported as a bad header");

    // A block table that points outside the data.
    mp::LosslessStream badTable = stream;
    badTable.blockOffsets[0] = badTable.data.size() + 16;
    MP_CHECK(!mp::decodeLosslessFrames(badTable, 0, frames, output.data(),
                                        &error),
             "a block table pointing outside the data must be refused");

    // A sample that does not fit the width is refused at the door.
    std::vector<int32_t> tooWide(4, 0);
    tooWide[1] = 1 << 20;
    MP_CHECK(!mp::encodeLosslessBlocks(tooWide.data(), 2, 2, 16, stream,
                                        &error),
             "a sample outside the width must be refused");
    MP_CHECK(error == mp::LosslessError::SamplesOutOfRange,
             "a sample outside the width must say so");
  }
};

} // namespace

// The tests register themselves, so they have to exist.
static LosslessRoundTripTest g_losslessRoundTrip;
static LosslessWindowTest g_losslessWindow;
static LosslessDamageTest g_losslessDamage;

#endif // MP_TEST_HAS_SAMPLER
