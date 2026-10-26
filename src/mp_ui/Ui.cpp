#include "Ui.h"

#include <algorithm>
#include <map>
#include <utility>
#include "Settings.h"
#include "OrganSettings.h"
#include "../mp_archive/OrganArchive.h"

#include "LoadingDialog.h"
#include "OpenWindow.h"
#include "Mobile.h"

#include <future>

namespace mp::ui {
namespace {

// Divisions are numbered from the pedal upward in Hauptwerk; show that order
// with a readable name rather than a raw id.
juce::String divisionLabel(const MasterpieceProcessor& proc, Id divisionId) {
  const auto& divs = proc.organModel().divisions;
  const auto it = divs.find(divisionId);
  if (it != divs.end() && !it->second.name.empty())
    return juce::String(it->second.name);
  return TRANS("Division %1").replace("%1", juce::String(divisionId));
}

constexpr int kStopHeight = 30;
constexpr int kHeaderHeight = 24;
constexpr int kJambWidth = 320;
// A grid of stop tiles rather than bars the width of the window: as many
// columns as fit at this width.
constexpr int kTileWidth = 190;
constexpr int kGap = 4;

// The key that opens the stop list from the console: the platform's command
// key (Ctrl, or Cmd on a Mac) with L. The letter's code is the same on every
// platform, in whichever case the keyboard layout gives.
bool isStopListKey(const juce::KeyPress& key) {
  if (!key.getModifiers().isCommandDown()) return false;
  const int code = key.getKeyCode();
  return code == 'l' || code == 'L';
}

int columnsFor(int width) { return juce::jmax(1, (width - 8 + kGap) / (kTileWidth + kGap)); }

// A warning that the log explains further, with a button that opens the
// log's folder. A path alone was taken for one that does not exist: the
// folder is hidden on Windows (AppData) and on a Mac (Library), and after a
// restart the run in question has moved to masterpiece.previous.log.
void showWithLog(const juce::String& title, const juce::String& message) {
  juce::File log;
  if (auto* file = dynamic_cast<juce::FileLogger*>(juce::Logger::getCurrentLogger()))
    log = file->getLogFile();
  if (log == juce::File() || kMobile) {
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message);
    return;
  }
  juce::AlertWindow::showOkCancelBox(
      juce::MessageBoxIconType::WarningIcon, title,
      message + "\n\n" +
          TRANS("The details are in the log, %1. If Masterpiece has been started again since, "
                "they are in masterpiece.previous.log beside it.")
              .replace("%1", log.getFileName()),
      TRANS("Show the log"), TRANS("OK"), nullptr,
      juce::ModalCallbackFunction::create([log](int choice) {
        if (choice == 1) log.revealToUser();
      }));
}

} // namespace

// ------------------------------------------------------------------ jamb

void StopButton::setStopName(const juce::String& name, const juce::String& division) {
  accessibleName_ = division.isNotEmpty() ? name + ", " + division : name;
  // The title is what the accessibility layer reads; the tile keeps the
  // stop's own name on it.
  setTitle(accessibleName_);
}

void StopButton::accessibleToggle() {
  if (!isEnabled()) return;
  // The state changes and the click follows, so the stop is drawn the way a
  // click on the tile draws it.
  setToggleState(!getToggleState(), juce::sendNotification);
}

bool StopButton::keyPressed(const juce::KeyPress& key) {
  if (isEnabled() && (key.isKeyCode(juce::KeyPress::spaceKey) ||
                      key.isKeyCode(juce::KeyPress::returnKey))) {
    // As a click on the tile, and at once: the stop is drawn before the key
    // comes up.
    accessibleToggle();
    return true;
  }
  if (onArrowKey && (key.isKeyCode(juce::KeyPress::leftKey) ||
                     key.isKeyCode(juce::KeyPress::rightKey) ||
                     key.isKeyCode(juce::KeyPress::upKey) ||
                     key.isKeyCode(juce::KeyPress::downKey))) {
    onArrowKey(*this, key);
    return true;
  }
  return juce::TextButton::keyPressed(key);
}

void StopButton::mouseDown(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) {
    if (onMidiMenu) onMidiMenu(*this);
    return;
  }
  juce::TextButton::mouseDown(e);
}

void StopButton::mouseUp(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) return;
  juce::TextButton::mouseUp(e);
}

void StopButton::mouseDrag(const juce::MouseEvent& e) {
  if (e.mods.isPopupMenu()) return;
  juce::TextButton::mouseDrag(e);
}

std::unique_ptr<juce::AccessibilityHandler> StopButton::createAccessibilityHandler() {
  return std::make_unique<ToggleAccessibilityHandler>(*this, *this);
}

StopJamb::StopJamb(MasterpieceProcessor& p) : proc_(p) { rebuild(); }

void StopJamb::rebuild() {
  entries_.clear();
  headers_.clear();

  Id lastDivision = -1;
  for (const auto& s : proc_.stopList()) {
    if (s.divisionId != lastDivision) {
      auto header = std::make_unique<juce::Label>();
      header->setText(divisionLabel(proc_, s.divisionId),
                      juce::dontSendNotification);
      header->setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
      header->setColour(juce::Label::textColourId, juce::Colours::orange);
      addAndMakeVisible(*header);
      headers_.push_back(std::move(header));
      lastDivision = s.divisionId;
    }

    Entry e;
    e.stopId = s.stopId;
    e.divisionId = s.divisionId;
    e.playable = s.playable;
    e.name = juce::String(s.name);
    e.division = divisionLabel(proc_, s.divisionId);
    e.button = std::make_unique<StopButton>(juce::String(s.name));
    e.button->setClickingTogglesState(true);
    e.button->setToggleState(proc_.shownStopEngaged(s.stopId),
                             juce::dontSendNotification);
    // What a screen reader reads: the stop and the division it is in.
    e.button->setStopName(e.name, e.division);
    // Drawn reads as drawn at a glance: ivory with dark lettering, like a
    // stop face, against the dark of one pushed in.
    e.button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2f3a));
    e.button->setColour(juce::TextButton::textColourOffId, juce::Colour(0xffb9c2d0));
    e.button->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffeee3c6));
    e.button->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1b1e24));
    e.button->setEnabled(s.playable);
    if (s.playable && !proc_.stopLoaded(s.stopId)) {
      e.button->setAlpha(0.45f);
      e.button->setTooltip(TRANS("Not loaded: chosen in Organ settings, Stops"));
    }
    if (!s.playable) {
      // Say WHY rather than just greying it: on a demo set this is the single
      // most confusing thing about the instrument.
      e.button->setTooltip(TRANS("This stop's ranks ship no pipes in this sample set"));
      // And say it to a screen reader too, which reads no tooltip.
      e.button->setDescription(TRANS("Not available"));
    }
    const Id id = s.stopId;
    auto* raw = e.button.get();
    e.button->onClick = [this, id, raw] {
      proc_.setStopEngaged(id, raw->getToggleState());
    };
    e.button->onArrowKey = [this](StopButton& from, const juce::KeyPress& key) {
      moveFocus(from, key);
    };
    // Mapped on the drawstop the console draws for it, as on a panel page (#555).
    if (const Id sw = proc_.stopKnobFor(id); sw != 0) {
      e.button->onMidiMenu = [this, sw](StopButton& b) {
        showSwitchMidiMenu(proc_, sw, b.getScreenBounds(), nullptr);
      };
    }
    addAndMakeVisible(*e.button);
    entries_.push_back(std::move(e));
  }

  // The program couplers, in a section of their own after the organ's own.
  if (!proc_.programCouplerList().empty()) {
    auto header = std::make_unique<juce::Label>();
    header->setText(TRANS("Program couplers"), juce::dontSendNotification);
    header->setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    header->setColour(juce::Label::textColourId, juce::Colours::orange);
    addAndMakeVisible(*header);
    headers_.push_back(std::move(header));
    for (const auto& pc : proc_.programCouplerList()) {
      const auto& model = proc_.organModel();
      const auto it = model.switches.find(pc.switchId);
      Entry e;
      e.stopId = 0;
      e.switchId = pc.switchId;
      e.divisionId = 0;  // a group of its own
      e.playable = true;
      e.name = juce::String(it != model.switches.end() ? it->second.name : std::string());
      e.division = TRANS("Program couplers");
      e.button = std::make_unique<StopButton>(e.name);
      e.button->setClickingTogglesState(true);
      e.button->setToggleState(proc_.shownSwitchEngaged(pc.switchId),
                               juce::dontSendNotification);
      // Read by a screen reader as the stops are: the coupler and its section.
      e.button->setStopName(e.name, e.division);
      e.button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2f3a));
      e.button->setColour(juce::TextButton::textColourOffId, juce::Colour(0xffb9c2d0));
      e.button->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffeee3c6));
      e.button->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1b1e24));
      const Id id = pc.switchId;
      auto* raw = e.button.get();
      e.button->onClick = [this, id, raw] {
        proc_.setSwitchEngaged(id, raw->getToggleState());
      };
      e.button->onArrowKey = [this](StopButton& from, const juce::KeyPress& key) {
        moveFocus(from, key);
      };
      e.button->onMidiMenu = [this, id](StopButton& b) {
        showSwitchMidiMenu(proc_, id, b.getScreenBounds(), nullptr);
      };
      addAndMakeVisible(*e.button);
      entries_.push_back(std::move(e));
    }
  }

  // Height is content-driven; the Viewport scrolls it.
  const int width = getWidth() > 0 ? getWidth() : kJambWidth;
  setSize(width, heightFor(width));
  resized();
}

void StopJamb::refresh() {
  for (auto& e : entries_) {
    const bool on = e.switchId != 0 ? proc_.shownSwitchEngaged(e.switchId)
                                    : proc_.shownStopEngaged(e.stopId);
    if (e.button->getToggleState() == on) continue;
    e.button->setToggleState(on, juce::dontSendNotification);
    // A stop changed by a piston, a combination or MIDI: the screen reader
    // is told, so a player who cannot see the console knows what moved.
    // The announcement is a no-op while no screen reader is listening.
    juce::AccessibilityHandler::postAnnouncement(
        (on ? TRANS("%1 drawn") : TRANS("%1 not drawn")).replace("%1", e.button->accessibleName()),
        juce::AccessibilityHandler::AnnouncementPriority::medium);
  }
}

void StopJamb::focusFirstStop() {
  if (entries_.empty()) return;
  entries_.front().button->grabKeyboardFocus();
}

StopButton* StopJamb::stopButtonAt(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return nullptr;
  return entries_[static_cast<size_t>(index)].button.get();
}

void StopJamb::moveFocus(StopButton& from, const juce::KeyPress& key) {
  // Left and right move one stop along the list; up and down move a row of
  // the grid, so the columns follow the width.
  const int columns = columnsFor(getWidth());
  int step = 0;
  if (key.isKeyCode(juce::KeyPress::leftKey)) step = -1;
  else if (key.isKeyCode(juce::KeyPress::rightKey)) step = 1;
  else if (key.isKeyCode(juce::KeyPress::upKey)) step = -columns;
  else if (key.isKeyCode(juce::KeyPress::downKey)) step = columns;
  else return;

  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& e) { return e.button.get() == &from; });
  if (it == entries_.end()) return;
  const int index = static_cast<int>(std::distance(entries_.begin(), it));
  const int target = index + step;
  if (target < 0 || target >= static_cast<int>(entries_.size())) return;
  entries_[static_cast<size_t>(target)].button->grabKeyboardFocus();
}

int StopJamb::heightFor(int width) const {
  const int columns = columnsFor(width);
  int height = 8;
  size_t i = 0;
  while (i < entries_.size()) {
    const Id division = entries_[i].divisionId;
    size_t n = 0;
    while (i + n < entries_.size() && entries_[i + n].divisionId == division) ++n;
    height += kHeaderHeight + static_cast<int>((n + columns - 1) / columns) * (kStopHeight + kGap);
    i += n;
  }
  return height;
}

void StopJamb::resized() {
  const int columns = columnsFor(getWidth());
  const int tile = juce::jmax(80, (getWidth() - 8 - (columns - 1) * kGap) / columns);
  int y = 4;
  size_t headerIndex = 0;
  size_t i = 0;
  while (i < entries_.size()) {
    const Id division = entries_[i].divisionId;
    if (headerIndex < headers_.size())
      headers_[headerIndex++]->setBounds(4, y, getWidth() - 8, kHeaderHeight);
    y += kHeaderHeight;
    int column = 0;
    for (; i < entries_.size() && entries_[i].divisionId == division; ++i) {
      entries_[i].button->setBounds(4 + column * (tile + kGap), y, tile, kStopHeight);
      if (++column == columns) {
        column = 0;
        y += kStopHeight + kGap;
      }
    }
    if (column != 0) y += kStopHeight + kGap;
  }
}

void StopJamb::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff20232a)); }

// ------------------------------------------------------------ expression

ExpressionBar::ExpressionBar(MasterpieceProcessor& p) : proc_(p) { rebuild(); }

void ExpressionBar::rebuild() {
  shoes_.clear();
  labels_.clear();

  const auto& model = proc_.organModel();
  // One shoe per enclosure. Continuous controls that no enclosure uses are
  // console animation, not expression, and would only clutter this.
  std::vector<std::pair<Id, juce::String>> shoes;
  for (const auto& [id, enc] : model.enclosures) {
    (void)id;
    if (enc.continuousControlId == 0) continue;
    // A level, not a swell: the organ remembers it from one session to the
    // next, which it never does for a shoe, and draws it on a page of its own
    // where it is set (#137: fifteen audio-group and noise levels pushed
    // Saint-Jean-de-Luz's two swell pedals out of this strip).
    if (const auto c = model.continuousControls.find(enc.continuousControlId);
        c != model.continuousControls.end() && c->second.rememberState &&
        c->second.imageSetInstanceId != 0)
      continue;
    // The shoe the player moves, which may be upstream of the shutters.
    // The part of the name that tells two boxes apart. Nancy calls hers
    // "Enclosure R" and "Enclosure PO", and a column this narrow cut both to
    // "Enclosur..." -- two sliders that looked like one control twice (#53).
    juce::String name(enc.name);
    for (const char* word : {"Enclosure", "enclosure", "Swell box", "Swell"})
      if (name.startsWith(word) && name.length() > juce::String(word).length())
        name = name.substring(juce::String(word).length()).trim();
    shoes.emplace_back(proc_.playerControlFor(enc.continuousControlId),
                       name.isEmpty() ? juce::String(TRANS("Swell")) : name);
  }
  std::sort(shoes.begin(), shoes.end());
  // Two boxes worked by one shoe are one slider.
  shoes.erase(std::unique(shoes.begin(), shoes.end(),
                          [](const auto& a, const auto& b) { return a.first == b.first; }),
              shoes.end());

  for (const auto& [controlId, name] : shoes) {
    auto label = std::make_unique<juce::Label>();
    label->setText(name, juce::dontSendNotification);
    label->setJustificationType(juce::Justification::centred);
    label->setMinimumHorizontalScale(0.6f);
    label->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(*label);
    labels_.push_back(std::move(label));

    auto slider = std::make_unique<Shoe>(proc_, controlId);
    slider->setRange(0.0, 127.0, 1.0);
    // The organ's own name for the control, which is the long one: "01.
    // Enclosure Recit expressif".
    {
      const auto& controls = proc_.organModel().continuousControls;
      const auto cit = controls.find(controlId);
      if (cit != controls.end() && !cit->second.name.empty()) {
        juce::String full(juce::CharPointer_UTF8(cit->second.name.c_str()));
        if (full.initialSectionContainingOnly("0123456789").isNotEmpty() && full.contains(". "))
          full = full.fromFirstOccurrenceOf(". ", false, false);
        slider->setTooltip(full);
        labels_.back()->setTooltip(full);
      }
    }
    // Shoes start open: a console that boots with every box shut sounds broken.
    slider->setValue(127.0, juce::dontSendNotification);
    const Id id = controlId;
    auto* raw = slider.get();
    slider->onValueChange = [this, id, raw] {
      proc_.setContinuousControl(id, static_cast<int>(raw->getValue()));
    };
    proc_.setContinuousControl(id, 127);
    addAndMakeVisible(*slider);
    shoes_.push_back(std::move(slider));
  }
  resized();
}

