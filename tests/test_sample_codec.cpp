// Masterpiece tests -- the sample codec interface (#447).
//
// Every codec in the registry is given the same two tests: a bit-exact
// round trip over the signals and the widths an organ sample is made of,
// and damage that is reported instead of being decoded into rubbish. The
// tests loop over the registry, so a codec that is added later cannot
// skip them.
#include "MpTest.h"

#ifdef MP_TEST_HAS_SAMPLER
#include "../src/mp_sampler/SampleCodec.h"
#include "../src/mp_sampler/SplitRice.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using mp::test::Category;

#ifdef MP_TEST_HAS_SAMPLER

namespace {

// The signals a codec has to survive: the worst case (noise), the empty
// case (silence), and the two a real organ sample is made of (a square
// wave at full scale and a sine).
enum class Signal { Noise, Silence, Square, Sine };

// Interleaved samples of the given signal, at the given width.
std::vector<int32_t> makeSignal(Signal signal, size_t frames, int channels,
                                int bitDepth, uint32_t seed) {
  const int64_t low = mp::codecSampleMin(bitDepth);
  const int64_t high = mp::codecSampleMax(bitDepth);
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
        const double phase = 2.0 * 3.141592653589793 *
                             (static_cast<double>(f) * 0.01 + 0.25 * c);
        const double v = std::sin(phase) * 0.9;
        value = static_cast<int64_t>(v * static_cast<double>(high));
        break;
      }
      }
      out[f * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
          static_cast<int32_t>(value);
    }
  }
  return out;
}

// The widths a codec is given, taken from what it says it holds.
std::vector<int> widthsFor(const mp::CodecInfo& info) {
  std::vector<int> widths;
  for (int w : {16, 20, 24})
    if (w >= info.minBitDepth && w <= info.maxBitDepth) widths.push_back(w);
  return widths;
}

// The frames the round trip is tried at. The block sizes matter: a codec
// whose blocks are 4096 frames has to cope with a sample that ends
// exactly on a block, one that ends a frame short and one that ends a
// frame after.
const size_t kLengths[] = {1, 4095, 4096, 4097, 20000};

class CodecRoundTripTest : public mp::test::Test {
public:
  CodecRoundTripTest() : mp::test::Test("functional.codec.round-trip", Category::Functional) {}

  void run() override {
    const auto codecs = mp::CodecRegistry::instance().all();
    MP_CHECK(!codecs.empty(), "no codec is registered");
    for (mp::SampleCodec* codec : codecs) {
      const mp::CodecInfo& info = codec->info();
      for (int width : widthsFor(info)) {
        for (size_t frames : kLengths) {
          for (int channels : {1, 2}) {
            for (Signal signal : {Signal::Noise, Signal::Silence, Signal::Square,
                                  Signal::Sine}) {
              checkOne(*codec, info, width, frames, channels, signal);
            }
          }
        }
      }
    }
  }

private:
  void checkOne(mp::SampleCodec& codec, const mp::CodecInfo& info,
                int width, size_t frames, int channels, Signal signal) {
    const std::vector<int32_t> input =
        makeSignal(signal, frames, channels, width, 7u);
    const uint32_t blockFrames = info.blockFrames != 0 ? info.blockFrames : 0;

    std::vector<uint8_t> payload;
    mp::CodecError error = mp::CodecError::None;
    MP_CHECK(codec.encode(input.data(), frames, channels, width, blockFrames,
                          payload, &error),
             std::string("encode failed for ") + info.name + " at " +
                 std::to_string(width) + " bits: " +
                 mp::codecErrorText(error));

    std::vector<uint8_t> coded;
    MP_CHECK(mp::writeCodedSample(codec, frames, channels, width, blockFrames,
                                  payload, coded, &error),
             "writing the coded sample failed");

    mp::CodedHeader header;
    std::vector<uint8_t> readBack;
    MP_CHECK(mp::readCodedSample(coded.data(), coded.size(), header, readBack,
                                 &error),
             "reading the coded sample failed");
    MP_CHECK(header.id == info.id, "the coded sample names another codec");
    MP_CHECK(header.frames == frames, "the coded sample has the wrong length");
    MP_CHECK(header.channels == channels, "the coded sample has the wrong channels");
    MP_CHECK(header.bitDepth == width, "the coded sample has the wrong width");

    std::vector<int32_t> output(input.size(), 0);
    MP_CHECK(codec.decodeRange(readBack.data(), readBack.size(), header, 0,
                                frames, output.data(), &error),
             std::string("decode failed for ") + info.name + ": " +
                 mp::codecErrorText(error));
    for (size_t i = 0; i < input.size(); ++i)
      MP_CHECK(input[i] == output[i],
               std::string("the round trip is not exact for ") + info.name +
                   " at " + std::to_string(width) + " bits, frame " +
                   std::to_string(i / static_cast<size_t>(channels)));

    // A window from the middle, which is what a voice asks for.
    if (frames > 100) {
      const uint64_t first = frames / 3;
      const uint64_t count = frames / 4;
      std::vector<int32_t> window(static_cast<size_t>(count) *
                                      static_cast<size_t>(channels),
                                  0);
      MP_CHECK(codec.decodeRange(readBack.data(), readBack.size(), header, first,
                                 count, window.data(), &error),
               "decoding a window failed");
      for (uint64_t i = 0; i < count; ++i)
        for (int c = 0; c < channels; ++c) {
          const size_t at = static_cast<size_t>(i) * static_cast<size_t>(channels) +
                            static_cast<size_t>(c);
          const size_t from = static_cast<size_t>(first + i) *
                                  static_cast<size_t>(channels) +
                              static_cast<size_t>(c);
          MP_CHECK(input[from] == window[at],
                   "the window does not match the input");
        }
    }

    // Frames outside the sample are refused.
    std::vector<int32_t> beyond(4, 0);
    MP_CHECK(!codec.decodeRange(readBack.data(), readBack.size(), header, frames,
                                1, beyond.data(), &error),
             "frames outside the sample must be refused");
  }
};

