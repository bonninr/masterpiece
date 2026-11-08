#include "ConsoleProfileWindow.h"

#include <algorithm>

#include "MidiEventDialog.h"
#include "Mobile.h"

namespace mp::ui {

namespace {
const juce::Colour kBack(0xff15171c);
const juce::Colour kList(0xff1d2027);
const juce::Colour kText(0xffb9c2d0);
const juce::Colour kDim(0xff8a93a3);
const juce::Colour kArmed(0xff2d4f7c);
const juce::Colour kStrong(0xff6fbf73);
const juce::Colour kWeak(0xffe0a040);
const juce::Colour kRelated(0xff8fa8d8);  // a stop of a related sound
constexpr int kMargin = 10, kRowH = 26, kButtonH = 28;
constexpr int kKeyboards = 6;

juce::String keyboardText(int k) {
  static const char* const roman[] = {"I", "II", "III", "IV", "V", "VI"};
  if (k == 0) return TRANS("Pedal");
  if (k >= 1 && k <= kKeyboards) return roman[k - 1];
  return TRANS("Any keyboard");
}

juce::String kindText(ConsoleTabKind k) {
  switch (k) {
    // "Stop" alone is the halt button's word in the translations.
    case ConsoleTabKind::Stop: return TRANS("Speaking stop");
    case ConsoleTabKind::Coupler: return TRANS("Coupler");
    case ConsoleTabKind::Tremulant: return TRANS("Tremulant");
    case ConsoleTabKind::Piston: return TRANS("Piston");
  }
  return {};
}

// "Bourdon 8 (II)", "Swell to Great (I from II)".
juce::String tabDescription(const ConsoleTab& t) {
  auto s = juce::String::fromUTF8(t.label.c_str());
  if (t.kind == ConsoleTabKind::Coupler && t.keyboard >= 0 && t.fromKeyboard >= 0)
    return s + " (" +
           TRANS("%1 from %2").replace("%1", keyboardText(t.keyboard)).replace("%2", keyboardText(t.fromKeyboard)) +
           ")";
  if (t.keyboard >= 0) return s + " (" + keyboardText(t.keyboard) + ")";
  return s;
}

void fillKeyboards(juce::ComboBox& box) {
  box.clear(juce::dontSendNotification);
  box.addItem(TRANS("Any keyboard"), 1);
  for (int k = 0; k <= kKeyboards; ++k) box.addItem(keyboardText(k), k + 2);
}
}  // namespace

// ---- matches ---------------------------------------------------------------------

ProfileMatchPanel::ProfileMatchPanel(MasterpieceProcessor& p) : proc_(p) {
  title_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
  title_.setColour(juce::Label::textColourId, juce::Colours::white);
  addAndMakeVisible(title_);
  note_.setColour(juce::Label::textColourId, kDim);
  note_.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(note_);
  list_.setModel(this);
  list_.setRowHeight(kRowH);
  list_.setColour(juce::ListBox::backgroundColourId, kList);
  list_.setOutlineThickness(0);
  addAndMakeVisible(list_);
  apply_.onClick = [this] {
    const int n = applyTicked();
    setNote(TRANS("%1 mapped").replace("%1", juce::String(n)));
  };
  all_.onClick = [this] {
    for (size_t i = 0; i < ticks_.size(); ++i) ticks_[i] = true;
    list_.repaint();
    showState();
  };
  addAndMakeVisible(apply_);
  addAndMakeVisible(all_);
  refresh();
}

bool ProfileMatchPanel::refresh() {
  file_ = activeConsoleProfile();
  profile_ = ConsoleProfile();
  matches_.clear();
  ticks_.clear();
  controls_.clear();
  const bool have = file_.existsAsFile() && loadConsoleProfile(file_, profile_);
  if (have) {
    controls_ = organControls(proc_);
    matches_ = profileMatches(proc_, profile_, controls_);
    for (const auto& m : matches_) ticks_.push_back(m.strong);
  }
  title_.setText(have ? juce::String::fromUTF8(profile_.name.c_str()) : TRANS("No console profile in use"),
                 juce::dontSendNotification);
  list_.updateContent();
  list_.repaint();
  showState();
  return have;
}

int ProfileMatchPanel::applyStrong() {
  std::vector<ProfileMatch> chosen;
  for (const auto& m : matches_)
    if (m.strong) chosen.push_back(m);
  const int n = applyProfileMatches(proc_, profile_, controls_, chosen);
  refresh();
  return n;
}

int ProfileMatchPanel::applyTicked() {
  std::vector<ProfileMatch> chosen;
  for (size_t i = 0; i < matches_.size(); ++i)
    if (ticks_[i]) chosen.push_back(matches_[i]);
  const int n = applyProfileMatches(proc_, profile_, controls_, chosen);
  refresh();
  return n;
}

void ProfileMatchPanel::setTicked(int i, bool on) {
  if (i < 0 || i >= matchCount()) return;
  ticks_[static_cast<size_t>(i)] = on;
  list_.repaintRow(i);
  showState();
}

juce::String ProfileMatchPanel::tabText(int i) const { return tabDescription(profile_.tabs[match(i).tab]); }

juce::String ProfileMatchPanel::controlText(int i) const {
  const auto& c = controls_[match(i).control];
  auto s = juce::String::fromUTF8(c.tab.label.c_str());
  return c.section.empty() ? s : s + " (" + juce::String::fromUTF8(c.section.c_str()) + ")";
}

void ProfileMatchPanel::showState() {
  int ticked = 0;
  for (const bool t : ticks_) ticked += t ? 1 : 0;
  apply_.setEnabled(ticked > 0);
  all_.setEnabled(ticked < matchCount());
  if (note_.getText().isEmpty() || matchCount() == 0)
    note_.setText(matchCount() == 0 ? TRANS("Every tab of the profile that fits this organ is mapped.")
                                    : TRANS("Tick a match to map it. Your own mappings stay as they are."),
                  juce::dontSendNotification);
}

void ProfileMatchPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
  if (row < 0 || row >= matchCount()) return;
  if (selected) g.fillAll(kArmed.withAlpha(0.5f));
  const auto box = juce::Rectangle<float>(6.0f, (height - 14) / 2.0f, 14.0f, 14.0f);
  g.setColour(kText);
  g.drawRect(box, 1.0f);
  if (ticked(row)) g.fillRect(box.reduced(3.0f));
  const int w = width - 30 - 70;
  g.setFont(juce::FontOptions(14.0f));
  g.setColour(juce::Colours::white);
  g.drawText(tabText(row), 30, 0, w / 2 - 10, height, juce::Justification::centredLeft, true);
  g.setColour(kText);
  g.drawText(controlText(row), 30 + w / 2, 0, w / 2, height, juce::Justification::centredLeft, true);
  const auto& m = match(row);
  g.setColour(m.strong ? kStrong : m.related ? kRelated : kWeak);
  g.drawText(m.strong ? TRANS("Strong") : m.related ? TRANS("Related") : TRANS("Weak"), width - 66, 0, 60,
             height, juce::Justification::centredRight);
}

