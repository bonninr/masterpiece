// Samples held compressed (#447).
//
// A sample held split Rice coded is read by the voices through two small
// windows of decoded frames each (DecodedWindow.h). What is checked here: that
// the coder keeps every count, that a voice reading coded samples plays what
// the same voice plays from plain counts, to the bit, for every storage width,
// loop, crossfade, release, vector unit and thread count, and that the sample
// cache keeps coded samples coded.
#include "MpTest.h"

#ifdef MP_TEST_HAS_SAMPLER

#include "../src/mp_sampler/SimdRun.h"
#include "../src/mp_sampler/SplitRice.h"
#include "../src/mp_sampler/VoiceEngine.h"
#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/SampleLibrary.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using mp::test::Category;

namespace {

// How a test buffer holds its counts.
enum class Width { Int16, Int24, Int16In24 };

const char* widthName(Width w) {
  switch (w) {
    case Width::Int16: return "16-bit";
    case Width::Int24: return "24-bit";
    case Width::Int16In24: return "16-bit held at 24";
  }
  return "?";
}

// A pipe-like tone: a fundamental, two partials and a little noise, so the
// coder has residuals to work on. The channels differ.
std::vector<double> toneValues(int frames, int channels, double hz, double sr, uint32_t seed) {
  std::vector<double> out(static_cast<size_t>(frames) * static_cast<size_t>(channels));
  uint32_t state = seed;
  for (int f = 0; f < frames; ++f)
    for (int c = 0; c < channels; ++c) {
      const double x = 2.0 * 3.141592653589793 * hz * f / sr + 0.4 * c;
      state = state * 1664525u + 1013904223u;
      const double noise = (static_cast<double>(state >> 8) / 16777216.0 - 0.5) * 0.002;
      out[static_cast<size_t>(f) * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
          0.5 * std::sin(x) + 0.2 * std::sin(2.0 * x) + 0.1 * std::sin(3.0 * x + 1.0) + noise;
    }
  return out;
}

void putPcm24(mp::Pcm24& p, int32_t v) {
  p.b[0] = static_cast<unsigned char>(v & 0xFF);
  p.b[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
  p.b[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
}

// A buffer of plain counts.
mp::SampleBuffer plainBuffer(Width width, int frames, int channels, double hz, uint32_t seed) {
  const auto values = toneValues(frames, channels, hz, 48000.0, seed);
  mp::SampleBuffer b;
  b.numChannels = channels;
  b.sampleRate = 48000.0;
  b.numFrames = frames;
  if (width == Width::Int16) {
    b.pcm16.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i)
      b.pcm16[i] = static_cast<int16_t>(std::lround(values[i] * 32767.0));
    b.pcmScale = 1.0f / 32767.0f;
  } else {
    b.pcm24.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      const int32_t v = width == Width::Int24
                            ? static_cast<int32_t>(std::lround(values[i] * 8388607.0))
                            : static_cast<int32_t>(std::lround(values[i] * 32767.0)) * 256;
      putPcm24(b.pcm24[i], v);
    }
    b.pcmScale = 1.0f / 8388607.0f;
  }
  return b;
}

mp::SampleBuffer codedCopy(const mp::SampleBuffer& plain) {
  mp::SampleBuffer b = plain;
  const bool ok = mp::codeSampleBuffer(b);
  MP_CHECK(ok, "the buffer codes");
  return b;
}

// The vector units to compare on: the per-frame path and every unit this
// machine runs.
std::vector<mp::simd::Isa> units() {
  std::vector<mp::simd::Isa> out = {mp::simd::Isa::None};
  for (const auto isa : mp::simd::available())
    if (std::find(out.begin(), out.end(), isa) == out.end()) out.push_back(isa);
  return out;
}

// One playing of a set of voices: each starts at block 0 at its own ratio,
// and is let go at `holdBlocks`.
struct Playing {
  const mp::SampleBuffer* attack = nullptr;
  const mp::SampleBuffer* release = nullptr;  // none: the voice fades
  std::vector<double> ratios = {1.0};
  int64_t loopStart = -1, loopEnd = -1;  // -1: the buffer's own
  int loopCrossfade = 0;
  bool oneShot = false;
  double releaseCrossfadeMs = 10.0;
  bool phaseAlign = false;
  int holdBlocks = 100;
  int totalBlocks = 160;
  int threads = 0;
  int settleMs = 0;  // before the first block, for a stream to run ahead
};

struct Played {
  std::vector<float> out;  // left then right, a block at a time
  uint64_t decoded = 0;
  int started = 0;
};

constexpr int kBlock = 256;

Played play(const Playing& p, bool windows, mp::simd::Isa isa) {
  const int voices = static_cast<int>(p.ratios.size());
  std::vector<mp::Pipe> pipes(static_cast<size_t>(voices));
  for (int i = 0; i < voices; ++i) {
    mp::Pipe& pipe = pipes[static_cast<size_t>(i)];
    pipe.pipeId = 100 + i;
    pipe.midiNote = 60 + i;
    mp::PipeLayer layer;
    layer.layerId = 1 + i;
    mp::AttackSample a;
    a.id = 11;
    a.sample.sampleId = 1;
    a.sample.resolvedPitchHz = 220.0;
    layer.attacks.push_back(a);
    if (p.release != nullptr) {
      mp::ReleaseSample r;
      r.id = 21;
      r.sample.sampleId = 2;
      r.sample.resolvedPitchHz = 220.0;
      r.releaseCrossfadeMs = p.releaseCrossfadeMs;
      r.phaseAlign = p.phaseAlign;
      layer.releases.push_back(r);
    }
    pipe.layers.push_back(std::move(layer));
  }
  const mp::SampleBuffer* attack = p.attack;
  const mp::SampleBuffer* release = p.release;
  mp::SampleProvider provider = [attack, release](mp::Id id) -> const mp::SampleBuffer* {
    return id == 1 ? attack : id == 2 ? release : nullptr;
  };

  mp::VoiceEngine engine;
  engine.setCodedWindows(windows);
  engine.prepare(48000.0, std::max(8, voices), 2, kBlock);
  engine.setSimdIsa(isa);
  engine.setReleasePhaseAlign(p.phaseAlign);
  engine.setSampleProvider(provider);
  if (p.threads > 0) engine.setRenderThreads(p.threads, 1);

  Played result;
  for (int i = 0; i < voices; ++i) {
    mp::VoiceStart start;
    start.pipe = &pipes[static_cast<size_t>(i)];
    start.layer = &pipes[static_cast<size_t>(i)].layers[0];
    start.gain = 0.25f;
    start.ratio = p.ratios[static_cast<size_t>(i)];
    start.loopStartOverride = p.loopStart;
    start.loopEndOverride = p.loopEnd;
    start.loopCrossfadeFrames = p.loopCrossfade;
    start.oneShot = p.oneShot;
    if (engine.startVoice(start, static_cast<uint64_t>(i + 1)) >= 0) ++result.started;
  }
  if (p.settleMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(p.settleMs));

  std::vector<float> left(kBlock), right(kBlock);
  float* planes[2] = {left.data(), right.data()};
  for (int block = 0; block < p.totalBlocks; ++block) {
    if (block == p.holdBlocks)
      for (int i = 0; i < voices; ++i) engine.noteOff(static_cast<uint64_t>(i + 1), mp::NoteRelease{});
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.render(planes, 2, kBlock);
    result.out.insert(result.out.end(), left.begin(), left.end());
    result.out.insert(result.out.end(), right.begin(), right.end());
  }
  result.decoded = engine.blocksDecoded();
  return result;
}

double worstDifference(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 1e30;
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
  return worst;
}

double rms(const std::vector<float>& v) {
  if (v.empty()) return 0.0;
  double acc = 0.0;
  for (float x : v) acc += static_cast<double>(x) * x;
  return std::sqrt(acc / static_cast<double>(v.size()));
}

// The same to the bit. On ARM the compiler may fuse a multiply-add in the
// per-frame reader, which the vector code does not, and a coded voice moves
// between the two at other frames: there it is within a rounding.
bool same(double worst) {
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
  return worst < 1e-6;
#else
  return worst == 0.0;
#endif
}

// Compares the playing from plain counts with the same playing from coded
// ones, on every vector unit.
void compare(const std::string& what, Playing plainPlaying, Playing codedPlaying) {
  for (const auto isa : units()) {
    const std::string label = what + ", " + mp::simd::isaName(isa);
    const Played a = play(plainPlaying, false, isa);
    const Played b = play(codedPlaying, true, isa);
    const double worst = worstDifference(a.out, b.out);
    MP_CHECK(b.started == static_cast<int>(codedPlaying.ratios.size()),
             label + ": every coded voice starts");
    MP_CHECK(b.decoded > 0, label + ": the coded voices decode");
    MP_CHECK(rms(a.out) > 0.01, label + ": the note sounds");
    MP_CHECK(same(worst), label + ": coded plays what plain plays (worst difference " +
                              std::to_string(worst) + ")");
  }
}

} // namespace

// The coder keeps every count, at every width, and a 16-bit recording held at
// 24 bits is coded at 16 with its scale taking the eight bits back.
class CodedBufferTest final : public mp::test::Test {
public:
  CodedBufferTest() : Test("functional.samples.coded-buffer", Category::Functional) {}