class CodecDamageTest : public mp::test::Test {
public:
  CodecDamageTest() : mp::test::Test("functional.codec.damage", Category::Functional) {}

  void run() override {
    const auto codecs = mp::CodecRegistry::instance().all();
    MP_CHECK(!codecs.empty(), "no codec is registered");
    for (mp::SampleCodec* codec : codecs) {
      const mp::CodecInfo& info = codec->info();
      const int width = info.maxBitDepth;
      const size_t frames = 4096;
      const int channels = 2;
      const std::vector<int32_t> input =
          makeSignal(Signal::Noise, frames, channels, width, 3u);
      const uint32_t blockFrames = info.blockFrames != 0 ? info.blockFrames : 0;

      std::vector<uint8_t> payload;
      mp::CodecError error = mp::CodecError::None;
      MP_CHECK(codec->encode(input.data(), frames, channels, width, blockFrames,
                              payload, &error),
               "encode failed");
      std::vector<uint8_t> coded;
      MP_CHECK(mp::writeCodedSample(*codec, frames, channels, width, blockFrames,
                                    payload, coded, &error),
               "writing the coded sample failed");

      // Every byte damaged in turn. A damaged sample is either refused or
      // decodes to the same samples; it never decodes to something else.
      std::vector<uint8_t> damaged;
      std::vector<int32_t> output(input.size(), 0);
      size_t reported = 0;
      for (size_t at = 0; at < coded.size(); ++at) {
        damaged = coded;
        damaged[at] = static_cast<uint8_t>(damaged[at] ^ 0x5Au);
        mp::CodedHeader header;
        std::vector<uint8_t> readBack;
        if (!mp::readCodedSample(damaged.data(), damaged.size(), header,
                                 readBack, &error)) {
          ++reported;
          continue;
        }
        // The header survived, so the check over the payload did not
        // catch it: the payload has to be the same, or the codec has to
        // refuse it.
        if (readBack != payload) {
          MP_CHECK(!codec->decodeRange(readBack.data(), readBack.size(), header,
                                       0, frames, output.data(), &error),
                   "a damaged payload must not decode to something else");
          ++reported;
        }
      }
      MP_CHECK(reported > 0, "no damage was reported at all");

      // A sample cut short.
      for (size_t cut : {size_t{0}, size_t{1}, size_t{31}, size_t{32},
                         size_t{33}, coded.size() / 2, coded.size() - 1}) {
        damaged = coded;
        damaged.resize(cut);
        mp::CodedHeader header;
        std::vector<uint8_t> readBack;
        MP_CHECK(!mp::readCodedSample(damaged.data(), damaged.size(), header,
                                      readBack, &error),
                 "a cut-short sample must be refused");
      }

      // A header that is not one of ours.
      damaged = coded;
      damaged[0] = static_cast<uint8_t>('X');
      mp::CodedHeader header;
      std::vector<uint8_t> readBack;
      MP_CHECK(!mp::readCodedSample(damaged.data(), damaged.size(), header,
                                    readBack, &error),
               "a sample with the wrong magic must be refused");
      MP_CHECK(error == mp::CodecError::BadHeader,
               "a sample with the wrong magic must say so");

      // A codec nobody knows.
      damaged = coded;
      damaged[4] = 'Z';
      damaged[5] = 'Z';
      damaged[6] = 'Z';
      damaged[7] = 'Z';
      MP_CHECK(!mp::readCodedSample(damaged.data(), damaged.size(), header,
                                    readBack, &error),
               "a sample from an unknown codec must be refused");
      MP_CHECK(error == mp::CodecError::UnknownCodec,
               "a sample from an unknown codec must say so");
    }
  }
};

