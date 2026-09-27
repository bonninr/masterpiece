#include "CombinationsWindow.h"

#include <algorithm>

namespace mp::ui {

namespace {

constexpr int kMargin = 10;
constexpr int kRow = 30;
constexpr int kHeading = 22;
constexpr int kGap = 4;
constexpr int kSection = 12;
constexpr int kPistonW = 44;
constexpr int kPerRow = 10;
constexpr int kDivisionLabelW = 110;
constexpr int kNoteH = 48;

const juce::Colour kBackground{0xff1b1e24};
const juce::Colour kUnset{0xff262a33};
const juce::Colour kSet{0xff3a4150};
const juce::Colour kLit{0xffd9a441};
const juce::Colour kCapture{0xffb03a32};
const juce::Colour kTextDim{0xff7d8594};
const juce::Colour kText{0xffe6e9ef};

void styleHeading(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
  l.setColour(juce::Label::textColourId, juce::Colours::orange);
}

// Unset, set, or the one describing what is drawn now.
void paintPiston(juce::TextButton& b, bool set, bool lit) {
  const juce::Colour bg = lit ? kLit : (set ? kSet : kUnset);
  const juce::Colour fg = lit ? juce::Colours::black : (set ? kText : kTextDim);
  b.setColour(juce::TextButton::buttonColourId, bg);
  b.setColour(juce::TextButton::textColourOffId, fg);
}

juce::String describe(const MidiBinding& b) {
  juce::String what = b.source.kind == MidiSourceKind::Note          ? "Note "
                      : b.source.kind == MidiSourceKind::ProgramChange ? "Program "
                                                                       : "CC ";
  what << b.source.number;
  if (b.source.channel > 0) what << " ch " << b.source.channel;
  if (b.targetKind == MidiTargetKind::Setter)
    what << (b.trigger == MidiTrigger::Momentary ? "  (held)" : "  (toggles)");
  return what;
}

int pistonRows(int count) { return (count + kPerRow - 1) / kPerRow; }

} // namespace

// ------------------------------------------------------------ PistonButton

void PistonButton::mouseDown(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) {
    if (onRightClick) onRightClick();
    return;
  }
  juce::TextButton::mouseDown(e);
}

void PistonButton::mouseUp(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) return;
  juce::TextButton::mouseUp(e);
}

// ------------------------------------------------------- CombinationsPanel

