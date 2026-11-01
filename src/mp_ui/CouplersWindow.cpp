#include "CouplersWindow.h"

#include "Console.h"
#include "Mobile.h"
#include "Ui.h"

namespace mp::ui {

namespace {
const juce::Colour kBack(0xff15171c);
constexpr int kButtonW = 112, kButtonH = 40, kGap = 6, kHeaderH = 24, kMargin = 10;
}  // namespace

CouplersPanel::CouplersPanel(MasterpieceProcessor& p) : proc_(p) {
  extra_.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffb9c2d0));
  extra_.setTooltip(TRANS("16', 8' and 4' couplers between the divisions, and on each division. "
                          "Turning them on or off loads the organ again."));
  extra_.onClick = [this] {
    // Put back until the organ has loaded again with them: rebuild() reads
    // the setting then.
    extra_.setToggleState(proc_.programCouplers(), juce::dontSendNotification);
    if (onExtraCouplers) onExtraCouplers();
  };
  addAndMakeVisible(extra_);
  note_.setColour(juce::Label::textColourId, juce::Colour(0xff8a93a3));
  note_.setJustificationType(juce::Justification::topLeft);
  addChildComponent(note_);
  rebuild();
  startTimer(100);
}

CouplersPanel::~CouplersPanel() { stopTimer(); }

void CouplersPanel::rebuild() {
  for (auto& e : buttons_) removeChildComponent(e.button.get());
  for (auto& h : headers_) removeChildComponent(h.get());
  buttons_.clear();
  headers_.clear();
  extra_.setToggleState(proc_.programCouplers(), juce::dontSendNotification);

  for (const auto& section : panelSections(proc_)) {
    bool any = false;
    for (const auto& el : section.elements) {
      if (!el.coupler) continue;
      if (!any) {
        auto header = std::make_unique<juce::Label>();
        header->setText(juce::String::fromUTF8(section.title.c_str()), juce::dontSendNotification);
        header->setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
        header->setColour(juce::Label::textColourId, section.kind == PanelSection::Kind::Program
                                                         ? juce::Colours::orange
                                                         : juce::Colour(0xffd8dee9));
        addAndMakeVisible(*header);
        headers_.push_back(std::move(header));
        any = true;
      }
      Entry e;
      e.element = el;
      e.section = headers_.size() - 1;
      juce::String label = juce::String::fromUTF8(el.name.c_str());
      if (!el.footage.empty()) label << " " << juce::String::fromUTF8(el.footage.c_str());
      e.button = std::make_unique<StopButton>(label);
      e.button->setClickingTogglesState(true);
      e.button->setStopName(label, headers_.back()->getText());
      e.button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2f3a));
      e.button->setColour(juce::TextButton::textColourOffId, juce::Colour(0xffb9c2d0));
      e.button->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffeee3c6));
      e.button->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1b1e24));
      const PanelElement element = el;
      auto* raw = e.button.get();
      e.button->onClick = [this, element, raw] {
        if (element.kind == PanelElement::Kind::Stop)
          proc_.setStopEngaged(element.id, raw->getToggleState());
        else
          proc_.setSwitchEngaged(element.id, raw->getToggleState());
      };
      // The mapping is the knob's, as on the console and a panel page (#555).
      e.button->onMidiMenu = [this, element](StopButton& b) {
        const Id target = element.kind == PanelElement::Kind::Stop ? proc_.stopKnobFor(element.id)
                                                                    : proc_.playerSwitchFor(element.id);
        if (target != 0) showSwitchMidiMenu(proc_, target, b.getScreenBounds(), nullptr);
      };
      addAndMakeVisible(*e.button);
      buttons_.push_back(std::move(e));
    }
  }
  note_.setText(proc_.organModel().divisions.empty()
                    ? TRANS("No organ loaded.")
                    : TRANS("This organ draws no couplers of its own. The extra couplers add them."),
                juce::dontSendNotification);
  note_.setVisible(buttons_.empty());
  refresh();
  setSize(getWidth(), preferredHeight(getWidth()));
  resized();
}

int CouplersPanel::layOut(int width, bool place) {
  int y = kMargin;
  if (place) extra_.setBounds(kMargin, y, juce::jmax(0, width - 2 * kMargin), 24);
  y += 24 + kGap;
  if (buttons_.empty()) {
    if (place) note_.setBounds(kMargin, y, juce::jmax(0, width - 2 * kMargin), 40);
    return y + 40 + kMargin;
  }
  const int columns = juce::jmax(1, (width - 2 * kMargin + kGap) / (kButtonW + kGap));
  size_t section = SIZE_MAX;
  int column = 0;
  for (auto& e : buttons_) {
    if (e.section != section) {
      if (section != SIZE_MAX) y += kButtonH + kGap;
      section = e.section;
      if (place) headers_[section]->setBounds(kMargin - 4, y, juce::jmax(0, width - 2 * kMargin), kHeaderH);
      y += kHeaderH;
      column = 0;
    } else if (column == columns) {
      y += kButtonH + kGap;
      column = 0;
    }
    if (place) e.button->setBounds(kMargin + column * (kButtonW + kGap), y, kButtonW, kButtonH);
    ++column;
  }
  return y + kButtonH + kMargin;
}

int CouplersPanel::preferredHeight(int width) const {
  return const_cast<CouplersPanel*>(this)->layOut(juce::jmax(width, kButtonW + 2 * kMargin), false);
}

void CouplersPanel::resized() { layOut(getWidth(), true); }

void CouplersPanel::paint(juce::Graphics& g) { g.fillAll(kBack); }

void CouplersPanel::timerCallback() { refresh(); }

void CouplersPanel::refresh() {
  for (auto& e : buttons_) {
    const bool on = e.element.kind == PanelElement::Kind::Stop ? proc_.shownStopEngaged(e.element.id)
                                                                : proc_.shownSwitchEngaged(e.element.id);
    if (e.button->getToggleState() != on) e.button->setToggleState(on, juce::dontSendNotification);
  }
}

CouplersWindow::CouplersWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow(TRANS("Couplers"), kBack, juce::DocumentWindow::closeButton),
      panel_(std::make_unique<CouplersPanel>(p)) {
  setUsingNativeTitleBar(true);
  setResizable(true, false);
  viewport_.setViewedComponent(panel_.get(), false);
  viewport_.setScrollBarsShown(true, false);
  viewport_.setScrollBarThickness(10);
  auto& bar = viewport_.getVerticalScrollBar();
  bar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff4c5464));
  bar.setColour(juce::ScrollBar::trackColourId, kBack);
  bar.setColour(juce::ScrollBar::backgroundColourId, kBack);
  setContentNonOwned(&viewport_, false);
  const int w = 4 * (kButtonW + kGap) + 2 * kMargin + 10;
  setSize(w, juce::jlimit(160, 600, panel_->preferredHeight(w - 10)));
  fitToScreen(*this);
}

CouplersWindow::~CouplersWindow() { clearContentComponent(); }

void CouplersWindow::closeButtonPressed() { showFloating(*this, false, false); }

void CouplersWindow::resized() {
  juce::DocumentWindow::resized();
  const int w = viewport_.getMaximumVisibleWidth();
  if (w > 0) panel_->setSize(w, panel_->preferredHeight(w));
}

}  // namespace mp::ui
