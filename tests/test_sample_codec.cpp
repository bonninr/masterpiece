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

} // namespace

// The tests register themselves, so they have to exist.
static CodecRoundTripTest g_codecRoundTrip;
static CodecDamageTest g_codecDamage;

#endif // MP_TEST_HAS_SAMPLER
