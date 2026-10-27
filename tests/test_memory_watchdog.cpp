// The memory watchdog: a load asks the system what memory is free as the
// samples come in, and stops before the system has to end the program. What
// was read before it stopped is kept and plays, and so is what was read
// before the player pressed Cancel.
#include "MpTest.h"
#include "../src/mp_core/LoadProgress.h"

#include <string>

namespace {

using mp::test::Category;

class WatchdogChargeTest final : public mp::test::Test {
public:
  WatchdogChargeTest() : Test("functional.memory.watchdog-charge", Category::Functional) {}
  void run() override {
    constexpr int64_t MB = 1024 * 1024;
    {
      mp::LoadProgress p;
      p.resetBudget(0);
      int asked = 0;
      int64_t free = 1000 * MB;
      p.freeMemory = [&] {
        ++asked;
        return free;
      };
      p.reserveBytes = 100 * MB;
      for (int i = 0; i < 31; ++i) MP_CHECK(p.charge(MB), "small charges go on");
      MP_CHECK(asked == 0, "the system is not asked for every small charge");
      MP_CHECK(p.charge(MB) && asked == 1, "it is asked once 32 MB have been charged");
      free = 120 * MB;
      MP_CHECK(!p.charge(40 * MB), "a charge that would leave less than the reserve stops the load");
      MP_CHECK(p.memoryLow.load() && p.overBudget.load() && p.isCancelled(),
               "and says it stopped for want of memory");
    }
    {
      mp::LoadProgress p;
      p.resetBudget(0);
      p.freeMemory = [] { return int64_t{-1}; };
      p.reserveBytes = 100 * MB;
      MP_CHECK(p.charge(500 * MB) && !p.memoryLow.load(),
               "a system that gives no figure leaves the load to the ceiling");
    }
    {
      mp::LoadProgress p;
      p.resetBudget(10 * MB);
      MP_CHECK(!p.charge(11 * MB) && p.overBudget.load() && !p.memoryLow.load(),
               "the ceiling still stops a load on its own, and says so");
      p.resetBudget(10 * MB);
      MP_CHECK(!p.overBudget.load() && !p.memoryLow.load(), "and a new load starts clear");
    }
  }
};

WatchdogChargeTest watchdogChargeTest;

}  // namespace

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/SystemMemory.h"

#include <cmath>

namespace {

void writeTone(const juce::File& file, double hz) {
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  juce::StringPairArray meta;
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "1000");
  meta.set("Loop0End", "47000");
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, 48000);
  for (int i = 0; i < 48000; ++i)
    tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * hz * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, 48000);
}

constexpr int kPipes = 8;

