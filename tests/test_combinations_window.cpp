// The three choices above the pistons say what the program is doing, and the
// settings file is read after the console -- and so this window -- has been
// built (#591). "Set turns off after storing" showed off at every start while
// the choice was on, so a player ticking it again turned it off for good.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/CombinationsWindow.h"

using mp::test::Category;

namespace {
juce::ToggleButton* findToggle(juce::Component& root, const juce::String& text) {
  for (auto* c : root.getChildren()) {
    if (auto* b = dynamic_cast<juce::ToggleButton*>(c); b != nullptr && b->getButtonText() == text)
      return b;
    if (auto* deeper = findToggle(*c, text); deeper != nullptr) return deeper;
  }
  return nullptr;
}
}  // namespace

class CombinationsWindowTogglesTest final : public mp::test::Test {
public:
  CombinationsWindowTogglesTest()
      : Test("functional.ui.combinations-window-toggles", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    const juce::File global = proc.globalSettingsFile();
    const bool had = global.existsAsFile();
    const juce::String before = had ? global.loadFileAsString() : juce::String();

    // What a player who turned all three on left behind at the last quit.
    global.getParentDirectory().createDirectory();
    global.replaceWithText("# Masterpiece defaults for organs that have no settings of their own\n"
                           "combinationsontop 1\n"
                           "setoffafterstore 1\n"
                           "pistonsleavebass 1\n");

    {
      // The order the program builds them in: the window with the console,
      // the settings file afterwards (Main.cpp).
      mp::ui::CombinationsWindow window(proc);
      MP_CHECK(proc.loadGlobalDefaults(), "the settings file is read");
      MP_CHECK(proc.setOffAfterStore(), "the processor holds the choice");

      window.showOrHide(true);
      auto& content = *window.getContentComponent();
      auto* onTop = findToggle(content, TRANS("Always on top"));
      auto* setOff = findToggle(content, TRANS("Set turns off after storing"));
      auto* leaveBass = findToggle(content, TRANS("Pistons leave the bass coupler"));
      MP_CHECK(onTop != nullptr && setOff != nullptr && leaveBass != nullptr,
               "the window has its three boxes");
      if (onTop == nullptr || setOff == nullptr || leaveBass == nullptr) return;
      MP_CHECK(setOff->getToggleState(), "the box shows the choice at the next start");
      MP_CHECK(onTop->getToggleState() && leaveBass->getToggleState(),
               "and so do the other two");

      // A change made with the window hidden -- the player's own settings, or
      // the control API -- is there when it comes back.
      window.showOrHide(false);
      proc.setSetOffAfterStore(false);
      window.showOrHide(true);
      MP_CHECK(!setOff->getToggleState(), "the box follows a change made elsewhere");
      window.showOrHide(false);
    }

    // The round trip the boxes sit on: the choice is written out and read
    // back at the next start (#591).
    proc.setSetOffAfterStore(true);
    {
      mp::MasterpieceProcessor next;
      next.prepareToPlay(48000.0, 256);
      next.loadGlobalDefaults();
      MP_CHECK(next.setOffAfterStore(), "the choice survives a restart");
      next.releaseResources();
    }

    if (had) global.replaceWithText(before);
    else global.deleteFile();
  }
};
static CombinationsWindowTogglesTest g_combinationsWindowToggles;

#endif  // MP_TEST_HAS_AUDIO
