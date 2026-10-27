// A WavPack sample loads whole: every frame, its loop and its release marker.
//
// The guard against a damaged header caps a sample at the frames its file's
// bytes could hold, which is right for uncompressed audio. WavPack's reader
// is named "WavPack", and a name test on "WAV" took it in: a compressed
// sample lost about two thirds of its length, and with it the loop and the
// release marker, so a held note fell silent after a second and the release
// played the start of the file again.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO
#if __has_include(<wavpack.h>)

#include "../src/mp_audio/SampleLibrary.h"
#include "../src/mp_core/OrganModel.h"

#include <juce_core/juce_core.h>
#include <wavpack.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr int kRate = 48000;
constexpr int kFrames = 3 * kRate;
constexpr uint32_t kLoopStart = 48000;
constexpr uint32_t kLoopEnd = 96000;  // inclusive, as the smpl chunk writes it
constexpr uint32_t kCue = 120000;

void put16(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

// The WAV header the file was made from, as WavPack keeps it: fmt, smpl with
// one loop, cue with one point, and the data chunk's header.
std::vector<uint8_t> wavHeader() {
  std::vector<uint8_t> h;
  const uint32_t dataBytes = kFrames * 2;
  tag(h, "RIFF");
  put32(h, 0);  // filled in below
  tag(h, "WAVE");
  tag(h, "fmt ");
  put32(h, 16);
  put16(h, 1);  // PCM
  put16(h, 1);  // mono
  put32(h, kRate);
  put32(h, kRate * 2);
  put16(h, 2);
  put16(h, 16);
  tag(h, "smpl");
  put32(h, 36 + 24);
  for (int i = 0; i < 3; ++i) put32(h, 0);  // manufacturer, product, period
  put32(h, 60);                              // unity note
  put32(h, 0);                               // pitch fraction
  put32(h, 0);                               // SMPTE format
  put32(h, 0);                               // SMPTE offset
  put32(h, 1);                               // loops
  put32(h, 0);                               // sampler data
  put32(h, 0);                               // loop id
  put32(h, 0);                               // forward
  put32(h, kLoopStart);
  put32(h, kLoopEnd);
  put32(h, 0);
  put32(h, 0);
  tag(h, "cue ");
  put32(h, 4 + 24);
  put32(h, 1);
  put32(h, 1);  // id
  put32(h, kCue);
  tag(h, "data");
  put32(h, 0);
  put32(h, 0);
  put32(h, kCue);
  tag(h, "data");
  put32(h, dataBytes);
  const uint32_t riff = static_cast<uint32_t>(h.size()) - 8 + dataBytes;
  for (int i = 0; i < 4; ++i) h[4 + i] = static_cast<uint8_t>(riff >> (8 * i));
  return h;
}

int writeBlock(void* id, void* data, int32_t bytes) {
  auto* out = static_cast<std::vector<char>*>(id);
  out->insert(out->end(), static_cast<char*>(data), static_cast<char*>(data) + bytes);
  return 1;
}

// A quiet pipe-like tone, lossless WavPack, wrapped in the header above.
std::vector<char> encode() {
  std::vector<char> bytes;
  WavpackContext* wpc = WavpackOpenFileOutput(writeBlock, &bytes, nullptr);
  WavpackConfig cfg{};
  cfg.bytes_per_sample = 2;
  cfg.bits_per_sample = 16;
  cfg.channel_mask = 4;
  cfg.num_channels = 1;
  cfg.sample_rate = kRate;
  WavpackSetConfiguration64(wpc, &cfg, kFrames, nullptr);
  std::vector<uint8_t> header = wavHeader();
  WavpackAddWrapper(wpc, header.data(), static_cast<uint32_t>(header.size()));
  WavpackPackInit(wpc);
  std::vector<int32_t> s(kFrames);
  for (int i = 0; i < kFrames; ++i)
    s[static_cast<size_t>(i)] = static_cast<int32_t>(std::lround(300.0 * std::sin(2.0 * 3.141592653589793 * 220.0 * i / kRate)));
  WavpackPackSamples(wpc, s.data(), kFrames);
  WavpackFlushSamples(wpc);
  WavpackCloseFile(wpc);
  return bytes;
}

class WavPackWholeTest final : public mp::test::Test {
public:
  WavPackWholeTest() : Test("functional.files.wavpack-whole", Category::Functional) {}
  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-wavpack-whole");
    root.deleteRecursively();
    root.createDirectory();
    const std::vector<char> bytes = encode();
    const auto file = root.getChildFile("pipe.wv");
    file.replaceWithData(bytes.data(), bytes.size());
    MP_CHECK(static_cast<int64_t>(bytes.size()) < static_cast<int64_t>(kFrames) * 2 / 2,
             "sanity: the file holds less than half its audio's bytes: " + std::to_string(bytes.size()));

    mp::OrganModel model;
    mp::SampleRef ref;
    ref.sampleId = 1;
    ref.fileName = "pipe.wv";
    model.samples[1] = ref;
    mp::SampleLibrary lib;
    const auto report = lib.loadAll(model, root.getFullPathName().toStdString());
    MP_CHECK(report.loaded == 1, "the WavPack sample loads");
    const mp::SampleBuffer* b = lib.provider()(1);
    MP_CHECK(b != nullptr, "and is resident");
    if (b == nullptr) return;
    MP_CHECK(b->numFrames == kFrames,
             "every frame of it: " + std::to_string(b->numFrames) + " of " + std::to_string(kFrames));
    MP_CHECK(b->loopStart == kLoopStart && b->loopEnd == static_cast<int64_t>(kLoopEnd) + 1,
             "its loop: " + std::to_string(b->loopStart) + " to " + std::to_string(b->loopEnd));
    MP_CHECK(b->releaseCue == kCue, "and its release marker: " + std::to_string(b->releaseCue));
    root.deleteRecursively();
  }
};
static WavPackWholeTest g_wavPackWhole;

}  // namespace

#endif  // __has_include(<wavpack.h>)
#endif  // MP_TEST_HAS_AUDIO
