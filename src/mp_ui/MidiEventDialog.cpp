#include "MidiEventDialog.h"

#include "ObjectLabels.h"
#include "Mobile.h"

#include <optional>

namespace mp::ui {
namespace {

constexpr int kRow = 28;
constexpr int kGap = 6;
const juce::Colour kBg{0xff1b1e24};
const juce::Colour kText{0xffdfe6f0};
const juce::Colour kDim{0xff8b93a3};

juce::String noteName(int n) {
  static const char* kNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                 "F#", "G",  "G#", "A",  "A#", "B"};
  if (n < 0 || n > 127) return "-";
  return juce::String(kNames[n % 12]) + juce::String(n / 12 - 1) + " (" + juce::String(n) + ")";
}

// A target worked by a press, as a switch is: the organ's own switches, and
// the program's buttons -- the player's pistons, the cancels, the setter, the
// stepper. Everything but a shoe and a manual.
bool pressable(MidiTargetKind k) {
  return k != MidiTargetKind::ContinuousControl && k != MidiTargetKind::Keyboard &&
         k != MidiTargetKind::None;
}

juce::String objectName(const MasterpieceProcessor& p, MidiTargetKind kind, Id id) {
  const auto& m = p.organModel();
  if (kind == MidiTargetKind::Switch) return MidiEventDialog::switchName(m, id);
  if (kind == MidiTargetKind::ContinuousControl) {
    const auto it = m.continuousControls.find(id);
    if (it != m.continuousControls.end() && !it->second.name.empty())
      return juce::String(mp::readableName(it->second.name));
    return TRANS("Control %1").replace("%1", juce::String(static_cast<int>(id)));
  }
  return juce::String(p.keyboardName(id));
}

void fillEvents(juce::ComboBox& box, bool sysEx = true) {
  box.addItem(TRANS("Note"), 1);
  box.addItem(TRANS("Controller"), 2);
  box.addItem(TRANS("Program change"), 3);
  if (sysEx) box.addItem(TRANS("System exclusive"), 4);
}
int eventItem(MidiSourceKind k) {
  return k == MidiSourceKind::ControlChange   ? 2
         : k == MidiSourceKind::ProgramChange ? 3
         : k == MidiSourceKind::SysEx         ? 4
                                              : 1;
}
// A switch's controller at one value (#923): the controller, with the row's
// exact-values flag.
constexpr int kExactControllerItem = 5;
MidiSourceKind eventKind(int item) {
  return item == 2 || item == kExactControllerItem ? MidiSourceKind::ControlChange
                   : item == 3 ? MidiSourceKind::ProgramChange
                   : item == 4 ? MidiSourceKind::SysEx
                               : MidiSourceKind::Note;
}

// 128 numbers, named the way the event names them: a note by its pitch, a
// program counted from 1 as every console's manual does.
// "Any note" sits under its own id, past the 128 numbers.
constexpr int kAnyNoteItem = 1000;

void fillNumbers(juce::ComboBox& box, MidiSourceKind kind, int selected, bool offerAnyNote = false) {
  box.clear(juce::dontSendNotification);
  // A control set by a note's velocity can take it from every note (#199).
  if (kind == MidiSourceKind::Note && offerAnyNote) {
    box.addItem(TRANS("Any note"), kAnyNoteItem);
    if (selected == kAnyNote || selected == kAnyNoteItem - 1) {
      for (int n = 0; n < 128; ++n) box.addItem(noteName(n), n + 1);
      box.setSelectedId(kAnyNoteItem, juce::dontSendNotification);
      return;
    }
  }
  // A system exclusive message is identified by its bytes, taken by Listen.
  if (kind == MidiSourceKind::SysEx) {
    box.addItem(TRANS("Learned message"), 1);
    box.setSelectedId(1, juce::dontSendNotification);
    return;
  }
  for (int n = 0; n < 128; ++n)
    box.addItem(kind == MidiSourceKind::Note            ? noteName(n)
                : kind == MidiSourceKind::ProgramChange ? juce::String(n + 1)
                                                        : juce::String(n),
                n + 1);
  box.setSelectedId(juce::jlimit(0, 127, selected) + 1, juce::dontSendNotification);
}

void fillChannels(juce::ComboBox& box, bool any) {
  if (any) box.addItem(TRANS("Any"), 1);
  for (int c = 1; c <= 16; ++c) box.addItem(juce::String(c), c + 1);
}

void styleValue(juce::Slider& s, int lo, int hi) {
  s.setSliderStyle(juce::Slider::IncDecButtons);
  s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 40, kRow - 4);
  s.setIncDecButtonsMode(juce::Slider::incDecButtonsDraggable_Vertical);
  s.setRange(lo, hi, 1);
}

void styleNote(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setColour(juce::Label::textColourId, kDim);
  l.setJustificationType(juce::Justification::topLeft);
  l.setFont(juce::FontOptions(12.0f));
}

// Column titles over a list of rows, laid out at the same widths.
class Header : public juce::Component {
public:
  void set(const std::vector<std::pair<juce::String, int>>& columns) {
    labels_.clear();
    widths_.clear();
    for (const auto& [text, width] : columns) {
      auto l = std::make_unique<juce::Label>();
      l->setText(text, juce::dontSendNotification);
      l->setColour(juce::Label::textColourId, kDim);
      l->setFont(juce::FontOptions(12.0f));
      addAndMakeVisible(*l);
      labels_.push_back(std::move(l));
      widths_.push_back(width);
    }
    resized();
  }
  void resized() override {
    auto r = getLocalBounds();
    for (size_t i = 0; i < labels_.size(); ++i) {
      labels_[i]->setBounds(r.removeFromLeft(widths_[i]));
      r.removeFromLeft(kGap);
    }
  }

private:
  std::vector<std::unique_ptr<juce::Label>> labels_;
  std::vector<int> widths_;
};

// Lay a row's children left to right at the given widths.
void layoutRow(juce::Rectangle<int> r, const std::vector<std::pair<juce::Component*, int>>& cells) {
  for (const auto& [c, width] : cells) {
    auto cell = r.removeFromLeft(width);
    r.removeFromLeft(kGap);
    if (c != nullptr) c->setBounds(cell);
  }
}

// ------------------------------------------------------------------ Receive

class ReceiveRow : public juce::Component {
public:
  std::function<void()> onChange, onListen, onDelete;