void ExpressionBar::Shoe::mouseDown(const juce::MouseEvent& e) {
  if (!e.mods.isPopupMenu()) {
    juce::Slider::mouseDown(e);
    return;
  }
  // At the pointer, not beside the slider: the strip runs the height of the
  // window, and a menu placed against all of it can open far from the click.
  showControlMidiMenu(proc_, controlId_,
                      {e.getScreenX(), e.getScreenY(), 1, 1}, nullptr);
}

void ExpressionBar::refresh() {
  for (auto& s : shoes_) {
    if (s->isMouseButtonDown()) continue;  // the player has it in hand
    const int v = proc_.continuousControlValue(s->controlId());
    if (static_cast<int>(s->getValue()) != v)
      s->setValue(v, juce::dontSendNotification);
  }
}

void ExpressionBar::resized() {
  if (shoes_.empty()) return;
  auto r = getLocalBounds().reduced(4);
  const int w = juce::jmax(40, r.getWidth() / static_cast<int>(shoes_.size()));
  for (size_t i = 0; i < shoes_.size(); ++i) {
    auto col = r.removeFromLeft(w);
    labels_[i]->setBounds(col.removeFromBottom(18));
    shoes_[i]->setBounds(col);
  }
}

// --------------------------------------------------------------- top bar

namespace {
// Where the lamps change colour. An organ sits loud for long stretches, so
// amber has to mean "loud and fine" rather than "nearly clipping", or it is
// lit the whole time and says nothing.
constexpr int kMeterSegments = 14;
constexpr float kMeterFloorDb = -48.0f;
constexpr int kFirstAmber = 9;
constexpr int kFirstRed = 12;
} // namespace

// Ten looks a second: the peak it reads already falls slowly, and a lamp that
// moves repaints the bar it sits in.
LevelMeter::LevelMeter(MasterpieceProcessor& p) : proc_(p) { startTimerHz(10); }
LevelMeter::~LevelMeter() { stopTimer(); }

void LevelMeter::timerCallback() {
  bool changed = false;
  for (int c = 0; c < 2; ++c) {
    const float db =
        juce::Decibels::gainToDecibels(proc_.outputPeak(c), kMeterFloorDb);
    const int lit = juce::jlimit(
        0, kMeterSegments,
        juce::roundToInt((db - kMeterFloorDb) / -kMeterFloorDb * kMeterSegments));
    if (lit != lit_[c]) {
      lit_[c] = lit;
      changed = true;
    }
  }
  // Only when a lamp actually moved: the console behind this is expensive to
  // repaint and a silent organ should cost nothing.
  if (changed) repaint();

  // Dark for two seconds, it looks four times a second; a lamp lit, ten.
  if (lit_[0] > 0 || lit_[1] > 0) {
    darkTicks_ = 0;
    if (getTimerInterval() != 100) startTimer(100);
  } else if (getTimerInterval() != 250 && ++darkTicks_ >= 20) {
    startTimer(250);
  }
}

void LevelMeter::paint(juce::Graphics& g) {
  auto r = getLocalBounds().reduced(1);
  const int rowH = r.getHeight() / 2;
  const float segW = r.getWidth() / static_cast<float>(kMeterSegments);

  for (int c = 0; c < 2; ++c) {
    const int y = r.getY() + c * rowH;
    for (int s = 0; s < kMeterSegments; ++s) {
      const bool on = s < lit_[c];
      juce::Colour col = s >= kFirstRed     ? juce::Colour(0xffe05555)
                         : s >= kFirstAmber ? juce::Colour(0xffe0b155)
                                            : juce::Colour(0xff5fd07a);
      // Unlit lamps stay visible but dark, so the meter reads as a scale
      // rather than appearing and disappearing.
      g.setColour(on ? col : col.withAlpha(0.16f));
      g.fillRect(juce::Rectangle<float>(r.getX() + s * segW, float(y) + 1.0f,
                                        segW - 1.5f, float(rowH) - 2.0f));
    }
  }
}

TopBar::TopBar(MasterpieceProcessor& p)
    : proc_(p), meter_(p) {
  addAndMakeVisible(simple_);
  addAndMakeVisible(meter_);
  addAndMakeVisible(volume_);
  // The readout is given a fixed, modest width. Left to itself it takes a
  // proportion of the slider, which on a narrow bar leaves a track too short
  // to aim at.
  volume_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 20);
  volume_.setRange(-40.0, 24.0, 0.1);
  volume_.setTextValueSuffix(" dB");
  volume_.setSkewFactor(1.0);
  if (auto* gain = proc_.apvts().getRawParameterValue("masterGain"))
    volume_.setValue(juce::Decibels::gainToDecibels(gain->load(), -40.0f),
                     juce::dontSendNotification);
  volume_.onValueChange = [this] {
    // -40 dB is the bottom of the slider and means silence, not 0.01.
    const auto db = static_cast<float>(volume_.getValue());
    const float g = db <= -40.0f ? 0.0f : juce::Decibels::decibelsToGain(db);
    if (auto* p = proc_.apvts().getParameter("masterGain"))
      p->setValueNotifyingHost(p->convertTo0to1(g));
    // Marks the gain alone as needing a write, flushed on the editor's timer
    // below. Does NOT call proc_.markSettingsDirty(): that flag drives a
    // rewrite of the whole per-organ file from the live engine state, which
    // would also commit whatever was changed in Settings and left there
    // unsaved — turning a knob here would make "Keep changes" a lie. The
    // fader gets its own flag and its own writer that touches only the
    // "gain" line.
    proc_.markMasterGainDirty();
  };

  addAndMakeVisible(status_);
  status_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  simple_.onClick = [this] {
    auto sw = proc_.engineSwitch();
    sw.simpleWavOnly = simple_.getToggleState();
    proc_.setEngineSwitch(sw);
  };
}

namespace {
bool& pairingOpen() {
  static bool open = false;
  return open;
}

// Handed to the pairing window, which calls it once as it closes.
struct PairingClosed final : juce::ModalComponentManager::Callback {
  void modalStateFinished(int) override { pairingOpen() = false; }
};
}  // namespace

bool openPairingOnce(const std::function<bool(juce::ModalComponentManager::Callback*)>& open) {
  if (pairingOpen()) return false;
  pairingOpen() = true;
  // The window owns the callback from here, and deletes it even when it
  // cannot open.
  if (!open(new PairingClosed())) pairingOpen() = false;
  return pairingOpen();
}

bool guardPairingButton(juce::AudioDeviceSelectorComponent& selector) {
  // The selector keeps its button private; it is the one with this tooltip,
  // JUCE's own text, translated the way the selector translates it.
  const juce::String tooltip = juce::translate("Scan for bluetooth MIDI devices");
  for (auto* child : selector.getChildren()) {
    auto* button = dynamic_cast<juce::TextButton*>(child);
    if (button == nullptr || button->getTooltip() != tooltip)
      continue;
    button->onClick = [] {
      auto open = [](juce::ModalComponentManager::Callback* closed) {
        return juce::BluetoothMidiDevicePairingDialogue::open(closed);
      };
      // The selector's own steps: ask for the permission first where it is
      // not yet given.
      if (juce::RuntimePermissions::isGranted(juce::RuntimePermissions::bluetoothMidi)) {
        openPairingOnce(open);
        return;
      }
      juce::RuntimePermissions::request(juce::RuntimePermissions::bluetoothMidi, [open](bool) {
        if (juce::RuntimePermissions::isGranted(juce::RuntimePermissions::bluetoothMidi))
          openPairingOnce(open);
      });
    };
    return true;
  }
  return false;
}

AudioSettingsPanel::AudioSettingsPanel(juce::AudioDeviceManager& devices,
                                       MasterpieceProcessor& p)
    : proc_(p), devices_(devices), selector_(devices, 0, 0, 1, 8, true, true, true, false) {
  addAndMakeVisible(selector_);
  guardPairingButton(selector_);
  addAndMakeVisible(latency_);
  latency_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  latency_.setTooltip(TRANS("The buffer size sets the delay between a key and its sound. How many "
                            "pipes can sound is the polyphony, in Organ settings, Loading."));
  devices_.addChangeListener(this);
  showLatency();
  addAndMakeVisible(keepAwake_);
  addAndMakeVisible(level_);
  const float db = proc_.speakerKeepAlive();
  keepAwake_.setToggleState(db < 0.0f, juce::dontSendNotification);
  keepAwake_.setTooltip(
      TRANS("For a battery speaker that switches itself off after a few seconds of "
            "silence and swallows the first notes while it wakes. Plays a 20 Hz tone "
            "too low and too quiet to hear under the organ, so the line is never "
            "silent."));
  level_.setRange(-80.0, -40.0, 1.0);
  level_.setTextValueSuffix(" dB");
  level_.setValue(db < 0.0f ? db : -60.0, juce::dontSendNotification);
  level_.setTooltip(TRANS("Raise it if the speaker still goes to sleep; lower it if "
                          "you can hear it."));
  level_.setEnabled(keepAwake_.getToggleState());
  keepAwake_.onClick = [this] { apply(); };
  level_.onDragEnd = [this] { apply(); };
  level_.onValueChange = [this] {
    if (!level_.isMouseButtonDown()) apply();
  };

  // The peak limiter on the output, under the device.
  addAndMakeVisible(limiterOn_);
  limiterOn_.setToggleState(proc_.limiterEnabled(), juce::dontSendNotification);
  limiterOn_.setTooltip(
      TRANS("Holds the output below the ceiling. It works on the signal "
            "after the master fader and adds no delay."));
  limiterOn_.onClick = [this] {
    proc_.setLimiterEnabled(limiterOn_.getToggleState());
    updateLimiterControls();
  };

  addAndMakeVisible(ceilingLabel_);
  ceilingLabel_.setText(TRANS("Ceiling"), juce::dontSendNotification);
  ceilingLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(ceiling_);
  ceiling_.setRange(-12.0, -0.1, 0.1);
  ceiling_.setValue(proc_.limiterCeilingDb(), juce::dontSendNotification);
  ceiling_.setTextValueSuffix(" dBFS");
  ceiling_.setTooltip(TRANS("The level the output is held under. A little "
                             "below full scale leaves the converters something "
                             "to work with."));
  ceiling_.onValueChange = [this] {
    // The value reaches the limiter at once, so the player hears it while it
    // moves; the settings file is written when the drag ends.
    proc_.limiter().setCeilingDb(static_cast<float>(ceiling_.getValue()));
    if (!ceiling_.isMouseButtonDown())
      proc_.setLimiterCeilingDb(static_cast<float>(ceiling_.getValue()));
  };
  ceiling_.onDragEnd = [this] {
    proc_.setLimiterCeilingDb(static_cast<float>(ceiling_.getValue()));
  };

  addAndMakeVisible(releaseLabel_);
  releaseLabel_.setText(TRANS("Release"), juce::dontSendNotification);
  releaseLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(release_);
  release_.setRange(50.0, 1000.0, 10.0);
  release_.setValue(proc_.limiterReleaseMs(), juce::dontSendNotification);
  release_.setTextValueSuffix(" ms");
  release_.setTooltip(TRANS("How long the gain takes to come back after a "
                             "peak. A short one lets the level come back at "
                             "once, a long one holds it down for longer."));
  release_.onValueChange = [this] {
    proc_.limiter().setReleaseMs(static_cast<float>(release_.getValue()));
    if (!release_.isMouseButtonDown())
      proc_.setLimiterReleaseMs(static_cast<float>(release_.getValue()));
  };
  release_.onDragEnd = [this] {
    proc_.setLimiterReleaseMs(static_cast<float>(release_.getValue()));
  };
  updateLimiterControls();

  setSize(500, 510);
}

AudioSettingsPanel::~AudioSettingsPanel() { devices_.removeChangeListener(this); }

void AudioSettingsPanel::changeListenerCallback(juce::ChangeBroadcaster*) { showLatency(); }

void AudioSettingsPanel::showLatency() {
  auto* dev = devices_.getCurrentAudioDevice();
  const double rate = dev != nullptr ? dev->getCurrentSampleRate() : 0.0;
  if (dev == nullptr || rate <= 0.0) {
    latency_.setText({}, juce::dontSendNotification);
    return;
  }
  const int buffer = dev->getCurrentBufferSizeSamples();
  const int output = juce::jmax(0, dev->getOutputLatencyInSamples());
  auto ms = [rate](int samples) { return juce::String(juce::roundToInt(1000.0 * samples / rate)); };
  latency_.setText(TRANS("Key to speaker: about %1 ms, of which the buffer is %2 ms and the output %3 ms, "
                         "as the device reports it.")
                       .replace("%1", ms(buffer + output))
                       .replace("%2", ms(buffer))
                       .replace("%3", ms(output)),
                   juce::dontSendNotification);
}

void AudioSettingsPanel::apply() {
  level_.setEnabled(keepAwake_.getToggleState());
  proc_.setSpeakerKeepAlive(keepAwake_.getToggleState()
                                ? static_cast<float>(level_.getValue())
                                : 0.0f);
}

void AudioSettingsPanel::updateLimiterControls() {
  // The two settings belong to the limiter, so they are greyed out while it
  // is off. Their values are kept either way.
  const bool on = limiterOn_.getToggleState();
  ceiling_.setEnabled(on);
  release_.setEnabled(on);
}

void AudioSettingsPanel::resized() {
  auto r = getLocalBounds();

  // The limiter block at the foot of the tab: the switch, then the ceiling
  // and the release on a row each.
  auto block = r.removeFromBottom(96).reduced(12, 0);
  limiterOn_.setBounds(block.removeFromTop(26));
  block.removeFromTop(6);
  auto row = block.removeFromTop(26);
  ceilingLabel_.setBounds(row.removeFromLeft(90));
  row.removeFromLeft(6);
  ceiling_.setBounds(row.removeFromLeft(240));
  row = block.removeFromTop(26);
  releaseLabel_.setBounds(row.removeFromLeft(90));
  row.removeFromLeft(6);
  release_.setBounds(row.removeFromLeft(240));

  auto keep = r.removeFromBottom(40).reduced(12, 6);
  keepAwake_.setBounds(keep.removeFromLeft(240));
  level_.setBounds(keep);
  latency_.setBounds(r.removeFromBottom(28).reduced(12, 2));
  selector_.setBounds(r);
}

void TopBar::setStatus(const juce::String& text) {
  // Loading an organ restores its own volume, so the slider has to follow the
  // parameter rather than only drive it. Skipped while the player is dragging.
  if (!volume_.isMouseButtonDown())
    if (auto* g = proc_.apvts().getRawParameterValue("masterGain")) {
      const double db = juce::Decibels::gainToDecibels(g->load(), -40.0f);
      if (std::abs(db - volume_.getValue()) > 0.05)
        volume_.setValue(db, juce::dontSendNotification);
    }

  status_.setText(text, juce::dontSendNotification);
  // Cut short on a narrow window; whole on hover (#90).
  status_.setTooltip(text);
}

void TopBar::resized() {
  auto r = getLocalBounds().reduced(4);
  simple_.setBounds(r.removeFromLeft(88));
  r.removeFromLeft(10);
  // When the console's buttons leave too little room, things give way in
  // order of how much they are missed: the status line first, then the
  // meter, and the fader last -- never below a width a hand can still use.
  const int room = r.getWidth();
  // Wide enough to set a level by hand (#90: too small to use). The status
  // line has moved to the bottom of the window, so the fader and the meter
  // share the rest.
  const int volumeW = juce::jlimit(kFaderMinWidth, 220, room - 120);
  const int meterW = juce::jlimit(0, 112, room - volumeW - 8);
  volume_.setBounds(r.removeFromLeft(volumeW));
  r.removeFromLeft(8);
  // Beside the fader it answers for: the two are read together.
  meter_.setVisible(meterW >= 40);
  meter_.setBounds(r.removeFromLeft(meterW).reduced(0, 5));
}

