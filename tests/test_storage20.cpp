// Twenty-bit resident storage.
//
// The layout: the interleaved samples of a buffer form one little-endian bit
// stream, and sample i occupies bits [20i, 20i+20). Two samples take five
// bytes, so a sample costs two and a half bytes. See mp::pcm20At in
// src/mp_sampler/VoiceEngine.h.
//
// What is checked here: that the packed layout keeps every value, that the
// loader's conversion stays within one step, that the vector code reads the
// packed samples exactly as the per-frame reader does, that twenty bits and
// twenty-four sound the same, and that the loader and the sample cache treat
// the width as a width of its own.
#include "MpTest.h"

#ifdef MP_TEST_HAS_SAMPLER

#include "../src/mp_sampler/SimdRun.h"
#include "../src/mp_sampler/VoiceEngine.h"
#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/SampleLibrary.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using mp::test::Category;

namespace {

// What a twenty-bit sample can hold.
constexpr int32_t kMin = -524288;
constexpr int32_t kMax = 524287;
// The largest count, and the scale that turns a count back into a value.
constexpr float kFull = 524287.0f;

// A buffer of packed samples, with the padding left at zero.
mp::SampleBuffer packedBuffer(const std::vector<int32_t>& values, int channels = 1) {
  mp::SampleBuffer b;
  b.numChannels = channels;
  b.sampleRate = 48000.0;
  b.numFrames = static_cast<int64_t>(values.size() / static_cast<size_t>(channels));
  b.pcm20.assign(mp::pcm20Bytes(values.size()), mp::Pcm20{});
  for (size_t i = 0; i < values.size(); ++i)
    mp::pcm20Put(b.pcm20.data(), i, values[i]);
  b.pcmScale = 1.0f / kFull;
  return b;
}

// A tone, in counts. The two channels differ, so a run that mixed them up
// would show.
std::vector<int32_t> tone(int frames, int channels, double hz, double sr) {
  std::vector<int32_t> out(static_cast<size_t>(frames) * static_cast<size_t>(channels));
  for (int f = 0; f < frames; ++f)
    for (int c = 0; c < channels; ++c) {
      const double x = 2.0 * 3.141592653589793 * hz * f / sr;
      out[static_cast<size_t>(f) * static_cast<size_t>(channels) +
          static_cast<size_t>(c)] = static_cast<int32_t>(
          std::lround(300000.0 * std::sin(x + 0.3 * c)));
    }
  return out;
}

// The same tone as floats, and its peak.
std::vector<float> toneFloats(int frames, int channels, double hz, double sr, float* peak) {
  std::vector<float> out(static_cast<size_t>(frames) * static_cast<size_t>(channels));
  float top = 0.0f;
  for (int f = 0; f < frames; ++f)
    for (int c = 0; c < channels; ++c) {
      const float v = static_cast<float>(0.4 * std::sin(2.0 * 3.141592653589793 * hz * f / sr) *
                                         (1.0f - 0.2f * c));
      out[static_cast<size_t>(f) * static_cast<size_t>(channels) +
          static_cast<size_t>(c)] = v;
      top = std::max(top, std::fabs(v));
    }
  *peak = top;
  return out;
}

// A pipe with one attack, and what the engine makes of it.
std::vector<float> renderVoice(const mp::SampleBuffer& buf, mp::simd::Isa isa) {
  mp::Pipe pipe;
  pipe.pipeId = 100;
  pipe.midiNote = 69;
  mp::PipeLayer layer;
  layer.layerId = 1;
  mp::AttackSample attack;
  attack.id = 11;
  attack.sample.sampleId = 1;
  layer.attacks.push_back(attack);
  pipe.layers.push_back(layer);

  mp::SampleProvider provider = [&buf](mp::Id id) -> const mp::SampleBuffer* {
    return id == 1 ? &buf : nullptr;
  };

  mp::VoiceEngine engine;
  engine.prepare(48000.0, 8, 2);
  engine.setSimdIsa(isa);
  engine.setSampleProvider(provider);
  mp::VoiceStart start;
  start.pipe = &pipe;
  start.layer = &pipe.layers[0];
  start.gain = 1.0f;
  start.ratio = 1.0;
  engine.startVoice(start, 1);

  std::vector<float> out;
  std::vector<float> left(256), right(256);
  float* planes[2] = {left.data(), right.data()};
  for (int block = 0; block < 12; ++block) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    engine.render(planes, 2, 256);
    out.insert(out.end(), left.begin(), left.end());
    out.insert(out.end(), right.begin(), right.end());
  }
  return out;
}