  ReceiveRow(MasterpieceProcessor& p, MidiTargetKind kind, const MidiBinding& b)
      : kind_(kind), binding_(b) {
    device_.addItem(TRANS("Any console"), 1);
    const auto& names = p.midiMap().devices().names();
    for (size_t i = 0; i < names.size(); ++i)
      device_.addItem(juce::String(p.midiMap().devices().displayName(static_cast<int>(i) + 1)),
                      static_cast<int>(i) + 2);
    device_.setSelectedId(b.source.deviceId + 1, juce::dontSendNotification);
    if (device_.getSelectedId() == 0) device_.setSelectedId(1, juce::dontSendNotification);

    fillChannels(channel_, true);
    channel_.setSelectedId(b.source.channel + 1, juce::dontSendNotification);
    fillEvents(event_);
    if (kind_ == MidiTargetKind::Switch) event_.addItem(TRANS("Controller, one value"), kExactControllerItem);
    const bool set = b.source.kind != MidiSourceKind::None;
    if (set)
      event_.setSelectedId(b.exactValues && b.source.kind == MidiSourceKind::ControlChange
                               ? kExactControllerItem
                               : eventItem(b.source.kind),
                           juce::dontSendNotification);
    fillNumbers(number_, set ? b.source.kind : MidiSourceKind::Note, b.source.number,
                kind_ == MidiTargetKind::ContinuousControl);
    if (!set) {
      event_.setTextWhenNothingSelected(TRANS("Listen..."));
      event_.setSelectedId(0, juce::dontSendNotification);
      number_.setSelectedId(0, juce::dontSendNotification);
    }

    for (auto* c : {&device_, &channel_, &event_, &number_}) {
      addAndMakeVisible(*c);
      c->onChange = [this] { changed(); };
    }
    event_.onChange = [this] {
      fillNumbers(number_, eventKind(event_.getSelectedId()), number_.getSelectedId() - 1,
                  kind_ == MidiTargetKind::ContinuousControl);
      if (pressable(kind_)) {
        fillActions(read().trigger);
        showValues();
      }
      changed();
    };

    if (pressable(kind)) {
      fillActions(b.trigger);
      addAndMakeVisible(action_);
      action_.onChange = [this] { changed(); };
      // The values a button sends for off and for on (#922): a console that
      // sends 1 and 2, or 127 for off and 0 for on, as GrandOrgue's on/off
      // events take them. A press counts from half-way between the two.
      styleValue(low_, 0, 127);
      styleValue(high_, 0, 127);
      low_.setValue(b.lowValue, juce::dontSendNotification);
      high_.setValue(b.highValue, juce::dontSendNotification);
      low_.setTooltip(TRANS("The value your console sends for off"));
      high_.setTooltip(TRANS("The value your console sends for on"));
      for (juce::Slider* v : {&low_, &high_}) {
        addAndMakeVisible(*v);
        v->onValueChange = [this] { changed(); };
      }
      showValues();
      // Contact chatter (#926): a second press within this time is dropped.
      styleValue(debounce_, 0, 500);
      debounce_.setValue(b.debounceMs, juce::dontSendNotification);
      debounce_.setTooltip(TRANS("Milliseconds after a press in which a second press is ignored: for a "
                                 "contact that bounces. 0 is off."));
      addAndMakeVisible(debounce_);
      debounce_.onValueChange = [this] { changed(); };
    } else {
      // A shoe's travel, closed to open: a console's pedal rarely sends the
      // whole 0..127 (#90 sent 55..127, so the box only half closed).
      styleValue(low_, 0, 127);
      styleValue(high_, 0, 127);
      low_.setValue(b.lowValue, juce::dontSendNotification);
      high_.setValue(b.highValue, juce::dontSendNotification);
      invert_.setToggleState(b.invert, juce::dontSendNotification);
      for (juce::Component* c : {static_cast<juce::Component*>(&low_),
                                 static_cast<juce::Component*>(&high_),
                                 static_cast<juce::Component*>(&invert_)})
        addAndMakeVisible(*c);
      low_.onValueChange = [this] { changed(); };
      high_.onValueChange = [this] { changed(); };
      invert_.onClick = [this] { changed(); };
    }
    addAndMakeVisible(listen_);
    listen_.onClick = [this] { if (onListen) onListen(); };
    addAndMakeVisible(delete_);
    delete_.setTooltip(TRANS("Remove this event"));
    delete_.onClick = [this] { if (onDelete) onDelete(); };
  }

  static std::vector<std::pair<juce::String, int>> columns(MidiTargetKind kind) {
    if (pressable(kind))
      return {{TRANS("Device"), 120}, {TRANS("Channel"), 64}, {TRANS("Event"), 160}, {TRANS("Number"), 90},
              {TRANS("What it does"), 140}, {TRANS("Off at"), 84}, {TRANS("On at"), 84},
              {TRANS("Debounce ms"), 84}, {"", 84}, {"", 28}};
    return {{TRANS("Device"), 150}, {TRANS("Channel"), 64}, {TRANS("Event"), 116}, {TRANS("Number"), 96},
            {TRANS("Closed at"), 72}, {TRANS("Open at"), 72}, {"", 60}, {"", 84}, {"", 28}};
  }

  void resized() override {
    const auto cols = columns(kind_);
    std::vector<std::pair<juce::Component*, int>> cells{
        {&device_, cols[0].second}, {&channel_, cols[1].second},
        {&event_, cols[2].second}, {&number_, cols[3].second}};
    if (pressable(kind_)) {
      cells.push_back({&action_, cols[4].second});
      cells.push_back({&low_, cols[5].second});
      cells.push_back({&high_, cols[6].second});
      cells.push_back({&debounce_, cols[7].second});
    } else {
      cells.push_back({&low_, cols[4].second});
      cells.push_back({&high_, cols[5].second});
      cells.push_back({&invert_, cols[6].second});
    }
    cells.push_back({&listen_, cols[cols.size() - 2].second});
    cells.push_back({&delete_, cols.back().second});
    layoutRow(getLocalBounds().reduced(0, 2), cells);
  }

  void setListening(bool on) {
    listen_.setButtonText(on ? TRANS("Listening...") : TRANS("Listen"));
    listen_.setColour(juce::TextButton::buttonColourId,
                      on ? juce::Colours::darkorange : juce::Colour(0xff2a2f38));
  }

  MidiBinding read() const {
    MidiBinding b = binding_;
    b.source.deviceId = juce::jmax(0, device_.getSelectedId() - 1);
    b.source.channel = juce::jmax(0, channel_.getSelectedId() - 1);
    if (event_.getSelectedId() > 0) {
      b.source.kind = eventKind(event_.getSelectedId());
      b.exactValues = event_.getSelectedId() == kExactControllerItem;
      b.source.number = b.source.kind == MidiSourceKind::SysEx ? binding_.source.number
                        : number_.getSelectedId() == kAnyNoteItem    ? kAnyNote
                                                                      : juce::jmax(0, number_.getSelectedId() - 1);
    }
    if (pressable(kind_)) {
      const int a = action_.getSelectedId();
      b.trigger = a == 2   ? MidiTrigger::Momentary
                  : a == 3 ? MidiTrigger::EngageOnly
                  : a == 4 ? MidiTrigger::DisengageOnly
                           : MidiTrigger::Toggle;
      b.lowValue = static_cast<int>(low_.getValue());
      b.highValue = static_cast<int>(high_.getValue());
      b.debounceMs = static_cast<int>(debounce_.getValue());
    } else {
      b.lowValue = static_cast<int>(low_.getValue());
      b.highValue = static_cast<int>(high_.getValue());
      b.invert = invert_.getToggleState();
    }
    return b;
  }

private:
  void changed() {
    binding_ = read();
    if (onChange) onChange();
  }

  // A program change or a system exclusive message carries no value to
  // read on or off from.
  void showValues() {
    const int e = event_.getSelectedId();
    const bool values = e == 0 || eventKind(e) == MidiSourceKind::Note ||
                        eventKind(e) == MidiSourceKind::ControlChange;
    low_.setEnabled(values);
    high_.setEnabled(values);
  }

