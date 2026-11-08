#include "LearnInTurnWindow.h"

#include <set>

#include "MidiEventDialog.h"
#include "Mobile.h"
#include "Panels.h"

namespace mp::ui {

namespace {
const juce::Colour kBack(0xff15171c);
const juce::Colour kText(0xffb9c2d0);
const juce::Colour kDim(0xff8a93a3);
const juce::Colour kArmed(0xff2d4f7c);
const juce::Colour kWarn(0xffe0a040);
constexpr int kMargin = 10, kRowH = 24, kButtonH = 28;
}  // namespace

LearnInTurnPanel::LearnInTurnPanel(MasterpieceProcessor& p) : proc_(p) {
  prompt_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
  prompt_.setColour(juce::Label::textColourId, juce::Colours::white);
  prompt_.setJustificationType(juce::Justification::centredLeft);
  addAndMakeVisible(prompt_);
  note_.setColour(juce::Label::textColourId, kDim);
  note_.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(note_);

  list_.setModel(this);
  list_.setRowHeight(kRowH);
  list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff1d2027));
  list_.setOutlineThickness(0);
  addAndMakeVisible(list_);

  start_.onClick = [this] {
    const int selected = list_.getSelectedRow();
    start(selected >= 0 ? selected : 0);
  };
  skip_.onClick = [this] { skip(); };
  back_.onClick = [this] { back(); };
  stop_.onClick = [this] { stop(); };
  for (auto* b : {&start_, &skip_, &back_, &stop_}) addAndMakeVisible(*b);

  for (auto* t : {&pistons_, &unmappedOnly_}) {
    t->setColour(juce::ToggleButton::textColourId, kText);
    addAndMakeVisible(*t);
  }
  pistons_.setTooltip(TRANS("The organ's pistons follow its tabs in the list"));
  unmappedOnly_.setTooltip(TRANS("Tabs that already have a MIDI mapping are passed over"));
  pistons_.onClick = [this] {
    stop();
    rebuild();
  };
  rebuild();
  startTimer(50);
}

LearnInTurnPanel::~LearnInTurnPanel() {
  stopTimer();
  // An armed row is not left waiting for a press once the window is gone.
  if (current_ >= 0) stop();
}

void LearnInTurnPanel::rebuild() {
  // The rows of another organ, or of another list: nothing stays armed.
  stop();
  learned_ = 0;
  rows_.clear();
  const auto& m = proc_.organModel();
  std::set<Id> seen;
  auto add = [&](const std::string& section, const PanelElement& e, Id sw, bool piston) {
    if (sw == 0) return;
    sw = proc_.midiSwitchFor(sw);
    if (!seen.insert(sw).second) return;
    Row r;
    r.section = section;
    r.name = e.footage.empty() ? e.name : e.name + " " + e.footage;
    r.switchId = sw;
    const auto it = m.switches.find(sw);
    r.latching = it == m.switches.end() || it->second.latching;
    r.piston = piston;
    rows_.push_back(std::move(r));
  };
  const auto sections = panelSections(proc_);
  for (const auto& s : sections) {
    if (s.kind == PanelSection::Kind::Controls || s.kind == PanelSection::Kind::Presets) continue;
    for (const auto& e : s.elements) {
      // The knob the console's own mapping menu learns on (Panels.cpp): a
      // stop through its drawstop, a coupler or tremulant through the switch
      // a player moves.
      if (e.kind == PanelElement::Kind::Stop) add(s.title, e, proc_.stopKnobFor(e.id), false);
      else if (e.kind == PanelElement::Kind::Switch) add(s.title, e, proc_.playerSwitchFor(e.id), false);
    }
  }
  // The pistons last: a console's tabs come first, and most players map
  // them in one go.
  if (pistons_.getToggleState())
    for (const auto& s : sections)
      for (const auto& e : s.elements)
        if (e.kind == PanelElement::Kind::Piston) add(s.title, e, e.id, true);
  list_.updateContent();
  list_.repaint();
  showState();
}

bool LearnInTurnPanel::mapped(const Row& r) const {
  return !proc_.midiMap().bindingsFor(MidiTargetKind::Switch, r.switchId).empty();
}

juce::String LearnInTurnPanel::rowLabel(const Row& r) const {
  const auto name = juce::String::fromUTF8(r.name.c_str());
  const auto section = juce::String::fromUTF8(r.section.c_str());
  return section.isEmpty() ? name : name + " (" + section + ")";
}

