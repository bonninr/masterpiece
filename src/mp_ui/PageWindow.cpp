#include "PageWindow.h"
#include "Mobile.h"

namespace mp::ui {

PageView::PageView(MasterpieceProcessor& p, int page, int layout, bool followsShape)
    : console_(p), followsShape_(followsShape) {
  addAndMakeVisible(holder_);
  holder_.addAndMakeVisible(console_);
  console_.setLayout(layout);
  console_.rebuild();
  console_.setPage(page);
}

void PageView::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff0d0f12)); }

void PageView::mouseDown(const juce::MouseEvent& e) {
  if (e.mods.isRightButtonDown() && onMenu != nullptr) onMenu();
}

void PageView::resized() {
  // As the main window does it: a window turned on end shows the portrait
  // layout, and turned back the main one.
  if (followsShape_ && console_.layoutCount() > 1 && !getLocalBounds().isEmpty()) {
    const int tall = getHeight() > getWidth() ? 1 : 0;
    if (tall != shapeTall_) {
      shapeTall_ = tall;
      const int page = console_.currentPage();
      console_.setLayout(console_.layoutForShape(tall == 1));
      console_.setPage(page);
    }
  }
  const auto art = console_.artworkBounds();
  if (art.getWidth() <= 0 || art.getHeight() <= 0) {
    holder_.setBounds(getLocalBounds());
    console_.setTransform({});
    console_.setBounds(getLocalBounds());
    return;
  }
  // As the main window does it: the whole page, as large as the window
  // allows, centred, the spare room split evenly either side.
  const auto area = getLocalBounds();
  // A window of its own shows its page whole, however small the page is.
  const float scale = ConsoleView::fitScale(art, {}, area);
  console_.setTransform({});
  console_.setBounds(0, 0, art.getWidth(), art.getHeight());
  // The artwork is drawn from its own top-left.
  console_.setTransform(
      juce::AffineTransform::scale(scale).translated(-art.getX(), -art.getY()));
  const int w = juce::roundToInt(static_cast<float>(art.getWidth()) * scale);
  const int h = juce::roundToInt(static_cast<float>(art.getHeight()) * scale);
  holder_.setBounds(area.withSizeKeepingCentre(w, h));
}

PageWindow::PageWindow(MasterpieceProcessor& p, int page, int layout, const juce::String& organName,
                       int ownLayout)
    : juce::DocumentWindow(organName, juce::Colour(0xff15171c), juce::DocumentWindow::allButtons),
      layout_(ownLayout) {
  auto* view = new PageView(p, page, ownLayout >= 0 ? ownLayout : layout, ownLayout < 0);
  view_ = view;
  setName(view->pageName() + " - " + organName);
  setUsingNativeTitleBar(true);
  setResizable(true, true);
  setContentOwned(view, false);
  // The page's own size and shape, as far as a screen allows: a portrait
  // jamb opens tall and narrow, not in a landscape window with bars.
  const auto art = view->artworkBounds();
  if (art.getWidth() > 0 && art.getHeight() > 0) {
    const double scale =
        juce::jmin(1.0, 1600.0 / art.getWidth(), 1000.0 / art.getHeight());
    setSize(juce::roundToInt(art.getWidth() * scale),
            juce::roundToInt(art.getHeight() * scale));
  } else {
    setSize(1000, 700);
  }
  view->onMenu = [this] { showMenu(); };
  fitToScreen(*this);
}

void PageWindow::setBorderless(bool on) {
  borderless_ = on;
  // A title bar of no height: the window is the page and nothing else.
  setUsingNativeTitleBar(false);
  setTitleBarHeight(on ? 0 : 30);
  if (on) {
    // Nothing to click: the window is closed from its own menu.
    setResizable(true, true);
  }
}

void PageWindow::fitWindowToPage() {
  const auto art = view_->artworkBounds();
  if (art.getWidth() <= 0 || art.getHeight() <= 0) return;
  // The largest size the page can be drawn at and still fit the screen.
  const auto displays = juce::Desktop::getInstance().getDisplays();
  const auto* screen = displays.getDisplayForRect(getBounds());
  const auto usable = (screen != nullptr ? screen->userArea : getScreenBounds()).reduced(20);
  const float scale =
      juce::jmin(1.0f, static_cast<float>(usable.getWidth()) / static_cast<float>(art.getWidth()),
                 static_cast<float>(usable.getHeight()) / static_cast<float>(art.getHeight()));
  const int w = juce::roundToInt(static_cast<float>(art.getWidth()) * scale);
  const int h = juce::roundToInt(static_cast<float>(art.getHeight()) * scale);
  // The title bar, when there is one, is not the page's.
  const int bar = getHeight() - view_->getHeight();
  setSize(w, h + bar);
  if (screen != nullptr) {
    const auto s = screen->userArea;
    setBounds(s.getCentreX() - getWidth() / 2, s.getCentreY() - getHeight() / 2,
              getWidth(), getHeight());
  }
}

void PageWindow::showMenu() {
  juce::PopupMenu menu;
  menu.addItem(1, TRANS("Fit window to page"));
  menu.addItem(2, TRANS("Borderless"), true, borderless_);
  menu.addSeparator();
  menu.addItem(3, TRANS("Close"));
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                     [safe = juce::Component::SafePointer<PageWindow>(this)](int c) {
                       if (safe == nullptr || c == 0) return;
                       if (c == 1) safe->fitWindowToPage();
                       if (c == 2) safe->setBorderless(!safe->borderless());
                       if (c == 3) safe->closeButtonPressed();
                     });
}

void PageWindow::closeButtonPressed() {
  if (onClosed) onClosed(this);
}

void PageWindow::moved() {
  juce::DocumentWindow::moved();
  if (onPlaced) onPlaced();
}

void PageWindow::resized() {
  juce::DocumentWindow::resized();
  if (onPlaced) onPlaced();
}

}  // namespace mp::ui