void ProfileMatchPanel::listBoxItemClicked(int row, const juce::MouseEvent&) { setTicked(row, !ticked(row)); }

void ProfileMatchPanel::paint(juce::Graphics& g) { g.fillAll(kBack); }

void ProfileMatchPanel::resized() {
  auto r = getLocalBounds().reduced(kMargin);
  title_.setBounds(r.removeFromTop(28));
  note_.setBounds(r.removeFromTop(36));
  auto buttons = r.removeFromBottom(kButtonH);
  r.removeFromBottom(kMargin);
  apply_.setBounds(buttons.removeFromRight(130));
  buttons.removeFromRight(kMargin);
  all_.setBounds(buttons.removeFromRight(110));
  list_.setBounds(r);
}

ProfileMatchWindow::ProfileMatchWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow(TRANS("Map from console profile"), kBack, juce::DocumentWindow::closeButton),
      panel_(std::make_unique<ProfileMatchPanel>(p)) {
  setUsingNativeTitleBar(true);
  setResizable(true, false);
  setContentNonOwned(panel_.get(), false);
  setSize(640, 520);
  fitToScreen(*this);
}

ProfileMatchWindow::~ProfileMatchWindow() { clearContentComponent(); }

void ProfileMatchWindow::closeButtonPressed() { showFloating(*this, false, false); }

// ---- the profile -----------------------------------------------------------------