// ---------------------------------------------------------------- editor

// The organ file dialog. Its own method rather than a lambda in the member
// list, because the first-run wizard needs the same door.
#if JUCE_IOS
void MasterpieceEditor::askForFolderOf(const juce::File& package, bool graphicsOnly) {
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  juce::AlertWindow::showOkCancelBox(
      juce::MessageBoxIconType::QuestionIcon, TRANS("The rest of %1").replace("%1", package.getFileName()),
      TRANS("%1 is one part of a set of several files. Choose the folder that holds "
            "all of them, and Masterpiece reads the set from there.")
          .replace("%1", package.getFileName()),
      TRANS("Choose the folder"), TRANS("Cancel"), nullptr,
      juce::ModalCallbackFunction::create([self, package, graphicsOnly](int ok) {
        if (self == nullptr || ok == 0) return;
        self->chooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose the folder that holds the set"),
                                                             package.getParentDirectory());
        self->chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [self, package, graphicsOnly](const juce::FileChooser& fc) {
              if (self == nullptr) return;
              const auto picked = fc.getURLResult();
              if (picked.isEmpty()) return;
              const auto folder = holdPickedAccess(picked);
              // The package by the same name in the folder now lent, which is
              // the one picked when the folder is the one that holds it.
              const auto inFolder = folder.getChildFile(package.getFileName());
              const auto target = inFolder.existsAsFile() ? inFolder : package;
              juce::MessageManager::callAsync([self, target, graphicsOnly] {
                if (self != nullptr) self->loadOrgan(target, graphicsOnly);
              });
            });
      }));
}
#endif

void MasterpieceEditor::showOpenMenu() {
  // The favourites in a window of their own, searched, ordered and named by
  // the player, with the file dialog one button away (#250, #276). With no
  // favourite yet it goes straight to the dialog.
  juce::Component::SafePointer<MasterpieceEditor> safe(this);
  OpenPanel::show(
      proc_,
      [safe](const juce::File& f) {
        if (safe != nullptr) safe->loadOrgan(f);
      },
      [safe] {
        if (safe != nullptr) safe->chooseAndLoadOrgan();
      });
}

void MasterpieceEditor::chooseAndLoadOrgan(const juce::File& startIn) {
  // The extension pattern names the format because that IS the file name on
  // disk; the prompt does not, because the player is choosing an organ.
  // On a phone or tablet an organ comes as one package: a RAR or a .orgue.
  // A loose installation is thousands of files, which is slow to bring onto
  // the device and slow to read there through the system's document layer.
  // Where the last organ was chosen from, unless the caller names a folder:
  // the next organ is usually beside the last one (#251).
  const juce::File start = startIn != juce::File() ? startIn : proc_.openFolder();
  chooser_ = std::make_unique<juce::FileChooser>(
      kMobile ? TRANS("Choose an organ package") : TRANS("Choose an organ definition file"), start,
      kMobile ? "*.rar;*.orgue"
              : "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.Organ.Hauptwerk.xml;"
                "*.CustomOrgan.Hauptwerk.xml;*.organ;*.rar;*.orgue");
  auto flags = juce::FileBrowserComponent::openMode |
               juce::FileBrowserComponent::canSelectFiles;
 #if JUCE_MAC
  // Allow tagged folders to be selected in the native panel's search results.
  // A folder is a navigation destination, never an organ definition.
  flags |= juce::FileBrowserComponent::canSelectDirectories;
 #endif
  chooser_->launchAsync(flags,
                        [this](const juce::FileChooser& fc) {
                         #if JUCE_IOS
                          // Lent by the Files app: held and read where it is.
                          const auto picked = fc.getURLResult();
                          if (picked.isEmpty()) return;
                          const auto f = holdPickedAccess(picked);
                          if (!f.existsAsFile()) {
                            showWithLog(TRANS("Could not open the organ"),
                                        TRANS("The Files app did not lend this document to Masterpiece."));
                            return;
                          }
                         #else
                          const auto f = fc.getResult();
                         #endif
                         #if JUCE_MAC
                          if (f.isDirectory()) {
                            // Replacing chooser_ must wait until its callback returns.
                            juce::MessageManager::callAsync(
                                [safe = juce::Component::SafePointer<MasterpieceEditor>(this), f] {
                                  if (safe != nullptr) safe->chooseAndLoadOrgan(f);
                                });
                            return;
                          }
                         #endif
                          if (f.existsAsFile()) {
                            if (!isMobilePackage(f.getFileName())) return;
                           #if !JUCE_IOS && !JUCE_ANDROID
                            proc_.setOpenFolder(f.getParentDirectory());
                           #endif
                            loadOrgan(f);
                            return;
                          }
                         #if JUCE_ANDROID
                          // A document from Android's picker, not a path:
                          // copied into the app's storage, then opened.
                          const auto url = fc.getURLResult();
                          if (url.isEmpty()) return;
                          importDocument(url, [safe = juce::Component::SafePointer<MasterpieceEditor>(this)](
                                                  juce::File copy) {
                            if (safe != nullptr && copy.existsAsFile()) safe->loadOrgan(copy);
                          });
                         #endif
                        });
}

MasterpieceEditor::MasterpieceEditor(MasterpieceProcessor& p)
    : juce::AudioProcessorEditor(p),
      proc_(p),
      top_(p),
      console_(p),
      jamb_(p),
      expression_(p),
      keyboard_(p.keyboardState(),
                juce::MidiKeyboardComponent::horizontalKeyboard) {
  // A slider's value is drawn in its track colour, which JUCE's dark scheme
  // makes a near-black that vanishes into the track (#486). The thumb's blue,
  // dimmed, shows how far each slider is set. On the default look, so every
  // window's sliders have it.
  juce::LookAndFeel::getDefaultLookAndFeel().setColour(juce::Slider::trackColourId,
                                                       juce::Colour(0xff42a2c8).withAlpha(0.55f));

  // The licence question, asked from the loading thread before a sample is
  // read (#164). The box belongs to the message thread, so it is shown there
  // and the load waits for the answer. A window gone, or a question never
  // answered because the program is closing, is a no. So is a load cancelled
  // while it waits: the program closing waits for the load in turn.
  proc_.licenceAsker = [this](const std::string& organ, const std::string& who) {
    auto answer = std::make_shared<std::promise<bool>>();
    auto future = answer->get_future();
    const auto& progress = proc_.loadProgress();
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    juce::MessageManager::callAsync([self, answer, organ, who] {
      if (self == nullptr) {
        answer->set_value(false);
        return;
      }
      self->askLicenceQuestion(juce::String(organ), juce::String(who),
                               [answer](bool yes) { answer->set_value(yes); });
    });
    while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
      if (progress.isCancelled()) return false;
    try {
      return future.get();
    } catch (const std::future_error&) {
      return false;
    }
  };
  // The package question, asked the same way: a composite plays a package
  // that was not found beside it, and the player may point to it.
  proc_.packageAsker = [this](const std::string& package, const std::string& name,
                              const std::string& start) -> std::string {
    auto answer = std::make_shared<std::promise<std::string>>();
    auto future = answer->get_future();
    const auto& progress = proc_.loadProgress();
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    juce::MessageManager::callAsync([self, answer, package, name, start] {
      if (self == nullptr) {
        answer->set_value({});
        return;
      }
      self->askForPackage(juce::String(package), juce::String::fromUTF8(name.c_str()),
                          juce::File(juce::String::fromUTF8(start.c_str())),
                          [answer](const juce::String& path) { answer->set_value(path.toStdString()); });
    });
    while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
      if (progress.isCancelled()) return {};
    try {
      return future.get();
    } catch (const std::future_error&) {
      return {};
    }
  };
  addAndMakeVisible(top_);
  // A thin line along the bottom, the whole width of the window, instead of
  // whatever the top bar's buttons left of it.
  auto& statusLine = top_.statusLabel();
  addAndMakeVisible(statusLine);
  // A newer version, said at the right of that line for 30 s (UpdateCheck).
  addChildComponent(updateNotice_);
  updateNotice_.onShownOrHidden = [this] { resized(); };
  updateNotice_.isLoading = [this] { return loading_; };
  statusLine.setFont(juce::Font(juce::FontOptions(13.0f)));
  statusLine.setColour(juce::Label::backgroundColourId, juce::Colour(0xff15171c));
  statusLine.setBorderSize(juce::BorderSize<int>(0, 8, 0, 8));

  // The organ's own console when the set ships artwork; the plain jamb
  // otherwise, and on demand. A set without artwork must still be playable.
  addAndMakeVisible(consoleView_);
  consoleView_.setViewedComponent(&console_, false);
  addAndMakeVisible(pageTabs_);
  pageTabs_.addChangeListener(this);
  pageTabs_.onPopup = [this](int page) {
    if (page >= console_.pageCount()) {
      // A panel's tab: its name.
      const int at = page - console_.pageCount();
      if (at < 0 || at >= static_cast<int>(panelTabs_.size())) return;
      const int panel = panelTabs_[static_cast<size_t>(at)];
      juce::PopupMenu menu;
      menu.addItem(1, TRANS("Rename %1...").replace("%1", panels_[static_cast<size_t>(panel)]->title()));
      menu.showMenuAsync(juce::PopupMenu::Options(), [this, panel](int choice) {
        if (choice == 1 && panel < static_cast<int>(panels_.size()))
          panels_[static_cast<size_t>(panel)]->askForTitle();
      });
      return;
    }
    if (!pagesCanFloat()) return;
    juce::PopupMenu menu;
    const bool open = pageWindowFor(page) != nullptr;
    menu.addItem(1, open ? TRANS("Bring its window to the front") : TRANS("Open in its own window"));
    // A set can draw its pages again for another screen shape -- Buckeburg's
    // jambs in portrait -- and a window of its own can show that one while
    // the main window keeps its layout.
    if (!open && console_.layoutCount() > 1)
      for (int l = 0; l < console_.layoutCount(); ++l)
        menu.addItem(10 + l, TRANS("Open in its own window, %1").replace("%1", layoutName(l)));
    if (open) menu.addItem(2, TRANS("Close its window"));
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, page](int choice) {
      if (choice == 1) openPageWindow(page);
      if (choice == 2) closePageWindow(pageWindowFor(page));
      if (choice >= 10) openPageWindow(page, {}, choice - 10);
    });
  };
  for (auto* b : {&fileMenu_, &viewMenu_, &settingsButton_}) {
    addAndMakeVisible(*b);
    b->setTriggeredOnMouseDown(true);
  }
  fileMenu_.onClick = [this] { showFileMenu(); };
  viewMenu_.onClick = [this] { showViewMenu(); };
  settingsButton_.onClick = [this] { showSettingsMenu(); };

  // The sequencer. Held with "Set", stepping CAPTURES the frame it lands on,
  // which is how a registration is built for a piece.
  addChildComponent(manual_);
  manual_.onChange = [this] {
    // The item is a keyboard; the channel it plays on is looked up, so the two
    // cannot drift apart.
    keyboard_.setMidiChannel(
        proc_.channelForKeyboard(static_cast<Id>(manual_.getSelectedId())));
  };

  addChildComponent(layout_); // shown only when the organ offers a choice
  layout_.onChange = [this] {
    console_.setLayout(layout_.getSelectedId() - 1);
    resized();
  };

  addAndMakeVisible(setter_);
  setter_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffb0402c));
  setter_.onClick = [this] {
    if (proc_.captureMode()) {
      proc_.setCaptureMode(false);
      top_.setStatus(TRANS("Set is off: pistons recall again"));
      return;
    }
    const auto where = proc_.storeTargetName();
    if (!proc_.storeIntoTarget()) {
      top_.setStatus(TRANS("Choose a piston or a step first; then Set stores what is drawn into it"));
      return;
    }
    proc_.saveCombinationsIfDirty();
    top_.setStatus(TRANS("Stored in %1").replace("%1", juce::String(where)));
  };

  addAndMakeVisible(stepPrev_);
  stepPrev_.onClick = [this] { proc_.stepperPrev(); };
  addAndMakeVisible(stepNext_);
  stepNext_.onClick = [this] { proc_.stepperNext(); };
  addAndMakeVisible(stepFrame_);
  stepFrame_.setJustificationType(juce::Justification::centred);
  stepFrame_.setColour(juce::Label::textColourId, juce::Colour(0xffb9c2d0));
  stepFrame_.setTooltip(TRANS("The sequencer's frame, out of the frames that hold a registration. "
                              "Click to go to a frame by its number"));
  stepFrame_.setMouseCursor(juce::MouseCursor::PointingHandCursor);
  stepFrame_.onClick = [this] { askForFrame(proc_); };

  addAndMakeVisible(panicButton_);
  panicButton_.onClick = [this] { proc_.releaseAllKeys(); };
  addAndMakeVisible(saveStateButton_);
  saveStateButton_.onClick = [this] { proc_.saveCurrentState(); };

  combinations_ = std::make_unique<CombinationsWindow>(proc_);

  addAndMakeVisible(tuningButton_);
  tuningButton_.setTooltip(TRANS("Temperament, pitch and transposer for this organ"));
  tuningButton_.onClick = [this] {
    juce::CallOutBox::launchAsynchronously(std::make_unique<TuningPanel>(proc_),
                                           tuningButton_.getScreenBounds(), nullptr);
  };


  // The stop list's place on the toolbar is Panels' (#276): a panel shows
  // the same stops, arranged and named as the player likes. The plain list
  // stays as what an organ with no console artwork shows.

  addAndMakeVisible(jambView_);
  jambView_.setViewedComponent(&jamb_, false);

  // Scrollbars, made to stop shouting.
  //
  // JUCE's default thumb is a bright blue bar the full height of the window.
  // Next to an organ console that reads as a control -- a fader down the side
  // of the instrument -- rather than as a scrollbar, and it is the first thing
  // the eye goes to in a window whose subject is the artwork.
  //
  // The console never needs one at all: the artwork is scaled to fit, so it
  // cannot overflow. The stop list genuinely scrolls, and keeps a slim, dark
  // vertical one; its content is sized to the viewport width, so the
  // horizontal bar only ever appeared as a stub.
  consoleView_.setScrollBarsShown(false, false);
  jambView_.setScrollBarsShown(true, false);
  jambView_.setScrollBarThickness(10);
  for (auto* bar : {&jambView_.getVerticalScrollBar(),
                    &jambView_.getHorizontalScrollBar()}) {
    bar->setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff4c5464));
    bar->setColour(juce::ScrollBar::trackColourId, juce::Colour(0xff20232a));
    bar->setColour(juce::ScrollBar::backgroundColourId, juce::Colour(0xff20232a));
  }
  addChildComponent(expression_);  // shown by the Swell toggle
  addAndMakeVisible(keyboard_);
  keyboard_.setAvailableRange(24, 108);
  keyboard_.setOctaveForMiddleC(4);

  setSize(1280, 760);
  setResizable(true, true);
  top_.setStatus(TRANS("No organ loaded."));
  startTimerHz(4);
}

MasterpieceEditor::~MasterpieceEditor() {
  stopTimer();
  proc_.licenceAsker = nullptr;
  proc_.packageAsker = nullptr;
  if (keyWindow_ != nullptr) keyWindow_->removeKeyListener(this);
  // Where the page windows are, for the next time; then they go, before the
  // console they share a processor with.
  rememberPageWindows();
  proc_.saveSettingsIfDirty();
  pageWindows_.clear();
  // A level moved in the last second before quitting.
  proc_.saveRememberedStateIfPending();
}

void MasterpieceEditor::parentHierarchyChanged() {
  auto* top = getTopLevelComponent();
  if (top == keyWindow_.getComponent()) return;
  if (keyWindow_ != nullptr) keyWindow_->removeKeyListener(this);
  keyWindow_ = top;
  if (top != nullptr && top != this) top->addKeyListener(this);
  else if (top == this) addKeyListener(this);
}