// One manual, one stop, eight pipes from C2, each its own file.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  std::string samples, pipes, layers, attacks;
  for (int p = 0; p < kPipes; ++p) {
    const int note = 36 + p;
    const double hz = 440.0 * std::pow(2.0, (note - 69) / 12.0);
    writeTone(pkg.getChildFile("p" + juce::String(p) + ".wav"), hz);
    const std::string i = std::to_string(p);
    samples += "<Sample><SampleID>" + std::to_string(100 + p) +
               "</SampleID><InstallationPackageID>1</InstallationPackageID><SampleFilename>p" + i +
               ".wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
               "<Pitch_ExactSamplePitch>" + std::to_string(hz) + "</Pitch_ExactSamplePitch></Sample>";
    pipes += "<Pipe_SoundEngine01><PipeID>" + std::to_string(300 + p) +
             "</PipeID><RankID>201</RankID><NormalMIDINoteNumber>" + std::to_string(note) +
             "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    layers += "<Pipe_SoundEngine01_Layer><LayerID>" + std::to_string(400 + p) + "</LayerID><PipeID>" +
              std::to_string(300 + p) + "</PipeID></Pipe_SoundEngine01_Layer>";
    attacks += "<Pipe_SoundEngine01_AttackSample><UniqueID>" + std::to_string(500 + p) +
               "</UniqueID><LayerID>" + std::to_string(400 + p) + "</LayerID><SampleID>" +
               std::to_string(100 + p) + "</SampleID></Pipe_SoundEngine01_AttackSample>";
  }
  const auto odf = defs.getChildFile("watchdog.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Watchdog</Identification_Name>"
      "<Identification_UniqueOrganID>38</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\">" + samples + "</ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks + "</ObjectList>"
      "<ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>" + std::to_string(kPipes) + "</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

// How many of the eight keys sound, played one at a time.
int keysThatSound(mp::MasterpieceProcessor& proc) {
  juce::AudioBuffer<float> buf(2, 256);
  int sounding = 0;
  for (int p = 0; p < kPipes; ++p) {
    juce::MidiBuffer on, off, none;
    on.addEvent(juce::MidiMessage::noteOn(1, 36 + p, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36 + p), 0);
    buf.clear();
    proc.processBlock(buf, on);
    float peak = 0.0f;
    for (int b = 0; b < 8; ++b) {
      buf.clear();
      proc.processBlock(buf, none);
      peak = std::max(peak, buf.getMagnitude(0, 256));
    }
    if (peak > 0.01f) ++sounding;
    buf.clear();
    proc.processBlock(buf, off);
    for (int b = 0; b < 40; ++b) {
      buf.clear();
      proc.processBlock(buf, none);
    }
  }
  return sounding;
}

class StoppedLoadPlaysTest final : public mp::test::Test {
public:
  StoppedLoadPlaysTest() : Test("functional.memory.stopped-load-plays", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-watchdog");
    const juce::File odf = writeOrgan(root);

    // The player presses Cancel while the samples are read: what arrived
    // before it is kept, and its keys sound.
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      int asked = 0;
      proc.memoryProbe = [&] {
        if (++asked == 3) proc.cancelLoad();
        return int64_t{1} << 40;
      };
      const auto r = proc.loadOrgan(odf, 0, false);
      MP_CHECK(r.ok && r.incomplete && r.stoppedByPlayer && !r.outOfMemory,
               "a load cancelled while its samples were read keeps them (" +
                   std::to_string(r.samples.loaded) + " of " + std::to_string(r.samples.wanted) + ")");
      MP_CHECK(r.samples.loaded > 0 && r.samples.loaded < kPipes, "part of the organ was read");
      proc.setStopEngaged(901, true);
      const int sounding = keysThatSound(proc);
      MP_CHECK(sounding == r.samples.loaded,
               "every key whose sample arrived sounds, and only those (" + std::to_string(sounding) +
                   " sound)");
      proc.releaseResources();
    }
    // The system runs low partway: the load stops, keeps what it read, and
    // says why.
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      int asked = 0;
      proc.memoryProbe = [&] { return ++asked < 4 ? int64_t{1} << 40 : int64_t{0}; };
      const auto r = proc.loadOrgan(odf, 0, false);
      MP_CHECK(r.ok && r.incomplete && r.memoryLow && r.outOfMemory && !r.stoppedByPlayer,
               "a load stopped for want of memory keeps what it read (" +
                   std::to_string(r.samples.loaded) + " of " + std::to_string(r.samples.wanted) + ")");
      proc.setStopEngaged(901, true);
      MP_CHECK(keysThatSound(proc) == r.samples.loaded, "and those keys sound");
      proc.releaseResources();
    }
    // With memory to spare the whole organ loads, and nothing is said.
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      proc.memoryProbe = [] { return int64_t{1} << 40; };
      const auto r = proc.loadOrgan(odf, 0, false);
      MP_CHECK(r.ok && !r.incomplete && r.samples.loaded == kPipes, "a load with room reads it all");
      proc.releaseResources();
    }
    root.deleteRecursively();
  }
};

StoppedLoadPlaysTest stoppedLoadPlaysTest;

// The system's own figure is there to read on the platforms that give one.
class SystemMemoryReadTest final : public mp::test::Test {
public:
  SystemMemoryReadTest() : Test("functional.memory.system-figure", Category::Functional) {}
  void run() override {
    const int64_t free = mp::availableMemoryBytes();
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
    MP_CHECK(free > 0, "the system says how much memory is free (" + std::to_string(free) + ")");
#endif
    MP_CHECK(mp::memoryReserveBytes() >= int64_t{256} * 1024 * 1024, "at least 256 MB is kept free");
  }
};

SystemMemoryReadTest systemMemoryReadTest;

// Memory refused in the middle of a load, and a sample whose read throws: the
// program goes on in both. Refused memory stops the load and keeps what was
// read; a throw costs that one sample.
class ThrowingLoadTest final : public mp::test::Test {
public:
  ThrowingLoadTest() : Test("functional.memory.load-survives-throws", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-watchdog-throw");
    const juce::File odf = writeOrgan(root);
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      int asked = 0;
      proc.memoryProbe = [&]() -> int64_t {
        if (++asked == 4) throw std::bad_alloc();
        return int64_t{1} << 40;
      };
      const auto r = proc.loadOrgan(odf, 0, false);
      MP_CHECK(r.ok && r.incomplete && r.memoryLow,
               "memory refused mid-load stops it with what was read kept (" +
                   std::to_string(r.samples.loaded) + " of " + std::to_string(r.samples.wanted) + ")");
      proc.setStopEngaged(901, true);
      MP_CHECK(r.samples.loaded > 0 && keysThatSound(proc) == r.samples.loaded, "and those keys sound");
      proc.releaseResources();
    }
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      int asked = 0;
      proc.memoryProbe = [&]() -> int64_t {
        if (++asked == 2) throw std::runtime_error("unreadable");
        return int64_t{1} << 40;
      };
      const auto r = proc.loadOrgan(odf, 0, false);
      MP_CHECK(r.ok && r.samples.loaded == kPipes - 1 && r.samples.failed == 1,
               "a sample that throws costs that sample only (" + std::to_string(r.samples.loaded) +
                   " loaded, " + std::to_string(r.samples.failed) + " failed)");
      proc.releaseResources();
    }
    root.deleteRecursively();
  }
};

ThrowingLoadTest throwingLoadTest;