void LearnInTurnPanel::start(int from) {
  if (rows_.empty()) return;
  if (current_ < 0) {
    learned_ = 0;
    taken_.clear();
  }
  release();
  proc_.midiMap().cancelLearn();
  current_ = juce::jlimit(0, rowCount() - 1, from);
  refused_.clear();
  arm();
}

void LearnInTurnPanel::arm() {
  auto& map = proc_.midiMap();
  if (unmappedOnly_.getToggleState())
    while (current_ >= 0 && current_ < rowCount() && mapped(rows_[static_cast<size_t>(current_)])) ++current_;
  if (current_ >= rowCount()) {
    finish();
    return;
  }
  const Row& r = rows_[static_cast<size_t>(current_)];
  {
    // The row waits with no mapping of its own: whatever is pressed is new
    // to it, even the tab it had, and a learn cancelled elsewhere leaves it
    // empty. release() puts back what it had.
    const MasterpieceProcessor::AudioLock lock(proc_);
    before_.clear();
    for (const MidiBinding* b : map.bindingsFor(MidiTargetKind::Switch, r.switchId)) before_.push_back(*b);
    map.unbindTarget(MidiTargetKind::Switch, r.switchId);
    // The plain Learn of the console's right-click menu: a tab toggles, a
    // piston is held while pressed.
    map.beginLearnAs(MidiTargetKind::Switch, r.switchId,
                     r.latching ? MidiTrigger::Toggle : MidiTrigger::Momentary);
  }
  armed_ = true;
  list_.selectRow(current_);
  list_.scrollToEnsureRowIsOnscreen(current_);
  showState();
}

void LearnInTurnPanel::release() {
  if (!armed_ || current_ < 0 || current_ >= rowCount()) return;
  armed_ = false;
  auto& map = proc_.midiMap();
  const Id sw = rows_[static_cast<size_t>(current_)].switchId;
  const MasterpieceProcessor::AudioLock lock(proc_);
  if (map.learning() && map.learningTarget() == sw) map.cancelLearn();
  if (map.bindingsFor(MidiTargetKind::Switch, sw).empty() && !before_.empty())
    map.setBindingsFor(MidiTargetKind::Switch, sw, before_);
}

void LearnInTurnPanel::skip() {
  if (current_ < 0) return;
  release();
  ++current_;
  refused_.clear();
  arm();
}

void LearnInTurnPanel::back() {
  if (current_ < 0) return;
  release();
  // Back passes over nothing: the row before is asked again, mapped or not.
  current_ = juce::jmax(0, current_ - 1);
  refused_.clear();
  const bool only = unmappedOnly_.getToggleState();
  unmappedOnly_.setToggleState(false, juce::dontSendNotification);
  arm();
  unmappedOnly_.setToggleState(only, juce::dontSendNotification);
}

void LearnInTurnPanel::stop() {
  if (current_ < 0) return;
  release();
  current_ = -1;
  refused_.clear();
  showState();
}

void LearnInTurnPanel::finish() {
  current_ = -1;
  refused_.clear();
  list_.deselectAllRows();
  showState();
}

void LearnInTurnPanel::poll() {
  if (current_ < 0 || current_ >= rowCount()) return;
  auto& map = proc_.midiMap();
  const Row& r = rows_[static_cast<size_t>(current_)];
  if (!armed_) return;
  if (map.learning()) {
    // Another learn started elsewhere, from the console or a MIDI window:
    // that one is the player's now.
    if (map.learningTarget() != r.switchId) stop();
    return;
  }
  // The learn is over: taken, or cancelled from outside (Escape on the
  // console), which leaves the row as empty as it was armed.
  const auto now = map.bindingsFor(MidiTargetKind::Switch, r.switchId);
  if (now.empty()) {
    stop();
    return;
  }
  armed_ = false;
  const MidiSource source = now.front()->source;
  for (const auto& [taken, row] : taken_)
    if (taken == source && row != current_) {
      // The tab of an earlier row, pressed again to put it back: this row
      // gets back what it had and asks again; the earlier row keeps the tab.
      {
        const MasterpieceProcessor::AudioLock lock(proc_);
        map.setBindingsFor(MidiTargetKind::Switch, r.switchId, before_);
      }
      refused_ = TRANS("That tab is %1.").replace("%1", rowLabel(rows_[static_cast<size_t>(row)]));
      arm();
      return;
    }
  taken_.emplace_back(source, current_);
  ++learned_;
  proc_.saveMidiMap();
  ++current_;
  refused_.clear();
  arm();
}