bool MasterpieceEditor::keyPressed(const juce::KeyPress& key, juce::Component*) {
  // The plain stop list, for a player who cannot see the console: the same
  // place the console's own panel piston goes.
  if (isStopListKey(key)) {
    toggleStopList();
    return true;
  }
  if (!proc_.applyKeyShortcut(key.getTextDescription().toStdString())) return false;
  console_.repaint();
  for (const auto& w : pageWindows_) w->repaint();
  return true;
}

void MasterpieceEditor::toggleCombinations() {
  if (combinations_ == nullptr) return;
  combinations_->showOrHide(!combinations_->isVisible());
}

void MasterpieceEditor::loadOrgan(const juce::File& requested, bool graphicsOnly) {
  // The organ's files are not here -- its drive unplugged -- but a copy of its
  // definition was kept with its cache: open that (#90).
  juce::File odf = requested;
  if (!odf.existsAsFile())
    if (const auto copy = MasterpieceProcessor::portableCopyFor(odf); copy.existsAsFile()) {
      juce::Logger::writeToLog("load: " + requested.getFullPathName() +
                               " is not there; opening its kept copy " + copy.getFullPathName());
      odf = copy;
    }
  if (loading_) return;  // one load at a time; the dialog is the interlock

  // Reading the file list and extracting the definition/artwork can both
  // take time. Show the same dialog used later for loading its samples.
  if (mp::isOrganArchive(odf.getFullPathName().toStdString())) {
    loading_ = true;
    proc_.beginPackageLoad();
    top_.setStatus(TRANS("Opening %1...").replace("%1", odf.getFileName()));
    showLoadingDialog(odf);
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    auto* processor = &proc_;
    juce::Thread::launch([self, processor, odf, graphicsOnly] {
      juce::String error;
      const auto definitions = processor->openPackagedOrgan(odf, error);
      juce::MessageManager::callAsync([self, odf, graphicsOnly, definitions, error] {
        if (self == nullptr) return;
        self->loading_ = false;
        if (self->loadWindow_ != nullptr) {
          delete self->loadWindow_.getComponent();
          self->loadWindow_ = nullptr;
        }
        if (error == "cancelled") {
          self->top_.setStatus(TRANS("Archive opening cancelled"));
          return;
        }
        // Opened, but with files that did not read: said, and the load goes on.
        if (!definitions.isEmpty() && error.isNotEmpty())
          showWithLog(TRANS("%1 is damaged").replace("%1", odf.getFileName()), error);
        if (definitions.size() == 1) {
          self->loadOrgan(definitions.getFirst(), graphicsOnly);
        } else if (definitions.size() > 1) {
          self->chooseDefinition(definitions, graphicsOnly);
        } else {
          self->status_ = TRANS("Failed to open %1").replace("%1", odf.getFileName());
          self->top_.setStatus(self->status_);
         #if JUCE_IOS
          // One volume of a set, lent on its own: the others are beside it but
          // out of reach until the folder holding them is lent too (#197).
          if (error.contains("multi-volume")) {
            self->askForFolderOf(odf, graphicsOnly);
            return;
          }
         #endif
          showWithLog(TRANS("Could not open %1").replace("%1", odf.getFileName()), error);
        }
      });
    });
    return;
  }

  // The first time an organ is loaded, the engine settings come first: they
  // decide how much memory it will take -- 16-bit samples, mono, streamed
  // releases -- and they only act at load time. Closing them starts the load.
  if (!graphicsOnly && onBeforeFirstLoad && !proc_.settingsFileFor(odf).existsAsFile()) {
    // The organ is read first, without its audio: the console is up in
    // seconds, and Organ settings can then show this organ's own stops and
    // what each would cost. Closing them loads the audio.
    //
    // The window can also close because the application is quitting: then
    // there is nothing to load into, and perhaps no editor left.
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    afterLoad_ = [self, odf] {
      if (self == nullptr || !self->onBeforeFirstLoad) return;
      self->onBeforeFirstLoad([self, odf] {
        if (self == nullptr || juce::MessageManager::getInstance()->hasStopMessageBeenSent())
          return;
        self->startLoad(odf, false);
      });
    };
    startLoad(odf, true);
    return;
  }
  startLoad(odf, graphicsOnly);
}

void MasterpieceEditor::showLoadingDialog(const juce::File& file) {
  // The load runs on its own thread and the message loop keeps running, so
  // the window paints and the Cancel button answers. Doing this work on the
  // message thread is what used to whiten the window for minutes and let
  // Windows offer to kill the program.
  auto dialog = std::make_unique<LoadingDialog>(proc_, file.getFileName());
  dialog->onCancel = [this] { proc_.cancelLoad(); };
  // Closed while the load runs is a Cancel: the load ends as a cancelled one,
  // and its end finds no window left to close.
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  dialog->onClosed = [self] {
    if (self != nullptr && self->loading_) self->proc_.cancelLoad();
  };
  dialog->setSize(460, 190);

  juce::DialogWindow::LaunchOptions opts;
  opts.content.setOwned(dialog.release());
  opts.dialogTitle = TRANS("Loading");
  opts.dialogBackgroundColour = juce::Colour(0xff15171c);
  // No escape-to-close and no title-bar close: leaving this window while the
  // load runs would strand it with nothing watching and no way back.
  opts.escapeKeyTriggersCloseButton = false;
  opts.useNativeTitleBar = true;
  opts.resizable = false;
  loadWindow_ = launchDialog(opts);
  if (loadWindow_ != nullptr) loadWindow_->getProperties().set(kStaysOpen, true);

}

void MasterpieceEditor::startLoad(const juce::File& odf, bool graphicsOnly) {
  if (loading_) return;
  loading_ = true;
  closePageWindowsForLoad();
  console_.freeze();
  top_.setStatus(TRANS("Loading %1...").replace("%1", odf.getFileName()));

  showLoadingDialog(odf);

  juce::Component::SafePointer<MasterpieceEditor> self(this);
  auto& proc = proc_;
  juce::Thread::launch([self, &proc, odf, graphicsOnly] {
    // A load that throws ends as a failed one. JUCE catches what leaves this
    // thread, and the loading dialog then stayed open for good.
    MasterpieceProcessor::LoadResult result;
    try {
      result = proc.loadOrgan(odf, /*maxFramesPerSample*/ 0, graphicsOnly);
    } catch (const std::exception& e) {
      result.ok = false;
      result.error = e.what();
      juce::Logger::writeToLog("load: stopped: " + juce::String::fromUTF8(e.what()));
    }
    // Everything past here touches components, so it belongs to the message
    // thread. The lambda copies what it needs; the loader thread ends here.
    // The window may have closed during the load.
    juce::MessageManager::callAsync([self, odf, graphicsOnly, result] {
      if (self != nullptr) self->finishLoad(odf, graphicsOnly, result);
    });
  });
}

// The publisher's licence, asked for once per organ (ADR-003). Masterpiece
// cannot check it: the player says whether they hold one that lets them play
// the set here. Cancel is the default -- Escape, Enter, closing the box -- and
// leaves the licensed samples out.
void MasterpieceEditor::askForLicence() {
  // Asked after a load that left the licensed samples out: a yes loads the
  // organ again, with them. A load asks before reading any (licenceAsker), so
  // this is for a load that could not, and for the Settings menu.
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  askLicenceQuestion(juce::String(proc_.organModel().organName), juce::String(proc_.licencePublisher()),
                     [self](bool yes) {
                       if (self == nullptr) return;
                       if (!yes) {
                         self->top_.setStatus(self->status_ + "  -  " + TRANS("licensed samples not loaded"));
                         return;
                       }
                       juce::Logger::writeToLog("licence: the player confirmed a licence for this organ");
                       self->proc_.setLicenceConfirmed(true);
                       self->proc_.saveSettingsIfDirty();
                       self->reloadOrgan();
                     });
}

void MasterpieceEditor::askLicenceQuestion(const juce::String& organName, const juce::String& who,
                                           std::function<void(bool)> answered) {
  const juce::String organ = organName.isEmpty() ? TRANS("This organ") : organName;
  const juce::String from = who.isEmpty() ? TRANS("its publisher") : who;
  // Built by hand: a stock two-button box gives Enter to the first button and
  // Escape to the second, and neither order makes Cancel answer both. Here
  // Enter, Escape and closing the box all cancel; only a click on Load loads.
  auto* box = new juce::AlertWindow(
      TRANS("A licence is needed for these samples"),
      TRANS("%1 requires a licence from its publisher to use its samples. "
            "Masterpiece cannot check that licence.\n\n"
            "Load its samples only if your licence from %2 allows you to play this set "
            "in this program. Your answer is saved for this organ and can be withdrawn "
            "from the Settings menu.")
          .replace("%1", who.isEmpty() ? organ : TRANS("%1, from %2,").replace("%1", organ).replace("%2", who))
          .replace("%2", from),
      juce::MessageBoxIconType::QuestionIcon, this);
  box->addButton(TRANS("Load the samples"), 1);
  box->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey),
                 juce::KeyPress(juce::KeyPress::returnKey));
  box->enterModalState(true, juce::ModalCallbackFunction::create([answered](int result) {
                         answered(result == 1);
                       }),
                       true);  // deleted when dismissed
}

void MasterpieceEditor::askForPackage(const juce::String& package, const juce::String& name, const juce::File& start,
                                      std::function<void(const juce::String&)> answered) {
  auto* box = new juce::AlertWindow(
      TRANS("A package this organ plays is missing"),
      TRANS("This organ plays package %1, \"%2\", which is not in its folder, a folder in it, nor the package "
            "folder.\n\nChoose the archive of that package, or the folder that holds it. The choice is kept "
            "for this organ. A package folder for every organ is set in Organ settings, Loading.")
          .replace("%1", package)
          .replace("%2", name.isEmpty() ? TRANS("no name given") : name),
      juce::MessageBoxIconType::QuestionIcon, this);
  box->addButton(TRANS("Choose an archive..."), 1);
  box->addButton(TRANS("Choose a folder..."), 2);
  box->addButton(TRANS("Play without it"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  box->enterModalState(
      true, juce::ModalCallbackFunction::create([self, answered, start](int result) {
        if (result == 0 || self == nullptr) {
          answered({});
          return;
        }
        const bool folder = result == 2;
        // The chooser opens on a message of its own, once the box has gone
        // (#591). Launched from the box's own callback it opens while the box
        // is still up, and the desktop can put it behind the window that is
        // closing as it appears.
        juce::MessageManager::callAsync([self, answered, start, folder] {
          if (self == nullptr) {
            answered({});
            return;
          }
          self->packageChooser_ = std::make_unique<juce::FileChooser>(
              folder ? TRANS("The folder that holds the package") : TRANS("The package's archive"),
              start.isDirectory() ? start : juce::File::getSpecialLocation(juce::File::userHomeDirectory),
              folder ? juce::String() : "*.rar;*.zip;*.7z");
          const int flags = juce::FileBrowserComponent::openMode |
                            (folder ? juce::FileBrowserComponent::canSelectDirectories
                                    : juce::FileBrowserComponent::canSelectFiles);
          self->packageChooser_->launchAsync(flags, [answered](const juce::FileChooser& chooser) {
            answered(chooser.getResult() == juce::File() ? juce::String()
                                                        : chooser.getResult().getFullPathName());
          });
        });
      }),
      true);  // deleted when dismissed
}

// The message-thread half of a load: close the dialog, then either report the
// failure or build the console from the model that is now in place.
void MasterpieceEditor::chooseDefinition(const juce::Array<juce::File>& definitions,
                                         bool graphicsOnly) {
  top_.setStatus(TRANS("These packages hold several organs: choose one"));
  const juce::File last = MasterpieceProcessor::rememberedDefinition(definitions);
  juce::Array<juce::File> order;
  if (last != juce::File()) order.add(last);
  for (const auto& d : definitions)
    if (d != last) order.add(d);

  juce::PopupMenu menu;
  menu.addSectionHeader(TRANS("Which organ?"));
  for (int i = 0; i < order.size(); ++i)
    menu.addItem(i + 1, order[i].getFileNameWithoutExtension() +
                            (order[i] == last ? "  " + TRANS("(last opened)") : juce::String()));
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&top_),
                     [self, order, graphicsOnly](int choice) {
                       if (self == nullptr) return;
                       if (choice < 1 || choice > order.size()) {
                         self->top_.setStatus(TRANS("No organ opened"));
                         return;
                       }
                       self->loadOrgan(order[choice - 1], graphicsOnly);
                     });
}

namespace {

// A small square with an arrow out of its corner, on every page tab: the
// page can go to a window of its own. A right-click on the tab does the
// same, but nobody finds a right-click; the icon is there to be seen.
class PopOutIcon : public juce::Button {
public:
  PopOutIcon() : juce::Button("pop out") {
    setSize(16, 16);
    setTooltip(TRANS("Open this page in its own window, to move to another screen"));
  }
  void setOut(bool out) {
    if (out_ == out) return;
    out_ = out;
    setTooltip(out ? TRANS("Close this page's window and bring it back here")
                   : TRANS("Open this page in its own window, to move to another screen"));
    repaint();
  }
  void paintButton(juce::Graphics& g, bool over, bool) override {
    // A square in the middle, whatever height the tab gives the button.
    const float side = static_cast<float>(juce::jmin(getWidth(), getHeight()));
    const auto r = getLocalBounds().toFloat().withSizeKeepingCentre(side, side).reduced(3.0f);
    const auto colour = out_ ? juce::Colour(0xffe6cf7a)
                             : (over ? juce::Colours::white : juce::Colour(0xff9aa3b2));
    g.setColour(colour);
    const auto box = r.withTrimmedTop(3.0f).withTrimmedRight(3.0f);
    if (out_) g.fillRoundedRectangle(box, 1.5f);
    else g.drawRoundedRectangle(box, 1.5f, 1.2f);
    juce::Path arrow;
    arrow.startNewSubPath(box.getCentreX(), box.getCentreY());
    arrow.lineTo(r.getRight(), r.getY());
    g.strokePath(arrow, juce::PathStrokeType(1.4f));
    juce::Path head;
    head.addTriangle(r.getRight(), r.getY(), r.getRight() - 4.5f, r.getY(), r.getRight(), r.getY() + 4.5f);
    g.fillPath(head);
  }

private:
  bool out_ = false;
};

}  // namespace

void MasterpieceEditor::addPopOutIcons() {
  if (!pagesCanFloat()) return;
  for (int i = 0; i < console_.pageCount() && i < pageTabs_.getNumTabs(); ++i) {
    auto* icon = new PopOutIcon();
    icon->onClick = [this, i] {
      if (auto* w = pageWindowFor(i)) closePageWindow(w);
      else openPageWindow(i);
    };
    // Owned by the tab from here.
    pageTabs_.getTabButton(i)->setExtraComponent(icon, juce::TabBarButton::afterText);
  }
  refreshPopOutIcons();
}

void MasterpieceEditor::refreshPopOutIcons() {
  for (int i = 0; i < pageTabs_.getNumTabs(); ++i)
    if (auto* button = pageTabs_.getTabButton(i))
      if (auto* icon = dynamic_cast<PopOutIcon*>(button->getExtraComponent()))
        icon->setOut(pageWindowFor(i) != nullptr);
}

PageWindow* MasterpieceEditor::pageWindowFor(int page) const {
  for (const auto& w : pageWindows_)
    if (w->page() == page) return w.get();
  return nullptr;
}

juce::String MasterpieceEditor::layoutName(int layout) const {
  const auto& m = proc_.organModel();
  juce::String name = layout == 0 ? TRANS("main layout") : TRANS("alternate layout %1").replace("%1", juce::String(layout));
  if (layout >= 0 && layout < 4 && m.consoleHeightPx[layout] > m.consoleWidthPx[layout] &&
      m.consoleWidthPx[layout] > 0)
    name = TRANS("%1 (portrait)").replace("%1", name);
  return name;
}

