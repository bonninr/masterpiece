// An organ opened again, and again, then another: the memory the earlier
// loads took goes back to the system, so the next load finds it free. On
// Linux the C library kept the freed samples, and a third load of an organ
// stopped for lack of memory that nothing was using (#636).
#include "MpTest.h"

#if defined(MP_TEST_HAS_AUDIO) && defined(__linux__) && !defined(__ANDROID__)

#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

using mp::test::Category;

class ReloadMemoryTest final : public mp::test::Test {
public:
  ReloadMemoryTest() : Test("functional.load.reload-returns-memory", Category::Functional) {}

  static int64_t residentBytes() {
    std::ifstream statm("/proc/self/statm");
    int64_t size = 0, resident = 0;
    statm >> size >> resident;
    return resident * static_cast<int64_t>(sysconf(_SC_PAGESIZE));
  }

  // A one-manual organ of four ranks of 61 pipes, each a WAV of 3 s of noise.
  static juce::File writeOrgan(const juce::File& dir) {
    constexpr int kRanks = 4, kPipes = 61, kFrames = 3 * 48000;
    juce::String organ;
    organ << "[Organ]\r\nChurchName=Reload\r\nHasPedals=N\r\nNumberOfManuals=1\r\nNumberOfRanks=" << kRanks
          << "\r\nNumberOfWindchestGroups=1\r\nNumberOfPanels=0\r\n[WindchestGroup001]\r\nName=W\r\n"
          << "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=61\r\nNumberOfAccessibleKeys=61\r\n"
          << "FirstAccessibleKeyMIDINoteNumber=36\r\nNumberOfStops=" << kRanks << "\r\n";
    for (int s = 1; s <= kRanks; ++s) organ << "Stop" << juce::String(s).paddedLeft('0', 3) << "=" << s << "\r\n";
    uint32_t seed = 12345;
    juce::AudioBuffer<float> noise(1, kFrames);
    for (int r = 1; r <= kRanks; ++r) {
      const auto rank = juce::String(r).paddedLeft('0', 3);
      organ << "[Rank" << rank << "]\r\nName=R" << r << "\r\nFirstMidiNoteNumber=36\r\nNumberOfLogicalPipes="
            << kPipes << "\r\n";
      dir.getChildFile("r" + juce::String(r)).createDirectory();
      for (int p = 1; p <= kPipes; ++p) {
        const juce::String path = "r" + juce::String(r) + "/p" + juce::String(p) + ".wav";
        organ << "Pipe" << juce::String(p).paddedLeft('0', 3) << "=" << path << "\r\n";
        for (int i = 0; i < kFrames; ++i) {
          seed = seed * 1664525u + 1013904223u;
          noise.setSample(0, i, static_cast<float>(seed >> 8) / 16777216.0f * 0.2f - 0.1f);
        }
        juce::WavAudioFormat wav;
        const juce::File f = dir.getChildFile(path);
        std::unique_ptr<juce::FileOutputStream> out(f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(out.get(), 48000.0, 1, 24, {}, 0));
        if (w != nullptr) {
          out.release();
          w->writeFromAudioSampleBuffer(noise, 0, kFrames);
        }
      }
      organ << "[Stop" << rank << "]\r\nName=Stop " << r << "\r\nNumberOfRanks=1\r\nRank001=" << r
            << "\r\nNumberOfAccessiblePipes=61\r\n";
    }
    const juce::File file = dir.getChildFile("reload.organ");
    file.replaceWithText(organ);
    return file;
  }

  void run() override {
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-reload-memory");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File big = writeOrgan(dir);
    const juce::File small(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
    {
      mp::MasterpieceProcessor proc;
      proc.setCacheMode(mp::SampleLibrary::CacheMode::Off);
      proc.setSampleStorage(mp::SampleStorage::Int24);
      proc.setCodeSamples(false);
      proc.setLoadMono(false);

      const int64_t before = residentBytes();
      int64_t samples = 0;
      for (int i = 0; i < 3; ++i) {
        MP_CHECK(proc.loadOrgan(big, 0, false).ok, "the organ loads, time " + std::to_string(i + 1));
        samples = proc.sampleLibrary().residentBytes();
      }
      MP_CHECK(samples > 80ll * 1024 * 1024, "its samples take " + std::to_string(samples >> 20) + " MB");
      MP_CHECK(proc.loadOrgan(small, 0, false).ok, "then another organ loads");
      const int64_t after = residentBytes();
      // What the samples took comes back; a quarter of it is room for the
      // program's own allocations on the way.
      MP_CHECK(after - before < samples / 4,
               "the memory comes back: " + std::to_string((after - before) >> 20) + " MB still held of " +
                   std::to_string(samples >> 20) + " MB of samples");
    }
    dir.deleteRecursively();
  }
};
static ReloadMemoryTest g_reloadMemory;

#endif
