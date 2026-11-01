// A dialog appears at the size and place it opens at, and does not move once
// it shows (#637). Shown first and sized afterwards, a manual's MIDI window
// opened where the window manager put it and then jumped to its place.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_ui/Mobile.h"

using mp::test::Category;

namespace {
// A screen to put a window on. A build box has none: on Linux, with no X or
// Wayland server, a window put on the desktop crashes the program.
bool screenToShowOn() {
#if JUCE_LINUX || JUCE_BSD
  if (juce::SystemStats::getEnvironmentVariable("DISPLAY", {}).isEmpty() &&
      juce::SystemStats::getEnvironmentVariable("WAYLAND_DISPLAY", {}).isEmpty())
    return false;
#endif
  const auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
  return primary != nullptr && primary->userArea.getWidth() >= 900 && primary->userArea.getHeight() >= 600;
}
}  // namespace

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
    if (!screenToShowOn()) return;
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

// A dialog reopens where it was last left, unless that place is off the
// screens now (#637, the owner's choice of 2026-11-01).
class DialogReopensWhereLeftTest final : public mp::test::Test {
public:
  DialogReopensWhereLeftTest() : Test("functional.ui.dialog-reopens-where-left", Category::Functional) {}

  void run() override {
    using R = juce::Rectangle<int>;
    const juce::Array<R> one{R{0, 0, 1920, 1040}};
    const juce::Array<R> two{R{0, 0, 1920, 1040}, R{1920, 0, 1280, 1024}};
    MP_CHECK(mp::ui::dialogPlaceUsable({100, 80, 700, 640}, one), "a place on the screen is kept");
    MP_CHECK(mp::ui::dialogPlaceUsable({2100, 100, 700, 640}, two), "and one on the second screen");
    MP_CHECK(!mp::ui::dialogPlaceUsable({2100, 100, 700, 640}, one),
             "a place on a screen since unplugged is not");
    MP_CHECK(!mp::ui::dialogPlaceUsable({100, -20, 700, 640}, one), "nor one with its title bar above the top");
    MP_CHECK(!mp::ui::dialogPlaceUsable({1700, 80, 700, 640}, one), "nor one mostly off the right edge");
    MP_CHECK(mp::ui::dialogPlaceUsable({1300, 80, 700, 640}, one), "a little over the edge is kept");
    MP_CHECK(!mp::ui::dialogPlaceUsable({100, 80, 700, 640}, {}), "with no screen known, centred");

    if (mp::ui::kMobile) return;
    juce::MessageManager::getInstance();
    if (!screenToShowOn()) return;
    const auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto area = primary->userArea;

    const auto data =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-dialog-reopens-where-left");
    data.deleteRecursively();
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);
    const auto file = data.getChildFile("dialogs.txt");

    const auto open = [] {
      juce::DialogWindow::LaunchOptions o;
      o.dialogTitle = "Test manual - MIDI";
      auto* content = new juce::Component();
      content->setSize(500, 300);
      o.content.setOwned(content);
      o.resizable = true;
      o.useNativeTitleBar = true;
      return mp::ui::launchDialog(o);
    };

    const R left{area.getX() + 40, area.getY() + 60, 620, 400};
    file.replaceWithText("MIDI=620 400 " + juce::String(left.getX()) + " " + juce::String(left.getY()) + "\n");
    auto* d = open();
    MP_CHECK(d != nullptr && d->getBounds() == left,
             "it opens where it was left, got " + (d != nullptr ? d->getBounds().toString().toStdString() : ""));
    // Moved by the player, then closed: the new place is written down.
    const R moved = left.translated(30, 20);
    if (d != nullptr) {
      d->setBounds(moved);
      delete d;
    }
    MP_CHECK(file.loadFileAsString().trim() ==
                 "MIDI=620 400 " + juce::String(moved.getX()) + " " + juce::String(moved.getY()),
             "the place it was closed at is kept, file: " + file.loadFileAsString().toStdString());

    // Left on a screen that is gone: centred at its size.
    file.replaceWithText("MIDI=620 400 -5000 -5000\n");
    d = open();
    MP_CHECK(d != nullptr && d->getWidth() == 620 && area.contains(d->getBounds().getTopLeft()),
             "a place off every screen opens centred, got " +
                 (d != nullptr ? d->getBounds().toString().toStdString() : ""));
    delete d;

    // A line from before places were kept: its size, centred.
    file.replaceWithText("MIDI=620 400\n");
    d = open();
    MP_CHECK(d != nullptr && d->getWidth() == 620 && d->getHeight() == 400 && area.contains(d->getBounds().getTopLeft()),
             "a size-only line opens centred at its size");
    delete d;

    // View > Remember window positions off: its size, centred, though the
    // place is on the screen.
    file.replaceWithText("MIDI=620 400 " + juce::String(left.getX()) + " " + juce::String(left.getY()) + "\n");
    mp::ui::setRememberWindowPlaces(false);
    d = open();
    MP_CHECK(d != nullptr && d->getWidth() == 620 && d->getHeight() == 400 && d->getBounds() != left,
             "with the setting off it opens centred at its size, got " +
                 (d != nullptr ? d->getBounds().toString().toStdString() : ""));
    delete d;
    mp::ui::setRememberWindowPlaces(true);

    mp::setDataFolder(previous);
    data.deleteRecursively();
  }
};
static DialogReopensWhereLeftTest g_dialogReopensWhereLeft;

#endif  // MP_TEST_HAS_AUDIO
