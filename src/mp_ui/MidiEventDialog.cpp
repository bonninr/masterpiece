#include "MidiEventDialog.h"
#include "Mobile.h"

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

// A name as a player would read it. Sets name the switch behind a drawn knob
// "__Img_14. GO  Principale 8'": the prefix marks it hidden, and is dropped.
juce::String readable(const std::string& raw) {
  juce::String n(raw);
  if (n.startsWith("__")) {
    n = n.trimCharactersAtStart("_");
    const int cut = n.indexOfChar('_');
    if (cut > 0 && cut < 8 && !n.substring(0, cut).containsChar(' ')) n = n.substring(cut + 1);
  }
  while (n.contains("  ")) n = n.replace("  ", " ");
  return n.trim();
}

juce::String objectName(const MasterpieceProcessor& p, MidiTargetKind kind, Id id) {
  const auto& m = p.organModel();
  if (kind == MidiTargetKind::Switch) {
    const auto it = m.switches.find(id);
    if (it != m.switches.end() && !it->second.name.empty()) return readable(it->second.name);
    return "Switch " + juce::String(static_cast<int>(id));
  }
  if (kind == MidiTargetKind::ContinuousControl) {
    const auto it = m.continuousControls.find(id);
    if (it != m.continuousControls.end() && !it->second.name.empty())
      return readable(it->second.name);
    return "Control " + juce::String(static_cast<int>(id));
  }
  return juce::String(p.keyboardName(id));
}