  void run() override {
    for (const Width width : {Width::Int16, Width::Int24, Width::Int16In24})
      for (const int channels : {1, 2}) {
        const std::string label =
            std::string(widthName(width)) + ", " + std::to_string(channels) + " channels";
        const auto plain = plainBuffer(width, 9001, channels, 220.0, 7);
        const auto coded = codedCopy(plain);
        MP_CHECK(coded.coded != nullptr, label + ": held coded");
        MP_CHECK(coded.pcm16.empty() && coded.pcm24.empty(), label + ": the plain counts freed");
        MP_CHECK(coded.compact(), label + ": compact");
        MP_CHECK(coded.residentBytes() < plain.residentBytes(),
                 label + ": " + std::to_string(coded.residentBytes()) + " bytes against " +
                     std::to_string(plain.residentBytes()));
        if (width == Width::Int16In24) {
          MP_CHECK(coded.coded->bitDepth == 16, label + ": coded at 16 bits");
          MP_CHECK(coded.pcmScale == plain.pcmScale * 256.0f, label + ": the scale takes the bits back");
          MP_CHECK(!coded.wide(), label + ": read as 16-bit counts");
        } else {
          MP_CHECK(coded.wide() == plain.wide(), label + ": read at the width it was held at");
        }
        bool all = true;
        for (int64_t f = 0; f < plain.numFrames && all; ++f)
          for (int c = 0; c < channels; ++c)
            all = all && coded.sample(f, c) == plain.sample(f, c);
        MP_CHECK(all, label + ": every value reads back the same");
        MP_CHECK(coded.coded->data.capacity() == coded.coded->data.size(),
                 label + ": the stream holds no room it does not use");
      }

    // What it leaves alone.
    mp::SampleBuffer floats;
    floats.numFrames = 100;
    floats.frames.assign(100, 0.25f);
    MP_CHECK(!mp::codeSampleBuffer(floats) && floats.coded == nullptr,
             "float samples are left as they are");
    auto wide = plainBuffer(Width::Int16, 100, 2, 220.0, 3);
    wide.numChannels = 4;
    wide.numFrames = 50;
    MP_CHECK(!mp::codeSampleBuffer(wide) && !wide.pcm16.empty(),
             "four channels are left as they are");
  }
};
static CodedBufferTest g_codedBuffer;

// A decoder's kept blocks and a window belong to the stream they were decoded
// from, and to no other stream that later has its address: other frames coded
// into the same object read as themselves.
class CodedScratchForgetsTest final : public mp::test::Test {
public:
  CodedScratchForgetsTest() : Test("functional.samples.coded-scratch-forgets", Category::Functional) {}

