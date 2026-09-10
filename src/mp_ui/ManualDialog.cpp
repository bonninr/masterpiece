#include "ManualDialog.h"

namespace mp::ui {
namespace {

constexpr int kRow = 26;

juce::String noteName(int n) {
  static const char* kNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                 "F#", "G",  "G#", "A",  "A#", "B"};
  if (n < 0 || n > 127) return "-";
  return juce::String(kNames[n % 12]) + juce::String(n / 12 - 1) + " (" +
         juce::String(n) + ")";
}

void styleLabel(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setColour(juce::Label::textColourId, juce::Colour(0xffb9c2d0));
}

void styleSlider(juce::Slider& s, double lo, double hi, double step) {
  s.setSliderStyle(juce::Slider::LinearHorizontal);
  s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 20);
  s.setRange(lo, hi, step);
}

// One row per binding, described the way a player would describe it.
class BindingList : public juce::ListBoxModel {
public:
  BindingList(MasterpieceProcessor& p, Id kb) : proc_(p), keyboardId_(kb) {}

  int getNumRows() override { return static_cast<int>(rows().size()); }

  void paintListBoxItem(int row, juce::Graphics& g, int w, int h,
                        bool selected) override {
    const auto& list = rows();
    if (row < 0 || row >= static_cast<int>(list.size())) return;
    if (selected) g.fillAll(juce::Colour(0xff2f3a4d));
    g.setColour(juce::Colour(0xffdfe6f0));
    g.setFont(13.0f);

    const auto& b = list[static_cast<size_t>(row)];
    juce::String text;
    text << (b.deviceId == 0 ? juce::String("Any device")
                             : juce::String(proc_.midiDevices().nameFor(b.deviceId)));
    text << "  ch " << (b.channel == 0 ? juce::String("any")
                                       : juce::String(b.channel));
    text << "  keys " << noteName(b.lowKey) << " to " << noteName(b.highKey);
    if (b.transpose != 0)
      text << "  transposed " << (b.transpose > 0 ? "+" : "") << b.transpose;
    if (b.ignoreVelocity) text << "  no velocity";
    if (b.shortOctave) text << "  short octave";
    if (b.debounceMs > 0) text << "  debounce " << b.debounceMs << " ms";
    g.drawText(text, 6, 0, w - 12, h, juce::Justification::centredLeft, true);
  }

  // Only the bindings for this manual, and their indices in the full list.
  std::vector<MidiMap::KeyboardBinding> rows() const {
    std::vector<MidiMap::KeyboardBinding> out;
    for (const auto& b : proc_.midiMap().keyboardBindings())
      if (b.keyboardId == keyboardId_) out.push_back(b);
    return out;
  }

private:
  MasterpieceProcessor& proc_;
  Id keyboardId_;
};

} // namespace

