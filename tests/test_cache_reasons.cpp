// The sample cache of an organ opened from another folder (#1106). A cache
// written for a set on one drive serves the same set opened from another, and
// a cache passed over says why in the log: none there, another definition or
// another version of it, or other settings. A cache not written says why too.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/SampleLibrary.h"

#include <cmath>

using mp::test::Category;

namespace {

struct CapturedLog final : juce::Logger {
  CapturedLog() : previous(juce::Logger::getCurrentLogger()) { juce::Logger::setCurrentLogger(this); }
  ~CapturedLog() override { juce::Logger::setCurrentLogger(previous); }
  void logMessage(const juce::String& message) override { lines.add(message); }
  bool has(const juce::String& text) const {
    for (const auto& line : lines)
      if (line.contains(text)) return true;
    return false;
  }
  std::string all() const { return lines.joinIntoString(" | ").toStdString(); }
  juce::Logger* previous;
  juce::StringArray lines;
};

void writeWav(const juce::File& f) {
  juce::WavAudioFormat wav;
  std::unique_ptr<juce::FileOutputStream> os(f.createOutputStream());
  std::unique_ptr<juce::AudioFormatWriter> w(
      wav.createWriterFor(os.release(), 48000.0, 1, 16, juce::StringPairArray(), 0));
  juce::AudioBuffer<float> tone(1, 4800);
  for (int i = 0; i < 4800; ++i)
    tone.setSample(0, i, 0.3f * static_cast<float>(std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, 4800);
}

}  // namespace

class CacheReasonsTest final : public mp::test::Test {
public:
  CacheReasonsTest() : Test("functional.load.cache-reasons", Category::Functional) {}

