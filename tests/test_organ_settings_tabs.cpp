// Reverb is saved with each organ, so it is in Organ settings with the rest of
// it, and Settings keeps what every organ shares (#637). Combination sets are
// in the Combinations window only (#853).
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/OrganSettings.h"
#include "../src/mp_ui/Settings.h"

using mp::test::Category;

class OrganSettingsTabsTest final : public mp::test::Test {
public:
  OrganSettingsTabsTest() : Test("functional.ui.organ-settings-tabs", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    {
      mp::ui::OrganSettingsWindow organ(proc, {});
      organ.showTab("Reverb");
      MP_CHECK(organ.currentTab() == "Reverb", "Organ settings has Reverb; it showed " +
                                                   organ.currentTab().toStdString());
      organ.showTab("Loading");
      MP_CHECK(organ.currentTab() == "Loading", "the first tab is still there");
      organ.showTab("Combination sets");
      MP_CHECK(organ.currentTab() == "Loading", "Organ settings no longer has Combination sets");
    }
    {
      mp::ui::SettingsWindow settings(proc, nullptr);
      settings.showTab("Metronome");
      for (const char* name : {"Reverb", "Combination sets"}) {
        settings.showTab(name);
        MP_CHECK(settings.currentTab() == "Metronome", std::string("Settings no longer has ") + name);
      }
    }
  }
};
static OrganSettingsTabsTest g_organSettingsTabs;

#endif  // MP_TEST_HAS_AUDIO
