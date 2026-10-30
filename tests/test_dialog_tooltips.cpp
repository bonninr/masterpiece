// A dialog's tooltips are drawn inside it. JUCE raises a modal dialog over
// any other window brought to the front, so a tip shown by the app's desktop
// tooltip window came up behind the dialog it belonged to.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/Mobile.h"

#include <memory>

using mp::test::Category;

class DialogTooltipsTest final : public mp::test::Test {
public:
  DialogTooltipsTest() : Test("functional.ui.dialog-tooltips", Category::Functional) {}

  static int tooltipWindowsIn(juce::Component& c) {
    int n = 0;
    for (auto* child : c.getChildren())
      if (dynamic_cast<juce::TooltipWindow*>(child) != nullptr) ++n;
    return n;
  }

  // JUCE shows no tip while the program is in the background, as it is on a
  // build machine, so these check which tooltip window a control belongs to.
  void run() override {
    juce::MessageManager::getInstance();
    mp::ui::DesktopTooltips desktop;

    juce::TextButton onConsole("Set");
    onConsole.setTooltip("Stores the drawn stops");
    auto content = std::make_unique<juce::Component>();
    juce::TextButton inDialog("Choose...");
    inDialog.setTooltip("The folder of the organs");
    content->addAndMakeVisible(inDialog);
    MP_CHECK(!mp::ui::inOwnTooltips(inDialog), "before: the desktop tooltips served the dialog's controls too");

    mp::ui::giveOwnTooltips(*content);
    MP_CHECK(tooltipWindowsIn(*content) == 1, "the dialog's content holds a tooltip window of its own");
    MP_CHECK(mp::ui::inOwnTooltips(inDialog), "which serves the dialog's controls");
    MP_CHECK(desktop.getTipFor(inDialog).isEmpty(), "the desktop tooltips leave them to it");
    MP_CHECK(!mp::ui::inOwnTooltips(onConsole), "and still serve the console's");

    mp::ui::giveOwnTooltips(*content);
    MP_CHECK(tooltipWindowsIn(*content) == 1, "a second call adds no second tooltip window");

    content->removeChildComponent(&inDialog);
    content.reset();  // the dialog's tooltip window goes with it
    MP_CHECK(!mp::ui::inOwnTooltips(inDialog), "a control taken out of the dialog is the desktop's again");
  }
};
static DialogTooltipsTest g_dialogTooltips;

#endif  // MP_TEST_HAS_AUDIO