  void run() override {
    using L = mp::SampleLibrary;
    // The words, from the fingerprints alone.
    const std::string base = "v1|definition 00aa|100|w2|m0|r0";
    MP_CHECK(L::cacheDifference(base, "v1|definition 00bb|100|w2|m0|r0") ==
                 "was written for another definition, or another version of this one "
                 "(definition 00aa; this load: definition 00bb)",
             "another definition is named: " + L::cacheDifference(base, "v1|definition 00bb|100|w2|m0|r0"));
    MP_CHECK(L::cacheDifference(base, "v1|definition 00aa|101|w2|m0|r0").find("another version of this one") !=
                 std::string::npos,
             "a definition of another size is named");
    MP_CHECK(L::cacheDifference(base, "v1|definition 00aa|100|w3|m0|r0") ==
                 "was written with other settings: w2 -> w3",
             "a setting that differs is named: " + L::cacheDifference(base, "v1|definition 00aa|100|w3|m0|r0"));
    MP_CHECK(L::cacheDifference(base, base + "|z1") == "was written with other settings: - -> z1",
             "a setting only one side has is named");

    // The same in a real load.
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp_cache_reasons");
    root.deleteRecursively();
    root.createDirectory();
    const auto cacheDir = root.getChildFile("cache");
    cacheDir.createDirectory();
    mp::OrganModel model;
    for (int i = 1; i <= 20; ++i) {
      const auto name = "s" + std::to_string(i) + ".wav";
      writeWav(root.getChildFile(name));
      mp::SampleRef ref;
      ref.sampleId = i;
      ref.fileName = name;
      model.samples[i] = ref;
    }
    auto load = [&](const std::string& stamp, CapturedLog& log) {
      mp::SampleLibrary lib;
      lib.setLoadThreads(1);
      lib.setCacheDir(cacheDir.getFullPathName().toStdString());
      lib.setCacheMode(L::CacheMode::PerOrgan);
      lib.setCacheIdentity("Organ-1", stamp);
      const auto report = lib.loadAll(model, root.getFullPathName().toStdString(), 0, mp::LoopSelection::Longest);
      lib.finishCacheWrite(false);
      (void)log;
      return report.loaded;
    };
    {
      CapturedLog log;
      MP_CHECK(load("definition 00aa|100", log) == 20, "the first load reads every sample");
      MP_CHECK(log.has("cache: not read: there is no cache at") && log.has("samples-Organ-1.mpcache"),
               "and says there was no cache yet: " + log.all());
      MP_CHECK(log.has("cache: saved"), "and saves one");
    }
    {
      CapturedLog log;
      MP_CHECK(load("definition 00aa|100", log) == 20 && !log.has("not read"),
               "the same load reads the cache and says nothing of a miss: " + log.all());
    }
    {
      CapturedLog log;
      MP_CHECK(load("definition 00bb|100", log) == 20, "another version of the definition loads");
      MP_CHECK(log.has("was written for another definition, or another version of this one"),
               "and the log says why the cache was passed over: " + log.all());
    }
    {
      // A sample that is there and cannot be read: no cache, and the log says why.
      root.getChildFile("s3.wav").replaceWithText("not a wav file");
      cacheDir.deleteRecursively();
      cacheDir.createDirectory();
      CapturedLog log;
      load("definition 00aa|100", log);
      MP_CHECK(log.has("cache: not written: 1 sample(s) could not be read"),
               "an unreadable sample leaves no cache, and the log says so: " + log.all());
    }
    root.deleteRecursively();
  }
};
static CacheReasonsTest g_cacheReasons;

namespace {

// One manual, one rank, one sample, under `root` as an installed set lies.
juce::File writeInstalledOrgan(const juce::File& root) {
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  writeWav(pkg.getChildFile("tone.wav"));
  const auto odf = defs.getChildFile("moved.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Moved</Identification_Name>"
      "<Identification_UniqueOrganID>1106</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>440</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>69</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>Gt</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

}  // namespace

class CacheAnyFolderTest final : public mp::test::Test {
public:
  CacheAnyFolderTest() : Test("functional.load.cache-any-folder", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp_cache_any_folder");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto data = dir.getChildFile("data");
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);

    // The set installed on one drive, and the same files on another, copied
    // later: the copy's definition carries another date.
    const auto first = writeInstalledOrgan(dir.getChildFile("E"));
    MP_CHECK(dir.getChildFile("E").copyDirectoryTo(dir.getChildFile("G")), "the set is copied");
    const auto second = dir.getChildFile("G").getChildFile("OrganDefinitions").getChildFile("moved.Organ_Hauptwerk_xml");
    second.setLastModificationTime(juce::Time::getCurrentTime() + juce::RelativeTime::hours(2));
    // The suite's own cache folder; a cache left by an earlier run goes first.
    mp::MasterpieceProcessor::defaultCacheDirectory().getChildFile("samples-Moved-1106.mpcache").deleteFile();

    juce::File cacheFile;
    juce::Time written;
    {
      CapturedLog log;
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(first, 0, false).ok, "the organ loads from its first folder");
      cacheFile = proc.organCacheFile();
      MP_CHECK(cacheFile.existsAsFile() && log.has("cache: saved"),
               "and its cache is written: " + cacheFile.getFullPathName().toStdString() + " " + log.all());
      written = cacheFile.getLastModificationTime();
    }
    {
      CapturedLog log;
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(second, 0, false).ok, "the same organ loads from the other folder");
      MP_CHECK(proc.organCacheFile() == cacheFile, "with the same cache file");
      MP_CHECK(!log.has("cache: not read") && !log.has("cache: saved"),
               "read from that cache, which is left as it was: " + log.all());
      MP_CHECK(cacheFile.getLastModificationTime() == written, "the cache is not written again");
    }
    {
      // A changed definition is another organ's cache: read from the files,
      // and the log says why.
      second.replaceWithText(second.loadFileAsString().replace("<Name>A</Name></Rank>", "<Name>B</Name></Rank>"));
      CapturedLog log;
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(second, 0, false).ok, "the changed definition loads");
      MP_CHECK(log.has("was written for another definition, or another version of this one"),
               "from its files, and the log says why: " + log.all());
    }

    cacheFile.deleteFile();
    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static CacheAnyFolderTest g_cacheAnyFolder;

#endif  // MP_TEST_HAS_AUDIO