class ConsoleProfilePanel::Row : public juce::Component {
public:
  explicit Row(ConsoleProfilePanel& owner) : owner_(owner) {
    for (auto k : {ConsoleTabKind::Stop, ConsoleTabKind::Coupler, ConsoleTabKind::Tremulant, ConsoleTabKind::Piston})
      kind_.addItem(kindText(k), static_cast<int>(k) + 1);
    kind_.onChange = [this] { owner_.setKind(row_, static_cast<ConsoleTabKind>(kind_.getSelectedId() - 1)); };
    name_.setColour(juce::TextEditor::backgroundColourId, kList);
    name_.setColour(juce::TextEditor::textColourId, juce::Colours::white);
    name_.onReturnKey = [this] { commit(); };
    name_.onFocusLost = [this] { commit(); };
    fillKeyboards(keyboard_);
    fillKeyboards(from_);
    keyboard_.setTooltip(TRANS("The keyboard the tab sits on. A coupler: the keyboard it couples onto."));
    from_.setTooltip(TRANS("The keyboard the coupler brings in"));
    keyboard_.onChange = [this] { owner_.setKeyboard(row_, keyboard_.getSelectedId() - 2); };
    from_.onChange = [this] { owner_.setFromKeyboard(row_, from_.getSelectedId() - 2); };
    midi_.setColour(juce::Label::textColourId, kText);
    learn_.onClick = [this] {
      if (owner_.learningTab() == row_) owner_.stopLearning();
      else owner_.learn(row_);
    };
    remove_.setTooltip(TRANS("Remove this tab"));
    remove_.onClick = [this] { owner_.removeTab(row_); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&kind_, &name_, &keyboard_, &from_, &midi_,
                                                                      &learn_, &remove_})
      addAndMakeVisible(c);
  }

  void show(int row) {
    row_ = row;
    const auto& t = owner_.profile().tabs[static_cast<size_t>(row)];
    kind_.setSelectedId(static_cast<int>(t.kind) + 1, juce::dontSendNotification);
    if (!name_.hasKeyboardFocus(true)) name_.setText(juce::String::fromUTF8(t.label.c_str()), false);
    keyboard_.setSelectedId(t.keyboard + 2, juce::dontSendNotification);
    from_.setSelectedId(t.fromKeyboard + 2, juce::dontSendNotification);
    from_.setVisible(t.kind == ConsoleTabKind::Coupler);
    midi_.setText(owner_.midiText(row), juce::dontSendNotification);
    learn_.setButtonText(owner_.learningTab() == row ? TRANS("Waiting...") : TRANS("Learn"));
    learn_.setColour(juce::TextButton::buttonColourId,
                     owner_.learningTab() == row ? kArmed : getLookAndFeel().findColour(juce::TextButton::buttonColourId));
  }

  void resized() override {
    auto r = getLocalBounds().reduced(2, 1);
    kind_.setBounds(r.removeFromLeft(100));
    r.removeFromLeft(4);
    remove_.setBounds(r.removeFromRight(28));
    r.removeFromRight(4);
    learn_.setBounds(r.removeFromRight(80));
    r.removeFromRight(4);
    midi_.setBounds(r.removeFromRight(110));
    r.removeFromRight(4);
    from_.setBounds(r.removeFromRight(70));
    r.removeFromRight(4);
    keyboard_.setBounds(r.removeFromRight(70));
    r.removeFromRight(4);
    name_.setBounds(r);
  }

private:
  void commit() {
    if (row_ < 0 || row_ >= static_cast<int>(owner_.profile().tabs.size())) return;
    const auto text = name_.getText().trim().toStdString();
    if (text != owner_.profile().tabs[static_cast<size_t>(row_)].label) owner_.setLabel(row_, text);
  }

  ConsoleProfilePanel& owner_;
  int row_ = -1;
  juce::ComboBox kind_;
  juce::TextEditor name_;
  juce::ComboBox keyboard_;
  juce::ComboBox from_;
  juce::Label midi_;
  juce::TextButton learn_{TRANS("Learn")};
  juce::TextButton remove_{"x"};
};