double rms(const std::vector<float>& v) {
  if (v.empty()) return 0.0;
  double acc = 0.0;
  for (float x : v) acc += static_cast<double>(x) * x;
  return std::sqrt(acc / static_cast<double>(v.size()));
}

} // namespace

// Every value written comes back exactly, at even and odd indexes, and the
// bytes the samples do not reach are left at zero.
class Storage20PackTest final : public mp::test::Test {
public:
  Storage20PackTest() : Test("functional.samples.storage20-pack", Category::Functional) {}

  void run() override {
    const int32_t edges[] = {kMin, kMin + 1, -2, -1, 0, 1, 2, kMax - 1, kMax};
    for (const size_t count : {size_t(1), size_t(2), size_t(3), size_t(1001)}) {
      std::vector<int32_t> values(count);
      for (size_t i = 0; i < count; ++i)
        values[i] = edges[i % (sizeof(edges) / sizeof(edges[0]))];
      const auto b = packedBuffer(values);
      for (size_t i = 0; i < count; ++i)
        MP_CHECK(mp::pcm20At(b.pcm20.data(), i) == values[i],
                 "a value reads back exactly, even index or odd");
      // The bytes the samples do not reach.
      const size_t usedBits = count * 20;
      const size_t usedBytes = (usedBits + 7) / 8;
      for (size_t i = usedBytes; i < b.pcm20.size(); ++i)
        MP_CHECK(b.pcm20[i].byte == 0, "the padding is zero");
      const size_t usedInLast = usedBits - (usedBytes - 1) * 8;
      const auto keep = static_cast<unsigned char>((1u << usedInLast) - 1u);
      MP_CHECK((b.pcm20[usedBytes - 1].byte & static_cast<unsigned char>(~keep)) == 0,
               "the bits the last sample does not reach are zero");
    }

    // Ten thousand values over the whole range.
    std::mt19937 rng(20261019);
    std::uniform_int_distribution<int32_t> dist(kMin, kMax);
    std::vector<int32_t> values(10000);
    for (auto& v : values) v = dist(rng);
    const auto b = packedBuffer(values);
    for (size_t i = 0; i < values.size(); ++i)
      MP_CHECK(mp::pcm20At(b.pcm20.data(), i) == values[i],
               "a value from the whole range reads back exactly");
    // And the buffer's own reader gives the same value.
    for (size_t i = 0; i < values.size(); ++i)
      MP_CHECK(std::fabs(b.sample(static_cast<int64_t>(i), 0) -
                         static_cast<float>(values[i]) / kFull) < 1e-6,
               "the buffer reads the same value");
  }
};
static Storage20PackTest g_storage20Pack;

// A float buffer converted to twenty bits and read back is within one step of
// the original everywhere: the loader's own conversion, and the scale that
// goes with it.
class Storage20ConvertTest final : public mp::test::Test {
public:
  Storage20ConvertTest()
    : Test("functional.samples.storage20-convert", Category::Functional) {}

  void run() override {
    const int frames = 4000;
    const int channels = 2;
    float peak = 0.0f;
    const auto src = toneFloats(frames, channels, 220.0, 48000.0, &peak);

    mp::SampleBuffer b;
    b.numChannels = channels;
    b.numFrames = frames;
    b.pcmScale = peak / kFull;
    const float toCounts = kFull / peak;
    b.pcm20.assign(mp::pcm20Bytes(src.size()), mp::Pcm20{});
    for (size_t i = 0; i < src.size(); ++i) {
      const float r = std::round(src[i] * toCounts);
      const float q = std::isfinite(r) ? std::clamp(r, -524288.0f, kFull) : 0.0f;
      mp::pcm20Put(b.pcm20.data(), i, static_cast<int32_t>(q));
    }

    MP_CHECK(b.packed20(), "the buffer says it holds packed twenty-bit samples");
    MP_CHECK(b.compact(), "and that it is compact");
    MP_CHECK(!b.wide(), "and that it is not 24-bit");
    const float step = peak / kFull;
    for (size_t i = 0; i < src.size(); ++i) {
      const auto frame = static_cast<int64_t>(i / static_cast<size_t>(channels));
      const auto channel = static_cast<int>(i % static_cast<size_t>(channels));
      MP_CHECK(std::fabs(b.sample(frame, channel) - src[i]) <= step,
               "the value comes back within one 20-bit step");
    }
  }
};
static Storage20ConvertTest g_storage20Convert;