void MasterpieceEditor::openPageWindow(int page, juce::Rectangle<int> bounds, int layout,
                                       bool borderless) {
  if (auto* open = pageWindowFor(page)) {
    open->toFront(true);
    return;
  }
  const auto& m = proc_.organModel();
  auto window = std::make_unique<PageWindow>(proc_, page, console_.layout(),
                                             m.organName.empty() ? juce::String("Masterpiece")
                                                                 : juce::String(m.organName),
                                             layout);
  // A portrait layout goes to a portrait screen, when there is one.
  const int shown = layout >= 0 ? layout : console_.layout();
  const bool portrait = shown >= 0 && shown < 4 && m.consoleHeightPx[shown] > m.consoleWidthPx[shown];
  const auto& displays = juce::Desktop::getInstance().getDisplays();
  if (!bounds.isEmpty() && displays.getDisplayForRect(bounds) != nullptr &&
      displays.getTotalBounds(true).intersects(bounds)) {
    window->setBounds(bounds);
    keepOnScreen(*window);
  } else {
    // A screen other than this window's, if there is one: that is what the
    // window is for. Otherwise beside this one, a little down and across.
    const auto* here = displays.getDisplayForRect(getScreenBounds());
    const juce::Displays::Display* other = nullptr;
    for (const auto& d : displays.displays)
      if (portrait && d.userArea.getHeight() > d.userArea.getWidth()) {
        other = &d;
        break;
      }
    if (other == nullptr)
      for (const auto& d : displays.displays)
        if (here == nullptr || d.userArea != here->userArea) {
          other = &d;
          break;
        }
    const auto area = (other != nullptr ? other->userArea : (here != nullptr ? here->userArea
                                                                            : getScreenBounds()))
                          .reduced(40);
    const int w = juce::jmin(window->getWidth(), area.getWidth());
    const int h = juce::jmin(window->getHeight(), area.getHeight());
    const int offset = other != nullptr ? 0 : 40 * static_cast<int>(pageWindows_.size() + 1);
    window->setBounds(area.getX() + offset, area.getY() + offset, w, h);
  }
  if (borderless) window->setBorderless(true);
  window->onClosed = [this](PageWindow* w) {
    // Not from inside the window's own callback.
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    juce::MessageManager::callAsync([self, w] {
      if (self != nullptr) self->closePageWindow(w);
    });
  };
  window->onPlaced = [this] { rememberPageWindows(); };
  // Shortcuts work from a page on another screen as well.
  window->addKeyListener(this);
  window->setVisible(true);
  pageWindows_.push_back(std::move(window));
  rememberPageWindows();
  refreshPopOutIcons();
}

void MasterpieceEditor::closePageWindow(PageWindow* window) {
  int page = -1;
  for (auto it = pageWindows_.begin(); it != pageWindows_.end(); ++it)
    if (it->get() == window) {
      page = (*it)->page();
      pageWindows_.erase(it);
      break;
    }
  rememberPageWindows();
  refreshPopOutIcons();
  // The page goes back where it came from: its tab, shown, in this window.
  if (page >= 0 && page < pageTabs_.getNumTabs() && showingConsole_)
    pageTabs_.setCurrentTabIndex(page, true);
}

void MasterpieceEditor::rememberPageWindows() {
  std::vector<MasterpieceProcessor::PagePlace> places;
  for (const auto& w : pageWindows_) {
    const auto b = w->getBounds();
    places.push_back({w->page(), b.getX(), b.getY(), b.getWidth(), b.getHeight(), w->layout(),
                       w->borderless()});
  }
  proc_.setPageWindowPlaces(std::move(places));
}

void MasterpieceEditor::closePageWindowsForLoad() {
  if (pageWindows_.empty()) return;
  // Written to the organ being left, now, before the load changes which
  // organ the settings file is.
  rememberPageWindows();
  proc_.saveSettingsIfDirty();
  pageWindows_.clear();
}

// Only in the program itself. A plugin that opens windows of its own fights
// the host, which arranges the plugin's window and expects it to be the only
// one; there the pages stay tabs.
bool MasterpieceEditor::pagesCanFloat() const {
  return proc_.wrapperType == juce::AudioProcessor::wrapperType_Undefined ||
         proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

void MasterpieceEditor::restorePageWindows() {
  if (!pagesCanFloat()) return;
  // A copy: each window opened writes the list again.
  const auto places = proc_.pageWindowPlaces();
  for (const auto& p : places)
    if (p.page >= 0 && p.page < console_.pageCount())
      openPageWindow(p.page, {p.x, p.y, p.w, p.h}, p.layout, p.borderless);
}

namespace {
// The recorder's own window: shown and hidden, never modal, so the console
// stays playable while it records.
class RecorderWindow : public juce::DocumentWindow {
public:
  explicit RecorderWindow(MasterpieceProcessor& p)
      : juce::DocumentWindow(TRANS("Recorder"), juce::Colour(0xff15171c), juce::DocumentWindow::closeButton) {
    setUsingNativeTitleBar(true);
    auto* panel = new RecorderPanel(p);
    setContentOwned(panel, false);
    setResizable(false, false);
    // The window is as wide as the buttons ask for: the labels are longer in
    // some languages than in others (#429).
    setSize(juce::jmax(420, panel->preferredWidth()), 420);
    fitToScreen(*this);
  }
  void closeButtonPressed() override { showFloating(*this, false, false); }
};
}  // namespace

void MasterpieceEditor::toggleRecorder() {
  if (recorderWindow_ == nullptr) {
    recorderWindow_ = std::make_unique<RecorderWindow>(proc_);
    // Beside the main window, at its top right, where it hides the least.
    const auto b = getScreenBounds();
    if (!kMobile)
      recorderWindow_->setTopLeftPosition(b.getRight() - recorderWindow_->getWidth() - 20, b.getY() + 80);
  }
  showFloating(*recorderWindow_, !recorderWindow_->isVisible(),
               juce::Process::isForegroundProcess());
}

void MasterpieceEditor::toggleSplitKeyboard() {
  if (splitWindow_ == nullptr) {
    splitWindow_ = std::make_unique<SplitKeyboardWindow>(proc_);
    const auto b = getScreenBounds();
    if (!kMobile) splitWindow_->setTopLeftPosition(b.getX() + 40, b.getY() + 80);
  }
  showFloating(*splitWindow_, !splitWindow_->isVisible(), juce::Process::isForegroundProcess());
}

void MasterpieceEditor::reloadOrgan() {
  const juce::File odf = proc_.loadedOrganFile();
  if (odf.existsAsFile()) loadOrgan(odf);
}

void MasterpieceEditor::finishLoad(const juce::File& odf, bool graphicsOnly,
                                   const MasterpieceProcessor::LoadResult& result) {
  loading_ = false;
  updateNotice_.loadFinished();
  // Taken now so a failed load does not leave it waiting for the next one.
  auto then = std::move(afterLoad_);
  afterLoad_ = nullptr;
  if (loadWindow_ != nullptr) {
    delete loadWindow_.getComponent();
    loadWindow_ = nullptr;
  }

  if (!result.ok) {
    // Cancelling is a choice, not a fault, and must not look like one.
    status_ = result.error == "cancelled"
                  ? TRANS("Load cancelled - no organ is loaded")
                  : TRANS("Failed to load %1: %2")
                        .replace("%1", odf.getFileName())
                        .replace("%2", juce::String(result.error));
    top_.setStatus(status_);
    // Stopping at the memory limit is the one failure a player can fix from
    // here, so it is said in full rather than left in the status line.
    if (result.outOfMemory)
      juce::AlertWindow::showMessageBoxAsync(
          juce::MessageBoxIconType::WarningIcon, TRANS("Not enough memory for this organ"),
          TRANS("%1 was not loaded: its samples need more than the memory limit (%2 GB).\n\n"
                "Its console is shown without sound, so Organ settings can be opened "
                "from the Settings menu: leave out perspectives or stops, or on the Loading tab "
                "load 16-bit samples, stream the release tails, load in mono or preload less of "
                "each sample -- or raise the memory limit on the same tab, if this computer has "
                "the memory to spare.")
              .replace("%1", odf.getFileNameWithoutExtension())
              .replace("%2", juce::String(proc_.memoryLimitBytes() / (1024.0 * 1024.0 * 1024.0), 1)));
    // The organ's console, without its audio, so the way out is on screen
    // (#53: the only way back was deleting the organ's settings file). The
    // same after Cancel, which is often a load taking too much (#90). Once:
    // a console-only load reads no samples, and never comes back here.
    const bool cancelled = result.error == "cancelled";
    if ((result.outOfMemory || cancelled) && !graphicsOnly && odf.existsAsFile()) {
      loadOrgan(odf, true);
      return;
    }
    // Whatever organ is left, or none: drawn from the model as it now is.
    jamb_.rebuild();
    expression_.rebuild();
    console_.rebuild();
    return;
  }
  // A licence confirmed during this load is the organ's from now on: saved
  // once the organ is in place, so the next load does not ask again.
  if (result.licenceAsked) proc_.saveSettingsIfDirty();

  // Timed separately from the model: this is where the console artwork is
  // actually decoded, and on a set with a thousand bitmaps it can dominate a
  // load that has no audio in it at all.
  const double artStart = juce::Time::getMillisecondCounterHiRes();
  jamb_.rebuild();
  expression_.rebuild();
  console_.rebuild();
  if (combinations_ != nullptr) {
    combinations_->panel().rebuild();
    // Opened only as the player last left it. It used to open by itself on
    // any organ without pistons of its own, so it would be found, and popped
    // up on nearly every Open (#90); the Combinations button finds it.
    combinations_->place(getScreenBounds(), false);
  }
  juce::Logger::writeToLog(
      "load: artwork       " +
      juce::String(juce::Time::getMillisecondCounterHiRes() - artStart, 1) +
      " ms");

  loadPanels();
  rebuildPageTabs();
  if (pageTabs_.getNumTabs() > 0) pageTabs_.setCurrentTabIndex(0, false);
  addPopOutIcons();
  restorePageWindows();

  // A set with no console artwork opens on the stop list rather than on an
  // empty picture.
  // A set that ships for several console sizes lets the player pick. Rebuilt
  // per organ, because the count is the organ's.
  layout_.clear(juce::dontSendNotification);
  for (int i = 0; i < console_.layoutCount(); ++i) {
    juce::String name = i == 0 ? TRANS("Console: main")
                               : TRANS("Console: alt %1").replace("%1", juce::String(i));
    if (console_.isPortrait(i)) name = TRANS("%1 (portrait)").replace("%1", name);
    layout_.addItem(name, i + 1);
  }
  layout_.setSelectedId(console_.layout() + 1, juce::dontSendNotification);
  shapeTall_ = -1;  // the new organ's layout is chosen for the window's shape

  // Which manual the on-screen keys play. Named by division, because "Grand
  // Orgue" means something to a player and "channel 3" does not.
  manual_.clear(juce::dontSendNotification);
  // Keyed by KEYBOARD, not by channel. A ComboBox ticks every item sharing the
  // selected id, so two manuals answering to one channel used to look like
  // three selected at once and left one of them unreachable -- which is
  // exactly what a saved mapping with three manuals on channel 1 produced.
  // Keyboard ids are unique by construction.
  for (Id kb : proc_.playableKeyboards())
    manual_.addItem(juce::String(proc_.keyboardName(kb)), static_cast<int>(kb));
  if (manual_.getNumItems() > 0) {
    // The organ's preferred manual: the widest compass when declared, else
    // the unenclosed manual shipping the most pipework. On a set that
    // declares no compass (Nancy) widest-of-nothing is the pedal, which is
    // how the piano ends up playing the one division with no stops drawn.
    int best = manual_.getItemId(0);
    if (const Id def = proc_.preferredKeyboard())
      if (manual_.indexOfItemId(static_cast<int>(def)) >= 0)
        best = static_cast<int>(def);
    manual_.setSelectedId(best, juce::dontSendNotification);
    keyboard_.setMidiChannel(proc_.channelForKeyboard(static_cast<Id>(best)));
  }

  // The on-screen keyboard stays hidden on every organ, including the sets
  // whose manuals are backdrop photos and draw no clickable keys. It used to
  // force itself on for those, on the reasoning that it was the only thing
  // playable with a mouse -- but a mouse is not how this is played, and the
  // strip sat across the bottom of every console that happened to be
  // photographed that way. The Keys button is there when it is wanted.

  showingConsole_ = console_.hasArtwork();
  // A new organ brings its own console; the plain stop list is asked for
  // again if the player wants it. Program couplers just turned on from the
  // View menu are the exception: the list opens on them.
  showingStopList_ = false;
  if (std::exchange(showCouplersAfterLoad_, false) && proc_.programCouplers() &&
      !proc_.programCouplerList().empty() && console_.hasArtwork())
    showingStopList_ = true;
  resized();
  if (showingStopList_) jamb_.focusFirstStop();

  // The organ's name is in the window title, so the bar says what the title
  // cannot: how much instrument arrived, and whether it has any audio.
  const auto& m = proc_.organModel();
  // Each part is a sentence of its own, so a language can word and order it
  // as it needs to; they are joined by commas.
  juce::StringArray parts;
  parts.add(TRANS("%1 stops").replace("%1", juce::String(m.stops.size())));
  parts.add(TRANS("%1 ranks").replace("%1", juce::String(m.ranks.size())));
  if (graphicsOnly) {
    // "0 samples (0 MB)" reads as a set that failed to load. Say what was
    // actually asked for, so a silent console is not mistaken for a broken one.
    parts.add(TRANS("graphics only - no audio loaded"));
  } else {
    parts.add(TRANS("%1 samples (%2 MB)")
                  .replace("%1", juce::String(result.samples.loaded))
                  .replace("%2", juce::String(proc_.sampleLibrary().residentBytes() / (1024 * 1024))));
    // Which ones, in the log: a count alone says nothing to act on (#90).
    for (const auto& f : result.samples.missingFiles)
      juce::Logger::writeToLog("samples: missing " + juce::String(f));
    if (result.samples.missing > static_cast<int>(result.samples.missingFiles.size()))
      juce::Logger::writeToLog("samples: and " +
                               juce::String(result.samples.missing - static_cast<int>(result.samples.missingFiles.size())) +
                               " more missing");
    for (const auto& f : result.samples.failedFiles)
      juce::Logger::writeToLog("samples: unreadable " + juce::String(f));
    if (result.samples.standIns > 0)
      juce::Logger::writeToLog("samples: " + juce::String(result.samples.standIns) +
                               " standard noises played as generated ones");
    if (result.samples.missing > 0)
      parts.add(TRANS("%1 missing (named in the log)").replace("%1", juce::String(result.samples.missing)));
    if (result.samples.failed > 0)
      parts.add(TRANS("%1 unreadable (named in the log)").replace("%1", juce::String(result.samples.failed)));
    if (result.samples.encrypted > 0)
      parts.add(TRANS("%1 encrypted").replace("%1", juce::String(result.samples.encrypted)));
    if (result.samples.licensed > 0)
      parts.add(TRANS("%1 awaiting a licence").replace("%1", juce::String(result.samples.licensed)));
  }
  status_ = parts.joinIntoString(", ");
  if (!graphicsOnly && result.incomplete)
    status_ += "  -  " + TRANS("INCOMPLETE (memory limit)");
  if (!graphicsOnly && result.incomplete) {
    const int percent = result.samples.wanted > 0
                            ? juce::roundToInt(100.0 * result.samples.loaded / result.samples.wanted)
                            : 0;
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon, TRANS("Only part of this organ fits"),
        TRANS("%1 of %2 samples (%3%) were loaded before the memory limit (%4 GB) was "
              "reached. The organ plays, but some stops or notes will be silent.\n\n"
              "To load all of it: leave out perspectives or stops in Organ settings, load "
              "16-bit samples or stream the release tails on its Loading tab, or raise the "
              "limit on the same tab if this computer has the memory to spare.")
            .replace("%1", juce::String(result.samples.loaded))
            .replace("%2", juce::String(result.samples.wanted))
            .replace("%3", juce::String(percent))
            .replace("%4", juce::String(proc_.memoryLimitBytes() / (1024.0 * 1024.0 * 1024.0), 1)));
  } else if (!graphicsOnly && result.samples.licensed > 0 && !proc_.licenceConfirmed()) {
    // Asked already, before any sample was read, and answered no: said, not
    // asked again. Only a load that could not ask is asked now.
    if (result.licenceAsked) top_.setStatus(status_ + "  -  " + TRANS("licensed samples not loaded"));
    else askForLicence();
  } else if (!graphicsOnly && result.samples.loaded == 0 && result.samples.encrypted > 0) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon, TRANS("This organ's samples are encrypted"),
        TRANS("All %1 of its samples are encrypted (.hbw/.hbx) for the program they were made "
              "for, and Masterpiece cannot play them. The console loads, but the organ "
              "makes no sound.")
            .replace("%1", juce::String(result.samples.encrypted)));
  } else if (!graphicsOnly && result.samples.failed > 0) {
    // The organ plays, with holes in it. Without this the only sign was a
    // line in the log and pipes that stay silent for no visible reason.
    juce::String which;
    for (size_t i = 0; i < result.samples.failedFiles.size() && i < 3; ++i)
      which << juce::String(result.samples.failedFiles[i]) << "\n";
    juce::String fix =
        which.contains(" is damaged") || which.contains(": damaged")
            ? TRANS("The archive they come from is damaged, usually by a download that was cut "
                    "short or corrupted. Test it in 7-Zip or WinRAR, and download it again.")
            : TRANS("The files are damaged or not sample files Masterpiece can read. Installing "
                    "the organ again usually puts them right.");
    // No plurals in the translation files: one sentence for a single file,
    // another for several.
    showWithLog(TRANS("Some samples could not be read"),
                (result.samples.failed == 1
                     ? TRANS("1 sample file was unreadable, so the pipes it belongs to are silent. "
                             "The organ plays without it.")
                     : TRANS("%1 sample files were unreadable, so the pipes they belong to are "
                             "silent. The organ plays without them.")
                           .replace("%1", juce::String(result.samples.failed))) +
                    "\n\n" + which + "\n" + fix);
  }
  // Say it out loud, once. The status line has no room beside the console's
  // buttons, and a mapping that changes without a word costs more trust than
  // the fault it fixes. Only shown when a repair actually happened, which is
  // the first load of a mapping saved by 0.3.7 or earlier.
  if (const int fixed = proc_.midiMapRepairedOnLoad(); fixed > 0) {
    status_ += "  -  " + TRANS("MIDI mapping repaired");
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon, TRANS("MIDI mapping repaired"),
        TRANS("The saved MIDI mapping for this organ assigned more than one manual "
              "to the same channel, which left some manuals silent.") +
            " " +
            (fixed == 1 ? TRANS("1 conflicting assignment was removed, and each manual now "
                                "uses the organ's own channel.")
                        : TRANS("%1 conflicting assignments were removed, and each manual now "
                                "uses the organ's own channel.")
                              .replace("%1", juce::String(fixed))) +
            "\n\n" +
            TRANS("Your previous mapping was saved next to the new one with the "
                  "ending \".before-repair\". You can reassign manuals in "
                  "Settings > MIDI."));
  }
  top_.setStatus(status_);

  if (onOrganLoaded)
    onOrganLoaded(m.organName.empty()
                      ? odf.getFileNameWithoutExtension()
                      : juce::String(m.organName));
  if (!graphicsOnly) MasterpieceProcessor::rememberDefinition(odf);
  if (then) then();
}