  void run() override {
    std::vector<int32_t> first(9000), second(9000);
    for (size_t i = 0; i < first.size(); ++i) {
      first[i] = static_cast<int32_t>(i % 1000) - 500;
      second[i] = 700 - static_cast<int32_t>(i % 777);
    }
    mp::SplitRiceStream stream;
    MP_CHECK(mp::encodeSplitRice(first.data(), first.size(), 1, 16, stream), "the first codes");
    mp::DecodeScratch scratch;
    std::vector<unsigned char> bytes(mp::kWindowBytes, 0);
    mp::DecodedWindow window;
    window.data = bytes.data();
    MP_CHECK(mp::holdFrames(window, stream, 5000, 5010, scratch), "a window holds the first");
    MP_CHECK(mp::codedValue(stream, nullptr, nullptr, 100, 0, scratch) == first[100],
             "the scratch reads the first");
    MP_CHECK(window.value(5005, 0) == first[5005], "and so does the window");

    MP_CHECK(mp::encodeSplitRice(second.data(), second.size(), 1, 16, stream),
             "the second codes into the same stream");
    MP_CHECK(!window.holds(&stream, 5000, 5010), "the window no longer holds it");
    MP_CHECK(mp::codedValue(stream, &window, nullptr, 5005, 0, scratch) == second[5005],
             "the window's frame is read as the second");
    MP_CHECK(mp::codedValue(stream, nullptr, nullptr, 100, 0, scratch) == second[100],
             "the scratch's block is read as the second");
  }
};
static CodedScratchForgetsTest g_codedScratchForgets;

// A voice with no windows to read a coded sample through does not start: the
// pool was made for plain samples.
class CodedNeedsWindowsTest final : public mp::test::Test {
public:
  CodedNeedsWindowsTest() : Test("functional.voice.coded-needs-windows", Category::Functional) {}