  // The ways this row's message can work its target. A switch held by a
  // message with no release would stay drawn, so it toggles.
  void fillActions(MidiTrigger t) {
    const auto source = event_.getSelectedId() > 0 ? eventKind(event_.getSelectedId())
                                                   : MidiSourceKind::None;
    action_.clear(juce::dontSendNotification);
    if (kind_ == MidiTargetKind::Switch) {
      action_.addItem(TRANS("Each press toggles"), 1);
      if (MidiEventDialog::heldOffered(kind_, source)) action_.addItem(TRANS("Held while pressed"), 2);
      action_.addItem(TRANS("Draws it"), 3);
      action_.addItem(TRANS("Cancels it"), 4);
    } else {
      // A piston fires when pressed; the setter may also be worked by a
      // button that toggles it, as its Learn menu offers.
      action_.addItem(TRANS("Fires when pressed"), 2);
      if (kind_ == MidiTargetKind::Setter) action_.addItem(TRANS("Each press toggles"), 1);
    }
    const int want = t == MidiTrigger::Momentary       ? 2
                     : t == MidiTrigger::EngageOnly    ? 3
                     : t == MidiTrigger::DisengageOnly ? 4
                                                       : 1;
    action_.setSelectedId(action_.indexOfItemId(want) >= 0 ? want : action_.getItemId(0),
                          juce::dontSendNotification);
  }

  MidiTargetKind kind_;
  MidiBinding binding_;
  juce::ComboBox device_, channel_, event_, number_, action_;
  juce::Slider low_, high_, debounce_;
  juce::ToggleButton invert_{TRANS("Invert")};
  juce::TextButton listen_{TRANS("Listen")}, delete_{"X"};
};

// Every message that works one object, a row each.
class ReceivePanel : public juce::Component, private juce::Timer {
public:
  ReceivePanel(MasterpieceProcessor& p, MidiTargetKind kind, Id id)
      : proc_(p), kind_(kind), id_(id) {
    for (const MidiBinding* b : p.midiMap().bindingsFor(kind, id)) rows_.push_back(*b);
    addAndMakeVisible(header_);
    header_.set(ReceiveRow::columns(kind));
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(add_);
    add_.onClick = [this] { addRow(); };
    addAndMakeVisible(empty_);
    styleNote(empty_, TRANS("Nothing works this yet. Press New event, then press the button, pull the "
                            "drawstop or move the pedal on your console."));
    addAndMakeVisible(note_);
    styleNote(note_,
              kind == MidiTargetKind::Switch
                  ? TRANS("Add a row for every message that works this. A rocker tab or a console "
                          "with separate draw and cancel buttons is two rows: one that draws it and "
                          "one that cancels it. Listen fills a row from the next message your "
                          "console sends. One message can work several stops and couplers; a "
                          "message used by anything else moves here. Saved with this organ.")
              : pressable(kind)
                  ? TRANS("Add a row for every message that works this. A rocker tab or a console "
                          "with separate draw and cancel buttons is two rows: one that draws it and "
                          "one that cancels it. Listen fills a row from the next message your "
                          "console sends. A message already used by something else moves here. "
                          "Saved with this organ.")
                  : TRANS("Add a row for every pedal or knob that moves this. Closed at and Open "
                          "at are the values your pedal sends at its two ends, when it does not "
                          "travel the whole 0 to 127; Invert turns a pedal wired backwards around. "
                          "One pedal can move several shoes. Saved with this organ."));
    rebuild();
    startTimerHz(20);
  }
  ~ReceivePanel() override {
    if (listening_ >= 0) proc_.cancelMidiListen();
  }

  void paint(juce::Graphics& g) override { g.fillAll(kBg); }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    header_.setBounds(r.removeFromTop(20));
    note_.setBounds(r.removeFromBottom(64));
    r.removeFromBottom(6);
    add_.setBounds(r.removeFromBottom(kRow + 2).removeFromLeft(140));
    r.removeFromBottom(6);
    viewport_.setBounds(r);
    empty_.setBounds(r.removeFromTop(48));
    layoutList();
  }

private:
  void layoutList() {
    const int w = juce::jmax(100, viewport_.getMaximumVisibleWidth());
    list_.setSize(w, static_cast<int>(rowViews_.size()) * kRow);
    for (size_t i = 0; i < rowViews_.size(); ++i)
      rowViews_[i]->setBounds(0, static_cast<int>(i) * kRow, w, kRow);
  }

  void rebuild() {
    rowViews_.clear();
    for (size_t i = 0; i < rows_.size(); ++i) {
      auto row = std::make_unique<ReceiveRow>(proc_, kind_, rows_[i]);
      ReceiveRow* raw = row.get();
      row->onChange = [this, i, raw] {
        rows_[i] = raw->read();
        commit();
      };
      row->onListen = [this, i] { listen(static_cast<int>(i)); };
      row->onDelete = [this, i] {
        stopListening();
        rows_.erase(rows_.begin() + static_cast<long>(i));
        commit();
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this)] {
          if (safe != nullptr) safe->rebuild();
        });
      };
      row->setListening(static_cast<int>(i) == listening_);
      list_.addAndMakeVisible(*row);
      rowViews_.push_back(std::move(row));
    }
    empty_.setVisible(rows_.empty());
    layoutList();
  }

  void addRow() {
    MidiBinding b;
    if (kind_ == MidiTargetKind::Switch) {
      const auto sw = proc_.organModel().switches.find(id_);
      const bool latching = sw == proc_.organModel().switches.end() || sw->second.latching;
      b.trigger = latching ? MidiTrigger::Toggle : MidiTrigger::Momentary;
    } else if (pressable(kind_)) {
      b.trigger = MidiTrigger::Momentary;
    }
    rows_.push_back(b);
    listening_ = static_cast<int>(rows_.size()) - 1;
    proc_.midiMap().cancelLearn();
    proc_.beginMidiListen();
    rebuild();
  }

  void listen(int row) {
    if (listening_ == row) {
      stopListening();
      return;
    }
    proc_.midiMap().cancelLearn();
    proc_.beginMidiListen();
    listening_ = row;
    for (size_t i = 0; i < rowViews_.size(); ++i)
      rowViews_[i]->setListening(static_cast<int>(i) == listening_);
  }

  void stopListening() {
    if (listening_ < 0) return;
    proc_.cancelMidiListen();
    listening_ = -1;
    for (auto& v : rowViews_) v->setListening(false);
  }

  void timerCallback() override {
    if (listening_ < 0) return;
    MidiSource heard;
    int value = 0;
    if (proc_.takeHeardMidi(heard, value)) {
      if (listening_ < static_cast<int>(rows_.size())) {
        auto& row = rows_[static_cast<size_t>(listening_)];
        row.source = heard;
        // A controller at one value takes the value it was pressed at.
        if (row.exactValues && heard.kind == MidiSourceKind::ControlChange) row.highValue = value;
        if (row.trigger == MidiTrigger::Momentary && !MidiEventDialog::heldOffered(kind_, heard.kind))
          row.trigger = MidiTrigger::Toggle;
      }
      listening_ = -1;
      commit();
      rebuild();
    } else if (!proc_.midiListening()) {
      stopListening();
    }
  }

  void commit() { proc_.setObjectBindings(kind_, id_, rows_); }

  MasterpieceProcessor& proc_;
  MidiTargetKind kind_;
  Id id_;
  std::vector<MidiBinding> rows_;
  std::vector<std::unique_ptr<ReceiveRow>> rowViews_;
  int listening_ = -1;
  Header header_;
  juce::Viewport viewport_;
  juce::Component list_;
  juce::TextButton add_{TRANS("New event")};
  juce::Label empty_, note_;
};

// --------------------------------------------------------------------- Send

