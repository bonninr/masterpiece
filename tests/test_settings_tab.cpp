// General settings opens on the tab it was last closed on (#591). A caller
// that names a tab still gets that tab.
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
      first.showTab("Console display");
      MP_CHECK(first.currentTab() == "Console display", "the Console display tab can be chosen");
    }
    {
      mp::ui::SettingsWindow again(proc, nullptr);
      MP_CHECK(again.currentTab() == "Console display",
               "Settings opens on the tab it was closed on; it opened on " +
                   again.currentTab().toStdString());
      // A caller that names a tab gets that tab.
      again.showTab("Reverb");
      MP_CHECK(again.currentTab() == "Reverb", "a named tab wins over the remembered one");
    }
    {
      mp::ui::SettingsWindow after(proc, nullptr);
      MP_CHECK(after.currentTab() == "Reverb", "the named tab is remembered in its turn");
    }
  }
};
static SettingsLastTabTest g_settingsLastTab;

#endif  // MP_TEST_HAS_AUDIO