CombinationsPanel::CombinationsPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(setter_);
  setter_.setClickingTogglesState(true);
  setter_.setTooltip("While Set is on, pressing a piston stores what is drawn "
                     "instead of recalling it");
  setter_.onClick = [this] { proc_.setCaptureMode(setter_.getToggleState()); };
  setter_.onRightClick = [this] {
    showMidiMenu(setter_, MidiTargetKind::Setter, kSetterTarget, true);
  };

  addAndMakeVisible(generalCancel_);
  generalCancel_.setTooltip("General cancel: push in every stop, coupler and tremulant");
  generalCancel_.onClick = [this] { proc_.pressGeneralCancel(); };
  generalCancel_.onRightClick = [this] {
    showMidiMenu(generalCancel_, MidiTargetKind::PlayerGeneralCancel, 0, false);
  };

  addAndMakeVisible(stepPrev_);
  stepPrev_.onClick = [this] { proc_.stepperPrev(); };
  stepPrev_.onRightClick = [this] {
    showMidiMenu(stepPrev_, MidiTargetKind::StepperPrev, 0, false);
  };
  addAndMakeVisible(stepNext_);
  stepNext_.onClick = [this] { proc_.stepperNext(); };
  stepNext_.onRightClick = [this] {
    showMidiMenu(stepNext_, MidiTargetKind::StepperNext, 0, false);
  };
  addAndMakeVisible(frame_);
  frame_.setJustificationType(juce::Justification::centred);
  frame_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(insertFrame_);
  insertFrame_.setTooltip("Open an empty frame here; the frames after it move up one");
  insertFrame_.onClick = [this] { proc_.stepperInsertFrame(); };
  addAndMakeVisible(deleteFrame_);
  deleteFrame_.setTooltip("Take this frame out; the frames after it move down one");
  deleteFrame_.onClick = [this] { proc_.stepperDeleteFrame(); };

  addAndMakeVisible(generalsLabel_);
  styleHeading(generalsLabel_, "Generals");
  for (auto* b : {&fewerGenerals_, &moreGenerals_, &fewerDivisionals_, &moreDivisionals_})
    addAndMakeVisible(*b);
  fewerGenerals_.setTooltip("Show fewer generals");
  moreGenerals_.setTooltip("Show more generals");
  fewerDivisionals_.setTooltip("Show fewer divisionals on each division");
  moreDivisionals_.setTooltip("Show more divisionals on each division");
  auto changeCounts = [this](int dg, int dd) {
    const auto& pc = proc_.playerCombinations();
    const int g = dg == 0 ? pc.generalCount() : pc.generalCount() + dg * kPerRow;
    proc_.setPlayerPistonCounts(
        std::clamp(g, kPerRow, PlayerCombinations::kMaxGenerals),
        pc.divisionalCount() + dd);
    rebuild();
  };
  fewerGenerals_.onClick = [changeCounts] { changeCounts(-1, 0); };
  moreGenerals_.onClick = [changeCounts] { changeCounts(1, 0); };
  fewerDivisionals_.onClick = [changeCounts] { changeCounts(0, -1); };
  moreDivisionals_.onClick = [changeCounts] { changeCounts(0, 1); };

  addAndMakeVisible(divisionalsLabel_);
  styleHeading(divisionalsLabel_, "Divisionals");

  addAndMakeVisible(setLabel_);
  styleHeading(setLabel_, "Set in use");
  addAndMakeVisible(setBox_);
  setBox_.onChange = [this] {
    const int idx = setBox_.getSelectedId() - 1;
    if (idx < 0 || idx >= static_cast<int>(setNames_.size())) return;
    const std::string want = setNames_[static_cast<size_t>(idx)];
    if (want == proc_.combinationSetName()) return;
    // Switching saves the set being left first, so nothing just captured is
    // lost by changing books.
    proc_.switchCombinationSet(want);
    proc_.saveSettings();
  };
  addAndMakeVisible(setNew_);
  setNew_.onClick = [this] {
    setPrompt_ = std::make_unique<juce::AlertWindow>(
        "Save as new set", "A name for this registration set:",
        juce::MessageBoxIconType::NoIcon);
    setPrompt_->addTextEditor("name", "", "Name");
    setPrompt_->addButton("Save", 1);
    setPrompt_->addButton("Cancel", 0);
    setPrompt_->enterModalState(
        true, juce::ModalCallbackFunction::create([this](int result) {
          const juce::String name =
              result == 1 && setPrompt_ != nullptr
                  ? setPrompt_->getTextEditorContents("name").trim()
                  : juce::String();
          setPrompt_.reset();
          if (name.isEmpty()) return;
          // The player carries on in the new set, rather than having made a
          // backup and kept editing the old one.
          proc_.copyCombinationSetTo(name.toStdString());
          proc_.switchCombinationSet(name.toStdString());
          proc_.saveSettings();
          refreshSets();
        }));
  };
  addAndMakeVisible(setDelete_);
  setDelete_.onClick = [this] {
    const std::string live = proc_.combinationSetName();
    if (live.empty()) return;  // the default set cannot be deleted
    proc_.switchCombinationSet("");
    proc_.deleteCombinationSet(live);
    proc_.saveSettings();
    refreshSets();
  };

  addAndMakeVisible(note_);
  note_.setColour(juce::Label::textColourId, kTextDim);
  note_.setFont(juce::Font(juce::FontOptions(13.0f)));
  note_.setJustificationType(juce::Justification::topLeft);

  rebuild();
  startTimerHz(10);
}

CombinationsPanel::~CombinationsPanel() { stopTimer(); }