// GrandOrgue's Send tab: one event at a time in a form whose fields follow
// the event chosen, the events listed beside it. The form shows the bytes the
// event puts out, and two buttons send them, so a lamp's colour is checked
// before it is played.

// The kinds of event a send offers, as one list.
enum SendEvent {
  kSendNote = 1,
  kSendNoteOff,
  kSendController,
  kSendProgram,
  kSendRpn,
  kSendNrpn,
  kSendHex,
  kSendLcd,
  kSendText,
};

int sendEventOf(const MidiSend& s) {
  switch (s.kind) {
    case MidiSourceKind::Note: return s.noteOff ? kSendNoteOff : kSendNote;
    case MidiSourceKind::ProgramChange: return kSendProgram;
    case MidiSourceKind::SysEx:
      return s.display == MidiDisplay::Lcd    ? kSendLcd
             : s.display == MidiDisplay::Text ? kSendText
                                              : kSendHex;
    case MidiSourceKind::ControlChange:
      return s.parameter == MidiParameter::Registered      ? kSendRpn
             : s.parameter == MidiParameter::NonRegistered ? kSendNrpn
                                                           : kSendController;
    case MidiSourceKind::None: break;
  }
  return kSendNote;
}

void applySendEvent(int event, MidiSend& s) {
  s.noteOff = event == kSendNoteOff;
  s.parameter = event == kSendRpn    ? MidiParameter::Registered
                : event == kSendNrpn ? MidiParameter::NonRegistered
                                     : MidiParameter::None;
  const auto display = event == kSendLcd    ? MidiDisplay::Lcd
                       : event == kSendText ? MidiDisplay::Text
                                            : MidiDisplay::None;
  // A display starts on its first id, showing the object's name.
  if (display != s.display) s.number = s.low = s.high = 0;
  s.display = display;
  s.kind = event == kSendNote || event == kSendNoteOff      ? MidiSourceKind::Note
           : event == kSendProgram                          ? MidiSourceKind::ProgramChange
           : event == kSendHex || display != MidiDisplay::None ? MidiSourceKind::SysEx
                                                            : MidiSourceKind::ControlChange;
}

juce::String describeSend(MidiTargetKind kind, const MidiSend& s) {
  if (kind == MidiTargetKind::Keyboard) {
    juce::String t = TRANS("Notes, ch %1").replace("%1", juce::String(s.channel));
    if (s.number != 0) t << ", " << (s.number > 0 ? "+" : "") << juce::String(s.number);
    return t;
  }
  juce::String what;
  switch (sendEventOf(s)) {
    case kSendHex: return TRANS("Hex messages");
    case kSendLcd:
    case kSendText: {
      juce::String t = (s.display == MidiDisplay::Lcd ? TRANS("Display %1") : TRANS("Text display %1"))
                           .replace("%1", juce::String(s.number));
      return t + ", " + (s.showsValue() ? TRANS("its value") : TRANS("its name"));
    }
    case kSendProgram: what = TRANS("Program %1").replace("%1", juce::String(s.number + 1)); break;
    case kSendRpn: what = TRANS("RPN %1").replace("%1", juce::String(s.number)); break;
    case kSendNrpn: what = TRANS("NRPN %1").replace("%1", juce::String(s.number)); break;
    case kSendController: what = TRANS("CC %1").replace("%1", juce::String(s.number)); break;
    default: what = TRANS("Note %1").replace("%1", noteName(s.number)); break;
  }
  return what + ", " + TRANS("ch %1").replace("%1", juce::String(s.channel));
}

class SendForm : public juce::Component {
public:
  std::function<void()> onChange;
  // The bytes a state puts out, sent at once by the test buttons.
  std::function<void(const std::vector<uint8_t>&)> onTest;

  SendForm(MidiTargetKind kind, std::optional<MidiSource> received, std::string name)
      : kind_(kind), onOff_(MidiMap::sendsOnOff(kind)), received_(received), name_(std::move(name)) {
    if (kind_ != MidiTargetKind::Keyboard) {
      event_.addItem(TRANS("Note"), kSendNote);
      if (onOff_) event_.addItem(TRANS("Note, off as a Note Off (8n)"), kSendNoteOff);
      event_.addItem(TRANS("Controller"), kSendController);
      event_.addItem(TRANS("Program change"), kSendProgram);
      event_.addItem(TRANS("RPN (registered parameter)"), kSendRpn);
      event_.addItem(TRANS("NRPN (non-registered parameter)"), kSendNrpn);
      // A hex send is a whole message for each state, so only a target with
      // two states offers it.
      if (onOff_) event_.addItem(TRANS("Hex messages, SysEx or any other"), kSendHex);
      // A console's text displays, in the GrandOrgue messages: a stop's or a
      // control's name, or a control's value.
      if (kind_ == MidiTargetKind::Switch || kind_ == MidiTargetKind::ContinuousControl) {
        event_.addItem(TRANS("Display, 32 characters"), kSendLcd);
        event_.addItem(TRANS("Display, 16 characters"), kSendText);
      }
      event_.onChange = [this] {
        applySendEvent(event_.getSelectedId(), send_);
        fillNumbers(number_, send_.kind, send_.number);
        show();
        changed();
      };
    }
    fillChannels(channel_, false);
    channel_.onChange = [this] { changed(); };
    number_.onChange = [this] { changed(); };
    offProgram_.addItem(TRANS("none"), 1);
    for (int n = 0; n < 128; ++n) offProgram_.addItem(juce::String(n + 1), n + 2);
    offProgram_.onChange = [this] { changed(); };
    shows_.addItem(TRANS("Its name"), 1);
    shows_.addItem(TRANS("Its value, in per cent"), 2);
    shows_.onChange = [this] { changed(); };

    styleValue(parameter_, 0, 16383);
    parameter_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 56, kRow - 4);
    styleValue(transpose_, -48, 48);
    styleValue(high_, 0, 127);
    styleValue(low_, onOff_ ? -1 : 0, 127);
    low_.textFromValueFunction = [this](double v) {
      if (kind_ == MidiTargetKind::Keyboard && v <= 0) return TRANS("as played");
      return v < 0 ? TRANS("none") : juce::String(static_cast<int>(v));
    };
    low_.valueFromTextFunction = [](const juce::String& t) {
      const auto s = t.trim();
      return s.equalsIgnoreCase("none") || s.equalsIgnoreCase(TRANS("none")) ? -1.0
             : s.equalsIgnoreCase(TRANS("as played"))                       ? 0.0
                                                                            : s.getDoubleValue();
    };
    for (auto* s : {&parameter_, &transpose_, &high_, &low_}) s->onValueChange = [this] { changed(); };

    for (auto* e : {&hexOn_, &hexOff_}) {
      e->setTooltip(TRANS("MIDI messages in hex, one or several separated by commas: a system "
                          "exclusive message F0 to F7, or any other, for example F0 7D 01 F7, "
                          "B0 14 7F. Sent again after each load, so a controller's mode "
                          "message reaches it every time."));
      e->onReturnKey = e->onFocusLost = [this, e] { hexEdited(*e); };
    }
    hexOn_.setTextToShowWhenEmpty(TRANS("hex messages"), juce::Colours::grey);
    hexOff_.setTextToShowWhenEmpty(TRANS("none"), juce::Colours::grey);