void fillEvents(juce::ComboBox& box) {
  box.addItem("Note", 1);
  box.addItem("Controller", 2);
  box.addItem("Program change", 3);
  box.addItem("System exclusive", 4);
}
int eventItem(MidiSourceKind k) {
  return k == MidiSourceKind::ControlChange   ? 2
         : k == MidiSourceKind::ProgramChange ? 3
         : k == MidiSourceKind::SysEx         ? 4
                                              : 1;
}
MidiSourceKind eventKind(int item) {
  return item == 2 ? MidiSourceKind::ControlChange
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
    box.addItem("Any note", kAnyNoteItem);
    if (selected == kAnyNote || selected == kAnyNoteItem - 1) {
      for (int n = 0; n < 128; ++n) box.addItem(noteName(n), n + 1);
      box.setSelectedId(kAnyNoteItem, juce::dontSendNotification);
      return;
    }
  }
  // A system exclusive message is identified by its bytes, taken by Listen.
  if (kind == MidiSourceKind::SysEx) {
    box.addItem("Learned message", 1);
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
  if (any) box.addItem("Any", 1);
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
    device_.addItem("Any console", 1);
    const auto& names = p.midiMap().devices().names();
    for (size_t i = 0; i < names.size(); ++i)
      device_.addItem(juce::String(p.midiMap().devices().displayName(static_cast<int>(i) + 1)),
                      static_cast<int>(i) + 2);
    device_.setSelectedId(b.source.deviceId + 1, juce::dontSendNotification);
    if (device_.getSelectedId() == 0) device_.setSelectedId(1, juce::dontSendNotification);

    fillChannels(channel_, true);
    channel_.setSelectedId(b.source.channel + 1, juce::dontSendNotification);
    fillEvents(event_);
    const bool set = b.source.kind != MidiSourceKind::None;
    if (set) event_.setSelectedId(eventItem(b.source.kind), juce::dontSendNotification);
    fillNumbers(number_, set ? b.source.kind : MidiSourceKind::Note, b.source.number,
                kind_ == MidiTargetKind::ContinuousControl);
    if (!set) {
      event_.setTextWhenNothingSelected("Listen...");
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
      changed();
    };

    if (kind == MidiTargetKind::Switch) {
      action_.addItem("Each press toggles", 1);
      action_.addItem("Held while pressed", 2);
      action_.addItem("Draws it", 3);
      action_.addItem("Cancels it", 4);
      action_.setSelectedId(b.trigger == MidiTrigger::Momentary     ? 2
                            : b.trigger == MidiTrigger::EngageOnly    ? 3
                            : b.trigger == MidiTrigger::DisengageOnly ? 4
                                                                      : 1,
                            juce::dontSendNotification);
      addAndMakeVisible(action_);
      action_.onChange = [this] { changed(); };
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
    delete_.setTooltip("Remove this event");
    delete_.onClick = [this] { if (onDelete) onDelete(); };
  }

  static std::vector<std::pair<juce::String, int>> columns(MidiTargetKind kind) {
    if (kind == MidiTargetKind::Switch)
      return {{"Device", 150}, {"Channel", 64}, {"Event", 116}, {"Number", 96},
              {"What it does", 150}, {"", 84}, {"", 28}};
    return {{"Device", 150}, {"Channel", 64}, {"Event", 116}, {"Number", 96},
            {"Closed at", 72}, {"Open at", 72}, {"", 60}, {"", 84}, {"", 28}};
  }

  void resized() override {
    const auto cols = columns(kind_);
    std::vector<std::pair<juce::Component*, int>> cells{
        {&device_, cols[0].second}, {&channel_, cols[1].second},
        {&event_, cols[2].second}, {&number_, cols[3].second}};
    if (kind_ == MidiTargetKind::Switch) {
      cells.push_back({&action_, cols[4].second});
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
    listen_.setButtonText(on ? "Listening..." : "Listen");
    listen_.setColour(juce::TextButton::buttonColourId,
                      on ? juce::Colours::darkorange : juce::Colour(0xff2a2f38));
  }

  MidiBinding read() const {
    MidiBinding b = binding_;
    b.source.deviceId = juce::jmax(0, device_.getSelectedId() - 1);
    b.source.channel = juce::jmax(0, channel_.getSelectedId() - 1);
    if (event_.getSelectedId() > 0) {
      b.source.kind = eventKind(event_.getSelectedId());
      b.source.number = b.source.kind == MidiSourceKind::SysEx ? binding_.source.number
                        : number_.getSelectedId() == kAnyNoteItem    ? kAnyNote
                                                                      : juce::jmax(0, number_.getSelectedId() - 1);
    }
    if (kind_ == MidiTargetKind::Switch) {
      const int a = action_.getSelectedId();
      b.trigger = a == 2   ? MidiTrigger::Momentary
                  : a == 3 ? MidiTrigger::EngageOnly
                  : a == 4 ? MidiTrigger::DisengageOnly
                           : MidiTrigger::Toggle;
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

  MidiTargetKind kind_;
  MidiBinding binding_;
  juce::ComboBox device_, channel_, event_, number_, action_;
  juce::Slider low_, high_;
  juce::ToggleButton invert_{"Invert"};
  juce::TextButton listen_{"Listen"}, delete_{"X"};
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
    styleNote(empty_, "Nothing works this yet. Press New event, then press the button, pull the "
                      "drawstop or move the pedal on your console.");
    addAndMakeVisible(note_);
    styleNote(note_,
              kind == MidiTargetKind::Switch
                  ? "Add a row for every message that works this. A rocker tab or a console "
                    "with separate draw and cancel buttons is two rows: one that draws it and "
                    "one that cancels it. Listen fills a row from the next message your "
                    "console sends. A message already used by something else moves here. "
                    "Saved with this organ."
                  : "Add a row for every pedal or knob that moves this. Closed at and Open "
                    "at are the values your pedal sends at its two ends, when it does not "
                    "travel the whole 0 to 127; Invert turns a pedal wired backwards around. "
                    "One pedal can move several shoes. Saved with this organ.");
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
      if (listening_ < static_cast<int>(rows_.size())) rows_[static_cast<size_t>(listening_)].source = heard;
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
  juce::TextButton add_{"New event"};
  juce::Label empty_, note_;
};

// --------------------------------------------------------------------- Send

class SendRow : public juce::Component {
public:
  std::function<void()> onChange, onDelete;

  SendRow(MidiTargetKind kind, const MidiSend& s) : kind_(kind), send_(s) {
    fillEvents(event_);
    event_.setSelectedId(eventItem(s.kind), juce::dontSendNotification);
    fillChannels(channel_, false);
    channel_.setSelectedId(juce::jlimit(1, 16, s.channel) + 1, juce::dontSendNotification);
    addAndMakeVisible(channel_);
    channel_.onChange = [this] { changed(); };

    if (kind == MidiTargetKind::Keyboard) {
      // The keys go out as notes, moved by this much.
      styleValue(transpose_, -48, 48);
      transpose_.setValue(s.number, juce::dontSendNotification);
      addAndMakeVisible(transpose_);
      transpose_.onValueChange = [this] { changed(); };
    } else {
      fillNumbers(number_, s.kind, s.number);
      addAndMakeVisible(event_);
      addAndMakeVisible(number_);
      event_.onChange = [this] {
        fillNumbers(number_, eventKind(event_.getSelectedId()), number_.getSelectedId() - 1);
        changed();
      };
      number_.onChange = [this] { changed(); };
      // A switch: what it sends on and off, and "none" for a console that
      // only wants to hear about the lamp coming on. A control: its range.
      styleValue(high_, 0, 127);
      styleValue(low_, kind == MidiTargetKind::Switch ? -1 : 0, 127);
      low_.textFromValueFunction = [](double v) {
        return v < 0 ? juce::String("none") : juce::String(static_cast<int>(v));
      };
      low_.valueFromTextFunction = [](const juce::String& t) {
        return t.trim().equalsIgnoreCase("none") ? -1.0 : t.getDoubleValue();
      };
      high_.setValue(s.high, juce::dontSendNotification);
      low_.setValue(s.low, juce::dontSendNotification);
      low_.updateText();
      addAndMakeVisible(high_);
      addAndMakeVisible(low_);
      high_.onValueChange = [this] { changed(); };
      low_.onValueChange = [this] { changed(); };
    }
    addAndMakeVisible(delete_);
    delete_.setTooltip("Remove this event");
    delete_.onClick = [this] { if (onDelete) onDelete(); };
  }

  static std::vector<std::pair<juce::String, int>> columns(MidiTargetKind kind) {
    if (kind == MidiTargetKind::Keyboard)
      return {{"Channel", 64}, {"Transpose", 110}, {"", 28}};
    if (kind == MidiTargetKind::Switch)
      return {{"Event", 116}, {"Channel", 64}, {"Number", 96}, {"When on", 80},
              {"When off", 80}, {"", 28}};
    return {{"Event", 116}, {"Channel", 64}, {"Number", 96}, {"Closed", 80},
            {"Open", 80}, {"", 28}};
  }

  void resized() override {
    const auto cols = columns(kind_);
    if (kind_ == MidiTargetKind::Keyboard)
      layoutRow(getLocalBounds().reduced(0, 2),
                {{&channel_, cols[0].second}, {&transpose_, cols[1].second},
                 {&delete_, cols[2].second}});
    else
      layoutRow(getLocalBounds().reduced(0, 2),
                {{&event_, cols[0].second}, {&channel_, cols[1].second},
                 {&number_, cols[2].second},
                 {kind_ == MidiTargetKind::Switch ? &high_ : &low_, cols[3].second},
                 {kind_ == MidiTargetKind::Switch ? &low_ : &high_, cols[4].second},
                 {&delete_, cols[5].second}});
  }

  MidiSend read() const {
    MidiSend s = send_;
    s.channel = juce::jmax(1, channel_.getSelectedId() - 1);
    if (kind_ == MidiTargetKind::Keyboard) {
      s.kind = MidiSourceKind::Note;
      s.number = static_cast<int>(transpose_.getValue());
    } else {
      s.kind = eventKind(event_.getSelectedId());
      s.number = juce::jmax(0, number_.getSelectedId() - 1);
      s.low = static_cast<int>(low_.getValue());
      s.high = static_cast<int>(high_.getValue());
    }
    return s;
  }

private:
  void changed() {
    send_ = read();
    if (onChange) onChange();
  }

  MidiTargetKind kind_;
  MidiSend send_;
  juce::ComboBox event_, channel_, number_;
  juce::Slider low_, high_, transpose_;
  juce::TextButton delete_{"X"};
};

// What the console is sent when this object changes.
class SendPanel : public juce::Component, private juce::Timer {
public:
  SendPanel(MasterpieceProcessor& p, MidiTargetKind kind, Id id) : proc_(p), kind_(kind), id_(id) {
    rows_ = p.midiMap().sendsFor(kind, id);
    addAndMakeVisible(header_);
    header_.set(SendRow::columns(kind));
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&list_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(add_);
    add_.onClick = [this] {
      MidiSend s;
      if (kind_ == MidiTargetKind::ContinuousControl) s.kind = MidiSourceKind::ControlChange;
      if (kind_ == MidiTargetKind::Keyboard) s.number = 0;
      // A lamp most often answers on the message that works the stop.
      if (const auto* b = proc_.midiMap().bindingFor(kind_, id_)) {
        if (kind_ != MidiTargetKind::Keyboard) {
          s.kind = b->source.kind;
          s.number = b->source.number;
        }
        if (b->source.channel > 0) s.channel = b->source.channel;
      }
      rows_.push_back(s);
      commit();
      rebuild();
    };
    addAndMakeVisible(empty_);
    styleNote(empty_, "Nothing is sent yet.");
    addAndMakeVisible(output_);
    output_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(note_);
    styleNote(note_,
              kind == MidiTargetKind::Switch
                  ? "Sent whenever this comes on or goes off, however it moved: by your "
                    "console, a piston, the crescendo or a click. For the lamp in a drawstop "
                    "or an LED by a piston. When off \"none\" sends nothing when it goes off. "
                    "Saved with this organ."
              : kind == MidiTargetKind::Keyboard
                  ? "Every key played on this manual is sent as a note on the channel, moved "
                    "by Transpose: to play a sound module or a second program from it. Saved "
                    "with this organ."
                  : "Sent whenever this moves, across Closed to Open: for a motorised fader, "
                    "or a display that shows the swell box. Saved with this organ.");
    rebuild();
    showOutput();
    startTimerHz(2);
  }

  void paint(juce::Graphics& g) override { g.fillAll(kBg); }

  void resized() override {
    auto r = getLocalBounds().reduced(12);
    output_.setBounds(r.removeFromTop(22));
    r.removeFromTop(6);
    header_.setBounds(r.removeFromTop(20));
    note_.setBounds(r.removeFromBottom(52));
    r.removeFromBottom(6);
    add_.setBounds(r.removeFromBottom(kRow + 2).removeFromLeft(140));
    r.removeFromBottom(6);
    viewport_.setBounds(r);
    empty_.setBounds(r.removeFromTop(30));
    layoutList();
  }

private:
  void timerCallback() override { showOutput(); }

  void showOutput() {
    const auto* out = proc_.midiOutput();
    output_.setText(out != nullptr ? "Sent to " + out->getName()
                                   : "No MIDI output is chosen: choose one in Settings, MIDI.",
                    juce::dontSendNotification);
    output_.setColour(juce::Label::textColourId, out != nullptr ? kText : juce::Colours::orange);
  }

  void layoutList() {
    const int w = juce::jmax(100, viewport_.getMaximumVisibleWidth());
    list_.setSize(w, static_cast<int>(rowViews_.size()) * kRow);
    for (size_t i = 0; i < rowViews_.size(); ++i)
      rowViews_[i]->setBounds(0, static_cast<int>(i) * kRow, w, kRow);
  }

  void rebuild() {
    rowViews_.clear();
    for (size_t i = 0; i < rows_.size(); ++i) {
      auto row = std::make_unique<SendRow>(kind_, rows_[i]);
      SendRow* raw = row.get();
      row->onChange = [this, i, raw] {
        rows_[i] = raw->read();
        commit();
      };
      row->onDelete = [this, i] {
        rows_.erase(rows_.begin() + static_cast<long>(i));
        commit();
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this)] {
          if (safe != nullptr) safe->rebuild();
        });
      };
      list_.addAndMakeVisible(*row);
      rowViews_.push_back(std::move(row));
    }
    empty_.setVisible(rows_.empty());
    layoutList();
  }

  void commit() { proc_.setObjectSends(kind_, id_, rows_); }

  MasterpieceProcessor& proc_;
  MidiTargetKind kind_;
  Id id_;
  std::vector<MidiSend> rows_;
  std::vector<std::unique_ptr<SendRow>> rowViews_;
  Header header_;
  juce::Viewport viewport_;
  juce::Component list_;
  juce::TextButton add_{"New event"};
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
    setButtonText(capturing_ ? "Press a key..." : key_.isEmpty() ? "None" : key_);
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
    upLabel_.setText(control ? "Opens it" : "Draws or cancels it", juce::dontSendNotification);
    upLabel_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(upKey_);
    upKey_.setKey(up_);
    upKey_.onKey = [this](const juce::String& k) { up_ = k; commit(); };
    addAndMakeVisible(clearUp_);
    clearUp_.onClick = [this] { up_.clear(); upKey_.setKey({}); commit(); };
    if (control) {
      addAndMakeVisible(downLabel_);
      downLabel_.setText("Closes it", juce::dontSendNotification);
      downLabel_.setColour(juce::Label::textColourId, kText);
      addAndMakeVisible(downKey_);
      downKey_.setKey(down_);
      downKey_.onKey = [this](const juce::String& k) { down_ = k; commit(); };
      addAndMakeVisible(clearDown_);
      clearDown_.onClick = [this] { down_.clear(); downKey_.setKey({}); commit(); };
      addAndMakeVisible(stepLabel_);
      stepLabel_.setText("Each press moves it by", juce::dontSendNotification);
      stepLabel_.setColour(juce::Label::textColourId, kText);
      styleValue(step_, 1, 64);
      if (step_.getValue() < 1) step_.setValue(8, juce::dontSendNotification);
      addAndMakeVisible(step_);
      step_.onValueChange = [this] { commit(); };
    }
    addAndMakeVisible(note_);
    styleNote(note_, "Click the button, then press the key, with Ctrl, Alt or Shift if you "
                     "like. Escape leaves it as it was. The key works while the console window "
                     "is in front. One key can work several things at once. Saved with this "
                     "organ.");
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
  juce::TextButton clearUp_{"Clear"}, clearDown_{"Clear"};
  juce::Slider step_;
};

// GrandOrgue's shape: one window per object, a tab per direction.
class MidiEventContent : public juce::Component {
public:
  MidiEventContent(MasterpieceProcessor& p, MidiTargetKind kind, Id id)
      : tabs_(juce::TabbedButtonBar::TabsAtTop) {
    addAndMakeVisible(tabs_);
    tabs_.addTab("Receive", kBg, new ReceivePanel(p, kind, id), true);
    tabs_.addTab("Send", kBg, new SendPanel(p, kind, id), true);
    tabs_.addTab("Shortcut", kBg, new ShortcutPanel(p, kind, id), true);
  }
  void resized() override { tabs_.setBounds(getLocalBounds()); }

private:
  juce::TabbedComponent tabs_;
};

} // namespace

void MidiEventDialog::show(MasterpieceProcessor& p, MidiTargetKind kind, Id id) {
  auto content = std::make_unique<MidiEventContent>(p, kind, id);
  content->setSize(kind == MidiTargetKind::Switch ? 760 : 840, 460);
  juce::DialogWindow::LaunchOptions o;
  o.content.setOwned(content.release());
  o.dialogTitle = objectName(p, kind, id) + " - MIDI";
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