// The vector code reads packed twenty-bit samples exactly as the per-frame
// reader does, for every unit this machine offers.
class Storage20KernelTest final : public mp::test::Test {
public:
  Storage20KernelTest()
    : Test("functional.samples.storage20-kernels", Category::Functional) {}

  void run() override {
    const auto units = mp::simd::available();
    for (const auto isa : units) std::printf("        vector unit: %s\n", mp::simd::isaName(isa));
    for (const auto isa : units) {
      for (const int channels : {1, 2, 3, 4}) {
        // SSE2 and NEON take two channels at a time; the engine asks before
        // it hands a wider layout to them.
        if (!mp::simd::runSupports(isa, channels)) continue;
        const int frames = 4096;
        const int outFrames = 256;
        const auto b = packedBuffer(tone(frames, channels, 440.0, 48000.0), channels);
        for (const double ratio : {0.5, 1.0, 1.4983, 2.0}) {
          // The last cursor that still reads inside the buffer: the reader
          // wants three frames of margin, the way the engine asks for.
          const double nearEnd =
              static_cast<double>(frames) - 6.0 - (outFrames - 1) * ratio;
          for (const double cursor : {1.0, 1000.5, nearEnd}) {
            std::vector<float> la(outFrames), ra(outFrames), lb(outFrames), rb(outFrames);
            float* plain[2] = {la.data(), ra.data()};
            float* fast[2] = {lb.data(), rb.data()};
            const double a =
                mp::simd::renderRun(mp::simd::Isa::None, b.pcm20.data(), channels, cursor,
                                    ratio, 0.25f, plain, 2, 0, outFrames);
            const double c = mp::simd::renderRun(isa, b.pcm20.data(), channels, cursor, ratio,
                                                 0.25f, fast, 2, 0, outFrames);
            MP_CHECK(std::fabs(a - c) < 1e-9,
                     std::string(mp::simd::isaName(isa)) +
                         ": the cursor ends where the per-frame reader leaves it");
            double worst = 0.0;
            for (int i = 0; i < outFrames; ++i) {
              worst = std::max(worst, static_cast<double>(std::fabs(la[i] - lb[i])));
              worst = std::max(worst, static_cast<double>(std::fabs(ra[i] - rb[i])));
            }
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
            // GCC may fuse a multiply-add in the per-frame reader on ARM.
            MP_CHECK(worst < 1e-6, std::string(mp::simd::isaName(isa)) +
                                       ": the same samples, within a rounding (worst " +
                                       std::to_string(worst) + ")");
#else
            MP_CHECK(worst == 0.0, std::string(mp::simd::isaName(isa)) +
                                       ": the same samples either way (worst difference " +
                                       std::to_string(worst) + ")");
#endif
          }
        }
      }
    }
  }
};
static Storage20KernelTest g_storage20Kernel;

// The same tone at 24 bits and at 20 bits differs by less than a hundredth of
// the full scale: twenty bits is well past what a listener hears.
class Storage20Against24Test final : public mp::test::Test {
public:
  Storage20Against24Test()
    : Test("functional.samples.storage20-against-24", Category::Functional) {}