void CombinationsPanel::rebuild() {
  const auto& pc = proc_.playerCombinations();

  generals_.clear();
  for (int n = 1; n <= pc.generalCount(); ++n) {
    auto b = std::make_unique<PistonButton>(juce::String(n));
    auto* raw = b.get();
    b->onClick = [this, n] { proc_.pressGeneral(n); };
    b->onRightClick = [this, raw, n] {
      showMidiMenu(*raw, MidiTargetKind::PlayerGeneral, n, false);
    };
    addAndMakeVisible(*b);
    generals_.push_back(std::move(b));
  }

  divisions_.clear();
  for (const auto& d : pc.divisions()) {
    DivisionRow row;
    row.divisionId = d.divisionId;
    row.name = std::make_unique<juce::Label>();
    row.name->setText(juce::String(d.name), juce::dontSendNotification);
    row.name->setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(*row.name);
    const Id div = d.divisionId;
    for (int n = 1; n <= pc.divisionalCount(); ++n) {
      auto b = std::make_unique<PistonButton>(juce::String(n));
      auto* raw = b.get();
      b->onClick = [this, div, n] { proc_.pressDivisional(div, n); };
      b->onRightClick = [this, raw, div, n] {
        showMidiMenu(*raw, MidiTargetKind::PlayerDivisional,
                     playerDivisionalTarget(div, n), false);
      };
      addAndMakeVisible(*b);
      row.pistons.push_back(std::move(b));
    }
    row.cancel = std::make_unique<PistonButton>("0");
    row.cancel->setTooltip("Cancel this division");
    auto* raw = row.cancel.get();
    row.cancel->onClick = [this, div] { proc_.pressDivisionalCancel(div); };
    row.cancel->onRightClick = [this, raw, div] {
      showMidiMenu(*raw, MidiTargetKind::PlayerDivisionalCancel, div, false);
    };
    addAndMakeVisible(*row.cancel);
    divisions_.push_back(std::move(row));
  }
  divisionalsLabel_.setVisible(!divisions_.empty());
  fewerDivisionals_.setVisible(!divisions_.empty());
  moreDivisionals_.setVisible(!divisions_.empty());

  refreshSets();
  refreshState();
  setSize(getWidth() > 0 ? getWidth() : 620, preferredHeight(getWidth() > 0 ? getWidth() : 620));
  resized();
}

int CombinationsPanel::preferredHeight(int /*width*/) const {
  const auto& pc = proc_.playerCombinations();
  int h = kMargin + kRow + kSection;
  h += kHeading + kGap + pistonRows(pc.generalCount()) * (kRow + kGap) + kSection;
  if (!divisions_.empty()) h += kHeading + kGap + static_cast<int>(divisions_.size()) * (kRow + kGap) + kSection;
  h += kRow + kGap + kNoteH + kMargin;
  return h;
}

void CombinationsPanel::resized() {
  auto r = getLocalBounds().reduced(kMargin, 0);
  r.removeFromTop(kMargin);

  auto row = r.removeFromTop(kRow);
  setter_.setBounds(row.removeFromLeft(56).reduced(1));
  row.removeFromLeft(kGap);
  generalCancel_.setBounds(row.removeFromLeft(56).reduced(1));
  row.removeFromLeft(24);
  stepPrev_.setBounds(row.removeFromLeft(34).reduced(1));
  frame_.setBounds(row.removeFromLeft(120));
  stepNext_.setBounds(row.removeFromLeft(34).reduced(1));
  row.removeFromLeft(kGap * 2);
  insertFrame_.setBounds(row.removeFromLeft(64).reduced(1));
  deleteFrame_.setBounds(row.removeFromLeft(64).reduced(1));
  r.removeFromTop(kSection);

  row = r.removeFromTop(kHeading);
  generalsLabel_.setBounds(row.removeFromLeft(kDivisionLabelW));
  fewerGenerals_.setBounds(row.removeFromLeft(26).reduced(1));
  moreGenerals_.setBounds(row.removeFromLeft(26).reduced(1));
  r.removeFromTop(kGap);
  for (size_t i = 0; i < generals_.size(); ++i) {
    const int col = static_cast<int>(i) % kPerRow;
    const int line = static_cast<int>(i) / kPerRow;
    generals_[i]->setBounds(r.getX() + kDivisionLabelW + col * (kPistonW + kGap),
                            r.getY() + line * (kRow + kGap), kPistonW, kRow);
  }
  r.removeFromTop(pistonRows(static_cast<int>(generals_.size())) * (kRow + kGap));
  r.removeFromTop(kSection);

  if (!divisions_.empty()) {
    row = r.removeFromTop(kHeading);
    divisionalsLabel_.setBounds(row.removeFromLeft(kDivisionLabelW));
    fewerDivisionals_.setBounds(row.removeFromLeft(26).reduced(1));
    moreDivisionals_.setBounds(row.removeFromLeft(26).reduced(1));
    r.removeFromTop(kGap);
    for (auto& d : divisions_) {
      row = r.removeFromTop(kRow);
      r.removeFromTop(kGap);
      d.name->setBounds(row.removeFromLeft(kDivisionLabelW));
      for (auto& b : d.pistons) {
        b->setBounds(row.removeFromLeft(kPistonW));
        row.removeFromLeft(kGap);
      }
      row.removeFromLeft(kGap * 2);
      d.cancel->setBounds(row.removeFromLeft(kPistonW));
    }
    r.removeFromTop(kSection);
  }

  row = r.removeFromTop(kRow);
  setLabel_.setBounds(row.removeFromLeft(kDivisionLabelW));
  setBox_.setBounds(row.removeFromLeft(200).reduced(1));
  row.removeFromLeft(kGap);
  setNew_.setBounds(row.removeFromLeft(90).reduced(1));
  setDelete_.setBounds(row.removeFromLeft(70).reduced(1));
  r.removeFromTop(kGap);
  note_.setBounds(r.removeFromTop(kNoteH));
}

