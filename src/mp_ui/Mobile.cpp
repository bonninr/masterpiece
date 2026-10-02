#include "Mobile.h"

namespace mp::ui {

namespace {
// A finger is about 9 mm across; at a tablet's density that is near 48
// logical pixels, the size both platforms recommend for a touch target.
constexpr int kTouchTitleBar = 48;

// JUCE's default look, kept, with what a finger needs instead of a mouse.
class TouchLook : public juce::LookAndFeel_V4 {
public:
  void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                 int standardMenuItemHeight, int& idealWidth,
                                 int& idealHeight) override {
    juce::LookAndFeel_V4::getIdealPopupMenuItemSize(text, isSeparator, standardMenuItemHeight,
                                                    idealWidth, idealHeight);
    if (!isSeparator) {
      idealHeight = juce::jmax(idealHeight, kTouchTitleBar);
      idealWidth += 32;
    }
  }
  juce::Font getPopupMenuFont() override {
    return juce::LookAndFeel_V4::getPopupMenuFont().withHeight(20.0f);
  }
};

std::unique_ptr<TouchLook> touchLook;
}  // namespace

void installTouchLook() {
  if (!kMobile || touchLook != nullptr) return;
  touchLook = std::make_unique<TouchLook>();
  juce::LookAndFeel::setDefaultLookAndFeel(touchLook.get());
}

void removeTouchLook() {
  if (touchLook == nullptr) return;
  juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
  touchLook.reset();
}

juce::Rectangle<int> screenArea() {
  // The whole display: on a phone or tablet the console runs in kiosk mode,
  // with the system's bars hidden (see the app's main window). Each panel is
  // a window of its own there, so leaving room for the bars would mean
  // finding them for every one; hiding them gives every panel the screen.
  if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    return kMobile ? d->totalArea : d->userArea;
  return {0, 0, 1280, 800};
}

void fitToScreen(juce::DocumentWindow& window) {
  if (!kMobile) return;
  // A native title bar is a desktop thing; the platform draws none.
  window.setUsingNativeTitleBar(false);
  window.setTitleBarHeight(kTouchTitleBar);
  window.setResizable(false, false);
  window.setBounds(screenArea());
}

juce::DialogWindow* launchDialog(juce::DialogWindow::LaunchOptions& options) {
  if (kMobile) {
    options.useNativeTitleBar = false;
    options.resizable = false;
  }
  auto* dialog = options.launchAsync();
  if (dialog != nullptr) fitToScreen(*dialog);
  return dialog;
}

bool closeFrontWindow(juce::Component* mainWindow) {
  auto& desktop = juce::Desktop::getInstance();
  // The desktop lists its windows back to front.
  for (int i = desktop.getNumComponents(); --i >= 0;) {
    auto* window = dynamic_cast<juce::DocumentWindow*>(desktop.getComponent(i));
    if (window == nullptr || window == mainWindow || !window->isVisible()) continue;
    // A window the player may not leave (the load in progress): back does
    // nothing, rather than closing it or the app behind it.
    if (window->getProperties()[kStaysOpen]) return true;
    window->closeButtonPressed();
    return true;
  }
  return false;
}

}  // namespace mp::ui