  void run() override {
    const int frames = 4800;
    const int channels = 2;
    float peak = 0.0f;
    const auto src = toneFloats(frames, channels, 220.0, 48000.0, &peak);

    // At 24 bits: the integers of the file, unchanged.
    mp::SampleBuffer b24;
    b24.numChannels = channels;
    b24.numFrames = frames;
    b24.pcmScale = 1.0f / 8388607.0f;
    b24.pcm24.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      const auto v = static_cast<int>(std::lround(src[i] * 8388607.0f));
      b24.pcm24[i].b[0] = static_cast<unsigned char>(v & 0xFF);
      b24.pcm24[i].b[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
      b24.pcm24[i].b[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
    }
    b24.loopStart = 1000;
    b24.loopEnd = 4000;

    // At 20 bits: the same signal, quantised against its own peak.
    mp::SampleBuffer b20;
    b20.numChannels = channels;
    b20.numFrames = frames;
    b20.pcmScale = peak / kFull;
    const float toCounts = kFull / peak;
    b20.pcm20.assign(mp::pcm20Bytes(src.size()), mp::Pcm20{});
    for (size_t i = 0; i < src.size(); ++i) {
      const float r = std::round(src[i] * toCounts);
      const float q = std::isfinite(r) ? std::clamp(r, -524288.0f, kFull) : 0.0f;
      mp::pcm20Put(b20.pcm20.data(), i, static_cast<int32_t>(q));
    }
    b20.loopStart = 1000;
    b20.loopEnd = 4000;

    for (const auto isa : mp::simd::available()) {
      const auto a = renderVoice(b24, isa);
      const auto c = renderVoice(b20, isa);
      MP_CHECK(a.size() == c.size(), "both renders are the same length");
      double acc = 0.0;
      for (size_t i = 0; i < a.size() && i < c.size(); ++i) {
        const double d = static_cast<double>(a[i]) - c[i];
        acc += d * d;
      }
      const double difference = std::sqrt(acc / static_cast<double>(a.size()));
      const double db = 20.0 * std::log10(std::max(difference, 1e-30));
      MP_CHECK(db < -100.0,
               std::string(mp::simd::isaName(isa)) + ": 20 bits and 24 bits differ by " +
                   std::to_string(db) + " dBFS");
      MP_CHECK(rms(a) > 0.05, "the note sounds at both widths");
    }
  }
};
static Storage20Against24Test g_storage20Against24;

// A pipe held down sounds at twenty bits, and the buffer costs two and a
// half bytes a sample.
class Storage20EngineTest final : public mp::test::Test {
public:
  Storage20EngineTest()
    : Test("functional.samples.storage20-engine", Category::Functional) {}

  void run() override {
    const int frames = 4000;
    const int channels = 2;
    const auto values = tone(frames, channels, 220.0, 48000.0);
    auto b = packedBuffer(values, channels);
    b.loopStart = 1000;
    b.loopEnd = 3000;

    // Two and a half bytes a sample, plus the four bytes of padding.
    const auto expected =
        static_cast<int64_t>(mp::pcm20Bytes(static_cast<size_t>(frames * channels)));
    MP_CHECK(b.residentBytes() == expected, "the resident bytes are two and a half a sample");
    MP_CHECK(b.residentBytes() ==
                 static_cast<int64_t>((frames * channels * 5) / 2 + 4),
             "plus the four bytes of padding");

    for (const auto isa : mp::simd::available()) {
      const auto out = renderVoice(b, isa);
      double peak = 0.0;
      for (float x : out) peak = std::max(peak, static_cast<double>(std::fabs(x)));
      MP_CHECK(peak > 0.05, std::string(mp::simd::isaName(isa)) +
                                ": the note sounds at twenty bits");
    }
  }
};
static Storage20EngineTest g_storage20Engine;

#ifdef MP_TEST_HAS_AUDIO

namespace {

// A wav file with a sustain loop, for the loader to read. Twenty-four bits by
// default: a 16-bit file is held at its own sixteen bits.
void writeWav(const juce::File& file, int frames, int channels, double sr, int bits = 24) {
  file.getParentDirectory().createDirectory();
  file.deleteFile();
  juce::WavAudioFormat format;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  MP_CHECK(os != nullptr, "the wav file is writable");
  juce::StringPairArray meta;
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "1000");
  meta.set("Loop0End", "4000");
  std::unique_ptr<juce::AudioFormatWriter> writer(
      format.createWriterFor(os.release(), sr, static_cast<unsigned>(channels), bits, meta, 0));
  MP_CHECK(writer != nullptr, "the wav writer is made");
  juce::AudioBuffer<float> buf(channels, frames);
  for (int c = 0; c < channels; ++c)
    for (int i = 0; i < frames; ++i)
      buf.setSample(c, i,
                    static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 130.81 * i / sr)));
  writer->writeFromAudioSampleBuffer(buf, 0, frames);
}

} // namespace

// An organ loaded at twenty bits holds its samples packed, and a held note
// sounds.
class Storage20OrganTest final : public mp::test::Test {
public:
  Storage20OrganTest()
    : Test("functional.samples.storage20-organ", Category::Functional) {}

  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("mp_storage20_organ");
    root.deleteRecursively();
    const auto defs = root.getChildFile("OrganDefinitions");
    const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
    defs.createDirectory();
    pkg.createDirectory();
    writeWav(pkg.getChildFile("tone.wav"), 48000, 1, 48000.0);