void CombinationsPanel::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void CombinationsPanel::timerCallback() { refreshState(); }

void CombinationsPanel::refreshState() {
  const auto& pc = proc_.playerCombinations();
  // A count changed elsewhere -- a set switched to, or a file loaded -- means
  // the rows are out of date, not just their colours.
  if (static_cast<int>(generals_.size()) != pc.generalCount() ||
      divisions_.size() != pc.divisions().size() ||
      (!divisions_.empty() &&
       static_cast<int>(divisions_.front().pistons.size()) != pc.divisionalCount())) {
    rebuild();
    return;
  }

  const bool capturing = proc_.captureMode();
  setter_.setToggleState(capturing, juce::dontSendNotification);
  setter_.setColour(juce::TextButton::buttonOnColourId, kCapture);

  for (size_t i = 0; i < generals_.size(); ++i) {
    const int n = static_cast<int>(i) + 1;
    paintPiston(*generals_[i], pc.generalSet(n), pc.litGeneral() == n);
  }
  for (auto& d : divisions_) {
    const int lit = pc.litDivisional(d.divisionId);
    for (size_t i = 0; i < d.pistons.size(); ++i) {
      const int n = static_cast<int>(i) + 1;
      paintPiston(*d.pistons[i], pc.divisionalSet(d.divisionId, n), lit == n);
    }
  }

  const int last = pc.lastUsedFrame();
  frame_.setText(pc.frame() == 0 ? (last == 0 ? juce::String("No frames")
                                              : "- of " + juce::String(last))
                                 : "Frame " + juce::String(pc.frame()) + " of " +
                                       juce::String(std::max(last, pc.frame())),
                 juce::dontSendNotification);
  stepPrev_.setEnabled(pc.frame() > 1);
  stepNext_.setEnabled(capturing || pc.frame() < last);
  deleteFrame_.setEnabled(pc.frame() >= 1 && last > 0);
  setDelete_.setEnabled(!proc_.combinationSetName().empty());

  const auto& map = proc_.midiMap();
  note_.setText(map.learning()
                    ? "Now press the control on your console that should do "
                      "this. Right-click the piston again to cancel."
                    : "Hold Set and press a piston to store what is drawn. "
                      "A piston never set does nothing. Right-click any piston "
                      "to map it to your console.",
                juce::dontSendNotification);
}

void CombinationsPanel::refreshSets() {
  setNames_ = proc_.combinationSets();
  if (std::find(setNames_.begin(), setNames_.end(), std::string()) == setNames_.end())
    setNames_.insert(setNames_.begin(), std::string());
  setBox_.clear(juce::dontSendNotification);
  int selected = 1;
  for (size_t i = 0; i < setNames_.size(); ++i) {
    const auto& n = setNames_[i];
    setBox_.addItem(n.empty() ? "(default)" : juce::String(n), static_cast<int>(i) + 1);
    if (n == proc_.combinationSetName()) selected = static_cast<int>(i) + 1;
  }
  setBox_.setSelectedId(selected, juce::dontSendNotification);
}