    for (auto* l : {&eventLabel_, &channelLabel_, &numberLabel_, &highLabel_, &lowLabel_, &bytesLabel_}) {
      l->setColour(juce::Label::textColourId, kDim);
      l->setJustificationType(juce::Justification::centredRight);
      addChildComponent(*l);
    }
    eventLabel_.setText(TRANS("Event"), juce::dontSendNotification);
    channelLabel_.setText(TRANS("Channel"), juce::dontSendNotification);
    bytesLabel_.setText(TRANS("Sends"), juce::dontSendNotification);
    bytes_.setColour(juce::Label::textColourId, kText);
    bytes_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                     juce::Font::plain));
    bytes_.setJustificationType(juce::Justification::topLeft);

    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &event_, &channel_, &number_, &parameter_, &transpose_, &offProgram_, &shows_, &high_, &low_,
             &hexOn_, &hexOff_, &bytes_, &copy_, &testOn_, &testOff_})
      addChildComponent(*c);

    copy_.setTooltip(TRANS("The event, channel and number this object answers to on the Receive "
                           "tab: a lamp most often answers on the message that works it."));
    copy_.onClick = [this] {
      if (!received_) return;
      MidiSend s = send_;
      if (kind_ != MidiTargetKind::Keyboard) {
        s.kind = received_->kind;
        s.noteOff = false;
        s.parameter = MidiParameter::None;
        s.number = received_->number;
      }
      if (received_->channel > 0) s.channel = received_->channel;
      load(s);
      changed();
    };
    testOn_.onClick = [this] { test(true); };
    testOff_.onClick = [this] { test(false); };
    testOn_.setTooltip(TRANS("Sends it now, to check the lamp or the fader"));
    testOff_.setTooltip(TRANS("Sends it now, to check the lamp or the fader"));
    const bool keys = kind_ == MidiTargetKind::Keyboard;
    testOn_.setButtonText(keys ? TRANS("Send middle C") : onOff_ ? TRANS("Send on") : TRANS("Send open"));
    testOff_.setButtonText(keys ? TRANS("Release it") : onOff_ ? TRANS("Send off") : TRANS("Send closed"));
  }

  void load(const MidiSend& s) {
    send_ = s;
    if (kind_ != MidiTargetKind::Keyboard) event_.setSelectedId(sendEventOf(s), juce::dontSendNotification);
    channel_.setSelectedId(juce::jlimit(1, 16, s.channel) + 1, juce::dontSendNotification);
    fillNumbers(number_, s.kind, s.number);
    parameter_.setValue(juce::jlimit(0, 16383, s.number), juce::dontSendNotification);
    transpose_.setValue(s.number, juce::dontSendNotification);
    offProgram_.setSelectedId(s.offProgram < 0 ? 1 : juce::jmin(127, s.offProgram) + 2,
                              juce::dontSendNotification);
    shows_.setSelectedId(s.showsValue() ? 2 : 1, juce::dontSendNotification);
    high_.setValue(s.high, juce::dontSendNotification);
    low_.setValue(s.low, juce::dontSendNotification);
    low_.updateText();
    hexOn_.setText(juce::String(midiText(s.sysExOn)), juce::dontSendNotification);
    hexOff_.setText(juce::String(midiText(s.sysExOff)), juce::dontSendNotification);
    for (auto* e : {&hexOn_, &hexOff_}) e->removeColour(juce::TextEditor::outlineColourId);
    show();
  }

  MidiSend read() const { return send_; }

  void resized() override {
    auto r = getLocalBounds();
    auto line = [&r]() {
      auto l = r.removeFromTop(kRow);
      r.removeFromTop(kGap);
      return l;
    };
    auto put = [](juce::Rectangle<int> l, juce::Label& label, juce::Component& c, int width) {
      label.setBounds(l.removeFromLeft(110));
      l.removeFromLeft(kGap);
      c.setBounds(width > 0 ? l.removeFromLeft(width) : l);
      return l;
    };
    if (kind_ != MidiTargetKind::Keyboard) put(line(), eventLabel_, event_, 0);
    if (hex()) {
      put(line(), highLabel_, hexOn_, 0);
      put(line(), lowLabel_, hexOff_, 0);
    } else {
      put(line(), channelLabel_, channel_, 80);
      juce::Component& number = kind_ == MidiTargetKind::Keyboard ? static_cast<juce::Component&>(transpose_)
                                : parameterSend() || display()   ? static_cast<juce::Component&>(parameter_)
                                                                 : static_cast<juce::Component&>(number_);
      if (numberLabel_.isVisible()) put(line(), numberLabel_, number, 140);
      auto values = line();
      if (highLabel_.isVisible()) {
        juce::Component& high = programPair() ? static_cast<juce::Component&>(number_)
                                : display()   ? static_cast<juce::Component&>(shows_)
                                              : static_cast<juce::Component&>(high_);
        values = put(values, highLabel_, high, display() ? 200 : 110);
      }
      if (lowLabel_.isVisible()) {
        values.removeFromLeft(kGap);
        lowLabel_.setBounds(values.removeFromLeft(80));
        values.removeFromLeft(kGap);
        (programPair() ? static_cast<juce::Component&>(offProgram_) : low_).setBounds(values.removeFromLeft(110));
      }
    }
    auto b = r.removeFromTop(44);
    bytesLabel_.setBounds(b.removeFromLeft(110).removeFromTop(kRow));
    b.removeFromLeft(kGap);
    bytes_.setBounds(b);
    r.removeFromTop(kGap);
    auto buttons = r.removeFromTop(kRow).withTrimmedLeft(110 + kGap);
    copy_.setBounds(buttons.removeFromLeft(200));
    buttons.removeFromLeft(kGap * 2);
    testOn_.setBounds(buttons.removeFromLeft(120));
    buttons.removeFromLeft(kGap);
    testOff_.setBounds(buttons.removeFromLeft(120));
  }