// Damaged files ask for absurd amounts of memory before a byte is read. A WAV
// whose header claims two gigabytes from a file of ten kilobytes is refused
// as damaged, with nothing taken; a cache whose stored size is damaged is
// passed over, and the samples are read from their files.
class DamagedSizesTest final : public mp::test::Test {
public:
  DamagedSizesTest() : Test("functional.memory.damaged-sizes", Category::Functional) {}

  static void putLe32(juce::MemoryBlock& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xff);
  }

  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-damaged-sizes");
    root.deleteRecursively();
    root.createDirectory();
    constexpr int kFrames = 4800;
    // 16-bit mono, then its data chunk's length set to almost 2 GB.
    const auto wav = root.getChildFile("liar.wav");
    {
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(wav.createOutputStream());
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
      juce::AudioBuffer<float> tone(1, kFrames);
      for (int i = 0; i < kFrames; ++i) tone.setSample(0, i, static_cast<float>(0.3 * std::sin(0.05 * i)));
      w->writeFromAudioSampleBuffer(tone, 0, kFrames);
    }
    juce::MemoryBlock bytes;
    wav.loadFileAsData(bytes);
    bool patched = false;
    for (size_t i = 12; i + 8 <= bytes.getSize(); ++i)
      if (std::memcmp(static_cast<const char*>(bytes.getData()) + i, "data", 4) == 0) {
        putLe32(bytes, i + 4, 0x7ff00000u);
        patched = true;
        break;
      }
    MP_CHECK(patched, "the test file has a data chunk to damage");
    wav.replaceWithData(bytes.getData(), bytes.getSize());

    mp::OrganModel model;
    mp::SampleRef ref;
    ref.sampleId = 1;
    ref.fileName = "liar.wav";
    model.samples[1] = ref;
    const auto rootPath = root.getFullPathName().toStdString();
    {
      mp::SampleLibrary lib;
      mp::LoadProgress progress;
      const auto report = lib.loadAll(model, rootPath, 0, mp::LoopSelection::Longest, &progress);
      auto provider = lib.provider();
      const auto* buf = provider(1);
      MP_CHECK(report.failed == 1 && report.loaded == 0 && buf == nullptr,
               "a header that claims 2 GB from a 10 KB file is refused as damaged (" +
                   std::to_string(report.loaded) + " loaded, " + std::to_string(report.failed) + " failed)");
      MP_CHECK(progress.usedBytes.load() < 1024 * 1024, "and nothing like 2 GB was taken");
    }

    // A whole, honest sample written to the cache, then its stored length
    // damaged to 8 GB.
    const auto good = root.getChildFile("good.wav");
    {
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(good.createOutputStream());
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
      juce::AudioBuffer<float> tone(1, 12345);
      for (int i = 0; i < 12345; ++i) tone.setSample(0, i, static_cast<float>(0.3 * std::sin(0.05 * i)));
      w->writeFromAudioSampleBuffer(tone, 0, 12345);
    }
    model.samples[1].fileName = "good.wav";
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();
    auto makeLib = [&](mp::SampleLibrary& lib) {
      lib.setLoadThreads(1);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(mp::SampleLibrary::CacheMode::Single);
      lib.setCacheIdentity("damaged", "stamp");
    };
    int64_t storedBytes = 0;
    {
      mp::SampleLibrary lib;
      makeLib(lib);
      mp::LoadProgress progress;
      lib.loadAll(model, rootPath, 0, mp::LoopSelection::Longest, &progress);
      lib.finishCacheWrite(false);
      const auto* buf = lib.provider()(1);
      if (buf != nullptr) storedBytes = static_cast<int64_t>(buf->residentBytes());
    }
    const auto files = cacheDir.findChildFiles(juce::File::findFiles, false);
    MP_CHECK(files.size() == 1, "the cache was written");
    if (files.size() != 1) return;
    juce::MemoryBlock cache;
    files[0].loadFileAsData(cache);
    // The stored length is a 64-bit count just before the sample's bytes.
    bool damaged = false;
    for (size_t i = 0; i + 8 <= cache.getSize() && !damaged; ++i) {
      uint64_t v = 0;
      std::memcpy(&v, static_cast<const char*>(cache.getData()) + i, 8);
      if (storedBytes > 0 && v == static_cast<uint64_t>(storedBytes)) {
        const uint64_t huge = uint64_t{8} << 30;
        std::memcpy(static_cast<char*>(cache.getData()) + i, &huge, 8);
        damaged = true;
      }
    }
    MP_CHECK(damaged, "the cache's stored length found (" + std::to_string(storedBytes) + " bytes)");
    files[0].replaceWithData(cache.getData(), cache.getSize());
    {
      mp::SampleLibrary lib;
      makeLib(lib);
      mp::LoadProgress progress;
      const auto report = lib.loadAll(model, rootPath, 0, mp::LoopSelection::Longest, &progress);
      lib.finishCacheWrite(false);
      MP_CHECK(report.loaded == 1 && lib.provider()(1) != nullptr,
               "a cache with a damaged length is passed over and the sample read from its file");
    }
    root.deleteRecursively();
  }
};

DamagedSizesTest damagedSizesTest;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