// The predictive delta codec's bytes, worked out by hand, so a change to the
// format cannot pass unseen: a cache written by one version is read by the
// next.
//
//   sample   prediction            difference   byte units    two-byte units
//   0        0                     0            01            01 00
//   10       0                     10           15            15 00
//   100      10 + (10 - 0) / 2     85           02 55         AB 00
//   -1000    100 + (100-10) / 2    -1145        EE 87         0F F7
class DeltaBytesTest : public mp::test::Test {
public:
  DeltaBytesTest() : mp::test::Test("functional.codec.delta-bytes", Category::Functional) {}

  void run() override {
    mp::SampleCodec* codec = mp::CodecRegistry::instance().find("PDLT");
    MP_CHECK(codec != nullptr, "the predictive delta codec is registered");
    if (codec == nullptr) return;
    MP_CHECK(codec->info().access == mp::CodecAccess::Sequential, "it decodes forward");
    const int32_t samples[] = {0, 10, 100, -1000};
    const std::vector<uint8_t> narrow = {0x01, 0x15, 0x02, 0x55, 0xEE, 0x87};
    const std::vector<uint8_t> wide = {0x01, 0x00, 0x15, 0x00, 0xAB, 0x00, 0x0F, 0xF7};
    std::vector<uint8_t> payload;
    mp::CodecError error = mp::CodecError::None;
    MP_CHECK(codec->encode(samples, 4, 1, 16, 0, payload, &error), "16 bits encode");
    MP_CHECK(payload == narrow, "16 bits: one-byte units, as worked out");
    MP_CHECK(codec->encode(samples, 4, 1, 24, 0, payload, &error), "24 bits encode");
    MP_CHECK(payload == wide, "24 bits: two-byte units, as worked out");
  }
};

// A voice keeps the decoder's state and carries on from it: the state saved
// half way and restored into another decoder gives the same samples as the
// decoder that went on.
class DeltaResumeTest : public mp::test::Test {
public:
  DeltaResumeTest() : mp::test::Test("functional.codec.delta-resume", Category::Functional) {}

  void run() override {
    mp::SampleCodec* codec = mp::CodecRegistry::instance().find("PDLT");
    MP_CHECK(codec != nullptr, "the predictive delta codec is registered");
    if (codec == nullptr) return;
    for (int width : {16, 24}) {
      const size_t frames = 20000;
      const int channels = 2;
      const std::vector<int32_t> input = makeSignal(Signal::Sine, frames, channels, width, 5u);
      std::vector<uint8_t> payload;
      mp::CodecError error = mp::CodecError::None;
      MP_CHECK(codec->encode(input.data(), frames, channels, width, 0, payload, &error), "encode");
      mp::CodedHeader header;
      header.id = codec->info().id;
      header.frames = frames;
      header.channels = channels;
      header.bitDepth = width;
      header.payloadBytes = static_cast<uint32_t>(payload.size());

      auto a = codec->createDecoder();
      auto b = codec->createDecoder();
      MP_CHECK(a != nullptr && b != nullptr, "a sequential codec gives a decoder");
      if (a == nullptr || b == nullptr) return;
      MP_CHECK(a->start(payload.data(), payload.size(), header, &error), "start");
      const uint64_t half = 7777;
      std::vector<int32_t> head(half * channels), restA((frames - half) * channels),
          restB((frames - half) * channels);
      MP_CHECK(a->decodeNext(half, head.data(), &error), "the first part");
      const std::vector<uint8_t> state = a->saveState();
      MP_CHECK(a->decodeNext(frames - half, restA.data(), &error), "the rest");
      MP_CHECK(a->framesDecoded() == frames, "every frame decoded");
      MP_CHECK(!a->decodeNext(1, restA.data(), &error) && error == mp::CodecError::BadRange,
               "a frame past the end is refused");

      MP_CHECK(b->start(payload.data(), payload.size(), header, &error), "start the second");
      MP_CHECK(b->restoreState(state, &error), "restore the saved state");
      MP_CHECK(b->framesDecoded() == half, "it carries on from the saved frame");
      MP_CHECK(b->decodeNext(frames - half, restB.data(), &error), "the rest from the state");
      MP_CHECK(restA == restB, "the same samples from the saved state");
      for (size_t i = 0; i < head.size(); ++i)
        MP_CHECK(head[i] == input[i], "the first part is the input");
      for (size_t i = 0; i < restA.size(); ++i)
        MP_CHECK(restA[i] == input[half * channels + i], "the rest is the input");
    }
  }
};