  void run() override {
    const auto coded = codedCopy(plainBuffer(Width::Int16, 4000, 1, 220.0, 1));
    Playing p;
    p.attack = &coded;
    p.oneShot = true;
    p.totalBlocks = 4;
    const Played without = play(p, false, mp::simd::Isa::None);
    MP_CHECK(without.started == 0, "without windows the coded voice does not start");
    MP_CHECK(rms(without.out) == 0.0, "and nothing sounds");
    const Played with = play(p, true, mp::simd::Isa::None);
    MP_CHECK(with.started == 1 && rms(with.out) > 0.01, "with windows it sounds");
  }
};
static CodedNeedsWindowsTest g_codedNeedsWindows;

// Held notes on looped samples: short loops that cross a block of the coder,
// long ones, one that ends on the file's last frame, with and without a
// crossfade at the wrap, at four speeds at once.
class CodedLoopsTest final : public mp::test::Test {
public:
  CodedLoopsTest() : Test("functional.voice.coded-loops", Category::Functional) {}

  void run() override {
    struct Loop { int64_t start, end; const char* name; };
    const Loop loops[] = {
        {4000, 4200, "a short loop across a coder block"},
        {1000, 15000, "a long loop"},
        {10000, 20000, "a loop to the last frame"},
    };
    for (const Width width : {Width::Int16, Width::Int24, Width::Int16In24})
      for (const int channels : {1, 2}) {
        const auto plain = plainBuffer(width, 20000, channels, 220.0, 11);
        const auto coded = codedCopy(plain);
        for (const auto& loop : loops)
          for (const int xfade : {0, 64}) {
            Playing a;
            a.attack = &plain;
            a.ratios = {1.0, 0.7491, 1.4983, 2.17};
            a.loopStart = loop.start;
            a.loopEnd = loop.end;
            a.loopCrossfade = xfade;
            a.holdBlocks = 300;
            a.totalBlocks = 340;
            Playing b = a;
            b.attack = &coded;
            compare(std::string(widthName(width)) + ", " + std::to_string(channels) + " ch, " +
                        loop.name + ", crossfade " + std::to_string(xfade),
                    a, b);
          }
      }
  }
};
static CodedLoopsTest g_codedLoops;

// A note let go into its release sample: the attack coded, the release
// coded, both, with the release in phase with the attack and without, and
// with no crossfade.
class CodedReleasesTest final : public mp::test::Test {
public:
  CodedReleasesTest() : Test("functional.voice.coded-releases", Category::Functional) {}