ConsoleProfilePanel::ConsoleProfilePanel(MasterpieceProcessor& p) : proc_(p) {
  chooserLabel_.setText(TRANS("Console profile"), juce::dontSendNotification);
  chooserLabel_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(chooserLabel_);
  chooser_.onChange = [this] {
    const int i = chooser_.getSelectedId() - 2;
    useProfile(i >= 0 && i < files_.size() ? files_[i] : juce::File());
  };
  addAndMakeVisible(chooser_);
  new_.onClick = [this] {
    askName(TRANS("New console profile"), TRANS("My console"), [this](juce::String name) {
      newProfile(name, proc_.organModel().stops.size() > 0);
    });
  };
  rename_.onClick = [this] {
    askName(TRANS("Rename console profile"), juce::String::fromUTF8(profile_.name.c_str()),
            [this](juce::String name) { renameProfile(name); });
  };
  delete_.onClick = [this] {
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, TRANS("Delete console profile"),
        TRANS("Delete the console profile \"%1\"? The organs keep the mappings it gave them.")
            .replace("%1", juce::String::fromUTF8(profile_.name.c_str())),
        TRANS("Delete"), TRANS("Cancel"), this,
        juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<ConsoleProfilePanel>(this)](int r) {
          if (safe != nullptr && r == 1) safe->deleteProfile();
        }));
  };
  for (auto* b : {&new_, &rename_, &delete_, &addTab_, &addOrgan_, &mapNow_}) addAndMakeVisible(*b);
  note_.setColour(juce::Label::textColourId, kDim);
  note_.setJustificationType(juce::Justification::topLeft);
  addAndMakeVisible(note_);
  list_.setModel(this);
  list_.setRowHeight(kRowH + 4);
  list_.setColour(juce::ListBox::backgroundColourId, kList);
  list_.setOutlineThickness(0);
  addAndMakeVisible(list_);
  addTab_.onClick = [this] { addTab(); };
  addOrgan_.setTooltip(TRANS("Each stop, coupler, tremulant and piston you have mapped on this organ becomes a tab"));
  addOrgan_.onClick = [this] {
    const int n = addFromOrgan();
    note_.setText(TRANS("%1 tabs added").replace("%1", juce::String(n)), juce::dontSendNotification);
  };
  mapNow_.onClick = [this] {
    if (onMapNow) onMapNow();
  };
  reload();
  startTimer(50);
}

ConsoleProfilePanel::~ConsoleProfilePanel() {
  stopTimer();
  stopLearning();
}

void ConsoleProfilePanel::reload() {
  stopLearning();
  files_ = consoleProfileFiles();
  file_ = activeConsoleProfile();
  profile_ = ConsoleProfile();
  if (file_.existsAsFile() && !loadConsoleProfile(file_, profile_)) file_ = juce::File();
  chooser_.clear(juce::dontSendNotification);
  chooser_.addItem(TRANS("None"), 1);
  int selected = 1;
  for (int i = 0; i < files_.size(); ++i) {
    ConsoleProfile p;
    juce::String name = files_[i].getFileNameWithoutExtension();
    if (loadConsoleProfile(files_[i], p) && !p.name.empty()) name = juce::String::fromUTF8(p.name.c_str());
    chooser_.addItem(name, i + 2);
    if (files_[i] == file_) selected = i + 2;
  }
  chooser_.setSelectedId(selected, juce::dontSendNotification);
  list_.updateContent();
  list_.repaint();
  showState();
}

bool ConsoleProfilePanel::newProfile(const juce::String& name, bool fromOrgan) {
  ConsoleProfile p;
  p.name = name.trim().isEmpty() ? std::string("Console") : name.trim().toStdString();
  if (fromOrgan) addOrganToProfile(proc_, p);
  const auto f = newConsoleProfileFile(name);
  if (!saveConsoleProfile(f, p)) return false;
  setActiveConsoleProfile(f);
  reload();
  return true;
}

bool ConsoleProfilePanel::renameProfile(const juce::String& name) {
  if (!file_.existsAsFile() || name.trim().isEmpty()) return false;
  profile_.name = name.trim().toStdString();
  save();
  reload();
  return true;
}

void ConsoleProfilePanel::deleteProfile() {
  if (!file_.existsAsFile()) return;
  file_.deleteFile();
  setActiveConsoleProfile({});
  reload();
}

void ConsoleProfilePanel::useProfile(const juce::File& file) {
  setActiveConsoleProfile(file);
  reload();
}

void ConsoleProfilePanel::addTab() {
  if (!file_.existsAsFile()) return;
  ConsoleTab t;
  t.label = TRANS("New tab").toStdString();
  profile_.tabs.push_back(t);
  save();
  list_.updateContent();
  list_.scrollToEnsureRowIsOnscreen(static_cast<int>(profile_.tabs.size()) - 1);
  showState();
}

