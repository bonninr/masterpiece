#include "PageWindow.h"
#include "Mobile.h"

namespace mp::ui {

PageView::PageView(MasterpieceProcessor& p, int page, int layout) : console_(p) {
  addAndMakeVisible(holder_);
  holder_.addAndMakeVisible(console_);
  console_.setLayout(layout);
  console_.rebuild();
  console_.setPage(page);
}

void PageView::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff0d0f12)); }

void PageView::resized() {
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
  const float scale = juce::jmin(static_cast<float>(area.getWidth()) / static_cast<float>(art.getRight()),
                                 static_cast<float>(area.getHeight()) / static_cast<float>(art.getBottom()));
  console_.setTransform({});
  console_.setBounds(0, 0, art.getRight(), art.getBottom());
  console_.setTransform(juce::AffineTransform::scale(scale));
  const int w = juce::roundToInt(static_cast<float>(art.getRight()) * scale);
  const int h = juce::roundToInt(static_cast<float>(art.getBottom()) * scale);
  holder_.setBounds(area.withSizeKeepingCentre(w, h));
}

PageWindow::PageWindow(MasterpieceProcessor& p, int page, int layout, const juce::String& organName,
                       int ownLayout)
    : juce::DocumentWindow(organName, juce::Colour(0xff15171c), juce::DocumentWindow::allButtons),
      layout_(ownLayout) {
  auto* view = new PageView(p, page, ownLayout >= 0 ? ownLayout : layout);
  view_ = view;
  setName(view->pageName() + " - " + organName);
  setUsingNativeTitleBar(true);
  setResizable(true, true);
  setContentOwned(view, false);
  // The page's own size and shape, as far as a screen allows: a portrait
  // jamb opens tall and narrow, not in a landscape window with bars.
  const auto art = view->artworkBounds();
  if (art.getRight() > 0 && art.getBottom() > 0) {
    const double scale = juce::jmin(1.0, 1600.0 / art.getRight(), 1000.0 / art.getBottom());
    setSize(juce::roundToInt(art.getRight() * scale), juce::roundToInt(art.getBottom() * scale));
  } else {
    setSize(1000, 700);
  }
  fitToScreen(*this);
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