ManualDialog::ManualDialog(MasterpieceProcessor& p, Id keyboardId)
    : proc_(p), keyboardId_(keyboardId) {
  addAndMakeVisible(title_);
  styleLabel(title_, "What plays " + juce::String(proc_.keyboardName(keyboardId)));
  title_.setFont(juce::FontOptions(16.0f));

  model_ = std::make_unique<BindingList>(proc_, keyboardId_);
  addAndMakeVisible(list_);
  list_.setModel(model_.get());
  list_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff141820));
  list_.setRowHeight(22);

  addAndMakeVisible(add_);
  add_.onClick = [this] { addBinding(); };
  addAndMakeVisible(remove_);
  remove_.onClick = [this] {
    // Rebuild the whole manual's list without the selected row: the bindings
    // live in one flat vector and there is no stable handle into it.
    auto* bl = static_cast<BindingList*>(model_.get());
    const auto mine = bl->rows();
    const int row = selected();
    if (row < 0 || row >= static_cast<int>(mine.size())) return;
    proc_.midiMap().removeKeyboardBindingsFor(keyboardId_);
    for (int i = 0; i < static_cast<int>(mine.size()); ++i)
      if (i != row) proc_.midiMap().addKeyboardBinding(mine[static_cast<size_t>(i)]);
    proc_.saveMidiMap();
    rebuildList();
  };

  addAndMakeVisible(learn_);
  learn_.setTooltip("Press the lowest key you want, then the highest");
  learn_.onClick = [this] {
    // Learning a manual is two presses: the ends of the range. Anything in
    // between follows, and the transpose is worked out from where the organ's
    // own compass starts.
    proc_.beginKeyboardLearn(keyboardId_);
  };

  addAndMakeVisible(deviceLabel_);
  styleLabel(deviceLabel_, "Console");
  addAndMakeVisible(device_);
  device_.addItem("Any device", 1);
  for (size_t i = 0; i < proc_.midiDevices().names().size(); ++i)
    device_.addItem(juce::String(proc_.midiDevices().names()[i]),
                    static_cast<int>(i) + 2);
  device_.onChange = [this] { applyEdits(); };

  addAndMakeVisible(channelLabel_);
  styleLabel(channelLabel_, "Channel");
  addAndMakeVisible(channel_);
  channel_.addItem("Any channel", 1);
  for (int c = 1; c <= 16; ++c) channel_.addItem(juce::String(c), c + 1);
  channel_.onChange = [this] { applyEdits(); };

  addAndMakeVisible(rangeLabel_);
  styleLabel(rangeLabel_, "Keys");
  for (auto* sl : {&lowKey_, &highKey_}) {
    addAndMakeVisible(*sl);
    styleSlider(*sl, 0, 127, 1);
    sl->textFromValueFunction = [](double v) { return noteName(static_cast<int>(v)); };
    sl->onValueChange = [this] { applyEdits(); };
  }

  addAndMakeVisible(transposeLabel_);
  styleLabel(transposeLabel_, "Transpose");
  addAndMakeVisible(transpose_);
  styleSlider(transpose_, -48, 48, 1);
  transpose_.setTextValueSuffix(" semitones");
  transpose_.onValueChange = [this] { applyEdits(); };

  addAndMakeVisible(velocityLabel_);
  styleLabel(velocityLabel_, "Velocity");
  for (auto* sl : {&lowVel_, &highVel_}) {
    addAndMakeVisible(*sl);
    styleSlider(*sl, 0, 127, 1);
    sl->onValueChange = [this] { applyEdits(); };
  }

  addAndMakeVisible(ignoreVel_);
  ignoreVel_.onClick = [this] { applyEdits(); };
  addAndMakeVisible(shortOctave_);
  shortOctave_.onClick = [this] { applyEdits(); };

  addAndMakeVisible(debounceLabel_);
  styleLabel(debounceLabel_, "Debounce");
  addAndMakeVisible(debounce_);
  styleSlider(debounce_, 0, 200, 1);
  debounce_.setTextValueSuffix(" ms");
  debounce_.onValueChange = [this] { applyEdits(); };

  addAndMakeVisible(note_);
  note_.setColour(juce::Label::textColourId, juce::Colour(0xff8b93a3));
  note_.setJustificationType(juce::Justification::topLeft);
  note_.setText(
      "A manual can have several of these. One keyboard split across two "
      "manuals is two entries, and so is a manual played from two consoles.\n\n"
      "An inverted velocity range (low above high) reverses the sense, which "
      "is how a normally-closed key contact is read.",
      juce::dontSendNotification);
  note_.setFont(juce::FontOptions(12.0f));

  list_.updateContent();
  if (model_->getNumRows() > 0) list_.selectRow(0);
  startTimerHz(4);
}

ManualDialog::~ManualDialog() { stopTimer(); }

int ManualDialog::selected() const { return list_.getSelectedRow(); }

void ManualDialog::rebuildList() {
  const int row = selected();
  list_.updateContent();
  list_.selectRow(juce::jlimit(0, juce::jmax(0, model_->getNumRows() - 1), row));
  timerCallback();
}

void ManualDialog::addBinding() {
  MidiMap::KeyboardBinding b;
  b.keyboardId = keyboardId_;
  // A sensible starting point: this manual's own compass, if the organ says
  // what it is, and the whole of MIDI if it does not.
  const auto& kbs = proc_.organModel().keyboards;
  const auto it = kbs.find(keyboardId_);
  if (it != kbs.end() && it->second.numKeys > 0) {
    b.lowKey = it->second.firstMidiNote;
    b.highKey = it->second.firstMidiNote + it->second.numKeys - 1;
  }
  proc_.midiMap().addKeyboardBinding(b);
  proc_.saveMidiMap();
  list_.updateContent();
  list_.selectRow(model_->getNumRows() - 1);
  timerCallback();
}

void ManualDialog::applyEdits() {
  if (loading_) return;
  auto* bl = static_cast<BindingList*>(model_.get());
  auto mine = bl->rows();
  const int row = selected();
  if (row < 0 || row >= static_cast<int>(mine.size())) return;

  MidiMap::KeyboardBinding& b = mine[static_cast<size_t>(row)];
  b.deviceId = device_.getSelectedId() - 1;
  b.channel = channel_.getSelectedId() - 1;
  b.lowKey = static_cast<int>(lowKey_.getValue());
  b.highKey = static_cast<int>(highKey_.getValue());
  b.transpose = static_cast<int>(transpose_.getValue());
  b.lowVelocity = static_cast<int>(lowVel_.getValue());
  b.highVelocity = static_cast<int>(highVel_.getValue());
  b.ignoreVelocity = ignoreVel_.getToggleState();
  b.shortOctave = shortOctave_.getToggleState();
  b.debounceMs = static_cast<int>(debounce_.getValue());

  proc_.midiMap().removeKeyboardBindingsFor(keyboardId_);
  for (const auto& x : mine) proc_.midiMap().addKeyboardBinding(x);
  proc_.saveMidiMap();
  list_.repaint();
}