// What it is for: a slow, smooth wave takes less room than plain storage.
// A bass pipe's fundamental, 400 frames a period (120 Hz at 48 kHz) at
// -20 dB, against the plain bytes at the same width: the differences fit
// one byte at 16 bits and two at 24.
class DeltaRatioTest : public mp::test::Test {
public:
  DeltaRatioTest() : mp::test::Test("functional.codec.delta-ratio", Category::Functional) {}

  void run() override {
    mp::SampleCodec* codec = mp::CodecRegistry::instance().find("PDLT");
    MP_CHECK(codec != nullptr, "the predictive delta codec is registered");
    if (codec == nullptr) return;
    for (int width : {16, 24}) {
      const size_t frames = 48000;
      std::vector<int32_t> input(frames * 2);
      const double amplitude = 0.1 * static_cast<double>(mp::codecSampleMax(width));
      for (size_t f = 0; f < frames; ++f)
        for (size_t c = 0; c < 2; ++c)
          input[f * 2 + c] = static_cast<int32_t>(std::lround(
              amplitude * std::sin(2.0 * 3.141592653589793 * (static_cast<double>(f) / 400.0 + 0.25 * static_cast<double>(c)))));
      std::vector<uint8_t> payload;
      mp::CodecError error = mp::CodecError::None;
      MP_CHECK(codec->encode(input.data(), frames, 2, width, 0, payload, &error), "encode");
      const double plain = static_cast<double>(frames) * 2.0 * (width == 16 ? 2.0 : 3.0);
      const double ratio = static_cast<double>(payload.size()) / plain;
      std::printf("    predictive delta, a 120 Hz tone at %d bits: %.1f %% of plain\n", width, 100.0 * ratio);
      MP_CHECK(ratio < 0.75, "a smooth wave takes less than three quarters of plain storage");
    }
  }
};

// Split Rice's bytes for one mono 16-bit frame of value 5, worked out by
// hand, so a change to the format cannot pass unseen.
//
// Order 0 costs least: no history, and the residual is the sample, zigzagged
// to 10. The one group of 256 (the frame and 255 of padding) takes k = 0: 256
// unary ones and a q of 10 cost 266 bits, against 517 for k = 1. So:
//
//   00                 stereo mode (mono)
//   00                 predictor order
//   00                 the group's k, 5 bits, padded to a byte
//                      no low bits at k = 0
//   00 FC FF x31 03    the unary stream: ten zeros and a one for the frame,
//                      then a one for each of the 255 padding values: 266 bits
//   00 x6              padded to 40 bytes
//   00 x16             the stream's tail
class SplitRiceBytesTest : public mp::test::Test {
public:
  SplitRiceBytesTest() : mp::test::Test("functional.codec.split-rice-bytes", Category::Functional) {}

