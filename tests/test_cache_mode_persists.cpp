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
      MP_CHECK(proc.cacheMode() == CacheMode::PerOrgan, "the cache starts as one per organ (#1033)");
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
    {
      // The single shared cache chosen before #1033 is one per organ now.
      data.getChildFile("settings.mpglobal").replaceWithText("samplecache 1\n");
      mp::MasterpieceProcessor single;
      single.loadGlobalDefaults();
      MP_CHECK(single.cacheMode() == CacheMode::PerOrgan, "the old single cache reads as one per organ");
    }

    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static CacheModePersistsTest g_cacheModePersists;

// An organ's own sample cache choice (#1033): a cache for the organs played
// most with the program's setting Off, none for one organ with it on. Kept
// with the organ, and the one the load uses.
class OrganCacheChoiceTest final : public mp::test::Test {
public:
  OrganCacheChoiceTest() : Test("functional.settings.organ-cache-choice", Category::Functional) {}

  void run() override {
    using OrganCache = mp::MasterpieceProcessor::OrganCache;
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-organ-cache");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto odf = writeOrgan(dir);
    const auto data = dir.getChildFile("data");
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);

    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      MP_CHECK(proc.organCache() == OrganCache::AsProgram, "an organ follows the program's setting at first");
      MP_CHECK(proc.organCacheMode() == CacheMode::PerOrgan, "which is one cache per organ");

      proc.setCacheMode(CacheMode::Off);
      MP_CHECK(proc.organCacheMode() == CacheMode::Off, "the program's Off holds for an organ with no choice");
      proc.setOrganCache(OrganCache::Own);
      MP_CHECK(proc.organCacheMode() == CacheMode::PerOrgan, "an organ's own cache holds with the program's Off");
      MP_CHECK(proc.saveSettings(), "the organ's settings are saved");
      MP_CHECK(proc.settingsFileFor(odf).loadFileAsString().contains("organcache 1"),
               "with the organ's choice in its file");
    }
    {
      mp::MasterpieceProcessor again;
      again.prepareToPlay(48000.0, 256);
      again.loadGlobalDefaults();
      MP_CHECK(again.loadOrgan(odf, 0, false).ok, "the organ loads again");
      MP_CHECK(again.organCache() == OrganCache::Own, "its own cache is read back");
      MP_CHECK(again.sampleLibrary().cacheMode() == CacheMode::PerOrgan, "and the load used a cache of its own");

      // Deleting this organ's cache leaves the other organs' alone.
      const auto mine = again.organCacheFile();
      MP_CHECK(mine.getFileName().startsWith("samples-") && mine.getFileName().endsWith(".mpcache"),
               "the organ's cache is its own file, got " + mine.getFullPathName().toStdString());
      const auto other = mine.getSiblingFile("samples-another-organ-7.mpcache");
      mine.getParentDirectory().createDirectory();
      mine.replaceWithText("cache");
      mine.getSiblingFile(mine.getFileName() + ".part").replaceWithText("part");
      other.replaceWithText("cache");
      MP_CHECK(again.deleteOrganCache(), "the organ's cache is deleted");
      MP_CHECK(!mine.exists() && !mine.getSiblingFile(mine.getFileName() + ".part").exists(),
               "with the part of a write that stopped");
      MP_CHECK(other.existsAsFile(), "and another organ's cache stays");
      other.deleteFile();

      again.setCacheMode(CacheMode::Single);
      again.setOrganCache(OrganCache::None);
      MP_CHECK(again.organCacheMode() == CacheMode::Off, "no cache for this organ holds with the program's on");
      again.setOrganCache(OrganCache::AsProgram);
      MP_CHECK(again.organCacheMode() == CacheMode::Single, "and back to the program's choice");
      MP_CHECK(again.saveSettings(), "saved again");
      MP_CHECK(!again.settingsFileFor(odf).loadFileAsString().contains("organcache"),
               "an organ that follows the program writes no line");
      again.setKeepPortableCopy(true);
      again.setOrganCache(OrganCache::None);
      MP_CHECK(again.organCacheMode() == CacheMode::PerOrgan, "a portable copy keeps its cache");
    }

    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static OrganCacheChoiceTest g_organCacheChoice;

#endif  // MP_TEST_HAS_AUDIO