private:
  bool hex() const { return send_.kind == MidiSourceKind::SysEx && !display(); }
  bool display() const { return send_.display != MidiDisplay::None; }
  bool parameterSend() const {
    return send_.kind == MidiSourceKind::ControlChange && send_.parameter != MidiParameter::None;
  }
  // A program change on an on/off target sends one program when on and
  // another when off: its two values are programs.
  bool programPair() const { return onOff_ && send_.kind == MidiSourceKind::ProgramChange; }

  void show() {
    const bool keys = kind_ == MidiTargetKind::Keyboard, h = hex();
    const bool program = send_.kind == MidiSourceKind::ProgramChange;
    eventLabel_.setVisible(!keys);
    event_.setVisible(!keys);
    hexOn_.setVisible(h);
    hexOff_.setVisible(h);
    const bool d = display(), lcd = send_.display == MidiDisplay::Lcd;
    channelLabel_.setVisible(!h && !d);
    channel_.setVisible(!h && !d);
    // The number: a note, controller or parameter; a manual's transpose; a
    // display's id. A control sending programs sends them across its range,
    // with no number.
    const bool number = !h && !programPair() && !(program && !onOff_);
    numberLabel_.setVisible(number);
    number_.setVisible(!h && !d && !keys && !parameterSend() && (number || programPair()));
    parameter_.setRange(0, d && !lcd ? 127 : 16383, 1);
    parameter_.setVisible(!h && (parameterSend() || d));
    transpose_.setVisible(keys);
    numberLabel_.setText(keys                                      ? TRANS("Transpose")
                         : d                                       ? TRANS("Display")
                         : parameterSend()                         ? TRANS("Parameter")
                         : send_.kind == MidiSourceKind::ControlChange ? TRANS("Controller")
                                                                   : TRANS("Note"),
                         juce::dontSendNotification);
    // The values: on and off; closed and open; a manual's velocity; what a
    // display shows, and in what colour. Only a control has a value to show.
    highLabel_.setVisible(!keys && (!d || kind_ == MidiTargetKind::ContinuousControl));
    high_.setVisible(!h && !d && !keys && !programPair());
    shows_.setVisible(d && kind_ == MidiTargetKind::ContinuousControl);
    lowLabel_.setVisible(!d || lcd);
    low_.setRange(onOff_ && !d ? -1 : 0, 127, 1);
    low_.setVisible(!h && !programPair() && (!d || lcd));
    offProgram_.setVisible(programPair());
    highLabel_.setText(d               ? TRANS("Shows")
                       : programPair() ? TRANS("Program when on")
                       : onOff_        ? TRANS("When on")
                                       : TRANS("Open"),
                       juce::dontSendNotification);
    lowLabel_.setText(keys     ? TRANS("Velocity")
                      : d      ? TRANS("Colour")
                      : onOff_ ? TRANS("When off")
                               : TRANS("Closed"),
                      juce::dontSendNotification);
    bytesLabel_.setVisible(true);
    bytes_.setVisible(true);
    copy_.setVisible(true);
    copy_.setButtonText(TRANS("Copy the received event"));
    copy_.setEnabled(received_.has_value() && !h && !d);
    testOn_.setVisible(true);
    testOff_.setVisible(true);
    showBytes();
    resized();
  }

  // The bytes a state sends, for the form and the test buttons.
  std::vector<uint8_t> bytesFor(bool on) const {
    if (kind_ == MidiTargetKind::Keyboard) {
      RawMidi raw;
      if (!MidiMap::keyMessage(send_, 60, 100, on, raw)) return {};
      return {raw.bytes, raw.bytes + raw.size};
    }
    const int value = onOff_ ? (on ? 1 : 0) : (on ? 127 : 0);
    if (display()) return MidiMap::displayBytes(send_, MidiMap::displayText(send_, name_, value));
    return MidiMap::sendBytes(send_, value);
  }

  void showBytes() {
    // A display's message shows its text as text: in hex it runs past the form.
    const size_t head = send_.display == MidiDisplay::Lcd ? 6 : 4;
    auto text = [this, head](const std::vector<uint8_t>& b) {
      if (b.empty()) return TRANS("nothing");
      if (!display() || b.size() <= head + 1) return juce::String(midiText(b));
      const std::string chars(b.begin() + static_cast<long>(head), b.end() - 1);
      const std::vector<uint8_t> prefix(b.begin(), b.begin() + static_cast<long>(head));
      return juce::String(midiText(prefix)) + " \"" +
             juce::String(chars).trimEnd() + "\" F7";
    };
    const bool keys = kind_ == MidiTargetKind::Keyboard;
    juce::String on = keys ? TRANS("Middle C: %1") : onOff_ ? TRANS("When on: %1") : TRANS("Open: %1");
    juce::String off = keys ? TRANS("Its release: %1") : onOff_ ? TRANS("When off: %1") : TRANS("Closed: %1");
    bytes_.setText(on.replace("%1", text(bytesFor(true))) + "\n" + off.replace("%1", text(bytesFor(false))),
                   juce::dontSendNotification);
  }

  void test(bool on) {
    if (onTest) onTest(bytesFor(on));
  }

  // A message that does not read is outlined in red and left out.
  void hexEdited(juce::TextEditor& e) {
    std::vector<uint8_t> bytes;
    if (parseMidiText(e.getText().toStdString(), bytes)) {
      e.removeColour(juce::TextEditor::outlineColourId);
      e.setText(juce::String(midiText(bytes)), juce::dontSendNotification);
    } else {
      e.setColour(juce::TextEditor::outlineColourId, juce::Colours::red);
    }
    e.repaint();
    changed();
  }

  void changed() {
    MidiSend s = send_;
    s.channel = juce::jmax(1, channel_.getSelectedId() - 1);
    if (kind_ == MidiTargetKind::Keyboard) {
      s.kind = MidiSourceKind::Note;
      s.number = static_cast<int>(transpose_.getValue());
      s.low = juce::jmax(0, static_cast<int>(low_.getValue()));
    } else {
      s.number = parameterSend() || display() ? static_cast<int>(parameter_.getValue())
                                              : juce::jmax(0, number_.getSelectedId() - 1);
      s.low = static_cast<int>(low_.getValue());
      s.high = display() ? (kind_ == MidiTargetKind::ContinuousControl && shows_.getSelectedId() == 2 ? 1 : 0)
                         : static_cast<int>(high_.getValue());
      if (programPair()) s.offProgram = offProgram_.getSelectedId() - 2;
      if (hex()) {
        // A message that does not read keeps the one before.
        std::vector<uint8_t> bytes;
        if (parseMidiText(hexOn_.getText().toStdString(), bytes)) s.sysExOn = bytes;
        if (parseMidiText(hexOff_.getText().toStdString(), bytes)) s.sysExOff = bytes;
      }
    }
    send_ = s;
    showBytes();
    if (onChange) onChange();
  }

  MidiTargetKind kind_;
  bool onOff_;
  std::optional<MidiSource> received_;
  std::string name_;  // what a display of the object's name shows
  MidiSend send_;
  juce::Label eventLabel_, channelLabel_, numberLabel_, highLabel_, lowLabel_, bytesLabel_, bytes_;
  juce::ComboBox event_, channel_, number_, offProgram_, shows_;
  juce::Slider parameter_, transpose_, high_, low_;
  juce::TextEditor hexOn_, hexOff_;
  juce::TextButton copy_, testOn_, testOff_;
};