  void run() override {
    mp::SampleCodec* codec = mp::CodecRegistry::instance().find("SRIC");
    MP_CHECK(codec != nullptr, "split Rice is registered");
    if (codec == nullptr) return;
    MP_CHECK(codec->info().access == mp::CodecAccess::RandomAccess, "any block decodes on its own");
    MP_CHECK(codec->info().blockFrames == mp::kSplitRiceBlockFrames, "in blocks of 4096 frames");
    std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x00, 0xFC};
    expected.insert(expected.end(), 31, 0xFF);
    expected.push_back(0x03);
    expected.insert(expected.end(), 6 + 16, 0x00);
    const int32_t sample = 5;
    mp::SplitRiceStream stream;
    mp::CodecError error = mp::CodecError::None;
    MP_CHECK(mp::encodeSplitRice(&sample, 1, 1, 16, stream, &error), "one frame encodes");
    MP_CHECK(stream.data == expected, "the bytes are as worked out");
    MP_CHECK(stream.blockOffsets == std::vector<uint32_t>{0}, "one block, at the start");
    std::vector<uint8_t> payload;
    MP_CHECK(codec->encode(&sample, 1, 1, 16, mp::kSplitRiceBlockFrames, payload, &error), "the codec encodes");
    std::vector<uint8_t> framed = {1, 0, 0, 0, 0, 0, 0, 0};
    framed.insert(framed.end(), expected.begin(), expected.end());
    MP_CHECK(payload == framed, "its payload is the block count, the offsets and the stream");
    int32_t back = 0;
    MP_CHECK(mp::decodeSplitRiceFrames(stream, 0, 1, &back, &error) && back == 5, "and it decodes to 5");
  }
};

// The vector kernels give the plain kernels' numbers, block by block, on
// every signal and width, and on full-scale noise at 24 bits, whose groups
// take a k too wide for the vector unpack.
class SplitRiceKernelsTest : public mp::test::Test {
public:
  SplitRiceKernelsTest() : mp::test::Test("functional.codec.split-rice-kernels", Category::Functional) {}

  void run() override {
    const bool had = mp::splitRiceVectorKernels();
    std::printf("    split Rice: vector kernels %s on this machine\n", had ? "present" : "absent");
    mp::SplitRiceScratch scratch;
    for (int width : {8, 16, 20, 24})
      for (int channels : {1, 2})
        for (Signal signal : {Signal::Noise, Signal::Silence, Signal::Square, Signal::Sine}) {
          const size_t frames = 3 * mp::kSplitRiceBlockFrames + 1000;
          const auto input = makeSignal(signal, frames, channels, width, 11u);
          mp::SplitRiceStream stream;
          mp::CodecError error = mp::CodecError::None;
          MP_CHECK(mp::encodeSplitRice(input.data(), frames, channels, width, stream, &error), "encode");
          std::vector<int32_t> plain(mp::kSplitRiceBlockFrames * static_cast<size_t>(channels));
          std::vector<int32_t> fast(plain.size());
          for (size_t b = 0; b < stream.blockCount(); ++b) {
            mp::setSplitRiceVectorKernels(false);
            MP_CHECK(mp::decodeSplitRiceBlock(stream, b, plain.data(), scratch, &error), "plain decode");
            mp::setSplitRiceVectorKernels(true);
            MP_CHECK(mp::decodeSplitRiceBlock(stream, b, fast.data(), scratch, &error), "vector decode");
            const size_t n = static_cast<size_t>(stream.framesInBlock(b)) * static_cast<size_t>(channels);
            const size_t at = b * mp::kSplitRiceBlockFrames * static_cast<size_t>(channels);
            MP_CHECK(std::equal(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(n), fast.begin()),
                     "the two kernels agree, block " + std::to_string(b) + " at " + std::to_string(width) + " bits");
            MP_CHECK(std::equal(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(n),
                                input.begin() + static_cast<std::ptrdiff_t>(at)),
                     "and give the input back");
          }
        }
    mp::setSplitRiceVectorKernels(true);
  }
};

// What it is for: a bass pipe's tone, as in the delta codec's test, takes
// under half the room of plain storage and less than the delta codec.
class SplitRiceRatioTest : public mp::test::Test {
public:
  SplitRiceRatioTest() : mp::test::Test("functional.codec.split-rice-ratio", Category::Functional) {}

