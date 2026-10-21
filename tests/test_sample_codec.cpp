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

} // namespace

// The tests register themselves, so they have to exist.
static CodecRoundTripTest g_codecRoundTrip;
static CodecDamageTest g_codecDamage;
static DeltaBytesTest g_deltaBytes;
static DeltaResumeTest g_deltaResume;
static DeltaRatioTest g_deltaRatio;

#endif // MP_TEST_HAS_SAMPLER
