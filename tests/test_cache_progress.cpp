// A load from the sample cache says so and counts the samples (#591). The
// cache was read while the loading window still said "Finding the samples",
// with no progress, for as long as reading it took.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/SampleLibrary.h"
#include "../src/mp_core/LoadProgress.h"

#include <cmath>

using mp::test::Category;

class CacheProgressTest final : public mp::test::Test {
public:
  CacheProgressTest() : Test("functional.load.cache-progress", Category::Functional) {}

  static void writeWav(const juce::File& f) {
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(os.release(), 48000.0, 1, 16, juce::StringPairArray(), 0));
    juce::AudioBuffer<float> tone(1, 4800);
    for (int i = 0; i < 4800; ++i)
      tone.setSample(0, i, 0.3f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 4800);
  }

  void run() override {
    using Phase = mp::LoadProgress::Phase;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp_cache_progress");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();
    mp::OrganModel model;
    constexpr int kSamples = 200;
    for (int i = 1; i <= kSamples; ++i) {
      const auto name = "s" + std::to_string(i) + ".wav";
      writeWav(root.getChildFile(name));
      mp::SampleRef ref;
      ref.sampleId = i;
      ref.fileName = name;
      model.samples[i] = ref;
    }
    auto load = [&](mp::LoadProgress& progress) {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("progress", "stamp");
      const auto report = lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                                      mp::LoopSelection::Longest, &progress);
      lib.finishCacheWrite(false);
      return report.loaded;
    };

    mp::LoadProgress first;
    MP_CHECK(load(first) == kSamples, "the first load reads every sample");
    MP_CHECK(first.phase.load() == Phase::LoadingSamples, "from their files");

    mp::LoadProgress cached;
    MP_CHECK(load(cached) == kSamples, "the second load has every sample");
    MP_CHECK(cached.phase.load() == Phase::ReadingCache,
             "from the cache, and the loading window says so");
    MP_CHECK(cached.total.load() == kSamples && cached.done.load() == kSamples,
             "counting the samples, for the bar");
    MP_CHECK(std::string(mp::LoadProgress::phaseName(Phase::ReadingCache)) == "Loading samples from the cache",
             "the stage is named for what it does");
    MP_CHECK(static_cast<size_t>(Phase::Failed) < mp::LoadProgress::kPhases, "every stage has a slot");
    root.deleteRecursively();
  }
};
static CacheProgressTest g_cacheProgress;

#endif  // MP_TEST_HAS_AUDIO