  void run() override {
    for (const Width width : {Width::Int16, Width::Int24, Width::Int16In24})
      for (const int channels : {1, 2}) {
        auto attackPlain = plainBuffer(width, 20000, channels, 220.0, 21);
        attackPlain.loopStart = 6000;
        attackPlain.loopEnd = 18000;
        const auto releasePlain = plainBuffer(width, 9000, channels, 220.0, 22);
        auto attackCoded = codedCopy(attackPlain);
        const auto releaseCoded = codedCopy(releasePlain);
        struct Mix { bool attack, release; const char* name; };
        const Mix mixes[] = {
            {true, true, "both coded"},
            {true, false, "the attack coded"},
            {false, true, "the release coded"},
        };
        for (const auto& mix : mixes)
          for (const bool align : {false, true})
            for (const double xfadeMs : {10.0, 0.0}) {
              Playing a;
              a.attack = &attackPlain;
              a.release = &releasePlain;
              a.ratios = {1.0, 0.7491, 1.4983};
              a.phaseAlign = align;
              a.releaseCrossfadeMs = xfadeMs;
              a.holdBlocks = 90;
              a.totalBlocks = 140;
              Playing b = a;
              b.attack = mix.attack ? &attackCoded : &attackPlain;
              b.release = mix.release ? &releaseCoded : &releasePlain;
              compare(std::string(widthName(width)) + ", " + std::to_string(channels) +
                          " ch, " + mix.name + (align ? ", in phase" : "") +
                          ", crossfade " + std::to_string(static_cast<int>(xfadeMs)) + " ms",
                      a, b);
            }
        // Let go during the attack, before the loop: the release crossfade
        // reads the attack where it has not looped yet.
        Playing early;
        early.attack = &attackPlain;
        early.release = &releasePlain;
        early.ratios = {1.0, 1.4983};
        early.phaseAlign = true;
        early.holdBlocks = 3;
        early.totalBlocks = 60;
        Playing earlyCoded = early;
        earlyCoded.attack = &attackCoded;
        earlyCoded.release = &releaseCoded;
        compare(std::string(widthName(width)) + ", " + std::to_string(channels) +
                    " ch, let go during the attack",
                early, earlyCoded);
      }
  }
};
static CodedReleasesTest g_codedReleases;

// Many voices across the render threads, each thread with its own decoder.
class CodedThreadsTest final : public mp::test::Test {
public:
  CodedThreadsTest() : Test("functional.voice.coded-threads", Category::Functional) {}

  void run() override {
    auto attackPlain = plainBuffer(Width::Int24, 20000, 2, 220.0, 31);
    attackPlain.loopStart = 6000;
    attackPlain.loopEnd = 18000;
    const auto releasePlain = plainBuffer(Width::Int24, 9000, 2, 220.0, 32);
    const auto attackCoded = codedCopy(attackPlain);
    const auto releaseCoded = codedCopy(releasePlain);
    Playing a;
    a.attack = &attackPlain;
    a.release = &releasePlain;
    a.ratios.clear();
    for (int i = 0; i < 24; ++i) a.ratios.push_back(0.6 + 0.071 * i);
    a.loopCrossfade = 64;
    a.phaseAlign = true;
    a.holdBlocks = 60;
    a.totalBlocks = 110;
    a.threads = 3;
    Playing b = a;
    b.attack = &attackCoded;
    b.release = &releaseCoded;
    compare("24 voices on 3 threads", a, b);
  }
};
static CodedThreadsTest g_codedThreads;

// A sample whose head is resident and whose rest streams from the disk: the
// head coded, the rest arriving as it does for any sample.
class CodedStreamingTest final : public mp::test::Test {
public:
  CodedStreamingTest() : Test("functional.voice.coded-streaming", Category::Functional) {}