// What the console is sent when this object changes: its events listed on
// the left, the one chosen in the form.
class SendPanel : public juce::Component, private juce::ListBoxModel, private juce::Timer {
public:
  SendPanel(MasterpieceProcessor& p, MidiTargetKind kind, Id id)
      : proc_(p), kind_(kind), id_(id), form_(kind, received(p, kind, id), p.displayNameOf(kind, id)) {
    rows_ = p.midiMap().sendsFor(kind, id);
    list_.setModel(this);
    list_.setRowHeight(kRow);
    list_.setColour(juce::ListBox::backgroundColourId, kBg.brighter(0.06f));
    addAndMakeVisible(list_);
    addAndMakeVisible(form_);
    form_.onChange = [this] {
      if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return;
      rows_[static_cast<size_t>(selected_)] = form_.read();
      commit();
      list_.repaintRow(selected_);
    };
    form_.onTest = [this](const std::vector<uint8_t>& bytes) { proc_.sendMidiNow(bytes); };
    for (auto* b : {&add_, &duplicate_, &delete_}) addAndMakeVisible(*b);
    add_.onClick = [this] {
      MidiSend s;
      if (kind_ == MidiTargetKind::ContinuousControl) s.kind = MidiSourceKind::ControlChange;
      // A lamp most often answers on the message that works the stop.
      if (const auto* b = proc_.midiMap().bindingFor(kind_, id_)) {
        if (kind_ != MidiTargetKind::Keyboard && b->source.kind != MidiSourceKind::SysEx) {
          s.kind = b->source.kind;
          s.number = b->source.number;
          // A console that toggles on one program change expects it back both ways.
          if (s.kind == MidiSourceKind::ProgramChange && b->trigger == MidiTrigger::Toggle)
            s.offProgram = s.number;
        }
        if (b->source.channel > 0) s.channel = b->source.channel;
      }
      add(s);
    };
    duplicate_.onClick = [this] {
      if (selected_ >= 0) add(rows_[static_cast<size_t>(selected_)]);
    };
    delete_.onClick = [this] {
      if (selected_ < 0) return;
      rows_.erase(rows_.begin() + selected_);
      commit();
      select(juce::jmin(selected_, static_cast<int>(rows_.size()) - 1));
    };
    duplicate_.setTooltip(TRANS("A copy of this event, to send the same to a second lamp or device"));
    addAndMakeVisible(empty_);
    styleNote(empty_, TRANS("Nothing is sent yet. New event adds one."));
    addAndMakeVisible(output_);
    output_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(note_);
    styleNote(note_,
              MidiMap::sendsOnOff(kind)
                  ? TRANS("Sent whenever this comes on or goes off, however it moved: by your "
                          "console, a piston, the crescendo or a click. For the lamp in a drawstop "
                          "or an LED by a piston. When off \"none\" sends nothing when it goes off. "
                          "Saved with this organ.")
              : kind == MidiTargetKind::Keyboard
                  ? TRANS("Every key played on this manual is sent as a note on the channel, moved "
                          "by Transpose: to play a sound module or a second program from it. Saved "
                          "with this organ.")
                  : TRANS("Sent whenever this moves, across Closed to Open: for a motorised fader, "
                          "or a display that shows the swell box. Saved with this organ."));
    select(rows_.empty() ? -1 : 0);
    showOutput();
    startTimerHz(2);
  }

  ~SendPanel() override { list_.setModel(nullptr); }

  void paint(juce::Graphics& g) override { g.fillAll(kBg); }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    output_.setBounds(r.removeFromTop(22));
    r.removeFromTop(8);
    note_.setBounds(r.removeFromBottom(52));
    r.removeFromBottom(6);
    auto left = r.removeFromLeft(260);
    r.removeFromLeft(12);
    auto buttons = left.removeFromBottom(kRow);
    left.removeFromBottom(kGap);
    const int w = (buttons.getWidth() - 2 * kGap) / 3;
    add_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(kGap);
    duplicate_.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(kGap);
    delete_.setBounds(buttons);
    list_.setBounds(left);
    form_.setBounds(r);
    empty_.setBounds(r.removeFromTop(40));
  }

private:
  static std::optional<MidiSource> received(MasterpieceProcessor& p, MidiTargetKind kind, Id id) {
    const auto* b = p.midiMap().bindingFor(kind, id);
    if (b == nullptr || b->source.kind == MidiSourceKind::SysEx || b->source.kind == MidiSourceKind::None)
      return std::nullopt;
    return b->source;
  }

  int getNumRows() override { return static_cast<int>(rows_.size()); }

  void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (selected) g.fillAll(juce::Colour(0xff2f4f7a));
    g.setColour(kText);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(juce::String(row + 1) + ".  " + describeSend(kind_, rows_[static_cast<size_t>(row)]),
               8, 0, w - 12, h, juce::Justification::centredLeft, true);
  }

  void selectedRowsChanged(int row) override {
    if (row != selected_) select(row);
  }

  void timerCallback() override { showOutput(); }

  void showOutput() {
    const auto* out = proc_.midiOutput();
    output_.setText(out != nullptr ? TRANS("Sent to %1").replace("%1", out->getName())
                                   : TRANS("No MIDI output is chosen: choose one in Settings, MIDI."),
                    juce::dontSendNotification);
    output_.setColour(juce::Label::textColourId, out != nullptr ? kText : juce::Colours::orange);
  }

  void add(const MidiSend& s) {
    // Kept, and sent from its first change: an unset row goes nowhere.
    rows_.push_back(s);
    commit(true);
    select(static_cast<int>(rows_.size()) - 1);
  }

  void select(int row) {
    selected_ = row;
    list_.updateContent();
    const bool any = row >= 0 && row < static_cast<int>(rows_.size());
    if (any) {
      list_.selectRow(row, false, true);
      form_.load(rows_[static_cast<size_t>(row)]);
    } else {
      list_.deselectAllRows();
    }
    form_.setVisible(any);
    empty_.setVisible(rows_.empty());
    duplicate_.setEnabled(any);
    delete_.setEnabled(any);
    list_.repaint();
  }

  void commit(bool quietNew = false) { proc_.setObjectSends(kind_, id_, rows_, quietNew); }

  MasterpieceProcessor& proc_;
  MidiTargetKind kind_;
  Id id_;
  std::vector<MidiSend> rows_;
  int selected_ = -1;
  juce::ListBox list_;
  SendForm form_;
  juce::TextButton add_{TRANS("New event")}, duplicate_{TRANS("Duplicate")}, delete_{TRANS("Delete")};
  juce::Label empty_, output_, note_;
};

// ----------------------------------------------------------------- Shortcut

// A button that, once clicked, takes the next key pressed.
class KeyButton : public juce::TextButton {
public:
  std::function<void(const juce::String&)> onKey;

  KeyButton() {
    setWantsKeyboardFocus(true);
    onClick = [this] {
      capturing_ = true;
      show();
      grabKeyboardFocus();
    };
  }
  void setKey(const juce::String& key) {
    key_ = key;
    show();
  }
  bool keyPressed(const juce::KeyPress& k) override {
    if (!capturing_) return juce::TextButton::keyPressed(k);
    capturing_ = false;
    if (k != juce::KeyPress(juce::KeyPress::escapeKey)) {
      key_ = k.getTextDescription();
      if (onKey) onKey(key_);
    }
    show();
    return true;
  }
  void focusLost(FocusChangeType) override {
    capturing_ = false;
    show();
  }

private:
  void show() {
    setButtonText(capturing_ ? TRANS("Press a key...") : key_.isEmpty() ? TRANS("None") : key_);
    setColour(juce::TextButton::buttonColourId,
              capturing_ ? juce::Colours::darkorange : juce::Colour(0xff2a2f38));
  }
  juce::String key_;
  bool capturing_ = false;
};

class ShortcutPanel : public juce::Component {
public:
  ShortcutPanel(MasterpieceProcessor& p, MidiTargetKind kind, Id id)
      : proc_(p), kind_(kind), id_(id) {
    const auto existing = p.midiMap().shortcutsFor(kind, id);
    for (const auto& k : existing) {
      if (k.step >= 0 && up_.isEmpty()) {
        up_ = juce::String(k.key);
        if (k.step > 0) step_.setValue(k.step, juce::dontSendNotification);
      } else if (k.step < 0 && down_.isEmpty()) {
        down_ = juce::String(k.key);
        step_.setValue(-k.step, juce::dontSendNotification);
      }
    }
    const bool control = kind == MidiTargetKind::ContinuousControl;
    addAndMakeVisible(upLabel_);
    upLabel_.setText(control ? TRANS("Opens it") : TRANS("Draws or cancels it"), juce::dontSendNotification);
    upLabel_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(upKey_);
    upKey_.setKey(up_);
    upKey_.onKey = [this](const juce::String& k) { up_ = k; commit(); };
    addAndMakeVisible(clearUp_);
    clearUp_.onClick = [this] { up_.clear(); upKey_.setKey({}); commit(); };
    if (control) {
      addAndMakeVisible(downLabel_);
      downLabel_.setText(TRANS("Closes it"), juce::dontSendNotification);
      downLabel_.setColour(juce::Label::textColourId, kText);
      addAndMakeVisible(downKey_);
      downKey_.setKey(down_);
      downKey_.onKey = [this](const juce::String& k) { down_ = k; commit(); };
      addAndMakeVisible(clearDown_);
      clearDown_.onClick = [this] { down_.clear(); downKey_.setKey({}); commit(); };
      addAndMakeVisible(stepLabel_);
      stepLabel_.setText(TRANS("Each press moves it by"), juce::dontSendNotification);
      stepLabel_.setColour(juce::Label::textColourId, kText);
      styleValue(step_, 1, 64);
      if (step_.getValue() < 1) step_.setValue(8, juce::dontSendNotification);
      addAndMakeVisible(step_);
      step_.onValueChange = [this] { commit(); };
    }
    addAndMakeVisible(note_);
    styleNote(note_, TRANS("Click the button, then press the key, with Ctrl, Alt or Shift if you "
                           "like. Escape leaves it as it was. The key works while the console window "
                           "is in front. One key can work several things at once. Saved with this "
                           "organ."));
  }

