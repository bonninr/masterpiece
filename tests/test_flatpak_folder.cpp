// A Flatpak keeps its settings apart from a native install's (#891). Its first
// start takes the shared folder's settings along, leaves out what the player
// makes again, and never copies twice.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"

using mp::test::Category;

class FlatpakFolderTest final : public mp::test::Test {
public:
  FlatpakFolderTest() : Test("functional.settings.flatpak-folder", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-flatpak-folder");
    dir.deleteRecursively();
    const auto shared = dir.getChildFile("home-config").getChildFile("Masterpiece");
    const auto configHome = dir.getChildFile("var-app-config");
    shared.getChildFile("organs").createDirectory();
    shared.getChildFile("cache").createDirectory();
    shared.getChildFile("Packaged").getChildFile("1234").createDirectory();
    shared.getChildFile("settings.mpglobal").replaceWithText("limiter 1");
    shared.getChildFile("organs").getChildFile("Friesach-1.mporgan").replaceWithText("transpose 2");
    shared.getChildFile("cache").getChildFile("samples.mpcache").replaceWithText("cache");
    shared.getChildFile("Packaged").getChildFile("1234").getChildFile("organ.xml").replaceWithText("x");
    shared.getChildFile("masterpiece.log").replaceWithText("log");

    const auto own = mp::sandboxFolder(configHome, shared);
    MP_CHECK(own == configHome.getChildFile("Masterpiece"), "the Flatpak's folder is its own");
    MP_CHECK(own.getChildFile("settings.mpglobal").loadFileAsString() == "limiter 1", "the settings came along");
    MP_CHECK(own.getChildFile("organs").getChildFile("Friesach-1.mporgan").loadFileAsString() == "transpose 2",
             "and each organ's settings");
    MP_CHECK(!own.getChildFile("cache").exists() && !own.getChildFile("Packaged").exists() &&
                 !own.getChildFile("masterpiece.log").exists(),
             "the cache, the unpacked packages and the log stay behind");
    MP_CHECK(!configHome.getChildFile("Masterpiece.part").exists(), "no partial copy is left");
    MP_CHECK(shared.getChildFile("settings.mpglobal").existsAsFile(), "the shared folder is left as it was");

    // The native install changes its settings; the Flatpak keeps its own.
    shared.getChildFile("settings.mpglobal").replaceWithText("limiter 0");
    own.getChildFile("organs").getChildFile("Friesach-1.mporgan").replaceWithText("transpose 0");
    MP_CHECK(mp::sandboxFolder(configHome, shared) == own, "the next start finds the same folder");
    MP_CHECK(own.getChildFile("settings.mpglobal").loadFileAsString() == "limiter 1", "and copies nothing again");
    MP_CHECK(own.getChildFile("organs").getChildFile("Friesach-1.mporgan").loadFileAsString() == "transpose 0",
             "what the Flatpak changed stays");

    // A player new to Masterpiece: nothing to take along.
    const auto fresh = dir.getChildFile("fresh-config");
    const auto none = mp::sandboxFolder(fresh, dir.getChildFile("nothing").getChildFile("Masterpiece"));
    MP_CHECK(none == fresh.getChildFile("Masterpiece") && !none.exists(), "a first start begins empty");
    dir.deleteRecursively();
  }
};
static FlatpakFolderTest g_flatpakFolder;

#endif
