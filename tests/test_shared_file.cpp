// A file several pipes play is read once (#639). A GrandOrgue rank whose keys
// all sound one file -- a drum, a bell -- names it once per pipe, each at its
// own pitch, and each became a decoded copy of its own: 61 copies of every
// traps file on Barton, and the memory figures counted every one. Here the
// samples share one buffer, the memory counts it once, the sample cache keeps
// it shared, and the estimate counts the file once.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/SampleLibrary.h"
#include "../src/mp_core/LoadProgress.h"

#include <cmath>
#include <set>

using mp::test::Category;

class SharedFileTest final : public mp::test::Test {
public:
  SharedFileTest() : Test("functional.samples.shared-file-read-once", Category::Functional) {}

  static void writeWav(const juce::File& f, double hz) {
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w(
        wav.createWriterFor(os.release(), 48000.0, 1, 16, juce::StringPairArray(), 0));
    juce::AudioBuffer<float> tone(1, 48000);
    for (int i = 0; i < 48000; ++i)
      tone.setSample(0, i, 0.3f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * hz * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }

  void run() override {
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp_shared_file");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();
    writeWav(root.getChildFile("drum.wav"), 80.0);
    writeWav(root.getChildFile("pipe.wav"), 440.0);

    // One rank of twelve pipes, eleven playing the drum at their own pitch,
    // and one pipe of its own.
    constexpr int kDrums = 11;
    mp::OrganModel model;
    mp::Rank rank;
    rank.rankId = 1;
    for (int p = 1; p <= kDrums + 1; ++p) {
      mp::SampleRef ref;
      ref.sampleId = p;
      ref.fileName = p <= kDrums ? "drum.wav" : "pipe.wav";
      ref.pitchHz =80.0 * std::pow(2.0, (p - 1) / 12.0);
      model.samples[p] = ref;
      mp::AttackSample attack;
      attack.id = p;
      attack.sample = ref;
      mp::PipeLayer layer;
      layer.layerId = p;
      layer.attacks.push_back(attack);
      mp::Pipe pipe;
      pipe.pipeId = p;
      pipe.midiNote = 35 + p;
      pipe.layers.push_back(layer);
      rank.pipes.push_back(pipe);
    }
    model.ranks[1] = rank;
    const std::string dir = root.getFullPathName().toStdString();

    auto check = [&](mp::SampleLibrary& lib, const std::string& how) {
      auto provider = lib.provider();
      std::set<const mp::SampleBuffer*> buffers;
      for (int p = 1; p <= kDrums + 1; ++p) {
        MP_CHECK(provider(p) != nullptr, how + ": sample " + std::to_string(p) + " is there to play");
        buffers.insert(provider(p));
      }
      MP_CHECK(buffers.size() == 2, how + ": the drum's eleven samples share one buffer (" +
                                        std::to_string(buffers.size()) + " buffers for two files)");
      const int64_t one = provider(1)->residentBytes() + provider(kDrums + 1)->residentBytes();
      MP_CHECK(lib.residentBytes() == one, how + ": the memory counts the shared buffer once (" +
                                               std::to_string(lib.residentBytes()) + " bytes, " +
                                               std::to_string(one) + " for the two files)");
    };

    {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("shared", "stamp");
      mp::LoadProgress progress;
      const auto report = lib.loadAll(model, dir, 0, mp::LoopSelection::Longest, &progress);
      lib.finishCacheWrite(false);
      MP_CHECK(report.wanted == kDrums + 1 && report.loaded == kDrums + 1,
               "every sample counts as loaded (" + std::to_string(report.loaded) + " of " +
                   std::to_string(report.wanted) + ")");
      check(lib, "read from the files");
      MP_CHECK(lib.cacheBytesWritten() > 0 &&
                   lib.cacheBytesWritten() < 3 * static_cast<int64_t>(lib.residentBytes()) / 2,
               "the cache holds the shared buffer once (" + std::to_string(lib.cacheBytesWritten()) +
                   " bytes for " + std::to_string(lib.residentBytes()) + " resident)");
    }
    {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("shared", "stamp");
      mp::LoadProgress progress;
      const auto report = lib.loadAll(model, dir, 0, mp::LoopSelection::Longest, &progress);
      MP_CHECK(lib.cacheBytesRead() > 0 && report.loaded == kDrums + 1, "the second load reads the cache");
      check(lib, "read from the cache");
    }
    {
      // The estimate: the eleven drum samples name the first as the one read.
      mp::SampleLibrary lib;
      std::set<mp::Id> read;
      for (const auto& job : lib.shapeJobs(model, dir, nullptr)) {
        MP_CHECK(job.readAs != 0, "every sample says which is read for it");
        read.insert(job.readAs);
      }
      MP_CHECK(read.size() == 2, "the estimate counts two files (" + std::to_string(read.size()) + ")");
    }
    root.deleteRecursively();
  }
};
static SharedFileTest g_sharedFile;

#endif