void MasterpieceEditor::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff15171c));
}

void MasterpieceEditor::showUpdateNotice(const juce::String& latest) { updateNotice_.show(latest); }

void MasterpieceEditor::resized() {
  auto r = getLocalBounds();
  auto statusStrip = r.removeFromBottom(20);
  if (updateNotice_.isVisible()) {
    // Clear of the window's resize corner, which covers the strip's last
    // pixels and would hide the notice's close button.
    auto noticeArea = statusStrip;
    noticeArea.removeFromRight(20);
    updateNotice_.setBounds(noticeArea.removeFromRight(std::min(updateNotice_.preferredWidth(), noticeArea.getWidth())));
    statusStrip.setRight(updateNotice_.getX());
  }
  top_.statusLabel().setBounds(statusStrip);

  // One band for every control. These are the editor's own children rather
  // than the bar's, so they are placed into the right of the same strip and
  // the bar lays itself out in whatever is left -- no reparenting, and the
  // status line absorbs the difference.
  auto bar = r.removeFromTop(36);
  layout_.setVisible(showingConsole_ && !showingStopList_ && console_.layoutCount() > 1);
  // Wide enough to say "(portrait)" when the organ has a portrait layout.
  int layoutW = 130;
  for (int l = 1; l < console_.layoutCount(); ++l)
    if (console_.isPortrait(l)) layoutW = 190;
  // On a narrow window -- a monitor stood on end is 1080 wide -- the buttons
  // on the right left the bar on the left too little room, and the volume
  // fader went with it (#53). Below that, the buttons take a row of their own.
  const int menuW = kMobile ? 84 : 70;
  const int stepW = kMobile ? 44 : 30;
  bool stacked = false;
  {
    // Every button in the band, as placed below: the three menus on the
    // left; on the right panic, save, the layout, the temperament and the
    // sequencer. What is left is the bar's, and it needs No DSP and a fader
    // a hand can use.
    const int menus = 3 * menuW + 14;
    const int buttons = 2 * (kMobile ? 46 : 38) + (layout_.isVisible() ? layoutW : 0) + 140 +
                        2 * stepW + 96 + (kMobile ? 64 : 56);
    if (bar.getWidth() - menus - buttons < TopBar::kMinWidth) {
      auto row = bar;
      fileMenu_.setBounds(row.removeFromLeft(menuW).reduced(2));
      viewMenu_.setBounds(row.removeFromLeft(menuW).reduced(2));
      settingsButton_.setBounds(row.removeFromLeft(menuW + 14).reduced(2));
      top_.setBounds(row);
      bar = r.removeFromTop(36);
      stacked = true;
    }
  }
  if (!stacked) {
    fileMenu_.setBounds(bar.removeFromLeft(menuW).reduced(2));
    viewMenu_.setBounds(bar.removeFromLeft(menuW).reduced(2));
    settingsButton_.setBounds(bar.removeFromLeft(menuW + 14).reduced(2));
  }
  panicButton_.setBounds(bar.removeFromRight(kMobile ? 46 : 38).reduced(2));
  saveStateButton_.setBounds(bar.removeFromRight(kMobile ? 46 : 38).reduced(2));
  if (layout_.isVisible())
    layout_.setBounds(bar.removeFromRight(layoutW).reduced(2));
  tuningButton_.setBounds(bar.removeFromRight(140).reduced(2));
  // Sequencer, right to left: next, the frame it is on, previous, the setter.
  // A fingertip wide on a phone or tablet.
  stepNext_.setBounds(bar.removeFromRight(stepW).reduced(2));
  stepFrame_.setBounds(bar.removeFromRight(96).reduced(2));
  stepPrev_.setBounds(bar.removeFromRight(stepW).reduced(2));
  setter_.setBounds(bar.removeFromRight(kMobile ? 64 : 56).reduced(2));
  if (!stacked) top_.setBounds(bar);

  // The tabs keep a strip of their own, and only when there is more than one
  // page to choose between -- so a single-page organ shows one row in total.
  bool otherTabs = !panelTabs_.empty();
#if MP_HAS_SCORE
  otherTabs = otherTabs || scoreTab_ >= 0;
#endif
  pageTabs_.setVisible(showingConsole_ && !showingStopList_ &&
                       (console_.pageCount() > 1 || otherTabs));
  if (pageTabs_.isVisible()) pageTabs_.setBounds(r.removeFromTop(28));

  manual_.setVisible(showingKeyboard_ && manual_.getNumItems() > 1);
  keyboard_.setVisible(showingKeyboard_);
  if (showingKeyboard_) {
    auto keys = r.removeFromBottom(96);
    if (manual_.isVisible())
      manual_.setBounds(keys.removeFromTop(24).removeFromLeft(200).reduced(2));
    keyboard_.setBounds(keys);
    // Size the keys to the window rather than leaving a blank half: the
    // default key width leaves the component short of its own bounds.
    if (keys.getWidth() > 0)
      keyboard_.setKeyWidth(juce::jmax(
          8.0f, static_cast<float>(keys.getWidth()) / 52.0f));
  }
  // Collapsed, the strip takes no width at all -- it used to remove 120px
  // whether or not the organ had a single enclosure to show in it.
  const bool swellVisible = showingSwell_ && expression_.shoeCount() > 0;
  expression_.setVisible(swellVisible);
  if (swellVisible) expression_.setBounds(r.removeFromRight(120));

  for (size_t i = 0; i < panels_.size(); ++i)
    if (panelWindows_[i] == nullptr) panels_[i]->setVisible(false);
#if MP_HAS_SCORE
  if (score_ != nullptr && scoreWindow_ == nullptr) score_->setVisible(false);
#endif
  if (showingStopList_) {
    // The plain list of stops, in place of the console: the way in for a
    // player who cannot see the console.
    consoleView_.setVisible(false);
    jambView_.setVisible(true);
    jambView_.setBounds(r);
    const int jambWidth = jambView_.getWidth() - 12;
    jamb_.setSize(jambWidth, jamb_.heightFor(jambWidth));
    return;
  }
  if (showingConsole_ && showingPanel_ >= 0 &&
      showingPanel_ < static_cast<int>(panels_.size()) &&
      panelWindows_[static_cast<size_t>(showingPanel_)] == nullptr) {
    consoleView_.setVisible(false);
    jambView_.setVisible(false);
    auto& panel = *panels_[static_cast<size_t>(showingPanel_)];
    panel.setBounds(r);
    panel.setVisible(true);
    return;
  }
#if MP_HAS_SCORE
  if (showingConsole_ && showingScore_ && score_ != nullptr && scoreWindow_ == nullptr) {
    consoleView_.setVisible(false);
    jambView_.setVisible(false);
    score_->setBounds(r);
    score_->setVisible(true);
    return;
  }
#endif
  consoleView_.setVisible(showingConsole_);
  jambView_.setVisible(!showingConsole_);
  if (showingConsole_) {
    // The layout follows the shape of the room the console has (#366): a
    // portrait layout when it is taller than it is wide, the main one when
    // it is wider. A layout chosen by hand holds until the shape turns.
    if (console_.layoutCount() > 1 && !r.isEmpty()) {
      const int tall = r.getHeight() > r.getWidth() ? 1 : 0;
      if (tall != shapeTall_) {
        shapeTall_ = tall;
        const int chosen = console_.layoutForShape(tall == 1);
        if (chosen != console_.layout()) {
          console_.setLayout(chosen);
          layout_.setSelectedId(chosen + 1, juce::dontSendNotification);
        }
      }
    }
    // Scale the artwork to fit rather than scrolling a 1536x864 console
    // through a smaller window. JUCE routes mouse events back through the
    // transform, so drawstops stay clickable at any zoom.
    const auto art = console_.artworkBounds();
    consoleArea_ = r;
    if (art.getWidth() <= 0 || art.getHeight() <= 0) {
      consoleView_.setBounds(r);
    } else {
      console_.setTransform({});
      console_.setBounds(0, 0, art.getRight(), art.getBottom());
      // Scale UP as well as down. The old cap at 1.0 meant a console drawn
      // smaller than the window sat at its native size with a band of dead
      // background beside it, which read as part of the program rather than
      // as empty space.
      //
      // Uniformly, and never to fill the width exactly: the console is a
      // photograph of a real instrument, so stretching it to the window's
      // aspect would visibly distort the case and the keys. Whichever
      // dimension runs out first sets the size. A small panel of its own is
      // magnified as the console is, and no further.
      const float scale = console_.fitScale(r, proc_.smallPagesFill());
      // The artwork is drawn from its own top-left, so a page whose drawing
      // does not start at the origin keeps no band of nothing beside it.
      console_.setTransform(ConsoleView::artworkTransform(art, scale));

      // Centre by moving the VIEWPORT, not the console inside it: a viewport
      // positions its own viewed component, so a translation applied to the
      // console is overwritten the moment the viewport lays out. Sizing the
      // viewport to the scaled artwork and centring that leaves the spare
      // width split evenly either side instead of banked in one strip.
      const int w = juce::roundToInt(static_cast<float>(art.getWidth()) * scale);
      const int h = juce::roundToInt(static_cast<float>(art.getHeight()) * scale);
      consoleView_.setBounds(
          r.withSizeKeepingCentre(juce::jmin(w, r.getWidth()),
                                  juce::jmin(h, r.getHeight())));
    }
  } else {
    jambView_.setBounds(r);
    const int jambWidth = jambView_.getWidth() - 12;
    jamb_.setSize(jambWidth, jamb_.heightFor(jambWidth));
  }
}

// ---- panels (#237) ---------------------------------------------------------

void MasterpieceEditor::loadPanels() {
  for (auto& w : panelWindows_) w.reset();
  for (auto& p : panels_) removeChildComponent(p.get());
  panels_.clear();
  panelWindows_.clear();
  showingPanel_ = -1;
  const auto layouts = loadPanelLayouts(proc_);
  for (size_t i = 0; i < layouts.size(); ++i) {
    panels_.push_back(std::make_unique<PanelView>(proc_, static_cast<int>(i), layouts[i]));
    panelWindows_.emplace_back();
    auto* view = panels_.back().get();
    const int index = static_cast<int>(i);
    wirePanel(index);
    addChildComponent(*view);
    if (layouts[i].detached && !kMobile) setPanelDetached(index, true);
  }
}

void MasterpieceEditor::rebuildPageTabs() {
  pageTabs_.clearTabs();
  for (int i = 0; i < console_.pageCount(); ++i)
    pageTabs_.addTab(console_.pageName(i), juce::Colour(0xff2a2f3a), i);
  panelTabs_.clear();
  for (size_t i = 0; i < panels_.size(); ++i)
    if (panelWindows_[i] == nullptr) {
      panelTabs_.push_back(static_cast<int>(i));
      pageTabs_.addTab(panels_[i]->title(),
                       juce::Colour(0xff3a3326), pageTabs_.getNumTabs());
    }
#if MP_HAS_SCORE
  scoreTab_ = -1;
  if (score_ != nullptr && scoreWindow_ == nullptr) {
    scoreTab_ = pageTabs_.getNumTabs();
    pageTabs_.addTab(TRANS("Score"), juce::Colour(0xff26333a), scoreTab_);
  }
  if (scoreTab_ < 0) showingScore_ = false;
#endif
  if (showingPanel_ >= 0 &&
      std::find(panelTabs_.begin(), panelTabs_.end(), showingPanel_) == panelTabs_.end())
    showingPanel_ = -1;
  // The tabs were made again: their pop-out icons with them.
  addPopOutIcons();
  resized();
}

void MasterpieceEditor::savePanels() {
  std::vector<PanelLayout> layouts;
  for (size_t i = 0; i < panels_.size(); ++i) {
    if (auto& w = panelWindows_[i]; w != nullptr) panels_[i]->setWindowBounds(w->getBounds());
    layouts.push_back(panels_[i]->layout());
  }
  if (savePanelLayouts(proc_, layouts))
    for (auto& p : panels_) p->markSaved();
}

void MasterpieceEditor::addPanel() {
  if (static_cast<int>(panels_.size()) >= kMaxPanels) return;
  const int index = static_cast<int>(panels_.size());
  panels_.push_back(std::make_unique<PanelView>(proc_, index, PanelLayout{}));
  panelWindows_.emplace_back();
  auto* view = panels_.back().get();
  wirePanel(index);
  addChildComponent(*view);
  savePanels();
  showingConsole_ = true;
  rebuildPageTabs();
  // Straight to the new panel's tab.
  for (int t = 0; t < pageTabs_.getNumTabs(); ++t)
    if (t >= console_.pageCount() && panelTabs_[static_cast<size_t>(t - console_.pageCount())] == index)
      pageTabs_.setCurrentTabIndex(t, true);
}