    const auto odf = defs.getChildFile("twenty.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Twenty bits</Identification_Name>"
        "<Identification_UniqueOrganID>49</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
        "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
        "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
        "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
        "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
        "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
        "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division>"
        "</ObjectList><ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList><ObjectList ObjectType=\"Stop\">"
        "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
        "</ObjectList><ObjectList ObjectType=\"StopRank\">"
        "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteOfFirstMappedDivisionInputNode>36</MIDINoteOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
        "</ObjectList><ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
        "</ObjectList></Hauptwerk>");

    mp::MasterpieceProcessor proc;
    const juce::File settings = proc.settingsFileFor(odf);
    const bool had = settings.existsAsFile();
    const juce::String kept = had ? settings.loadFileAsString() : juce::String();
    settings.deleteFile();
    proc.setSampleStorage(mp::SampleStorage::Int20);
    proc.prepareToPlay(48000.0, 256);
    const auto result = proc.loadOrgan(odf, 0, false);
    MP_CHECK(result.ok, "the organ loads at twenty bits");

    auto provider = proc.sampleLibrary().provider();
    const mp::SampleBuffer* buf = provider(101);
    MP_CHECK(buf != nullptr, "the sample is resident");
    if (buf != nullptr) {
      MP_CHECK(buf->packed20(), "held as packed twenty-bit samples");
      MP_CHECK(buf->compact() && !buf->wide(), "and not as 16-bit or 24-bit");
      const auto expected = static_cast<int64_t>(
          mp::pcm20Bytes(static_cast<size_t>(buf->numFrames * buf->numChannels)));
      MP_CHECK(buf->residentBytes() == expected,
               "two and a half bytes a sample, plus the four bytes of padding");
    }

    proc.setStopEngaged(901, true);
    juce::AudioBuffer<float> out(2, 256);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    proc.processBlock(out, on);
    float peak = 0.0f;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 256; ++i) peak = std::max(peak, std::fabs(out.getSample(c, i)));
    MP_CHECK(peak > 0.001f, "a held note sounds at twenty bits");

    if (had)
      settings.replaceWithText(kept);
    else
      settings.deleteFile();
    root.deleteRecursively();
  }
};
static Storage20OrganTest g_storage20Organ;

// A cache written at one width is never read back at another.
class Storage20CacheTest final : public mp::test::Test {
public:
  Storage20CacheTest()
    : Test("functional.samples.storage20-cache", Category::Functional) {}

  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("mp_storage20_cache");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();

    mp::OrganModel model;
    for (int i = 1; i <= 4; ++i) {
      const auto name = "s" + std::to_string(i) + ".wav";
      writeWav(root.getChildFile(name), 4800, 1, 48000.0);
      mp::SampleRef ref;
      ref.sampleId = i;
      ref.fileName = name;
      model.samples[i] = ref;
    }

    const auto load = [&](mp::SampleStorage storage) {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setStorage(storage);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("storage20", "stamp");
      mp::LoadProgress progress;
      progress.resetBudget(0);
      const auto report = lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                                      mp::LoopSelection::Longest, &progress);
      lib.finishCacheWrite(false);
      return report;
    };

    // At 24 bits, with the cache on: the samples are read and the cache is
    // written.
    {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setStorage(mp::SampleStorage::Int24);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("storage20", "stamp");
      mp::LoadProgress progress;
      progress.resetBudget(0);
      const auto report = lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                                      mp::LoopSelection::Longest, &progress);
      lib.finishCacheWrite(false);
      MP_CHECK(report.loaded == 4, "the samples are read at 24 bits");
      MP_CHECK(lib.cacheBytesRead() == 0, "and none come from a cache");
    }

    // At 20 bits: the cache written at 24 bits is not read.
    {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setStorage(mp::SampleStorage::Int20);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("storage20", "stamp");
      mp::LoadProgress progress;
      progress.resetBudget(0);
      const auto report = lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                                      mp::LoopSelection::Longest, &progress);
      MP_CHECK(report.loaded == 4, "the samples are read at 20 bits");
      MP_CHECK(lib.cacheBytesRead() == 0,
               "the cache written at another width is not read");
      auto provider = lib.provider();
      const mp::SampleBuffer* buf = provider(1);
      MP_CHECK(buf != nullptr && buf->packed20(),
               "and the samples are held as packed twenty-bit samples");
      lib.finishCacheWrite(false);
    }

    // At 20 bits again: now the cache is read.
    {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setStorage(mp::SampleStorage::Int20);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("storage20", "stamp");
      mp::LoadProgress progress;
      progress.resetBudget(0);
      const auto report = lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                                      mp::LoopSelection::Longest, &progress);
      MP_CHECK(report.loaded == 4, "the samples are there");
      MP_CHECK(lib.cacheBytesRead() > 0, "and they come from the cache written at this width");
      auto provider = lib.provider();
      const mp::SampleBuffer* buf = provider(1);
      MP_CHECK(buf != nullptr && buf->packed20(),
               "and they are still packed twenty-bit samples");
    }

    root.deleteRecursively();
  }
};
static Storage20CacheTest g_storage20Cache;