void ManualDialog::timerCallback() {
  auto* bl = static_cast<BindingList*>(model_.get());
  const auto mine = bl->rows();
  const int row = selected();
  const bool has = row >= 0 && row < static_cast<int>(mine.size());

  const std::initializer_list<juce::Component*> editable{
      &device_,   &channel_,   &lowKey_,      &highKey_,    &transpose_,
      &lowVel_,   &highVel_,   &ignoreVel_,   &shortOctave_, &debounce_,
      &remove_};
  for (juce::Component* c : editable) c->setEnabled(has);
  if (!has) return;

  // Filling the controls must not read straight back as an edit.
  const juce::ScopedValueSetter<bool> guard(loading_, true);
  const auto& b = mine[static_cast<size_t>(row)];
  device_.setSelectedId(b.deviceId + 1, juce::dontSendNotification);
  channel_.setSelectedId(b.channel + 1, juce::dontSendNotification);
  lowKey_.setValue(b.lowKey, juce::dontSendNotification);
  highKey_.setValue(b.highKey, juce::dontSendNotification);
  transpose_.setValue(b.transpose, juce::dontSendNotification);
  lowVel_.setValue(b.lowVelocity, juce::dontSendNotification);
  highVel_.setValue(b.highVelocity, juce::dontSendNotification);
  ignoreVel_.setToggleState(b.ignoreVelocity, juce::dontSendNotification);
  shortOctave_.setToggleState(b.shortOctave, juce::dontSendNotification);
  debounce_.setValue(b.debounceMs, juce::dontSendNotification);

  learn_.setButtonText(proc_.keyboardLearning() == keyboardId_
                           ? "Press the lowest key..."
                           : "Learn from a key");
}

void ManualDialog::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff1b1e24)); }

void ManualDialog::resized() {
  auto r = getLocalBounds().reduced(12);
  title_.setBounds(r.removeFromTop(24));
  r.removeFromTop(6);

  auto listArea = r.removeFromTop(110);
  list_.setBounds(listArea);
  r.removeFromTop(6);
  auto buttons = r.removeFromTop(kRow);
  add_.setBounds(buttons.removeFromLeft(80));
  buttons.removeFromLeft(6);
  remove_.setBounds(buttons.removeFromLeft(90));
  buttons.removeFromLeft(6);
  learn_.setBounds(buttons.removeFromLeft(180));
  r.removeFromTop(10);

  const auto field = [&](juce::Label& l, juce::Component& c, int labelW = 90) {
    auto row = r.removeFromTop(kRow);
    l.setBounds(row.removeFromLeft(labelW));
    c.setBounds(row);
    r.removeFromTop(4);
  };
  field(deviceLabel_, device_);
  field(channelLabel_, channel_);

  auto keysRow = r.removeFromTop(kRow);
  rangeLabel_.setBounds(keysRow.removeFromLeft(90));
  lowKey_.setBounds(keysRow.removeFromLeft(keysRow.getWidth() / 2));
  highKey_.setBounds(keysRow);
  r.removeFromTop(4);

  field(transposeLabel_, transpose_);

  auto velRow = r.removeFromTop(kRow);
  velocityLabel_.setBounds(velRow.removeFromLeft(90));
  lowVel_.setBounds(velRow.removeFromLeft(velRow.getWidth() / 2));
  highVel_.setBounds(velRow);
  r.removeFromTop(4);

  field(debounceLabel_, debounce_);
  ignoreVel_.setBounds(r.removeFromTop(kRow));
  shortOctave_.setBounds(r.removeFromTop(kRow));
  r.removeFromTop(8);
  note_.setBounds(r);
}

void ManualDialog::show(MasterpieceProcessor& p, Id keyboardId) {
  auto content = std::make_unique<ManualDialog>(p, keyboardId);
  content->setSize(560, 560);
  juce::DialogWindow::LaunchOptions o;
  o.content.setOwned(content.release());
  o.dialogTitle = "Manual assignment";
  o.dialogBackgroundColour = juce::Colour(0xff1b1e24);
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = true;
  o.launchAsync();
}

} // namespace mp::ui