void MasterpieceEditor::wirePanel(int index) {
  auto& view = *panels_[static_cast<size_t>(index)];
  view.setIndex(index);
  view.onSave = [this] { savePanels(); };
  view.onRenamed = [this, index] {
    if (auto& w = panelWindows_[static_cast<size_t>(index)]; w != nullptr)
      w->setName(juce::String(proc_.organModel().organName) + " - " +
                 panels_[static_cast<size_t>(index)]->title());
    rebuildPageTabs();
  };
  view.onDetachToggle = [this, index] {
    setPanelDetached(index, panelWindows_[static_cast<size_t>(index)] == nullptr);
  };
}

void MasterpieceEditor::deletePanel(int index) {
  if (index < 0 || index >= static_cast<int>(panels_.size())) return;
  panelWindows_[static_cast<size_t>(index)].reset();
  removeChildComponent(panels_[static_cast<size_t>(index)].get());
  panels_.erase(panels_.begin() + index);
  panelWindows_.erase(panelWindows_.begin() + index);
  showingPanel_ = -1;
  // The remaining panel is renumbered; its callbacks name its new place.
  for (size_t i = 0; i < panels_.size(); ++i) wirePanel(static_cast<int>(i));
  savePanels();
  rebuildPageTabs();
}

void MasterpieceEditor::setPanelDetached(int index, bool detached) {
  if (index < 0 || index >= static_cast<int>(panels_.size()) || kMobile) return;
  auto& view = *panels_[static_cast<size_t>(index)];
  auto& window = panelWindows_[static_cast<size_t>(index)];
  if (detached && window == nullptr) {
    removeChildComponent(&view);
    view.setDetached(true);
    window = std::make_unique<PanelWindow>(
        juce::String(proc_.organModel().organName) + " - " + view.title(), view);
    window->onClose = [this, index] { setPanelDetached(index, false); };
    view.setVisible(true);
    window->setVisible(true);
  } else if (!detached && window != nullptr) {
    view.setWindowBounds(window->getBounds());
    window->clearContentComponent();
    window.reset();
    view.setDetached(false);
    addChildComponent(view);
  }
  rebuildPageTabs();
}

#if MP_HAS_SCORE
namespace {
// The score panel's window: every button, maximise included. Closing it puts
// the panel back among the tabs.
class ScoreWindow : public juce::DocumentWindow {
public:
  ScoreWindow(const juce::String& title, juce::Component& content, std::function<void()> onClose)
      : juce::DocumentWindow(title, juce::Colour(0xff15171c), juce::DocumentWindow::allButtons),
        onClose_(std::move(onClose)) {
    setUsingNativeTitleBar(true);
    setResizable(true, true);
    setContentNonOwned(&content, false);
  }
  void closeButtonPressed() override {
    if (onClose_) onClose_();
  }

private:
  std::function<void()> onClose_;
};
}  // namespace

void MasterpieceEditor::showScore() {
  if (score_ == nullptr) {
    score_ = std::make_unique<ScorePanel>(proc_);
    score_->setDetachable(!kMobile);
    score_->onDetachToggle = [this] { setScoreDetached(scoreWindow_ == nullptr); };
    addChildComponent(*score_);
    rebuildPageTabs();
  }
  if (scoreWindow_ != nullptr) {
    scoreWindow_->toFront(true);
    return;
  }
  showingConsole_ = true;
  showingStopList_ = false;
  if (scoreTab_ >= 0) pageTabs_.setCurrentTabIndex(scoreTab_, true);
  // Set here as well: a tab that is already the current one sends no change.
  showingScore_ = true;
  showingPanel_ = -1;
  resized();
}

void MasterpieceEditor::setScoreDetached(bool detached) {
  if (score_ == nullptr || kMobile) return;
  if (detached && scoreWindow_ == nullptr) {
    removeChildComponent(score_.get());
    score_->setDetached(true);
    scoreWindow_ = std::make_unique<ScoreWindow>(
        juce::String(proc_.organModel().organName) + " - " + TRANS("Score"), *score_,
        [this] { setScoreDetached(false); });
    if (scoreWindowBounds_.getWidth() > 100) scoreWindow_->setBounds(scoreWindowBounds_);
    else scoreWindow_->centreWithSize(1100, 760);
    score_->setVisible(true);
    scoreWindow_->setVisible(true);
    showingScore_ = false;
    rebuildPageTabs();
    if (console_.pageCount() > 0)
      pageTabs_.setCurrentTabIndex(juce::jlimit(0, console_.pageCount() - 1, console_.currentPage()), true);
    resized();
  } else if (!detached && scoreWindow_ != nullptr) {
    scoreWindowBounds_ = scoreWindow_->getBounds();
    scoreWindow_->clearContentComponent();
    scoreWindow_.reset();
    score_->setDetached(false);
    addChildComponent(*score_);
    rebuildPageTabs();
    showScore();
  }
}
#endif

void MasterpieceEditor::showPanel(int index) {
  if (index < 0 || index >= static_cast<int>(panels_.size())) return;
  if (auto& w = panelWindows_[static_cast<size_t>(index)]; w != nullptr) {
    w->toFront(true);
    return;
  }
  // From the plain list of an organ without artwork too.
  showingConsole_ = true;
  showingStopList_ = false;
  for (int t = console_.pageCount(); t < pageTabs_.getNumTabs(); ++t)
    if (panelTabs_[static_cast<size_t>(t - console_.pageCount())] == index)
      pageTabs_.setCurrentTabIndex(t, true);
  // Set here as well: a tab that is already the current one sends no change.
  showingPanel_ = index;
  resized();
}

void MasterpieceEditor::togglePanel() {
  if (panels_.empty()) {
    addPanel();
    return;
  }
  if (showingConsole_ && showingPanel_ >= 0) {
    // Back to the console page that was showing, or to the plain list of an
    // organ that draws no console.
    if (console_.pageCount() > 0)
      pageTabs_.setCurrentTabIndex(juce::jlimit(0, console_.pageCount() - 1, console_.currentPage()),
                                   true);
    if (!console_.hasArtwork()) showingConsole_ = false;
    showingPanel_ = -1;
    resized();
    return;
  }
  showPanel(0);
}

void MasterpieceEditor::addPanelItems(juce::PopupMenu& menu) {
  const bool onPanel = showingConsole_ && showingPanel_ >= 0;
  bool onScore = false;
#if MP_HAS_SCORE
  onScore = showingConsole_ && showingScore_;
#endif
  menu.addItem(1002, console_.hasArtwork() ? TRANS("Console") : TRANS("Stop list"), true,
               !onPanel && !onScore && !showingStopList_);
  // The plain list of stops, for a player who cannot see the console. The
  // key that opens it is named in the manual.
  if (console_.hasArtwork()) menu.addItem(15, TRANS("Stop list"), true, showingStopList_);
  // The program couplers (#468), a menu away (#591). They are switches the
  // organ is built with, so changing this loads the organ again.
  if (proc_.loadedOrganFile().existsAsFile())
    menu.addItem(19, TRANS("Program couplers"), true, proc_.programCouplers());
  for (size_t i = 0; i < panels_.size(); ++i)
    menu.addItem(1300 + static_cast<int>(i), panels_[i]->title(), true,
                 onPanel && showingPanel_ == static_cast<int>(i));
  // The first panel is the whole organ, every stop by division: what the
  // stop list showed, as big buttons to arrange and name.
  menu.addItem(1001, panels_.empty() ? TRANS("Panel of the whole organ") : TRANS("New panel"),
               static_cast<int>(panels_.size()) < kMaxPanels);
  if (!panels_.empty()) {
    juce::PopupMenu edit;
    for (size_t i = 0; i < panels_.size(); ++i)
      edit.addItem(1200 + static_cast<int>(i), TRANS("Rename %1...").replace("%1", panels_[i]->title()));
    for (size_t i = 0; i < panels_.size(); ++i)
      edit.addItem(1100 + static_cast<int>(i), TRANS("Delete %1").replace("%1", panels_[i]->title()));
    menu.addSubMenu(TRANS("Rename or delete a panel"), edit);
  }
}

void MasterpieceEditor::panelItemChosen(int r) {
  if (r == 1001) addPanel();
  else if (r == 1002) {
    // Back to the console, or to the plain list of an organ that draws no
    // console.
    if (showingStopList_) {
      showingStopList_ = false;
      resized();
      return;
    }
#if MP_HAS_SCORE
    if (showingConsole_ && showingScore_) {
      if (console_.pageCount() > 0)
        pageTabs_.setCurrentTabIndex(juce::jlimit(0, console_.pageCount() - 1, console_.currentPage()),
                                     true);
      if (!console_.hasArtwork()) showingConsole_ = false;
      showingScore_ = false;
      resized();
      return;
    }
#endif
    if (showingConsole_ && showingPanel_ >= 0) togglePanel();
  } else if (r >= 1300) showPanel(r - 1300);
  else if (r >= 1200) panels_[static_cast<size_t>(r - 1200)]->askForTitle();
  else if (r >= 1100) deletePanel(r - 1100);
}

void MasterpieceEditor::toggleKeyboard() {
  showingKeyboard_ = !showingKeyboard_;
  resized();
}

void MasterpieceEditor::toggleStopList() {
  if (!console_.hasArtwork()) {
    // The list is already what the window shows: the keyboard focus goes to
    // the first stop, and the screen reader reads it.
    jamb_.focusFirstStop();
    return;
  }
  showingStopList_ = !showingStopList_;
  if (showingStopList_) showingPanel_ = -1;
  resized();
  if (showingStopList_) jamb_.focusFirstStop();
}

void MasterpieceEditor::toggleProgramCouplers() {
  // The organ's own setting, the one on Organ settings > Loading, saved for
  // this organ before it is loaded again with it.
  const bool on = !proc_.programCouplers();
  proc_.setProgramCouplers(on);
  proc_.saveSettings();
  showCouplersAfterLoad_ = on;
  reloadOrgan();
}

void MasterpieceEditor::toggleSwell() {
  showingSwell_ = !showingSwell_;
  resized();
}

void MasterpieceEditor::showFileMenu() {
  juce::PopupMenu menu;
  // One door (#276): the favourites, with the file dialog a button away in
  // the same window; with no favourite yet, the file dialog itself.
  menu.addItem(1, TRANS("Open..."));
  // The same organ as another definition: a perspective, or full and light.
  const juce::File loaded = proc_.loadedOrganFile();
  const auto versions = MasterpieceProcessor::organVersions(loaded);
  if (versions.size() > 1) {
    juce::PopupMenu other;
    for (int i = 0; i < versions.size(); ++i)
      other.addItem(100 + i, versions[i].getFileNameWithoutExtension(), versions[i] != loaded,
                    versions[i] == loaded);
    menu.addSubMenu(TRANS("Other versions of this organ"), other);
  }
  menu.addSeparator();
  menu.addItem(5, TRANS("Recorder..."), true, recorderWindow_ != nullptr && recorderWindow_->isVisible());
  // The application's own way out (#591), the same path as the window's close
  // button. A plug-in's host owns quitting; on a Mac it is in the app menu.
#if !JUCE_MAC
  if (!kMobile && juce::JUCEApplicationBase::isStandaloneApp()) {
    menu.addSeparator();
#if JUCE_WINDOWS
    menu.addItem(6, TRANS("Exit"));
#else
    menu.addItem(6, TRANS("Quit"));
#endif
  }
#endif
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&fileMenu_),
                     [safe = juce::Component::SafePointer<MasterpieceEditor>(this), versions](int c) {
                       if (safe == nullptr || c == 0) return;
                       if (c == 1) safe->showOpenMenu();
                       if (c == 5) safe->toggleRecorder();
                       if (c == 6)
                         if (auto* app = juce::JUCEApplicationBase::getInstance())
                           app->systemRequestedQuit();
                       if (c >= 100 && c - 100 < versions.size()) safe->loadOrgan(versions[c - 100]);
                     });
}

void MasterpieceEditor::fitWindowToPage() {
  const auto art = console_.artworkBounds();
  auto* top = getTopLevelComponent();
  if (art.getWidth() <= 0 || art.getHeight() <= 0 || consoleArea_.isEmpty() || top == nullptr) return;
  // Everything the window shows besides the page: its bars and strips, the
  // frame the system draws around it, and the window's own edges.
  const auto frame = top->getPeer() != nullptr ? top->getPeer()->getFrameSize() : juce::BorderSize<int>();
  const int besideW = top->getWidth() - consoleArea_.getWidth() + frame.getLeftAndRight();
  const int besideH = top->getHeight() - consoleArea_.getHeight() + frame.getTopAndBottom();
  // The page at 100%, or at the largest size that fits the screen it is on.
  const auto* screen = juce::Desktop::getInstance().getDisplays().getDisplayForRect(top->getScreenBounds());
  const auto usable = screen != nullptr ? screen->userArea : top->getScreenBounds();
  const float scale = juce::jmin(
      1.0f, static_cast<float>(usable.getWidth() - besideW) / static_cast<float>(art.getWidth()),
      static_cast<float>(usable.getHeight() - besideH) / static_cast<float>(art.getHeight()));
  const int w = juce::roundToInt(static_cast<float>(art.getWidth()) * scale);
  const int h = juce::roundToInt(static_cast<float>(art.getHeight()) * scale);
  // The window is sized and placed, in the middle of its screen; the
  // editor follows it.
  const int outerW = besideW + w, outerH = besideH + h;
  top->setBounds(usable.getCentreX() - outerW / 2 + frame.getLeft(),
                 usable.getCentreY() - outerH / 2 + frame.getTop(),
                 outerW - frame.getLeftAndRight(), outerH - frame.getTopAndBottom());
}

void MasterpieceEditor::showViewMenu() {
  juce::PopupMenu menu;
  addPanelItems(menu);
  menu.addSeparator();
  menu.addItem(10, TRANS("Combinations"), combinations_ != nullptr,
               combinations_ != nullptr && combinations_->isVisible());
  if (expression_.shoeCount() > 0) menu.addItem(11, TRANS("Swell faders"), true, showingSwell_);
  menu.addItem(12, TRANS("Keyboard"), true, showingKeyboard_);
  menu.addItem(13, TRANS("Show played keys"), true, proc_.showPlayedKeys());
  // Each item its own number: the handler below runs every branch that
  // matches, so a shared number does two things at once. 15 is the stop list.
  menu.addItem(17, TRANS("Small pages fill the window"), true, proc_.smallPagesFill());
  menu.addItem(18, TRANS("Fit window to page"));
  menu.addItem(14, TRANS("Split keyboard"), true, splitWindow_ != nullptr && splitWindow_->isVisible());
#if MP_HAS_SCORE
  menu.addItem(16, TRANS("Score"), true,
               (showingConsole_ && showingScore_) || scoreWindow_ != nullptr);
#endif
  // Pages in windows of their own, for a second screen: the same as a
  // right-click on a page's tab, here where it can be found.
  if (pagesCanFloat() && showingConsole_ && console_.pageCount() > 0) {
    juce::PopupMenu pages;
    for (int i = 0; i < console_.pageCount(); ++i)
      pages.addItem(300 + i, console_.pageName(i), true, pageWindowFor(i) != nullptr);
    menu.addSeparator();
    menu.addSubMenu(TRANS("A page in its own window"), pages);
  }
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&viewMenu_),
                     [safe = juce::Component::SafePointer<MasterpieceEditor>(this)](int c) {
                       if (safe == nullptr || c == 0) return;
                       if (c >= 1000) safe->panelItemChosen(c);
                       if (c == 15) safe->toggleStopList();
                       if (c == 19) safe->toggleProgramCouplers();
                       if (c == 10) safe->toggleCombinations();
                       if (c == 11) safe->toggleSwell();
                       if (c == 12) safe->toggleKeyboard();
                       if (c == 14) safe->toggleSplitKeyboard();
#if MP_HAS_SCORE
                       if (c == 16) safe->showScore();
#endif
                       if (c == 13) {
                         safe->proc_.setShowPlayedKeys(!safe->proc_.showPlayedKeys());
                         safe->console_.repaint();
                       }
                       if (c == 17) {
                         safe->proc_.setSmallPagesFill(!safe->proc_.smallPagesFill());
                         safe->resized();
                       }
                       if (c == 18) safe->fitWindowToPage();
                       if (c >= 300 && c < 1000 && c - 300 < safe->console_.pageCount()) {
                         if (auto* w = safe->pageWindowFor(c - 300)) safe->closePageWindow(w);
                         else safe->openPageWindow(c - 300);
                       }
                     });
}