void ConsoleProfilePanel::removeTab(int tab) {
  if (tab < 0 || tab >= static_cast<int>(profile_.tabs.size())) return;
  stopLearning();
  profile_.removeTab(static_cast<size_t>(tab));
  save();
  list_.updateContent();
  list_.repaint();
  showState();
}

void ConsoleProfilePanel::setLabel(int tab, const std::string& label) {
  if (tab < 0 || tab >= static_cast<int>(profile_.tabs.size())) return;
  auto& t = profile_.tabs[static_cast<size_t>(tab)];
  t.label = label;
  // A coupler's keyboards and pitch read from its new name when it says them.
  // The pitch from the name always; the keyboards only where it numbers
  // them ("II/I"), since "Swell" is another keyboard on each organ.
  if (t.kind == ConsoleTabKind::Coupler) {
    t.shift = couplerShiftFromName(label);
    int onto = -1, from = -1, shift = 0;
    if (couplerKeyboardsFromName(label, {}, onto, from, shift)) {
      t.keyboard = onto;
      t.fromKeyboard = from;
    }
  }
  save();
  list_.updateContent();
  list_.repaint();
}

void ConsoleProfilePanel::setKind(int tab, ConsoleTabKind kind) {
  if (tab < 0 || tab >= static_cast<int>(profile_.tabs.size())) return;
  auto& t = profile_.tabs[static_cast<size_t>(tab)];
  t.kind = kind;
  t.shift = kind == ConsoleTabKind::Coupler ? couplerShiftFromName(t.label) : 0;
  save();
  list_.updateContent();
  list_.repaint();
}

void ConsoleProfilePanel::setKeyboard(int tab, int keyboard) {
  if (tab < 0 || tab >= static_cast<int>(profile_.tabs.size())) return;
  profile_.tabs[static_cast<size_t>(tab)].keyboard = keyboard;
  save();
}

void ConsoleProfilePanel::setFromKeyboard(int tab, int keyboard) {
  if (tab < 0 || tab >= static_cast<int>(profile_.tabs.size())) return;
  profile_.tabs[static_cast<size_t>(tab)].fromKeyboard = keyboard;
  save();
}

int ConsoleProfilePanel::addFromOrgan() {
  if (!file_.existsAsFile()) return 0;
  const int n = addOrganToProfile(proc_, profile_);
  save();
  list_.updateContent();
  list_.repaint();
  showState();
  return n;
}

void ConsoleProfilePanel::learn(int tab) {
  if (tab < 0 || tab >= static_cast<int>(profile_.tabs.size())) return;
  learning_ = tab;
  proc_.beginMidiListen();
  list_.updateContent();
  list_.repaint();
  showState();
}

void ConsoleProfilePanel::stopLearning() {
  if (learning_ < 0) return;
  learning_ = -1;
  proc_.cancelMidiListen();
  list_.updateContent();
  list_.repaint();
  showState();
}

void ConsoleProfilePanel::poll() {
  if (learning_ < 0) return;
  MidiSource source;
  int value = 0;
  if (!proc_.takeHeardMidi(source, value)) {
    // Cancelled elsewhere: another window listened.
    if (!proc_.midiListening()) stopLearning();
    return;
  }
  const size_t tab = static_cast<size_t>(learning_);
  MidiBinding b;
  b.source = source;
  // The device named again in the profile's own numbering.
  if (b.source.deviceId != MidiDeviceMap::kAnyDevice)
    b.source.deviceId = profile_.messages.devices().idFor(proc_.midiMap().devices().nameFor(source.deviceId),
                                                          proc_.midiMap().devices().identifierFor(source.deviceId));
  b.targetKind = MidiTargetKind::Switch;
  b.targetId = ConsoleProfile::targetOf(tab);
  b.trigger = profile_.tabs[tab].kind == ConsoleTabKind::Piston ? MidiTrigger::Momentary : MidiTrigger::Toggle;
  b.latching = b.trigger == MidiTrigger::Toggle;
  // One message is one tab: taken off any other tab that had it.
  for (size_t i = 0; i < profile_.tabs.size(); ++i) {
    if (i == tab) continue;
    auto rows = profile_.bindingsOf(i);
    const auto before = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const MidiBinding& o) { return o.source == b.source; }),
               rows.end());
    if (rows.size() != before) profile_.setBindingsOf(i, rows);
  }
  profile_.setBindingsOf(tab, {b});
  learning_ = -1;
  save();
  list_.updateContent();
  list_.repaint();
  showState();
}

