// Missing sample files counted as files (#1211). The warning after a load says
// how many files the organ names and the disk lacks; a file several pipes play
// counts once, as the player has one file to find, not eleven.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/SampleLibrary.h"
#include "../src/mp_core/LoadProgress.h"

#include <algorithm>
#include <cmath>

using mp::test::Category;

class MissingSamplesTest final : public mp::test::Test {
public:
  MissingSamplesTest() : Test("functional.samples.missing-files-counted", Category::Functional) {}

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
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp_missing_samples");
    root.deleteRecursively();
    root.createDirectory();
    writeWav(root.getChildFile("present.wav"));

    // Thirteen pipes: eleven play one missing drum file, one a missing file of
    // its own, one a file that is there.
    constexpr int kDrums = 11;
    mp::OrganModel model;
    mp::Rank rank;
    rank.rankId = 1;
    for (int p = 1; p <= kDrums + 2; ++p) {
      mp::SampleRef ref;
      ref.sampleId = p;
      ref.fileName = p <= kDrums ? "drum.wav" : p == kDrums + 1 ? "lost.wav" : "present.wav";
      ref.pitchHz = 80.0 * std::pow(2.0, (p - 1) / 12.0);
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

    mp::SampleLibrary lib;
    lib.setLoadThreads(1);
    lib.setCacheMode(mp::SampleLibrary::CacheMode::Off);
    mp::LoadProgress progress;
    const auto report =
        lib.loadAll(model, root.getFullPathName().toStdString(), 0, mp::LoopSelection::Longest, &progress);

    MP_CHECK(report.loaded == 1, "the file that is there loads (" + std::to_string(report.loaded) + ")");
    MP_CHECK(report.missing == kDrums + 1,
             "every sample of a missing file counts as missing (" + std::to_string(report.missing) + ")");
    MP_CHECK(report.missingFileCount == 2,
             "two files are missing, the shared one counted once (" + std::to_string(report.missingFileCount) + ")");
    auto named = [&](const char* name) {
      return std::any_of(report.missingFiles.begin(), report.missingFiles.end(),
                         [&](const std::string& f) { return f.find(name) != std::string::npos; });
    };
    MP_CHECK(report.missingFiles.size() == 2 && named("drum.wav") && named("lost.wav"),
             "the list names each missing file once (" + std::to_string(report.missingFiles.size()) + " names)");
    root.deleteRecursively();
  }
};

static MissingSamplesTest g_missingSamples;

#endif