// A 16-bit file at twenty bits is held at its own sixteen, as at 24: the same
// counts in two bytes a sample, never two and a half. A 24-bit file is still
// packed. The estimate before a load says the same.
class Storage20SixteenBitTest final : public mp::test::Test {
public:
  Storage20SixteenBitTest()
    : Test("functional.samples.storage20-sixteen-bit", Category::Functional) {}

  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("mp_storage20_sixteen");
    root.deleteRecursively();
    root.createDirectory();

    mp::OrganModel model;
    for (int i = 1; i <= 2; ++i) {
      const auto name = "s" + std::to_string(i) + ".wav";
      writeWav(root.getChildFile(name), 4800, 2, 48000.0, i == 1 ? 16 : 24);
      mp::SampleRef ref;
      ref.sampleId = i;
      ref.fileName = name;
      model.samples[i] = ref;
    }

    const auto load = [&](mp::SampleLibrary& lib, mp::SampleStorage storage) {
      lib.setLoadThreads(1);
      lib.setStorage(storage);
      mp::LoadProgress progress;
      progress.resetBudget(0);
      return lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                         mp::LoopSelection::Longest, &progress);
    };
    mp::SampleLibrary at20, at24;
    MP_CHECK(load(at20, mp::SampleStorage::Int20).loaded == 2, "both files load at 20 bits");
    MP_CHECK(load(at24, mp::SampleStorage::Int24).loaded == 2, "and at 24 bits");

    const mp::SampleBuffer* s20 = at20.provider()(1);
    const mp::SampleBuffer* s24 = at24.provider()(1);
    MP_CHECK(s20 != nullptr && s24 != nullptr, "the 16-bit file is resident both ways");
    if (s20 != nullptr && s24 != nullptr) {
      MP_CHECK(!s20->packed20() && s20->compact() && !s20->wide(),
               "the 16-bit file is held at 16 bits under the 20-bit setting");
      MP_CHECK(s20->residentBytes() == s20->numFrames * s20->numChannels * 2,
               "two bytes a sample: " + std::to_string(s20->residentBytes()));
      MP_CHECK(s20->residentBytes() == s24->residentBytes(),
               "the same as the 24-bit setting holds it");
      bool same = s20->numFrames == s24->numFrames && s20->numChannels == s24->numChannels;
      for (int64_t f = 0; same && f < s20->numFrames; ++f)
        for (int c = 0; c < s20->numChannels; ++c)
          same = same && s20->sample(f, c) == s24->sample(f, c);
      MP_CHECK(same, "and every sample is the file's own, exactly");
    }

    const mp::SampleBuffer* wide = at20.provider()(2);
    MP_CHECK(wide != nullptr && wide->packed20(), "a 24-bit file is still packed at twenty bits");

    mp::SampleLibrary::SampleShape shape;
    shape.frames = 4800;
    shape.channels = 2;
    shape.rate = 48000.0;
    shape.bits = 16;
    MP_CHECK(at20.residentBytes(shape) == at24.residentBytes(shape),
             "the estimate gives a 16-bit file the same bytes at 20 as at 24 bits");
    shape.bits = 24;
    MP_CHECK(at20.residentBytes(shape) < at24.residentBytes(shape),
             "and a 24-bit file fewer at 20");

    root.deleteRecursively();
  }
};
static Storage20SixteenBitTest g_storage20SixteenBit;

#endif  // MP_TEST_HAS_AUDIO

#endif  // MP_TEST_HAS_SAMPLER
