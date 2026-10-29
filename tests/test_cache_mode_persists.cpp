// The sample cache choice is the program's and outlives it (#565). It was
// kept with each organ, so "Off" chosen in Settings came back as "One cache"
// after a restart: every organ's file carried a "cache 1" line from before,
// and that line was read at each load.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;
using CacheMode = mp::SampleLibrary::CacheMode;

namespace {

juce::File writeOrgan(const juce::File& dir) {
  const auto odf = dir.getChildFile("cache.Organ_Hauptwerk_xml");
  odf.replaceWithText(R"(<?xml version="1.0"?><Hauptwerk FileFormat="Organ">)"
                      R"(<ObjectList ObjectType="_General"><_General>)"
                      R"(<Identification_Name>Cache</Identification_Name>)"
                      R"(<Identification_UniqueOrganID>83</Identification_UniqueOrganID>)"
                      R"(</_General></ObjectList></Hauptwerk>)");
  return odf;
}

}  // namespace

class CacheModePersistsTest final : public mp::test::Test {
public:
  CacheModePersistsTest() : Test("functional.settings.cache-mode-persists", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-cache-mode");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto odf = writeOrgan(dir);
    // The program's own settings file is not this test's to change.
    const auto data = dir.getChildFile("data");
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);

    {
      // An organ saved by an earlier version, with the cache line in its file.
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      MP_CHECK(proc.cacheMode() == CacheMode::Single, "the cache starts as one cache");
      const auto organFile = proc.settingsFileFor(odf);
      organFile.getParentDirectory().createDirectory();
      organFile.replaceWithText("# Masterpiece per-organ settings\ncache 1\n");

      proc.setCacheMode(CacheMode::Off);
      MP_CHECK(proc.globalSettingsFile().loadFileAsString().contains("samplecache 0"),
               "the choice is written the moment it is made");
    }
    {
      // The next start: a new engine, the same settings, the same organ.
      mp::MasterpieceProcessor again;
      again.prepareToPlay(48000.0, 256);
      again.loadGlobalDefaults();
      MP_CHECK(again.cacheMode() == CacheMode::Off, "Off is read back at the next start");
      MP_CHECK(again.loadOrgan(odf, 0, false).ok, "the organ loads again");
      MP_CHECK(again.cacheMode() == CacheMode::Off,
               "and the organ's old cache line does not bring one cache back");
      MP_CHECK(again.saveSettings(), "the organ's settings are saved");
      MP_CHECK(!again.settingsFileFor(odf).loadFileAsString().contains("\ncache "),
               "and the organ's file holds no cache line of its own");

      // From the command line: for this session, never written.
      again.overrideCacheMode(CacheMode::PerOrgan);
      MP_CHECK(again.cacheMode() == CacheMode::PerOrgan, "the command line holds for the session");
      again.setLastOrgan(dir.getChildFile("other.Organ_Hauptwerk_xml"));
      MP_CHECK(again.globalSettingsFile().loadFileAsString().contains("samplecache 0"),
               "and the player's own choice is the one written");
    }
    {
      // A global file from before, with "Save as global defaults"'s line only.
      data.getChildFile("settings.mpglobal").replaceWithText("cache 2\n");
      mp::MasterpieceProcessor older;
      older.loadGlobalDefaults();
      MP_CHECK(older.cacheMode() == CacheMode::PerOrgan, "a default saved by an earlier version is kept");
    }

    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static CacheModePersistsTest g_cacheModePersists;

#endif  // MP_TEST_HAS_AUDIO