  void run() override {
    for (const Width width : {Width::Int16, Width::Int24})
      for (const int channels : {1, 2}) {
        const auto whole = plainBuffer(width, 30000, channels, 220.0, 41);
        auto all = std::make_shared<std::vector<float>>(
            static_cast<size_t>(whole.numFrames) * static_cast<size_t>(channels));
        for (int64_t f = 0; f < whole.numFrames; ++f)
          for (int c = 0; c < channels; ++c)
            (*all)[static_cast<size_t>(f) * static_cast<size_t>(channels) +
                   static_cast<size_t>(c)] = whole.sample(f, c);
        // The head: the first 5000 frames, the rest from the "disk".
        mp::SampleBuffer head = whole;
        head.numFrames = 5000;
        if (width == Width::Int16)
          head.pcm16.resize(static_cast<size_t>(5000 * channels));
        else
          head.pcm24.resize(static_cast<size_t>(5000 * channels));
        auto tail = std::make_shared<mp::SampleTail>();
        tail->totalFrames = whole.numFrames;
        const int fileChannels = channels;
        tail->read = [all, fileChannels](int64_t start, int n, float* dest, int ch) -> int64_t {
          for (int f = 0; f < n; ++f)
            for (int c = 0; c < ch; ++c) {
              const size_t idx = static_cast<size_t>(start + f) * static_cast<size_t>(fileChannels) +
                                 static_cast<size_t>(std::min(c, fileChannels - 1));
              dest[static_cast<size_t>(f) * static_cast<size_t>(ch) + static_cast<size_t>(c)] =
                  idx < all->size() ? (*all)[idx] : 0.0f;
            }
          return n;
        };
        head.tail = tail;
        const auto headCoded = codedCopy(head);
        MP_CHECK(headCoded.streams(), "the coded head still streams");
        Playing a;
        a.attack = &head;
        a.oneShot = true;
        a.ratios = {1.0};
        a.holdBlocks = 1000;
        a.totalBlocks = 100;
        a.settleMs = 150;
        Playing b = a;
        b.attack = &headCoded;
        compare(std::string(widthName(width)) + ", " + std::to_string(channels) +
                    " ch, a streamed sample",
                a, b);
      }
  }
};
static CodedStreamingTest g_codedStreaming;

#ifdef MP_TEST_HAS_AUDIO

namespace {

void writeWav(const juce::File& file, int frames, int channels, int bits, double sr) {
  file.getParentDirectory().createDirectory();
  file.deleteFile();
  juce::WavAudioFormat format;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  MP_CHECK(os != nullptr, "the wav file is writable");
  juce::StringPairArray meta;
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "1000");
  meta.set("Loop0End", "4000");
  std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(
      os.release(), sr, static_cast<unsigned>(channels), bits, meta, 0));
  MP_CHECK(writer != nullptr, "the wav writer is made");
  juce::AudioBuffer<float> buf(channels, frames);
  for (int c = 0; c < channels; ++c)
    for (int i = 0; i < frames; ++i)
      buf.setSample(c, i, static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 130.81 * i / sr +
                                                            0.3 * c)));
  writer->writeFromAudioSampleBuffer(buf, 0, frames);
}

} // namespace

// The loader codes what it reads, the cache keeps it coded, and a load with
// compression off never reads a coded cache.
class CodedCacheTest final : public mp::test::Test {
public:
  CodedCacheTest() : Test("functional.samples.coded-cache", Category::Functional) {}

  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("mp_coded_cache");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();

    mp::OrganModel model;
    int id = 1;
    for (const int bits : {16, 24})
      for (const int channels : {1, 2}) {
        const auto name = "s" + std::to_string(id) + ".wav";
        writeWav(root.getChildFile(name), 9600, channels, bits, 48000.0);
        mp::SampleRef ref;
        ref.sampleId = id;
        ref.fileName = name;
        model.samples[id] = ref;
        ++id;
      }
    const int count = id - 1;