void CombinationsPanel::showMidiMenu(PistonButton& b, MidiTargetKind kind,
                                     Id targetId, bool offerHold) {
  auto& map = proc_.midiMap();
  // A right-click while a learn is armed backs out of it.
  if (map.learning()) {
    map.cancelLearn();
    return;
  }
  juce::PopupMenu menu;
  const auto existing = map.bindingsFor(kind, targetId);
  if (existing.empty()) menu.addSectionHeader("Not mapped");
  for (const MidiBinding* binding : existing) menu.addSectionHeader(describe(*binding));
  menu.addSeparator();
  if (offerHold) {
    menu.addItem(1, "Learn: held while pressed");
    menu.addItem(2, "Learn: toggles");
  } else {
    menu.addItem(1, "Learn");
  }
  menu.addSeparator();
  menu.addItem(3, "Clear mapping", !existing.empty());

  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(&b),
      [this, kind, targetId](int choice) {
        auto& m = proc_.midiMap();
        switch (choice) {
          case 1:
            m.beginLearnAs(kind, targetId, MidiTrigger::Momentary);
            break;
          case 2:
            m.beginLearnAs(kind, targetId, MidiTrigger::Toggle);
            break;
          case 3:
            m.unbindTarget(kind, targetId);
            proc_.saveMidiMap();
            break;
          default:
            break;
        }
      });
}

// ------------------------------------------------------ CombinationsWindow

CombinationsWindow::CombinationsWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow("Combinations", kBackground,
                           juce::DocumentWindow::closeButton),
      proc_(p),
      panel_(std::make_unique<CombinationsPanel>(p)) {
  setUsingNativeTitleBar(true);
  setResizable(true, false);
  viewport_.setViewedComponent(panel_.get(), false);
  viewport_.setScrollBarsShown(true, false);
  viewport_.setScrollBarThickness(10);
  // The same slim, dark bar as the stop list: JUCE's default blue thumb reads
  // as a control beside an organ console.
  auto& bar = viewport_.getVerticalScrollBar();
  bar.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff4c5464));
  bar.setColour(juce::ScrollBar::trackColourId, kBackground);
  bar.setColour(juce::ScrollBar::backgroundColourId, kBackground);
  setContentNonOwned(&viewport_, false);
  setSize(660, std::min(panel_->preferredHeight(660), 640));
  placing_ = false;
}

void CombinationsWindow::resized() {
  juce::DocumentWindow::resized();
  const int w = viewport_.getMaximumVisibleWidth();
  if (w > 0) panel_->setSize(w, panel_->preferredHeight(w));
  remember();
}

void CombinationsWindow::moved() {
  juce::DocumentWindow::moved();
  remember();
}

void CombinationsWindow::closeButtonPressed() { showOrHide(false); }

void CombinationsWindow::showOrHide(bool show) {
  open_ = show;
  setVisible(show);
  if (show) toFront(true);
  remember();
}

void CombinationsWindow::place(juce::Rectangle<int> besideThis, bool openFirstTime) {
  const auto& saved = proc_.combinationsWindowPlace();
  placing_ = true;
  if (saved.w > 0) {
    juce::Rectangle<int> r(saved.x, saved.y, saved.w, saved.h);
    // Only where a display still is: a window restored onto a monitor that
    // has since been unplugged is a window nobody can reach.
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    if (displays.getDisplayForRect(r) != nullptr &&
        displays.getTotalBounds(true).intersects(r))
      setBounds(r);
    else
      centreWithSize(saved.w, saved.h);
  } else if (!besideThis.isEmpty()) {
    // Tall enough for this organ's divisions, as far as the screen allows,
    // and along the bottom right of the main window, where it covers the
    // least of a console.
    const int w = getWidth();
    setSize(w, juce::jmin(panel_->preferredHeight(w) + getTitleBarHeight() + 8,
                          besideThis.getHeight() - 40));
    setBounds(besideThis.getRight() - getWidth() - 20,
              besideThis.getBottom() - getHeight() - 20, getWidth(), getHeight());
  }
  open_ = saved.w > 0 ? saved.open : openFirstTime;
  setVisible(open_);
  placing_ = false;
  remember();
}

void CombinationsWindow::remember() {
  if (placing_ || getWidth() <= 0) return;
  MasterpieceProcessor::WindowPlace p;
  const auto b = getBounds();
  p.x = b.getX();
  p.y = b.getY();
  p.w = b.getWidth();
  p.h = b.getHeight();
  p.open = open_;
  const auto& old = proc_.combinationsWindowPlace();
  if (old.x == p.x && old.y == p.y && old.w == p.w && old.h == p.h && old.open == p.open)
    return;
  proc_.setCombinationsWindowPlace(p);
}

} // namespace mp::ui