  void run() override {
    mp::SampleCodec* rice = mp::CodecRegistry::instance().find("SRIC");
    mp::SampleCodec* delta = mp::CodecRegistry::instance().find("PDLT");
    MP_CHECK(rice != nullptr && delta != nullptr, "both codecs are registered");
    if (rice == nullptr || delta == nullptr) return;
    for (int width : {16, 24}) {
      const size_t frames = 48000;
      std::vector<int32_t> input(frames * 2);
      const double amplitude = 0.1 * static_cast<double>(mp::codecSampleMax(width));
      for (size_t f = 0; f < frames; ++f)
        for (size_t c = 0; c < 2; ++c)
          input[f * 2 + c] = static_cast<int32_t>(std::lround(
              amplitude * std::sin(2.0 * 3.141592653589793 * (static_cast<double>(f) / 400.0 + 0.25 * static_cast<double>(c)))));
      std::vector<uint8_t> ricePayload, deltaPayload;
      mp::CodecError error = mp::CodecError::None;
      MP_CHECK(rice->encode(input.data(), frames, 2, width, mp::kSplitRiceBlockFrames, ricePayload, &error), "encode");
      MP_CHECK(delta->encode(input.data(), frames, 2, width, 0, deltaPayload, &error), "delta encode");
      const double plain = static_cast<double>(frames) * 2.0 * (width == 16 ? 2.0 : 3.0);
      std::printf("    a 120 Hz tone at %d bits: split Rice %.1f %%, predictive delta %.1f %% of plain\n", width,
                  100.0 * static_cast<double>(ricePayload.size()) / plain,
                  100.0 * static_cast<double>(deltaPayload.size()) / plain);
      MP_CHECK(static_cast<double>(ricePayload.size()) < 0.5 * plain, "under half of plain storage");
      MP_CHECK(ricePayload.size() < deltaPayload.size(), "and smaller than predictive delta");
    }
  }
};

// How fast a voice's blocks decode: frames a second on one thread, with the
// vector kernels and with the plain ones, on a stereo 24-bit signal shaped
// like a pipe (a tone and a little noise).
class SplitRiceSpeedTest : public mp::test::Test {
public:
  SplitRiceSpeedTest() : mp::test::Test("perf.codec.split-rice-decode", Category::Perf) {}

  void run() override {
    const size_t frames = 64 * mp::kSplitRiceBlockFrames;
    std::vector<int32_t> input(frames * 2);
    std::mt19937 rng(9u);
    std::uniform_int_distribution<int> noise(-200, 200);
    for (size_t f = 0; f < frames; ++f)
      for (size_t c = 0; c < 2; ++c)
        input[f * 2 + c] = static_cast<int32_t>(
            std::lround(400000.0 * std::sin(2.0 * 3.141592653589793 * (static_cast<double>(f) / 183.7 + 0.1 * static_cast<double>(c))))) +
                           noise(rng);
    mp::SplitRiceStream stream;
    MP_CHECK(mp::encodeSplitRice(input.data(), frames, 2, 24, stream), "encode");
    std::printf("    split Rice: %.1f %% of plain 24-bit\n",
                100.0 * static_cast<double>(stream.bytes()) / (static_cast<double>(frames) * 6.0));
    mp::SplitRiceScratch scratch;
    std::vector<int32_t> out(mp::kSplitRiceBlockFrames * 2);
    auto rate = [&](bool useVector) {
      mp::setSplitRiceVectorKernels(useVector);
      const auto t0 = std::chrono::steady_clock::now();
      int rounds = 0;
      double seconds = 0.0;
      do {
        for (size_t b = 0; b < stream.blockCount(); ++b) mp::decodeSplitRiceBlock(stream, b, out.data(), scratch);
        ++rounds;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      } while (seconds < 0.5);
      return static_cast<double>(rounds) * static_cast<double>(frames) / seconds;
    };
    const double plain = rate(false);
    const double fast = rate(true);
    mp::setSplitRiceVectorKernels(true);
    std::printf("    split Rice decode: %.0f M frames/s plain, %.0f M frames/s vector\n", plain / 1e6, fast / 1e6);
    MP_CHECK(plain > 0.0 && fast > 0.0, "both decode");
    if (mp::splitRiceVectorKernels())
      MP_CHECK(fast > 0.8 * plain, "the vector kernels are no slower than the plain ones");
  }
};

} // namespace

// The tests register themselves, so they have to exist.
static CodecRoundTripTest g_codecRoundTrip;
static CodecDamageTest g_codecDamage;
static DeltaBytesTest g_deltaBytes;
static DeltaResumeTest g_deltaResume;
static DeltaRatioTest g_deltaRatio;
static SplitRiceBytesTest g_splitRiceBytes;
static SplitRiceKernelsTest g_splitRiceKernels;
static SplitRiceRatioTest g_splitRiceRatio;
static SplitRiceSpeedTest g_splitRiceSpeed;

#endif // MP_TEST_HAS_SAMPLER