void MasterpieceEditor::showSettingsMenu() {
  // Two windows: what belongs to this organ, and what belongs to the
  // program and the player's console; and the sound card, set up once.
  juce::PopupMenu menu;
  menu.addItem(1, TRANS("Organ settings..."), !proc_.loadedOrganFile().getFullPathName().isEmpty());
  // A licence confirmed for this organ can be taken back here.
  if (proc_.organModel().hasLicensedSamples)
    menu.addItem(4, TRANS("Licence confirmed for this organ"), true, proc_.licenceConfirmed());
  menu.addItem(2, TRANS("General settings..."));
  if (onAudioSettings) menu.addItem(6, TRANS("Audio and MIDI..."));
  addBackupItems(menu, 20);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&settingsButton_),
                     [safe = juce::Component::SafePointer<MasterpieceEditor>(this)](int c) {
                       if (safe == nullptr || c == 0) return;
                       if (c == 1 && safe->onOrganSettings) safe->onOrganSettings();
                       if (c == 2 && safe->onSettings) safe->onSettings();
                       if (c == 6 && safe->onAudioSettings) safe->onAudioSettings();
                       if (c >= 20 && c < 30) safe->backupItemChosen(c - 20);
                       if (c == 4) {
                         if (safe->proc_.licenceConfirmed()) {
                           safe->proc_.setLicenceConfirmed(false);
                           safe->proc_.saveSettingsIfDirty();
                           safe->reloadOrgan();
                         } else {
                           safe->askForLicence();
                         }
                       }
                     });
}

void MasterpieceEditor::showConsolePage(int oneBased) {
  const int index = oneBased - 1;
  if (index < 0 || index >= console_.pageCount()) return;
  pageTabs_.setCurrentTabIndex(index, true);
}

void MasterpieceEditor::changeListenerCallback(juce::ChangeBroadcaster* src) {
  if (src == &pageTabs_) {
    const int tab = pageTabs_.getCurrentTabIndex();
    const int pages = console_.pageCount();
    showingPanel_ = tab >= pages && tab - pages < static_cast<int>(panelTabs_.size())
                        ? panelTabs_[static_cast<size_t>(tab - pages)]
                        : -1;
#if MP_HAS_SCORE
    showingScore_ = scoreTab_ >= 0 && tab == scoreTab_;
    if (showingPanel_ < 0 && !showingScore_) console_.setPage(tab);
#else
    if (showingPanel_ < 0) console_.setPage(tab);
#endif
    resized();
  }
}

void MasterpieceEditor::timerCallback() {
  // The model is the loader's until the load ends: nothing reads it here.
  if (loading_) return;
  // The console's display, set up on the Display tab: only lines whose text
  // changed go out, so a steady display costs nothing.
  proc_.pumpLcdPanels();
  // The console follows the engine by itself, and repaints what changed. The
  // stop list follows the console, pistons and MIDI the same way (#56): it
  // used to show only what was clicked in it.
  jamb_.refresh();

  // The on-screen keyboard plays the chosen manual on that manual's channel,
  // which the player can move in Settings while the organ is loaded.
  if (manual_.getSelectedId() > 0) {
    const int channel = proc_.channelForKeyboard(static_cast<Id>(manual_.getSelectedId()));
    if (channel != keyboard_.getMidiChannel()) keyboard_.setMidiChannel(channel);
  }

  // A console piston pressed on a physical manual. Collected here because a
  // component may only be touched from the message thread.
  switch (proc_.takeConsoleAction()) {
    case MidiTargetKind::ConsoleNextPage:
      if (console_.pageCount() > 1)
        pageTabs_.setCurrentTabIndex(
            (pageTabs_.getCurrentTabIndex() + 1) % console_.pageCount());
      break;
    case MidiTargetKind::ConsolePrevPage:
      if (console_.pageCount() > 1)
        pageTabs_.setCurrentTabIndex(
            (pageTabs_.getCurrentTabIndex() + console_.pageCount() - 1) %
            console_.pageCount());
      break;
    case MidiTargetKind::ConsoleNextLayout:
      if (console_.layoutCount() > 1)
        layout_.setSelectedId(
            console_.layout() + 2 > console_.layoutCount() ? 1
                                                           : console_.layout() + 2);
      break;
    case MidiTargetKind::ConsoleToggleStopList:
      // The console's own stop-list piston: the first panel and back.
      togglePanel();
      break;
    case MidiTargetKind::ConsoleToggleKeyboard:
      toggleKeyboard();
      break;
    case MidiTargetKind::ConsoleToggleCombinations:
      toggleCombinations();
      break;
    case MidiTargetKind::TransposeUp:
      proc_.setTranspose(proc_.transpose() + 1);
      break;
    case MidiTargetKind::TransposeDown:
      proc_.setTranspose(proc_.transpose() - 1);
      break;
    case MidiTargetKind::TemperamentNext:
      proc_.stepTemperament(1);
      break;
    case MidiTargetKind::TemperamentPrev:
      proc_.stepTemperament(-1);
      break;
    default:
      break;
  }

  // A recorder button on the console, carried out here.
  if (const int asked = proc_.takeRecorderRequest(); asked != 0) {
    const auto saved = proc_.pressRecorderButton(asked == 1, MasterpieceProcessor::recordingsFolder());
    if (saved != juce::File())
      top_.setStatus(TRANS("The take was saved as %1").replace("%1", saved.getFileName()));
  }
  // The archive's take, written once it has ended.
  proc_.serviceArchive();

  // A voicing EQ preset asked for from MIDI, set here where its filters are
  // worked out, and named in the status line.
  if (const int asked = proc_.takeVoicingEqPresetRequest(); asked != 0) {
    const int id = asked > 0 ? (proc_.recallVoicingEqPreset(asked) ? asked : 0)
                             : proc_.stepVoicingEqPreset(asked == -1 ? 1 : -1);
    if (const auto* p = proc_.voicingEqPresets().find(id))
      top_.setStatus(TRANS("Voicing EQ: %1").replace("%1", voicingEqPresetName(*p)));
  }

  // A piston captured on the audio thread only raised a flag; the writing
  // happens here, where a file write is allowed. Combinations are the
  // player's own work and losing them to a crash would be unforgivable, so
  // this saves as soon as it sees one rather than at shutdown.
  proc_.saveCombinationsIfDirty();
  // The same for the per-organ settings and anything just learned: raised on
  // whichever thread changed it, written here, where a file write is allowed.
  proc_.saveSettingsIfDirty();
  proc_.splitKeyboard().service();
  // A temperament, a quit or a power-off a MIDI button asked for.
  proc_.serviceMidiRequests();
  proc_.saveMidiMapIfDirty();
  proc_.saveMasterGainIfDirty();
  proc_.saveGlobalIfDirty();
  proc_.saveRememberedStateIfSettled();

  // The sequencer's frame, out of the frames holding anything. With Set on
  // the next step is always open: stepping on is how a sequence grows.
  const auto& seq = proc_.playerCombinations();
  const int last = seq.lastUsedFrame();
  const bool capturing = proc_.captureMode();
  stepFrame_.setText(last == 0 && seq.frame() == 0
                         ? TRANS("No frames")
                         : TRANS("Frame %1 / %2")
                               .replace("%1", juce::String(seq.frame()))
                               .replace("%2", juce::String(juce::jmax(last, seq.frame()))),
                     juce::dontSendNotification);
  stepPrev_.setEnabled(seq.frame() > 1);
  stepNext_.setEnabled(capturing || seq.frame() < last);
  setter_.setToggleState(capturing, juce::dontSendNotification);
  {
    const auto where = proc_.storeTargetName();
    const juce::String tip =
        capturing ? TRANS("Set is on from the console or the combinations window: "
                          "pistons store. Press to let it go.")
        : where.empty() ? TRANS("Set: choose a piston or a step, draw the stops, "
                                "then press Set to store them there")
                        : TRANS("Set: store what is drawn into %1").replace("%1", juce::String(where));
    if (setter_.getTooltip() != tip) setter_.setTooltip(tip);
  }
  {
    const auto tuning = TuningPanel::summary(proc_);
    if (tuningButton_.getButtonText() != tuning) tuningButton_.setButtonText(tuning);
  }

  // Above the console while this program is in front, and not above anyone
  // else's windows when it is not. Not on Linux, where each change remakes
  // the window: there it is settled when the window opens (showFloating).
  if (kLiveOnTop && combinations_ != nullptr && combinations_->isVisible()) {
    const bool front = combinations_->onTopNow();
    if (combinations_->isAlwaysOnTop() != front) combinations_->setAlwaysOnTop(front);
  }
  if (kLiveOnTop && recorderWindow_ != nullptr && recorderWindow_->isVisible()) {
    const bool front = juce::Process::isForegroundProcess();
    if (recorderWindow_->isAlwaysOnTop() != front) recorderWindow_->setAlwaysOnTop(front);
  }

  // The swell strip follows a pedal moved over MIDI.
  if (expression_.isVisible()) expression_.refresh();

  // Voice count is the honest health readout: it says whether drawing a stop
  // and pressing a key actually produced sound.
  const auto& stats = proc_.voiceStats();
  juce::String live = status_;
  if (live.isNotEmpty()) live += "  |  ";
  live += TRANS("voices %1").replace("%1", juce::String(stats.activeVoices));
  // The organ plays while its samples are saved for next time (#120).
  // How far along, and how long is left once a few seconds give a rate to
  // go by: on a large organ this is minutes, and a bare "saving..." for that
  // long reads as stuck.
  if (proc_.cacheWriting()) {
    const double f = proc_.cacheWriteFraction();
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (cacheSaveStartMs_ <= 0.0 && f >= 0.0) {
      cacheSaveStartMs_ = now;
      cacheSaveStartFraction_ = f;
    }
    live += "  |  " + TRANS("creating the sample cache");
    if (f >= 0.0) {
      live += ": " + juce::String(juce::roundToInt(f * 100.0)) + "%";
      const double elapsed = (now - cacheSaveStartMs_) / 1000.0;
      const double moved = f - cacheSaveStartFraction_;
      if (elapsed > 3.0 && moved > 0.01) {
        const int left = juce::roundToInt(elapsed / moved * (1.0 - f));
        live += ", " + (left >= 90 ? TRANS("about %1 min left").replace("%1", juce::String((left + 30) / 60))
                                   : TRANS("about %1 s left").replace("%1", juce::String(juce::jmax(1, left))));
      }
    } else {
      live += "...";
    }
  } else {
    cacheSaveStartMs_ = 0.0;
    // A cache that could not be written is said, until the next load (#519).
    switch (proc_.cacheWriteFailure()) {
      case SampleLibrary::CacheFailure::DiskFull:
        live += "  |  " + TRANS("sample cache not saved: the disk is full");
        break;
      case SampleLibrary::CacheFailure::Failed:
        live += "  |  " + TRANS("sample cache not saved");
        break;
      case SampleLibrary::CacheFailure::None:
        break;
    }
  }
  if (stats.startsDropped > 0)
    live += ", " + TRANS("dropped %1").replace("%1", juce::String(stats.startsDropped));
  if (stats.samplesMissing > 0)
    live += ", " + TRANS("no audio %1").replace("%1", juce::String(stats.samplesMissing));

  // Audio that did not keep time: each late block is a glitch the player
  // heard, and the log says how late, so a report can say why (#120).
  const auto load = proc_.takeAudioLoad();
  worstBlockSinceLog_ = juce::jmax(worstBlockSinceLog_, load.worstPercent);
  if (load.late > 0) live += ", " + TRANS("late blocks %1").replace("%1", juce::String(load.late));
  // A streamed release the disk did not deliver in time goes quiet partway.
  // Said here, with what to do about it: the Loading tab's count is out of
  // sight, and whether a machine can stream is only known by playing (#120).
  if (proc_.streamReleases())
    if (const auto under = proc_.streamUnderruns(); under > 0)
      live += ", " + TRANS("streamed releases fell behind %1x: turn off \"Stream release tails\" in Organ settings")
                         .replace("%1", juce::String(under));
  const auto now = juce::Time::getMillisecondCounter();
  // The switch is on and the system said no: the log says so once, with its
  // answer, since the line written at the start only knew how it would ask.
  if (proc_.realtimeThreads() && !realtimeRefusalLogged_)
    if (const auto refusal = MasterpieceProcessor::realtimeRefusal(); refusal.isNotEmpty()) {
      juce::Logger::writeToLog("real-time render threads: refused (" + refusal +
                               "); the render threads run at normal priority");
      realtimeRefusalLogged_ = true;
    }
  if (load.late > lateBlocksLogged_ && now - lateLoggedAt_ > 5000) {
    juce::Logger::writeToLog("audio: " + juce::String(load.late - lateBlocksLogged_) +
                             " late block(s) of " + juce::String(load.blocks) +
                             "; the slowest took " + juce::String(worstBlockSinceLog_, 0) +
                             "% of its time" +
                             (proc_.realtimeThreads()
                                  ? "; " + juce::String(MasterpieceProcessor::realtimeWorkers()) +
                                        " render thread(s) real-time, " +
                                        juce::String(MasterpieceProcessor::workgroupWorkers()) +
                                        " in the audio workgroup"
                                  : juce::String()));
    lateBlocksLogged_ = load.late;
    lateLoggedAt_ = now;
    worstBlockSinceLog_ = 0.0;
  }
  top_.setStatus(live);
}

} // namespace mp::ui

// Processor -> editor hook (kept here to keep mp_audio UI-free).
namespace mp {
juce::AudioProcessorEditor* MasterpieceProcessor::createEditor() {
  // The plugins' editor. The application builds its own and gives it the
  // device manager; here the host owns the devices, so General settings has
  // no Audio or MIDI tab and Organ settings no MIDI tab.
  auto* ed = new ui::MasterpieceEditor(*this);
  auto launch = [](std::unique_ptr<juce::Component> panel, const juce::String& title) {
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned(panel.release());
    opts.dialogTitle = title;
    opts.dialogBackgroundColour = juce::Colour(0xff15171c);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    ui::launchDialog(opts);
  };
  // A host can close the editor while a settings window stays open.
  const juce::Component::SafePointer<ui::MasterpieceEditor> safe(ed);
  // A project's organ, restored while this window is open, loads through it.
  editorLoader = [safe](const juce::File& organ) {
    auto* e = safe.getComponent();
    if (e == nullptr) return false;
    e->loadOrgan(organ, false);
    return true;
  };
  ed->onSettings = [this, safe, launch] {
    auto panel = std::make_unique<ui::SettingsWindow>(*this, nullptr);
    panel->setOrganLoader([safe](const juce::File& f) {
      if (auto* e = safe.getComponent()) e->loadOrgan(f, false);
    });
    launch(std::move(panel), TRANS("General settings"));
  };
  ed->onOrganSettings = [this, safe, launch] {
    launch(std::make_unique<ui::OrganSettingsWindow>(*this,
                                                     [safe] {
                                                       if (auto* e = safe.getComponent()) e->reloadOrgan();
                                                     }),
           TRANS("Organ settings"));
  };
  return ed;
}
} // namespace mp
