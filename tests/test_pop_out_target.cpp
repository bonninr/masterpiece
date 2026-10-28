// The icon on a page tab that opens the page in a window of its own: on a
// touch screen it takes a target a finger can hit, the tab's full height and
// 44 pixels of its width, and the tab grows to keep its name readable.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/Ui.h"

#include <string>

using mp::test::Category;

class PopOutTargetTest final : public mp::test::Test {
public:
  PopOutTargetTest() : Test("functional.ui.pop-out-target", Category::Functional) {}

  // The icon's bounds, and the tab's width, for a tab bar 28 pixels deep.
  static void measure(bool touch, juce::Rectangle<int>& icon, int& tabWidth) {
    juce::TabbedButtonBar bar(juce::TabbedButtonBar::TabsAtTop);
    bar.addTab("Left Jamb", juce::Colours::darkgrey, -1);
    auto* extra = new juce::Component();
    extra->setSize(mp::ui::popOutIconWidth(touch), 16);
    bar.getTabButton(0)->setExtraComponent(extra, juce::TabBarButton::afterText);
    bar.setBounds(0, 0, 800, 28);
    bar.resized();
    icon = extra->getBounds();
    tabWidth = bar.getTabButton(0)->getWidth();
  }

  void run() override {
    juce::MessageManager::getInstance();
    juce::Rectangle<int> mouse, finger;
    int mouseTab = 0, fingerTab = 0;
    measure(false, mouse, mouseTab);
    measure(true, finger, fingerTab);
    MP_CHECK(mouse.getWidth() == 16, "with a mouse the icon keeps 16 pixels: " + mouse.toString().toStdString());
    MP_CHECK(finger.getWidth() == 44, "on a touch screen it takes 44 pixels: " + finger.toString().toStdString());
    MP_CHECK(finger.getHeight() >= 24, "and the tab's height: " + std::to_string(finger.getHeight()));
    MP_CHECK(fingerTab - mouseTab == 28, "the tab grows by the extra width, keeping room for its name: " +
                                              std::to_string(mouseTab) + " -> " + std::to_string(fingerTab));
  }
};
static PopOutTargetTest g_popOutTarget;

#endif  // MP_TEST_HAS_AUDIO
