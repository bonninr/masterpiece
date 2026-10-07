// iOS window placement probe. Launched with one variant name:
//
//   kiosk-display  kiosk mode, the window at JUCE's display less its insets
//                  (how Masterpiece 0.7.6 places its console)
//   display        the same without kiosk mode
//   scene          no kiosk mode, the window at the scene's bounds, the
//                  content at the scene's safe area, both followed
//   scene-safe     no kiosk mode, the window at the scene's safe area
//   kiosk-scene    kiosk mode, then the window moved to the scene's bounds
//
// Everything is written to Documents/probe.log: the geometry JUCE, the scene
// and the window report, at start and at every change, and every touch with
// the position and the component JUCE delivered it to.
#include <juce_gui_basics/juce_gui_basics.h>

bool probeScene(juce::Rectangle<int>& whole, juce::Rectangle<int>& safe, juce::String& about);

namespace {

juce::String variant = "kiosk-display";

void log(const juce::String& s) { juce::Logger::writeToLog(s); }

juce::Rectangle<int> displayArea(bool insets) {
  if (const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    return insets ? d->safeAreaInsets.subtractedFrom(d->totalArea) : d->totalArea;
  return {0, 0, 1024, 768};
}

bool usesScene() { return variant.contains("scene"); }

// Where a window goes and where its content goes, in screen coordinates.
void placement(juce::Rectangle<int>& window, juce::Rectangle<int>& content) {
  juce::Rectangle<int> whole, safe;
  juce::String about;
  const bool scene = probeScene(whole, safe, about);
  if (variant == "scene" || variant == "kiosk-scene") {
    window = scene ? whole : displayArea(false);
    content = scene ? safe : displayArea(true);
  } else if (variant == "scene-safe") {
    window = content = scene ? safe : displayArea(true);
  } else {
    window = content = displayArea(true);
  }
}

void logGeometry(const juce::String& when, juce::Component* window) {
  juce::Rectangle<int> whole, safe;
  juce::String about;
  probeScene(whole, safe, about);
  const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
  log(when + ": scene " + whole.toString() + " safe " + safe.toString() + " (" + about + ")");
  if (d != nullptr)
    log(when + ": display total " + d->totalArea.toString() + " user " + d->userArea.toString() +
        " insets t" + juce::String(d->safeAreaInsets.getTop()) + " l" +
        juce::String(d->safeAreaInsets.getLeft()) + " b" + juce::String(d->safeAreaInsets.getBottom()) +
        " r" + juce::String(d->safeAreaInsets.getRight()) + ", orientation " +
        juce::String((int)juce::Desktop::getInstance().getCurrentOrientation()));
  if (window != nullptr)
    log(when + ": window " + window->getScreenBounds().toString() + " on desktop " +
        juce::String((int)window->isOnDesktop()) + " visible " + juce::String((int)window->isShowing()) +
        (window->getPeer() != nullptr ? " peer " + window->getPeer()->getBounds().toString() : juce::String(" no peer")));
}

struct TouchLog final : juce::MouseListener {
  void mouseDown(const juce::MouseEvent& e) override {
    log("touch down at screen " + e.getScreenPosition().toString() + " on '" +
        e.eventComponent->getName() + "' (" + e.eventComponent->getScreenBounds().toString() + ")");
  }
};

struct Panel final : juce::Component {
  juce::TextButton alpha{"Alpha"}, menu{"Menu"}, second{"Window"};
  juce::Label label;
  std::function<void()> openSecond;

  Panel() {
    setName("panel");
    for (auto* b : {&alpha, &menu, &second}) {
      b->setName(b->getButtonText());
      addAndMakeVisible(*b);
    }
    label.setText("variant " + variant, juce::dontSendNotification);
    addAndMakeVisible(label);
    alpha.onClick = [] { log("clicked Alpha"); };
    menu.onClick = [this] {
      log("clicked Menu");
      juce::PopupMenu m;
      m.addItem(1, "Item One");
      m.addItem(2, "Item Two");
      m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&menu),
                      [](int r) { log("menu chose " + juce::String(r)); });
    };
    second.onClick = [this] {
      log("clicked Window");
      if (openSecond) openSecond();
    };
  }
  void paint(juce::Graphics& g) override {
    g.fillAll(juce::Colour(0xff2a6f97));
    g.setColour(juce::Colours::yellow);
    g.drawRect(getLocalBounds(), 6);
  }
  void resized() override {
    auto r = getLocalBounds().reduced(20);
    auto row = r.removeFromTop(48);
    alpha.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(12);
    menu.setBounds(row.removeFromLeft(140));
    row.removeFromLeft(12);
    second.setBounds(row.removeFromLeft(140));
    label.setBounds(r.removeFromTop(40));
    // The far corners, so a cut-off edge shows in a screenshot.
  }
};

