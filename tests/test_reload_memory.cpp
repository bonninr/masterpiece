// An organ opened again, and again, then another: the memory the earlier
// loads took goes back to the system, so the next load finds it free (#636).
// Checked on Linux, Windows and macOS against what each system counts as this
// program's own memory. The Linux case of #636 needs an organ of gigabytes
// spread over the loader threads' heaps; it was measured with Friesach on a
// 16 GB machine, three loads held at 7.2 GB.
#include "MpTest.h"

#if defined(__APPLE__)
 #include <TargetConditionals.h>
#endif

#if defined(MP_TEST_HAS_AUDIO) && !defined(__ANDROID__) && \
    (defined(__linux__) || defined(_WIN32) || (defined(__APPLE__) && !TARGET_OS_IPHONE))

#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
#elif defined(__APPLE__)
 #include <mach/mach.h>
#else
 #include <fstream>
#endif

using mp::test::Category;

class ReloadMemoryTest final : public mp::test::Test {
public:
  ReloadMemoryTest() : Test("functional.load.reload-returns-memory", Category::Functional) {}

  // The program's own memory, as the system counts it: sample files mapped
  // while they are read belong to the file cache and are left out.
  static int64_t ownBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                              sizeof(pmc)))
      return -1;
    return static_cast<int64_t>(pmc.PrivateUsage);
#elif defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS)
      return -1;
    return static_cast<int64_t>(info.phys_footprint);
#else
    std::ifstream status("/proc/self/status");
    std::string key;
    int64_t kb = 0;
    while (status >> key) {
      if (key == "RssAnon:") {
        status >> kb;
        break;
      }
      status.ignore(1 << 12, '\n');
    }
    return kb * 1024;
#endif
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
    const juce::File minimal(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
    {
      mp::MasterpieceProcessor proc;
      proc.setCacheMode(mp::SampleLibrary::CacheMode::Off);
      proc.setSampleStorage(mp::SampleStorage::Int24);
      proc.setCodeSamples(false);
      proc.setLoadMono(false);

      // The small organ first: the voices, threads and buffers the player
      // keeps from its first load onwards are in the starting figure.
      MP_CHECK(proc.loadOrgan(minimal, 0, false).ok, "a small organ loads first");
      const int64_t before = ownBytes();
      int64_t samples = 0;
      for (int i = 0; i < 3; ++i) {
        MP_CHECK(proc.loadOrgan(big, 0, false).ok, "the organ loads, time " + std::to_string(i + 1));
        samples = proc.sampleLibrary().residentBytes();
        std::printf("reload-memory: load %d, %lld MB of samples, %lld MB above the start\n", i + 1,
                    static_cast<long long>(samples >> 20),
                    static_cast<long long>((ownBytes() - before) >> 20));
      }
      MP_CHECK(samples > 80ll * 1024 * 1024, "its samples take " + std::to_string(samples >> 20) + " MB");
      MP_CHECK(proc.loadOrgan(minimal, 0, false).ok, "then the small organ loads again");
      const int64_t after = ownBytes();
      std::printf("reload-memory: %lld MB still held after the small organ again\n",
                  static_cast<long long>((after - before) >> 20));
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