juce::String ConsoleProfilePanel::midiText(int tab) const {
  juce::StringArray parts;
  for (const auto& b : profile_.bindingsOf(static_cast<size_t>(tab))) parts.add(MidiEventDialog::describeBinding(b));
  return parts.isEmpty() ? TRANS("Not learned") : parts.joinIntoString(", ");
}

void ConsoleProfilePanel::save() {
  if (file_.existsAsFile()) saveConsoleProfile(file_, profile_);
}

void ConsoleProfilePanel::showState() {
  const bool have = file_.existsAsFile();
  const bool loaded = proc_.organModel().stops.size() > 0;
  rename_.setEnabled(have);
  delete_.setEnabled(have);
  addTab_.setEnabled(have);
  addOrgan_.setEnabled(have && loaded);
  mapNow_.setEnabled(have && loaded);
  if (learning_ >= 0)
    note_.setText(TRANS("Press the tab for %1")
                      .replace("%1", juce::String::fromUTF8(profile_.tabs[static_cast<size_t>(learning_)].label.c_str())),
                  juce::dontSendNotification);
  else if (!have)
    note_.setText(TRANS("A console profile holds your console's tabs and their MIDI. Each organ you open is "
                        "mapped from it. Press New to make one from this organ's mappings."),
                  juce::dontSendNotification);
  else
    note_.setText(TRANS("%1 tabs. Each organ you open is mapped from them where it has no mapping of its own.")
                      .replace("%1", juce::String(static_cast<int>(profile_.tabs.size()))),
                  juce::dontSendNotification);
}

juce::Component* ConsoleProfilePanel::refreshComponentForRow(int row, bool, juce::Component* existing) {
  if (row < 0 || row >= static_cast<int>(profile_.tabs.size())) {
    delete existing;
    return nullptr;
  }
  auto* r = dynamic_cast<Row*>(existing);
  if (r == nullptr) {
    delete existing;
    r = new Row(*this);
  }
  r->show(row);
  return r;
}

void ConsoleProfilePanel::askName(const juce::String& title, const juce::String& start,
                                  std::function<void(juce::String)> done) {
  auto* w = new juce::AlertWindow(title, TRANS("Name"), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", start);
  w->addButton(TRANS("OK"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  w->enterModalState(true, juce::ModalCallbackFunction::create([w, done](int r) {
                       if (r == 1) done(w->getTextEditorContents("name"));
                     }),
                     true);
}

void ConsoleProfilePanel::paint(juce::Graphics& g) { g.fillAll(kBack); }

void ConsoleProfilePanel::resized() {
  auto r = getLocalBounds().reduced(kMargin);
  auto top = r.removeFromTop(kButtonH);
  chooserLabel_.setBounds(top.removeFromLeft(120));
  delete_.setBounds(top.removeFromRight(80));
  top.removeFromRight(6);
  rename_.setBounds(top.removeFromRight(90));
  top.removeFromRight(6);
  new_.setBounds(top.removeFromRight(80));
  top.removeFromRight(6);
  chooser_.setBounds(top);
  r.removeFromTop(6);
  note_.setBounds(r.removeFromTop(40));
  auto buttons = r.removeFromBottom(kButtonH);
  r.removeFromBottom(kMargin);
  addTab_.setBounds(buttons.removeFromLeft(90));
  buttons.removeFromLeft(6);
  addOrgan_.setBounds(buttons.removeFromLeft(200));
  mapNow_.setBounds(buttons.removeFromRight(140));
  list_.setBounds(r);
}

ConsoleProfileWindow::ConsoleProfileWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow(TRANS("Console profile"), kBack, juce::DocumentWindow::closeButton),
      panel_(std::make_unique<ConsoleProfilePanel>(p)) {
  setUsingNativeTitleBar(true);
  setResizable(true, false);
  setContentNonOwned(panel_.get(), false);
  setSize(760, 560);
  fitToScreen(*this);
}

ConsoleProfileWindow::~ConsoleProfileWindow() { clearContentComponent(); }

void ConsoleProfileWindow::closeButtonPressed() {
  panel_->stopLearning();
  showFloating(*this, false, false);
}

}  // namespace mp::ui