struct SecondWindow final : juce::DocumentWindow {
  juce::TextButton close{"Close"};
  SecondWindow() : DocumentWindow("Second", juce::Colours::darkgreen, 0) {
    setUsingNativeTitleBar(false);
    setTitleBarHeight(44);
    close.setName("Close");
    close.onClick = [this] { log("clicked Close"); setVisible(false); };
    auto* c = new juce::Component();
    c->setName("second content");
    c->addAndMakeVisible(close);
    close.setBounds(20, 20, 140, 48);
    setContentOwned(c, false);
    juce::Rectangle<int> w, content;
    placement(w, content);
    setBounds(content.reduced(40));
    setVisible(true);
    logGeometry("second", this);
  }
  void closeButtonPressed() override { setVisible(false); }
};

struct MainWindow final : juce::DocumentWindow, juce::Timer {
  Panel* panel = new Panel();
  std::unique_ptr<SecondWindow> secondWindow;
  juce::Rectangle<int> lastWindow, lastContent;

  MainWindow() : DocumentWindow("Probe", juce::Colours::black, 0) {
    setUsingNativeTitleBar(false);
    setTitleBarHeight(0);
    setResizable(false, false);
    panel->openSecond = [this] { secondWindow = std::make_unique<SecondWindow>(); };
    setContentNonOwned(panel, false);
    place();
    setVisible(true);
  }
  ~MainWindow() override { clearContentComponent(); delete panel; }

  void place() {
    juce::Rectangle<int> w, content;
    placement(w, content);
    lastWindow = w;
    lastContent = content;
    setBounds(w);
    resized();
  }
  void resized() override {
    juce::DocumentWindow::resized();
    if (!lastContent.isEmpty())
      panel->setBounds(getLocalArea(nullptr, lastContent).getIntersection(getLocalBounds()));
  }
  void timerCallback() override {
    juce::Rectangle<int> w, content;
    placement(w, content);
    if (w == lastWindow && content == lastContent && getScreenBounds() == w) return;
    logGeometry("change", this);
    if (usesScene()) place();
  }
  void closeButtonPressed() override {}
};

}  // namespace

class ProbeApp final : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return "Probe"; }
  const juce::String getApplicationVersion() override { return "0.1"; }
  void initialise(const juce::String&) override {
    for (const auto& a : getCommandLineParameterArray())
      if (a.isNotEmpty() && !a.startsWith("-")) variant = a.unquoted();
    const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    docs.getChildFile("probe.log").deleteFile();
    logger_.reset(new juce::FileLogger(docs.getChildFile("probe.log"), "probe " + variant));
    juce::Logger::setCurrentLogger(logger_.get());
    juce::Desktop::getInstance().addGlobalMouseListener(&touches_);

    win_ = std::make_unique<MainWindow>();
    logGeometry("start", win_.get());
    if (variant.startsWith("kiosk")) {
      juce::Desktop::getInstance().setKioskModeComponent(win_.get(), false);
      logGeometry("kiosk", win_.get());
    }
    if (variant == "kiosk-scene") win_->place();
    win_->startTimer(500);
    juce::Timer::callAfterDelay(3000, [this] { logGeometry("after 3 s", win_.get()); });
  }
  void shutdown() override {
    juce::Desktop::getInstance().removeGlobalMouseListener(&touches_);
    win_.reset();
    juce::Logger::setCurrentLogger(nullptr);
  }

private:
  std::unique_ptr<MainWindow> win_;
  std::unique_ptr<juce::FileLogger> logger_;
  TouchLog touches_;
};

START_JUCE_APPLICATION(ProbeApp)