void LearnInTurnPanel::showState() {
  const bool running = current_ >= 0 && current_ < rowCount();
  if (running) {
    const Row& r = rows_[static_cast<size_t>(current_)];
    prompt_.setText((r.piston ? TRANS("Press the piston for %1") : TRANS("Press the tab for %1"))
                        .replace("%1", rowLabel(r)),
                    juce::dontSendNotification);
  } else if (learned_ > 0) {
    prompt_.setText(TRANS("%1 tabs learned").replace("%1", juce::String(learned_)), juce::dontSendNotification);
  } else {
    prompt_.setText(rows_.empty() ? TRANS("This organ has no tabs to learn") : TRANS("Learn tabs in turn"),
                    juce::dontSendNotification);
  }
  note_.setColour(juce::Label::textColourId, refused_.isEmpty() ? kDim : kWarn);
  note_.setText(refused_.isNotEmpty()
                    ? refused_
                    : running ? TRANS("%1 of %2. The window moves on after each press.")
                                    .replace("%1", juce::String(current_ + 1))
                                    .replace("%2", juce::String(rowCount()))
                              : TRANS("Press Start. Then press each tab on your console as its name shows here. "
                                      "To start at another tab, select its row first."),
                juce::dontSendNotification);
  start_.setEnabled(!rows_.empty());
  start_.setButtonText(running ? TRANS("Restart here") : TRANS("Start"));
  skip_.setEnabled(running);
  back_.setEnabled(running && current_ > 0);
  stop_.setEnabled(running);
  list_.repaint();
}

void LearnInTurnPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
  if (row < 0 || row >= rowCount()) return;
  const Row& r = rows_[static_cast<size_t>(row)];
  if (row == current_)
    g.fillAll(kArmed);
  else if (selected)
    g.fillAll(kArmed.withAlpha(0.5f));
  const int sectionW = width / 4, nameW = width * 3 / 8;
  g.setFont(juce::FontOptions(14.0f));
  g.setColour(kDim);
  g.drawText(juce::String::fromUTF8(r.section.c_str()), 8, 0, sectionW - 12, height,
             juce::Justification::centredLeft, true);
  g.setColour(juce::Colours::white);
  g.drawText(juce::String::fromUTF8(r.name.c_str()), sectionW, 0, nameW - 8, height,
             juce::Justification::centredLeft, true);
  const auto bindings = proc_.midiMap().bindingsFor(MidiTargetKind::Switch, r.switchId);
  juce::String what = row == current_ ? TRANS("Waiting...") : TRANS("Not mapped");
  if (!bindings.empty()) {
    juce::StringArray parts;
    for (const MidiBinding* b : bindings) parts.add(MidiEventDialog::describeBinding(*b));
    what = parts.joinIntoString(", ");
  }
  g.setColour(bindings.empty() ? kDim : kText);
  g.drawText(what, sectionW + nameW, 0, width - sectionW - nameW - 8, height, juce::Justification::centredLeft,
             true);
}

void LearnInTurnPanel::listBoxItemClicked(int row, const juce::MouseEvent&) {
  // While running, a click moves the learn to that row.
  if (current_ >= 0 && row >= 0 && row < rowCount() && row != current_) start(row);
}

void LearnInTurnPanel::paint(juce::Graphics& g) { g.fillAll(kBack); }

void LearnInTurnPanel::resized() {
  auto r = getLocalBounds().reduced(kMargin);
  prompt_.setBounds(r.removeFromTop(32));
  note_.setBounds(r.removeFromTop(36));
  auto options = r.removeFromBottom(kButtonH);
  r.removeFromBottom(6);
  auto buttons = r.removeFromBottom(kButtonH);
  r.removeFromBottom(8);
  list_.setBounds(r);
  for (auto* b : {&start_, &back_, &skip_, &stop_}) {
    b->setBounds(buttons.removeFromLeft(112));
    buttons.removeFromLeft(6);
  }
  pistons_.setBounds(options.removeFromLeft(120));
  unmappedOnly_.setBounds(options.removeFromLeft(240));
}

LearnInTurnWindow::LearnInTurnWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow(TRANS("Learn tabs in turn"), kBack, juce::DocumentWindow::closeButton),
      panel_(std::make_unique<LearnInTurnPanel>(p)) {
  setUsingNativeTitleBar(true);
  setResizable(true, false);
  setContentNonOwned(panel_.get(), false);
  setSize(620, 560);
  fitToScreen(*this);
}

LearnInTurnWindow::~LearnInTurnWindow() { clearContentComponent(); }

void LearnInTurnWindow::closeButtonPressed() {
  panel_->stop();
  showFloating(*this, false, false);
}

}  // namespace mp::ui
