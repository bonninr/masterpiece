// General settings opens on the tab it was last closed on (#591). The menu's
// "General settings" and "Audio and MIDI" both opened the window on its first
// tab, so the two entries looked the same. "Audio and MIDI" names its tab, and
// that choice still wins.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Settings.h"

using mp::test::Category;

class SettingsLastTabTest final : public mp::test::Test {
public:
  SettingsLastTabTest() : Test("functional.ui.settings-last-tab", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    {
      mp::ui::SettingsWindow first(proc, nullptr);
      first.showTab("Display");
      MP_CHECK(first.currentTab() == "Display", "the Display tab can be chosen");
    }
    {
      mp::ui::SettingsWindow again(proc, nullptr);
      MP_CHECK(again.currentTab() == "Display",
               "General settings opens on the tab it was closed on; it opened on " +
                   again.currentTab().toStdString());
      // A caller that names a tab gets that tab.
      again.showTab("Room");
      MP_CHECK(again.currentTab() == "Room", "a named tab wins over the remembered one");
    }
    {
      mp::ui::SettingsWindow after(proc, nullptr);
      MP_CHECK(after.currentTab() == "Room", "the named tab is remembered in its turn");
    }
  }
};
static SettingsLastTabTest g_settingsLastTab;

#endif  // MP_TEST_HAS_AUDIO