  void paint(juce::Graphics& g) override { g.fillAll(kBg); }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    auto line = [&r](juce::Label& l, juce::Component& a, juce::Component& b) {
      auto row = r.removeFromTop(kRow + 2);
      r.removeFromTop(8);
      layoutRow(row, {{&l, 170}, {&a, 180}, {&b, 80}});
    };
    line(upLabel_, upKey_, clearUp_);
    if (kind_ == MidiTargetKind::ContinuousControl) {
      line(downLabel_, downKey_, clearDown_);
      auto row = r.removeFromTop(kRow + 2);
      r.removeFromTop(8);
      layoutRow(row, {{&stepLabel_, 170}, {&step_, 110}});
    }
    r.removeFromTop(8);
    note_.setBounds(r.removeFromTop(64));
  }

private:
  void commit() {
    std::vector<KeyShortcut> rows;
    const int step = static_cast<int>(step_.getValue());
    if (up_.isNotEmpty()) {
      KeyShortcut k;
      k.key = up_.toStdString();
      k.step = kind_ == MidiTargetKind::ContinuousControl ? step : 0;
      rows.push_back(k);
    }
    if (down_.isNotEmpty() && kind_ == MidiTargetKind::ContinuousControl) {
      KeyShortcut k;
      k.key = down_.toStdString();
      k.step = -step;
      rows.push_back(k);
    }
    proc_.setObjectShortcuts(kind_, id_, rows);
  }

  MasterpieceProcessor& proc_;
  MidiTargetKind kind_;
  Id id_;
  juce::String up_, down_;
  juce::Label upLabel_, downLabel_, stepLabel_, note_;
  KeyButton upKey_, downKey_;
  juce::TextButton clearUp_{TRANS("Clear")}, clearDown_{TRANS("Clear")};
  juce::Slider step_;
};

// GrandOrgue's shape: one window per object, a tab per direction.
class MidiEventContent : public juce::Component {
public:
  MidiEventContent(MasterpieceProcessor& p, MidiTargetKind kind, Id id)
      : tabs_(juce::TabbedButtonBar::TabsAtTop) {
    addAndMakeVisible(tabs_);
    tabs_.addTab(TRANS("Receive"), kBg, new ReceivePanel(p, kind, id), true);
    // Everything with a state a console can show: a stop, a control, a manual,
    // and the program's buttons that light -- the generals, the divisionals,
    // the setter, the recorder. A page turn has nothing to show.
    if (MidiMap::hasSendState(kind))
      tabs_.addTab(TRANS("Send"), kBg, new SendPanel(p, kind, id), true);
    tabs_.addTab(TRANS("Shortcut"), kBg, new ShortcutPanel(p, kind, id), true);
  }
  void resized() override { tabs_.setBounds(getLocalBounds()); }

private:
  juce::TabbedComponent tabs_;
};

} // namespace

juce::String MidiEventDialog::switchName(const OrganModel& m, Id id) {
  // The set's own words for the switch win, and the wiring answers only for a
  // switch named by a number: mp::switchLabel holds the rule, and its test
  // holds the two consoles that wrote it (#591).
  const auto name = mp::switchLabel(m, id);
  if (!name.empty()) return juce::String(name);
  return TRANS("Switch %1").replace("%1", juce::String(static_cast<int>(id)));
}

bool MidiEventDialog::heldOffered(MidiTargetKind kind, MidiSourceKind source) {
  return kind == MidiTargetKind::Switch && source != MidiSourceKind::ProgramChange &&
         source != MidiSourceKind::SysEx;
}

juce::String MidiEventDialog::describeBinding(const MidiBinding& b) {
  juce::String what = b.source.kind == MidiSourceKind::Note            ? TRANS("Note %1")
                      : b.source.kind == MidiSourceKind::ProgramChange ? TRANS("Program %1")
                      : b.source.kind == MidiSourceKind::SysEx         ? TRANS("SysEx #%1")
                                                                       : TRANS("CC %1");
  what = what.replace("%1", juce::String(b.source.number));
  if (b.source.channel > 0) what << " " << TRANS("ch %1").replace("%1", juce::String(b.source.channel));
  return what;
}

juce::StringArray MidiEventDialog::sendAndKeyLines(const MidiMap& map, MidiTargetKind kind, Id id) {
  juce::StringArray out;
  for (const MidiSend& s : map.sendsFor(kind, id)) {
    juce::String what = s.kind == MidiSourceKind::SysEx           ? TRANS("SysEx")
                        : s.kind == MidiSourceKind::Note          ? TRANS("Note %1")
                        : s.kind == MidiSourceKind::ProgramChange ? TRANS("Program %1")
                                                                  : TRANS("CC %1");
    what = what.replace("%1", juce::String(s.number));
    if (s.kind != MidiSourceKind::SysEx) what << " " << TRANS("ch %1").replace("%1", juce::String(s.channel));
    out.add(TRANS("Sends %1").replace("%1", what));
  }
  for (const KeyShortcut& k : map.shortcutsFor(kind, id))
    out.add(TRANS("Key %1").replace("%1", juce::String::fromUTF8(k.key.c_str())));
  return out;
}

juce::String MidiEventDialog::targetName(const MasterpieceProcessor& p, MidiTargetKind kind, Id id) {
  return objectName(p, kind, id);
}

void MidiEventDialog::show(MasterpieceProcessor& p, MidiTargetKind kind, Id id,
                           const juce::String& name) {
  auto content = std::make_unique<MidiEventContent>(p, kind, id);
  // Wide enough for a switch's or a piston's row with its off and on values
  // and its debounce time.
  content->setSize(MidiMap::hasSendState(kind) || pressable(kind) ? 1040 : 760, 500);
  juce::DialogWindow::LaunchOptions o;
  o.content.setOwned(content.release());
  o.dialogTitle = (name.isNotEmpty() ? name : objectName(p, kind, id)) + " - MIDI";
  o.dialogBackgroundColour = kBg;
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = true;
  launchDialog(o);
}

std::unique_ptr<juce::Component> MidiEventDialog::makeSendPanel(MasterpieceProcessor& p,
                                                                MidiTargetKind kind, Id id) {
  return std::make_unique<SendPanel>(p, kind, id);
}

} // namespace mp::ui
