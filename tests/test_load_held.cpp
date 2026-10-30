// A load held while the program is in the background waits at its next
// sample and goes on when let go (#650). iOS ended Masterpiece part way
// through a load the player had left running in the background: one thread
// at 100% for 48 seconds, over the system's 80% a minute.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/SampleLibrary.h"
#include "../src/mp_core/LoadProgress.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using mp::test::Category;

class LoadHeldTest final : public mp::test::Test {
public:
  LoadHeldTest() : Test("functional.load.held-in-background", Category::Functional) {}

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
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp_load_held");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();
    mp::OrganModel model;
    constexpr int kSamples = 100;
    for (int i = 1; i <= kSamples; ++i) {
      const auto name = "s" + std::to_string(i) + ".wav";
      writeWav(root.getChildFile(name));
      mp::SampleRef ref;
      ref.sampleId = i;
      ref.fileName = name;
      model.samples[i] = ref;
    }
    auto load = [&](mp::LoadProgress& progress, std::atomic<int>& loaded) {
      mp::SampleLibrary lib;
      lib.setLoadThreads(2);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("held", "stamp");
      loaded = lib.loadAll(model, root.getFullPathName().toStdString(), 0,
                           mp::LoopSelection::Longest, &progress)
                   .loaded;
      lib.finishCacheWrite(false);
    };
    using namespace std::chrono;

    // From the files, then from the cache the first load wrote.
    for (const Phase phase : {Phase::LoadingSamples, Phase::ReadingCache}) {
      const std::string from = phase == Phase::LoadingSamples ? "from the files" : "from the cache";
      mp::LoadProgress progress;
      progress.held.store(true);
      std::atomic<int> loaded{-1};
      std::thread t([&] { load(progress, loaded); });
      std::this_thread::sleep_for(milliseconds(500));
      // What the held load had done, checked once it is joined.
      const bool reached = progress.phase.load() == phase;
      const bool readNone = progress.done.load() == 0 && loaded.load() == -1;
      progress.held.store(false);
      t.join();
      MP_CHECK(reached, from + ": the load reached its samples");
      MP_CHECK(readNone, from + ": held, it reads none of them");
      MP_CHECK(loaded.load() == kSamples, from + ": let go, it reads every sample");
    }

    // Cancelled while held: the load ends at once, keeping the player's way out.
    mp::LoadProgress progress;
    progress.held.store(true);
    std::atomic<int> loaded{-1};
    std::thread t([&] { load(progress, loaded); });
    std::this_thread::sleep_for(milliseconds(300));
    const auto asked = steady_clock::now();
    progress.cancelled.store(true);
    t.join();
    const auto took = duration_cast<milliseconds>(steady_clock::now() - asked).count();
    MP_CHECK(took < 1000, "a cancel ends a held load within a second (took " + std::to_string(took) + " ms)");
    MP_CHECK(loaded.load() == 0, "and it read nothing");
    root.deleteRecursively();
  }
};
static LoadHeldTest g_loadHeld;

#endif  // MP_TEST_HAS_AUDIO