    struct Load {
      std::unique_ptr<mp::SampleLibrary> lib;
      int loaded = 0;
    };
    const auto load = [&](bool code, mp::SampleStorage storage) {
      Load out;
      out.lib = std::make_unique<mp::SampleLibrary>();
      out.lib->setLoadThreads(1);
      out.lib->setStorage(storage);
      out.lib->setCodeSamples(code);
      out.lib->setCacheDir(cacheDir.getFullPathName().toStdString());
      out.lib->setCacheMode(mp::SampleLibrary::CacheMode::Single);
      out.lib->setCacheIdentity("coded", "stamp");
      mp::LoadProgress progress;
      progress.resetBudget(0);
      const auto report = out.lib->loadAll(model, root.getFullPathName().toStdString(), 0,
                                           mp::LoopSelection::Longest, &progress);
      out.loaded = report.loaded;
      return out;
    };

    for (const auto storage : {mp::SampleStorage::Int16, mp::SampleStorage::Int24}) {
      const std::string label = storage == mp::SampleStorage::Int16 ? "16-bit" : "24-bit";
      cacheDir.deleteRecursively();
      cacheDir.createDirectory();

      // Plain, for the values to compare against.
      Load plain = load(false, storage);
      MP_CHECK(plain.loaded == count, label + ": the samples load plain");
      MP_CHECK(plain.lib->codedCount() == 0, label + ": none held coded");
      cacheDir.deleteRecursively();
      cacheDir.createDirectory();

      // Coded, from the files: the cache is written coded.
      Load first = load(true, storage);
      MP_CHECK(first.loaded == count, label + ": the samples load coded");
      MP_CHECK(first.lib->codedCount() == static_cast<size_t>(count), label + ": all held coded");
      MP_CHECK(first.lib->cacheBytesRead() == 0, label + ": none from a cache");
      MP_CHECK(first.lib->residentBytes() < plain.lib->residentBytes(),
               label + ": " + std::to_string(first.lib->residentBytes()) + " bytes against " +
                   std::to_string(plain.lib->residentBytes()));
      first.lib->finishCacheWrite(false);

      // Coded again: from the cache, still coded, the same values.
      Load second = load(true, storage);
      MP_CHECK(second.loaded == count, label + ": the samples are there");
      MP_CHECK(second.lib->cacheBytesRead() > 0, label + ": they come from the cache");
      MP_CHECK(second.lib->codedCount() == static_cast<size_t>(count),
               label + ": and they are still coded");
      auto plainOf = plain.lib->provider();
      auto codedOf = second.lib->provider();
      for (int s = 1; s <= count; ++s) {
        const mp::SampleBuffer* p = plainOf(s);
        const mp::SampleBuffer* c = codedOf(s);
        MP_CHECK(p != nullptr && c != nullptr, label + ": sample " + std::to_string(s) + " is resident");
        if (p == nullptr || c == nullptr) continue;
        MP_CHECK(c->coded != nullptr, label + ": sample " + std::to_string(s) + " is coded");
        MP_CHECK(p->numFrames == c->numFrames && p->loopStart == c->loopStart &&
                     p->loopEnd == c->loopEnd,
                 label + ": the same length and loop");
        bool all = true;
        for (int64_t f = 0; f < p->numFrames && all; ++f)
          for (int ch = 0; ch < p->numChannels; ++ch) all = all && p->sample(f, ch) == c->sample(f, ch);
        MP_CHECK(all, label + ": sample " + std::to_string(s) + " reads the same values");
      }
      second.lib->finishCacheWrite(false);

      // Compression off: the coded cache is not read.
      Load off = load(false, storage);
      MP_CHECK(off.loaded == count, label + ": the samples load plain again");
      MP_CHECK(off.lib->cacheBytesRead() == 0, label + ": the coded cache is not read");
      MP_CHECK(off.lib->codedCount() == 0, label + ": and nothing is coded");
      off.lib->finishCacheWrite(false);
    }
    root.deleteRecursively();
  }
};
static CodedCacheTest g_codedCache;

#endif  // MP_TEST_HAS_AUDIO

#endif  // MP_TEST_HAS_SAMPLER
