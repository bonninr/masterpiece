// A dialog appears at the size and place it opens at, and does not move once
// it shows (#637). Shown first and sized afterwards, a manual's MIDI window
// opened where the window manager put it and then jumped to its place.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_ui/Mobile.h"

using mp::test::Category;

namespace {
// The dialog's bounds at the moment it becomes visible, and every move or
// resize while it is visible.
class Watch final : public juce::ComponentListener {
public:
  juce::Rectangle<int> shownAt;
  int movesWhileShown = 0;

  void componentVisibilityChanged(juce::Component& c) override {
    if (c.isVisible() && shownAt.isEmpty()) shownAt = c.getBounds();
  }
  void componentMovedOrResized(juce::Component& c, bool, bool) override {
    if (c.isVisible()) ++movesWhileShown;
  }
};

// The content puts the watch on its window as soon as it is in one, which is
// before the window is shown.
class Content final : public juce::Component {
public:
  explicit Content(Watch& w) : watch_(w) { setSize(500, 300); }
  void parentHierarchyChanged() override {
    auto* top = getTopLevelComponent();
    if (top != this && top != watched_) {
      top->addComponentListener(&watch_);
      watched_ = top;
    }
  }

private:
  Watch& watch_;
  juce::Component* watched_ = nullptr;
};
}  // namespace

class DialogOpensInPlaceTest final : public mp::test::Test {
public:
  DialogOpensInPlaceTest() : Test("functional.ui.dialog-opens-in-place", Category::Functional) {}

  void run() override {
    if (mp::ui::kMobile) return;
    juce::MessageManager::getInstance();
    // The remembered sizes are the player's: this test keeps its own.
    const auto data =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-dialog-opens-in-place");
    data.deleteRecursively();
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);
    data.getChildFile("dialogs.txt").replaceWithText("MIDI=700 450\n");

    Watch watch;
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = "Test manual - MIDI";
    o.content.setOwned(new Content(watch));
    o.resizable = true;
    o.useNativeTitleBar = true;
    juce::Component::SafePointer<juce::DialogWindow> dialog = mp::ui::launchDialog(o);

    MP_CHECK(dialog != nullptr && dialog->isVisible(), "the dialog is open");
    MP_CHECK(watch.shownAt.getWidth() == 700 && watch.shownAt.getHeight() == 450,
             "it appears at its remembered size, got " + watch.shownAt.toString().toStdString());
    MP_CHECK(dialog != nullptr && dialog->getBounds() == watch.shownAt,
             "and where it appeared, got " + (dialog != nullptr ? dialog->getBounds().toString().toStdString() : "") +
                 " after " + watch.shownAt.toString().toStdString());
    MP_CHECK(watch.movesWhileShown == 0,
             "it was not moved once shown, moved " + std::to_string(watch.movesWhileShown) + " time(s)");

    if (dialog != nullptr) {
      dialog->removeComponentListener(&watch);
      delete dialog.getComponent();
    }
    mp::setDataFolder(previous);
    data.deleteRecursively();
  }
};
static DialogOpensInPlaceTest g_dialogOpensInPlace;

#endif  // MP_TEST_HAS_AUDIO
