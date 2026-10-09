#include "Ui.h"

#include <map>
#include "Settings.h"
#include "../mp_archive/OrganArchive.h"

#include "LoadingDialog.h"
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
  return "Division " + juce::String(divisionId);
}

constexpr int kStopHeight = 30;
constexpr int kHeaderHeight = 24;
constexpr int kJambWidth = 320;
// A grid of stop tiles rather than bars the width of the window: as many
// columns as fit at this width.
constexpr int kTileWidth = 190;
constexpr int kGap = 4;

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
      message + "\n\nThe details are in the log, " + log.getFileName() +
          ". If Masterpiece has been started again since, they are in "
          "masterpiece.previous.log beside it.",
      "Show the log", "OK", nullptr,
      juce::ModalCallbackFunction::create([log](int choice) {
        if (choice == 1) log.revealToUser();
      }));
}

} // namespace

// ------------------------------------------------------------------ jamb

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
    e.button = std::make_unique<juce::TextButton>(juce::String(s.name));
    e.button->setClickingTogglesState(true);
    e.button->setToggleState(proc_.stopEngaged(s.stopId),
                             juce::dontSendNotification);
    // Drawn reads as drawn at a glance: ivory with dark lettering, like a
    // stop face, against the dark of one pushed in.
    e.button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a2f3a));
    e.button->setColour(juce::TextButton::textColourOffId, juce::Colour(0xffb9c2d0));
    e.button->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffeee3c6));
    e.button->setColour(juce::TextButton::textColourOnId, juce::Colour(0xff1b1e24));
    e.button->setEnabled(s.playable);
    if (s.playable && !proc_.stopLoaded(s.stopId)) {
      e.button->setAlpha(0.45f);
      e.button->setTooltip("Not loaded: chosen in Organ settings, Stops");
    }
    if (!s.playable) {
      // Say WHY rather than just greying it: on a demo set this is the single
      // most confusing thing about the instrument.
      e.button->setTooltip("This stop's ranks ship no pipes in this sample set");
    }
    const Id id = s.stopId;
    auto* raw = e.button.get();
    e.button->onClick = [this, id, raw] {
      proc_.setStopEngaged(id, raw->getToggleState());
    };
    addAndMakeVisible(*e.button);
    entries_.push_back(std::move(e));
  }

  // Height is content-driven; the Viewport scrolls it.
  const int width = getWidth() > 0 ? getWidth() : kJambWidth;
  setSize(width, heightFor(width));
  resized();
}

void StopJamb::refresh() {
  for (auto& e : entries_) {
    const bool on = proc_.stopEngaged(e.stopId);
    if (e.button->getToggleState() != on) e.button->setToggleState(on, juce::dontSendNotification);
  }
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
                       name.isEmpty() ? juce::String("Swell") : name);
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

LevelMeter::LevelMeter(MasterpieceProcessor& p) : proc_(p) { startTimerHz(30); }
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

TopBar::TopBar(MasterpieceProcessor& p, Callback onLoad, Callback onAudioSettings)
    : proc_(p), meter_(p) {
  addAndMakeVisible(load_);
  addAndMakeVisible(audio_);
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
  load_.onClick = std::move(onLoad);
  audio_.onClick = std::move(onAudioSettings);
  simple_.onClick = [this] {
    auto sw = proc_.engineSwitch();
    sw.simpleWavOnly = simple_.getToggleState();
    proc_.setEngineSwitch(sw);
  };
}

AudioSettingsPanel::AudioSettingsPanel(juce::AudioDeviceManager& devices,
                                       MasterpieceProcessor& p)
    : proc_(p), selector_(devices, 0, 0, 1, 8, true, true, true, false) {
  addAndMakeVisible(selector_);
  addAndMakeVisible(keepAwake_);
  addAndMakeVisible(level_);
  const float db = proc_.speakerKeepAlive();
  keepAwake_.setToggleState(db < 0.0f, juce::dontSendNotification);
  keepAwake_.setTooltip(
      "For a battery speaker that switches itself off after a few seconds of "
      "silence and swallows the first notes while it wakes. Plays a 20 Hz tone "
      "too low and too quiet to hear under the organ, so the line is never "
      "silent.");
  level_.setRange(-80.0, -40.0, 1.0);
  level_.setTextValueSuffix(" dB");
  level_.setValue(db < 0.0f ? db : -60.0, juce::dontSendNotification);
  level_.setTooltip("Raise it if the speaker still goes to sleep; lower it if "
                    "you can hear it.");
  level_.setEnabled(keepAwake_.getToggleState());
  keepAwake_.onClick = [this] { apply(); };
  level_.onDragEnd = [this] { apply(); };
  level_.onValueChange = [this] {
    if (!level_.isMouseButtonDown()) apply();
  };
  setSize(500, 510);
}

void AudioSettingsPanel::apply() {
  level_.setEnabled(keepAwake_.getToggleState());
  proc_.setSpeakerKeepAlive(keepAwake_.getToggleState()
                                ? static_cast<float>(level_.getValue())
                                : 0.0f);
}

void AudioSettingsPanel::resized() {
  auto r = getLocalBounds();
  auto row = r.removeFromBottom(40).reduced(12, 6);
  keepAwake_.setBounds(row.removeFromLeft(240));
  level_.setBounds(row);
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
  load_.setBounds(r.removeFromLeft(64));
  r.removeFromLeft(6);
  audio_.setBounds(r.removeFromLeft(64));
  r.removeFromLeft(6);
  simple_.setBounds(r.removeFromLeft(88));
  r.removeFromLeft(10);
  // When the console's buttons leave too little room, things give way in
  // order of how much they are missed: the status line first, then the
  // meter, and the fader last -- never below a width a hand can still use.
  const int room = r.getWidth();
  // Wide enough to set a level by hand (#90: too small to use). The status
  // line has moved to the bottom of the window, so the fader and the meter
  // share the rest.
  const int volumeW = juce::jlimit(90, 220, room - 120);
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
      juce::MessageBoxIconType::QuestionIcon, "The rest of " + package.getFileName(),
      package.getFileName() + " is one part of a set of several files. Choose the folder that holds "
      "all of them, and Masterpiece reads the set from there.",
      "Choose the folder", "Cancel", nullptr,
      juce::ModalCallbackFunction::create([self, package, graphicsOnly](int ok) {
        if (self == nullptr || ok == 0) return;
        self->chooser_ = std::make_unique<juce::FileChooser>("Choose the folder that holds the set",
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
  const auto& bank = proc_.favourites().organs;
  const auto slots = bank.used();
  if (slots.empty()) {
    chooseAndLoadOrgan();
    return;
  }
  // Two favourites can share a name -- one organ as a Hauptwerk and a
  // GrandOrgue set -- and then the file tells them apart, as in Settings.
  std::map<std::string, int> named;
  for (int slot : slots) ++named[bank.at(slot).name];
  juce::PopupMenu menu;
  menu.addSectionHeader("Favourites");
  for (int slot : slots) {
    const auto& fav = bank.at(slot);
    juce::String shown = juce::String::fromUTF8(fav.name.c_str());
    if (named[fav.name] > 1) shown << "  (" << juce::File(juce::String(fav.target)).getFileName() << ")";
    menu.addItem(slot, shown);
  }
  menu.addSeparator();
  menu.addItem(-1, "Open another organ...");
  menu.showMenuAsync(
      juce::PopupMenu::Options().withMousePosition(),
      [safe = juce::Component::SafePointer<MasterpieceEditor>(this)](int chosen) {
        if (safe == nullptr || chosen == 0) return;
        if (chosen == -1) {
          safe->chooseAndLoadOrgan();
          return;
        }
        const juce::File f(juce::String(safe->proc_.favourites().organs.at(chosen).target));
        if (!f.existsAsFile() && !MasterpieceProcessor::portableCopyFor(f).existsAsFile()) {
          // A set on a drive not plugged in: said, and the favourite kept.
          safe->top_.setStatus("Not found: " + f.getFullPathName());
          return;
        }
        safe->loadOrgan(f);
      });
}

void MasterpieceEditor::chooseAndLoadOrgan(const juce::File& startIn) {
  // The extension pattern names the format because that IS the file name on
  // disk; the prompt does not, because the player is choosing an organ.
  // On a phone or tablet an organ comes as one package: a RAR or a .orgue.
  // A loose installation is thousands of files, which is slow to bring onto
  // the device and slow to read there through the system's document layer.
  chooser_ = std::make_unique<juce::FileChooser>(
      kMobile ? "Choose an organ package" : "Choose an organ definition file", startIn,
      kMobile ? "*.rar;*.orgue"
              : "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.organ;*.rar;*.orgue");
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
                            showWithLog("Could not open the organ",
                                        "The Files app did not lend this document to Masterpiece.");
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
      top_(p, [this] { showOpenMenu(); },
           [this] { if (onAudioSettings) onAudioSettings(); }),
      console_(p),
      jamb_(p),
      expression_(p),
      keyboard_(p.keyboardState(),
                juce::MidiKeyboardComponent::horizontalKeyboard) {
  // The licence question, asked from the loading thread before a sample is
  // read (#164). The box belongs to the message thread, so it is shown there
  // and the load waits for the answer. A window gone, or a question never
  // answered because the program is closing, is a no.
  proc_.licenceAsker = [this](const std::string& organ, const std::string& who) {
    auto answer = std::make_shared<std::promise<bool>>();
    auto future = answer->get_future();
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    juce::MessageManager::callAsync([self, answer, organ, who] {
      if (self == nullptr) {
        answer->set_value(false);
        return;
      }
      self->askLicenceQuestion(juce::String(organ), juce::String(who),
                               [answer](bool yes) { answer->set_value(yes); });
    });
    try {
      return future.get();
    } catch (const std::future_error&) {
      return false;
    }
  };
  addAndMakeVisible(top_);
  // A thin line along the bottom, the whole width of the window, instead of
  // whatever the top bar's buttons left of it.
  auto& statusLine = top_.statusLabel();
  addAndMakeVisible(statusLine);
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
    if (!pagesCanFloat()) return;
    juce::PopupMenu menu;
    const bool open = pageWindowFor(page) != nullptr;
    menu.addItem(1, open ? "Bring its window to the front" : "Open in its own window");
    // A set can draw its pages again for another screen shape -- Buckeburg's
    // jambs in portrait -- and a window of its own can show that one while
    // the main window keeps its layout.
    if (!open && console_.layoutCount() > 1)
      for (int l = 0; l < console_.layoutCount(); ++l)
        menu.addItem(10 + l, "Open in its own window, " + layoutName(l));
    if (open) menu.addItem(2, "Close its window");
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, page](int choice) {
      if (choice == 1) openPageWindow(page);
      if (choice == 2) closePageWindow(pageWindowFor(page));
      if (choice >= 10) openPageWindow(page, {}, choice - 10);
    });
  };
  addAndMakeVisible(settingsButton_);
  settingsButton_.onClick = [this] {
    // Two windows: what belongs to this organ, and what belongs to the
    // program and the player's console.
    juce::PopupMenu menu;
    menu.addItem(1, "Organ settings...", !proc_.loadedOrganFile().getFullPathName().isEmpty());
    // A licence confirmed for this organ can be taken back here.
    if (proc_.organModel().hasLicensedSamples)
      menu.addItem(4, "Licence confirmed for this organ", true, proc_.licenceConfirmed());
    menu.addItem(2, "General settings...");
    menu.addItem(5, "Recorder...", true, recorderWindow_ != nullptr && recorderWindow_->isVisible());
    // Pages in windows of their own, for a second screen: the same as a
    // right-click on a page's tab, here where it can be found.
    if (pagesCanFloat() && showingConsole_ && console_.pageCount() > 0) {
      juce::PopupMenu pages;
      for (int i = 0; i < console_.pageCount(); ++i)
        pages.addItem(300 + i, console_.pageName(i), true, pageWindowFor(i) != nullptr);
      menu.addSeparator();
      menu.addSubMenu("Open a page in its own window", pages);
    }
    // The same organ as another definition: a perspective, or full and light.
    const juce::File loaded = proc_.loadedOrganFile();
    const auto versions = MasterpieceProcessor::organVersions(loaded);
    if (versions.size() > 1) {
      juce::PopupMenu other;
      for (int i = 0; i < versions.size(); ++i)
        other.addItem(100 + i, versions[i].getFileNameWithoutExtension(), versions[i] != loaded,
                      versions[i] == loaded);
      menu.addSeparator();
      menu.addSubMenu("Other versions of this organ", other);
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&settingsButton_),
                       [this, versions](int choice) {
                         if (choice == 1 && onOrganSettings) onOrganSettings();
                         if (choice == 2 && onSettings) onSettings();
                         if (choice == 5) toggleRecorder();
                         if (choice == 4) {
                           if (proc_.licenceConfirmed()) {
                             proc_.setLicenceConfirmed(false);
                             proc_.saveSettingsIfDirty();
                             reloadOrgan();
                           } else {
                             askForLicence();
                           }
                         }
                         if (choice >= 100 && choice - 100 < versions.size())
                           loadOrgan(versions[choice - 100]);
                         if (choice >= 300 && choice - 300 < console_.pageCount()) {
                           if (auto* w = pageWindowFor(choice - 300)) closePageWindow(w);
                           else openPageWindow(choice - 300);
                         }
                       });
  };

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
  setter_.setTooltip("While on, a piston or a step stores what is drawn "
                     "instead of recalling it");
  setter_.onClick = [this] { proc_.setCaptureMode(setter_.getToggleState()); };

  addAndMakeVisible(stepPrev_);
  stepPrev_.onClick = [this] { proc_.stepperPrev(); };
  addAndMakeVisible(stepNext_);
  stepNext_.onClick = [this] { proc_.stepperNext(); };
  addAndMakeVisible(stepFrame_);
  stepFrame_.setJustificationType(juce::Justification::centred);
  stepFrame_.setColour(juce::Label::textColourId, juce::Colour(0xffb9c2d0));

  addAndMakeVisible(keysButton_);
  keysButton_.onClick = [this] {
    showingKeyboard_ = !showingKeyboard_;
    resized();
  };

  addAndMakeVisible(panicButton_);
  panicButton_.setTooltip("Release every key on every manual and the pedal");
  panicButton_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe0a0a0));
  panicButton_.onClick = [this] { proc_.releaseAllKeys(); };

  combinations_ = std::make_unique<CombinationsWindow>(proc_);
  addAndMakeVisible(combinationsButton_);
  combinationsButton_.setTooltip("Generals, divisionals, the stepper and the "
                                 "combination set, in a window of their own");
  combinationsButton_.onClick = [this] { toggleCombinations(); };

  addAndMakeVisible(tuningButton_);
  tuningButton_.setTooltip("Temperament, pitch and transposer for this organ");
  tuningButton_.onClick = [this] {
    juce::CallOutBox::launchAsynchronously(std::make_unique<TuningPanel>(proc_),
                                           tuningButton_.getScreenBounds(), nullptr);
  };

  addAndMakeVisible(swellButton_);
  swellButton_.onClick = [this] {
    showingSwell_ = !showingSwell_;
    resized();
  };

  addAndMakeVisible(toggleView_);
  toggleView_.onClick = [this] {
    showingConsole_ = !showingConsole_;
    toggleView_.setButtonText(showingConsole_ ? "Stop list" : "Console");
    resized();
    repaint();
  };

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
  top_.setStatus("No organ loaded.");
  startTimerHz(4);
}

MasterpieceEditor::~MasterpieceEditor() {
  stopTimer();
  proc_.licenceAsker = nullptr;
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
    top_.setStatus("Opening " + odf.getFileName() + "...");
    showLoadingDialog(odf);
    juce::Component::SafePointer<MasterpieceEditor> self(this);
    auto* processor = &proc_;
    juce::Thread::launch([self, processor, odf, graphicsOnly] {
      juce::String error;
      const auto definitions = processor->openPackagedOrgan(odf, error);
      juce::MessageManager::callAsync([self, odf, graphicsOnly, definitions, error] {
        if (self == nullptr) return;
        if (self->loadWindow_ != nullptr) {
          delete self->loadWindow_;
          self->loadWindow_ = nullptr;
        }
        self->loading_ = false;
        if (error == "cancelled") {
          self->top_.setStatus("Archive opening cancelled");
          return;
        }
        // Opened, but with files that did not read: said, and the load goes on.
        if (!definitions.isEmpty() && error.isNotEmpty())
          showWithLog(odf.getFileName() + " is damaged", error);
        if (definitions.size() == 1) {
          self->loadOrgan(definitions.getFirst(), graphicsOnly);
        } else if (definitions.size() > 1) {
          self->chooseDefinition(definitions, graphicsOnly);
        } else {
          self->status_ = "Failed to open " + odf.getFileName();
          self->top_.setStatus(self->status_);
         #if JUCE_IOS
          // One volume of a set, lent on its own: the others are beside it but
          // out of reach until the folder holding them is lent too (#197).
          if (error.contains("multi-volume")) {
            self->askForFolderOf(odf, graphicsOnly);
            return;
          }
         #endif
          showWithLog("Could not open " + odf.getFileName(), error);
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
  dialog->setSize(460, 190);

  juce::DialogWindow::LaunchOptions opts;
  opts.content.setOwned(dialog.release());
  opts.dialogTitle = "Loading";
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
  top_.setStatus("Loading " + odf.getFileName() + "...");

  showLoadingDialog(odf);

  juce::Thread::launch([this, odf, graphicsOnly] {
    const auto result = proc_.loadOrgan(odf, /*maxFramesPerSample*/ 0, graphicsOnly);
    // Everything past here touches components, so it belongs to the message
    // thread. The lambda copies what it needs; the loader thread ends here.
    juce::MessageManager::callAsync(
        [this, odf, graphicsOnly, result] { finishLoad(odf, graphicsOnly, result); });
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
                         self->top_.setStatus(self->status_ + "  -  licensed samples not loaded");
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
  const juce::String organ = organName.isEmpty() ? juce::String("This organ") : organName;
  const juce::String from = who.isEmpty() ? juce::String("its publisher") : who;
  // Built by hand: a stock two-button box gives Enter to the first button and
  // Escape to the second, and neither order makes Cancel answer both. Here
  // Enter, Escape and closing the box all cancel; only a click on Load loads.
  auto* box = new juce::AlertWindow(
      "A licence is needed for these samples",
      organ + (who.isEmpty() ? juce::String() : ", from " + who) +
          ", requires a licence from its publisher to use its samples. "
          "Masterpiece cannot check that licence.\n\n"
          "Load its samples only if your licence from " + from +
          " allows you to play this set in this program. Your answer is saved for "
          "this organ and can be withdrawn from the Settings menu.",
      juce::MessageBoxIconType::QuestionIcon, this);
  box->addButton("Load the samples", 1);
  box->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey),
                 juce::KeyPress(juce::KeyPress::returnKey));
  box->enterModalState(true, juce::ModalCallbackFunction::create([answered](int result) {
                         answered(result == 1);
                       }),
                       true);  // deleted when dismissed
}

// The message-thread half of a load: close the dialog, then either report the
// failure or build the console from the model that is now in place.
void MasterpieceEditor::chooseDefinition(const juce::Array<juce::File>& definitions,
                                         bool graphicsOnly) {
  top_.setStatus("These packages hold several organs: choose one");
  const juce::File last = MasterpieceProcessor::rememberedDefinition(definitions);
  juce::Array<juce::File> order;
  if (last != juce::File()) order.add(last);
  for (const auto& d : definitions)
    if (d != last) order.add(d);

  juce::PopupMenu menu;
  menu.addSectionHeader("Which organ?");
  for (int i = 0; i < order.size(); ++i)
    menu.addItem(i + 1, order[i].getFileNameWithoutExtension() +
                            (order[i] == last ? juce::String("  (last opened)") : juce::String()));
  juce::Component::SafePointer<MasterpieceEditor> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&top_),
                     [self, order, graphicsOnly](int choice) {
                       if (self == nullptr) return;
                       if (choice < 1 || choice > order.size()) {
                         self->top_.setStatus("No organ opened");
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
    setTooltip("Open this page in its own window, to move to another screen");
  }
  void setOut(bool out) {
    if (out_ == out) return;
    out_ = out;
    setTooltip(out ? "Close this page's window and bring it back here"
                   : "Open this page in its own window, to move to another screen");
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
  for (int i = 0; i < pageTabs_.getNumTabs(); ++i) {
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
  juce::String name = layout == 0 ? juce::String("main layout") : "alternate layout " + juce::String(layout);
  if (layout >= 0 && layout < 4 && m.consoleHeightPx[layout] > m.consoleWidthPx[layout] &&
      m.consoleWidthPx[layout] > 0)
    name << " (portrait)";
  return name;
}

void MasterpieceEditor::openPageWindow(int page, juce::Rectangle<int> bounds, int layout) {
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
    places.push_back({w->page(), b.getX(), b.getY(), b.getWidth(), b.getHeight(), w->layout()});
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
      openPageWindow(p.page, {p.x, p.y, p.w, p.h}, p.layout);
}

namespace {
// The recorder's own window: shown and hidden, never modal, so the console
// stays playable while it records.
class RecorderWindow : public juce::DocumentWindow {
public:
  explicit RecorderWindow(MasterpieceProcessor& p)
      : juce::DocumentWindow("Recorder", juce::Colour(0xff15171c), juce::DocumentWindow::closeButton) {
    setUsingNativeTitleBar(true);
    setContentOwned(new RecorderPanel(p), false);
    setResizable(false, false);
    setSize(420, 420);
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

void MasterpieceEditor::reloadOrgan() {
  const juce::File odf = proc_.loadedOrganFile();
  if (odf.existsAsFile()) loadOrgan(odf);
}

void MasterpieceEditor::finishLoad(const juce::File& odf, bool graphicsOnly,
                                   const MasterpieceProcessor::LoadResult& result) {
  loading_ = false;
  // Taken now so a failed load does not leave it waiting for the next one.
  auto then = std::move(afterLoad_);
  afterLoad_ = nullptr;
  if (loadWindow_ != nullptr) {
    delete loadWindow_;
    loadWindow_ = nullptr;
  }

  if (!result.ok) {
    // Cancelling is a choice, not a fault, and must not look like one.
    status_ = result.error == "cancelled"
                  ? "Load cancelled - no organ is loaded"
                  : "Failed to load " + odf.getFileName() + ": " +
                        juce::String(result.error);
    top_.setStatus(status_);
    // Stopping at the memory limit is the one failure a player can fix from
    // here, so it is said in full rather than left in the status line.
    if (result.outOfMemory)
      juce::AlertWindow::showMessageBoxAsync(
          juce::MessageBoxIconType::WarningIcon, "Not enough memory for this organ",
          odf.getFileNameWithoutExtension() +
              " was not loaded: its samples need more than the memory limit (" +
              juce::String(proc_.memoryLimitBytes() / (1024.0 * 1024.0 * 1024.0), 1) +
              " GB).\n\nIts console is shown without sound, so Organ settings can be opened "
              "from the Settings menu: leave out perspectives or stops, or on the Loading tab "
              "load 16-bit samples, stream the release tails, load in mono or preload less of "
              "each sample -- or raise the limit in General settings, if this computer has "
              "the memory to spare.");
    // The organ's console, without its audio, so the way out is on screen
    // (#53: the only way back was deleting the organ's settings file). The
    // same after Cancel, which is often a load taking too much (#90). Once:
    // a console-only load reads no samples, and never comes back here.
    const bool cancelled = result.error == "cancelled";
    if ((result.outOfMemory || cancelled) && !graphicsOnly && odf.existsAsFile()) loadOrgan(odf, true);
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

  pageTabs_.clearTabs();
  for (int i = 0; i < console_.pageCount(); ++i)
    pageTabs_.addTab(console_.pageName(i), juce::Colour(0xff2a2f3a), i);
  if (console_.pageCount() > 0) pageTabs_.setCurrentTabIndex(0, false);
  addPopOutIcons();
  restorePageWindows();

  // A set with no console artwork opens on the stop list rather than on an
  // empty picture.
  // A set that ships for several console sizes lets the player pick. Rebuilt
  // per organ, because the count is the organ's.
  layout_.clear(juce::dontSendNotification);
  for (int i = 0; i < console_.layoutCount(); ++i)
    layout_.addItem(i == 0 ? "Console: main"
                           : "Console: alt " + juce::String(i),
                    i + 1);
  layout_.setSelectedId(console_.layout() + 1, juce::dontSendNotification);

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
  toggleView_.setButtonText(showingConsole_ ? "Stop list" : "Console");
  resized();

  // The organ's name is in the window title, so the bar says what the title
  // cannot: how much instrument arrived, and whether it has any audio.
  const auto& m = proc_.organModel();
  status_ = juce::String(m.stops.size()) + " stops, " +
            juce::String(m.ranks.size()) + " ranks, ";
  if (graphicsOnly) {
    // "0 samples (0 MB)" reads as a set that failed to load. Say what was
    // actually asked for, so a silent console is not mistaken for a broken one.
    status_ += "graphics only — no audio loaded";
  } else {
    status_ += juce::String(result.samples.loaded) + " samples (" +
               juce::String(
                   proc_.sampleLibrary().residentBytes() / (1024 * 1024)) +
               " MB)";
    // Which ones, in the log: a count alone says nothing to act on (#90).
    for (const auto& f : result.samples.missingFiles)
      juce::Logger::writeToLog("samples: missing " + juce::String(f));
    if (result.samples.missing > static_cast<int>(result.samples.missingFiles.size()))
      juce::Logger::writeToLog("samples: and " +
                               juce::String(result.samples.missing - static_cast<int>(result.samples.missingFiles.size())) +
                               " more missing");
    for (const auto& f : result.samples.failedFiles)
      juce::Logger::writeToLog("samples: unreadable " + juce::String(f));
    if (result.samples.missing > 0)
      status_ += ", " + juce::String(result.samples.missing) + " missing (named in the log)";
    if (result.samples.failed > 0)
      status_ += ", " + juce::String(result.samples.failed) + " unreadable (named in the log)";
    if (result.samples.encrypted > 0)
      status_ += ", " + juce::String(result.samples.encrypted) + " encrypted";
    if (result.samples.licensed > 0)
      status_ += ", " + juce::String(result.samples.licensed) + " awaiting a licence";
    if (result.incomplete)
      status_ += "  -  INCOMPLETE (memory limit)";
  }
  if (!graphicsOnly && result.incomplete) {
    const int percent = result.samples.wanted > 0
                            ? juce::roundToInt(100.0 * result.samples.loaded / result.samples.wanted)
                            : 0;
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon, "Only part of this organ fits",
        juce::String(result.samples.loaded) + " of " + juce::String(result.samples.wanted) +
            " samples (" + juce::String(percent) + "%) were loaded before the memory limit (" +
            juce::String(proc_.memoryLimitBytes() / (1024.0 * 1024.0 * 1024.0), 1) +
            " GB) was reached. The organ plays, but some stops or notes will be silent.\n\n"
            "To load all of it: leave out perspectives or stops in Organ settings, load "
            "16-bit samples or stream the release tails on its Loading tab, or raise the "
            "limit in General settings if this computer has the memory to spare.");
  } else if (!graphicsOnly && result.samples.licensed > 0 && !proc_.licenceConfirmed()) {
    // Asked already, before any sample was read, and answered no: said, not
    // asked again. Only a load that could not ask is asked now.
    if (result.licenceAsked) top_.setStatus(status_ + "  -  licensed samples not loaded");
    else askForLicence();
  } else if (!graphicsOnly && result.samples.loaded == 0 && result.samples.encrypted > 0) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon, "This organ's samples are encrypted",
        "All " + juce::String(result.samples.encrypted) +
            " of its samples are encrypted (.hbw/.hbx) for the program they were made "
            "for, and Masterpiece cannot play them. The console loads, but the organ "
            "makes no sound.");
  } else if (!graphicsOnly && result.samples.failed > 0) {
    // The organ plays, with holes in it. Without this the only sign was a
    // line in the log and pipes that stay silent for no visible reason.
    juce::String which;
    for (size_t i = 0; i < result.samples.failedFiles.size() && i < 3; ++i)
      which << juce::String(result.samples.failedFiles[i]) << "\n";
    juce::String fix =
        which.contains(" is damaged") || which.contains(": damaged")
            ? "The archive they come from is damaged, usually by a download that was cut "
              "short or corrupted. Test it in 7-Zip or WinRAR, and download it again."
            : "The files are damaged or not sample files Masterpiece can read. Installing "
              "the organ again usually puts them right.";
    showWithLog("Some samples could not be read",
                juce::String(result.samples.failed) + " sample file" +
                    (result.samples.failed == 1 ? " was" : "s were") +
                    " unreadable, so the pipes they belong to are silent. The organ plays "
                    "without them.\n\n" + which + "\n" + fix);
  }
  // Say it out loud, once. The status line has no room beside the console's
  // buttons, and a mapping that changes without a word costs more trust than
  // the fault it fixes. Only shown when a repair actually happened, which is
  // the first load of a mapping saved by 0.3.7 or earlier.
  if (const int fixed = proc_.midiMapRepairedOnLoad(); fixed > 0) {
    status_ += "  -  MIDI mapping repaired";
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon, "MIDI mapping repaired",
        "The saved MIDI mapping for this organ assigned more than one manual "
        "to the same channel, which left some manuals silent. " +
            juce::String(fixed) + " conflicting assignment" +
            (fixed == 1 ? " was" : "s were") +
            " removed, and each manual now uses the organ's own channel.\n\n"
            "Your previous mapping was saved next to the new one with the "
            "ending \".before-repair\". You can reassign manuals in "
            "Settings > MIDI.");
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

void MasterpieceEditor::resized() {
  auto r = getLocalBounds();
  top_.statusLabel().setBounds(r.removeFromBottom(20));

  // One band for every control. These are the editor's own children rather
  // than the bar's, so they are placed into the right of the same strip and
  // the bar lays itself out in whatever is left -- no reparenting, and the
  // status line absorbs the difference.
  auto bar = r.removeFromTop(36);
  layout_.setVisible(showingConsole_ && console_.layoutCount() > 1);
  swellButton_.setVisible(expression_.shoeCount() > 0);
  // On a narrow window -- a monitor stood on end is 1080 wide -- the buttons
  // on the right left the bar on the left too little room, and the volume
  // fader went with it (#53). Below that, the buttons take a row of their own.
  bool stacked = false;
  {
    constexpr int kFaderRowMin = 350;  // Open, Audio, No DSP and the fader
    const int buttons = 90 + 70 + 64 + 110 + 110 + 140 + 30 + 64 + 30 + 56 +
                        (layout_.isVisible() ? 130 : 0) +
                        (swellButton_.isVisible() ? 70 : 0);
    if (bar.getWidth() - buttons < kFaderRowMin) {
      top_.setBounds(bar);
      bar = r.removeFromTop(36);
      stacked = true;
    }
  }
  settingsButton_.setBounds(bar.removeFromRight(90).reduced(2));
  if (layout_.isVisible())
    layout_.setBounds(bar.removeFromRight(130).reduced(2));
  keysButton_.setBounds(bar.removeFromRight(70).reduced(2));
  // No swell button on an organ with nothing to enclose.
  panicButton_.setBounds(bar.removeFromRight(64).reduced(2));
  if (swellButton_.isVisible())
    swellButton_.setBounds(bar.removeFromRight(70).reduced(2));
  toggleView_.setBounds(bar.removeFromRight(110).reduced(2));
  combinationsButton_.setBounds(bar.removeFromRight(110).reduced(2));
  tuningButton_.setBounds(bar.removeFromRight(140).reduced(2));
  // Sequencer, right to left: next, the frame it is on, previous, the setter.
  stepNext_.setBounds(bar.removeFromRight(30).reduced(2));
  stepFrame_.setBounds(bar.removeFromRight(64).reduced(2));
  stepPrev_.setBounds(bar.removeFromRight(30).reduced(2));
  setter_.setBounds(bar.removeFromRight(56).reduced(2));
  if (!stacked) top_.setBounds(bar);

  // The tabs keep a strip of their own, and only when there is more than one
  // page to choose between -- so a single-page organ shows one row in total.
  pageTabs_.setVisible(showingConsole_ && console_.pageCount() > 1);
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

  consoleView_.setVisible(showingConsole_);
  jambView_.setVisible(!showingConsole_);
  if (showingConsole_) {
    // Scale the artwork to fit rather than scrolling a 1536x864 console
    // through a smaller window. JUCE routes mouse events back through the
    // transform, so drawstops stay clickable at any zoom.
    const auto art = console_.artworkBounds();
    if (art.getWidth() <= 0 || art.getHeight() <= 0) {
      consoleView_.setBounds(r);
    } else {
      console_.setTransform({});
      console_.setBounds(0, 0, art.getRight(), art.getBottom());
      const float sx = static_cast<float>(r.getWidth()) /
                       static_cast<float>(art.getRight());
      const float sy = static_cast<float>(r.getHeight()) /
                       static_cast<float>(art.getBottom());
      // Scale UP as well as down. The old cap at 1.0 meant a console drawn
      // smaller than the window sat at its native size with a band of dead
      // background beside it, which read as part of the program rather than
      // as empty space.
      //
      // Uniformly, and never to fill the width exactly: the console is a
      // photograph of a real instrument, so stretching it to the window's
      // aspect would visibly distort the case and the keys. Whichever
      // dimension runs out first sets the size.
      const float scale = juce::jmin(sx, sy);
      console_.setTransform(juce::AffineTransform::scale(scale));

      // Centre by moving the VIEWPORT, not the console inside it: a viewport
      // positions its own viewed component, so a translation applied to the
      // console is overwritten the moment the viewport lays out. Sizing the
      // viewport to the scaled artwork and centring that leaves the spare
      // width split evenly either side instead of banked in one strip.
      const int w = juce::roundToInt(static_cast<float>(art.getRight()) * scale);
      const int h = juce::roundToInt(static_cast<float>(art.getBottom()) * scale);
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

void MasterpieceEditor::showConsolePage(int oneBased) {
  const int index = oneBased - 1;
  if (index < 0 || index >= console_.pageCount()) return;
  pageTabs_.setCurrentTabIndex(index, true);
}

void MasterpieceEditor::changeListenerCallback(juce::ChangeBroadcaster* src) {
  if (src == &pageTabs_) console_.setPage(pageTabs_.getCurrentTabIndex());
}

void MasterpieceEditor::timerCallback() {
  // A drawstop clicked on the console changes the jamb too, and vice versa.
  if (showingConsole_) console_.repaint();
  // And the stop list follows the console, pistons and MIDI the same way
  // (#56): it used to show only what was clicked in it.
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
      toggleView_.triggerClick();
      break;
    case MidiTargetKind::ConsoleToggleKeyboard:
      keysButton_.triggerClick();
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

  // A piston captured on the audio thread only raised a flag; the writing
  // happens here, where a file write is allowed. Combinations are the
  // player's own work and losing them to a crash would be unforgivable, so
  // this saves as soon as it sees one rather than at shutdown.
  proc_.saveCombinationsIfDirty();
  // The same for the per-organ settings and anything just learned: raised on
  // whichever thread changed it, written here, where a file write is allowed.
  proc_.saveSettingsIfDirty();
  proc_.saveMidiMapIfDirty();
  proc_.saveMasterGainIfDirty();
  proc_.saveRememberedStateIfSettled();

  // The sequencer's frame, out of the frames holding anything. With Set on
  // the next step is always open: stepping on is how a sequence grows.
  const auto& seq = proc_.playerCombinations();
  const int last = seq.lastUsedFrame();
  const bool capturing = proc_.captureMode();
  stepFrame_.setText(last == 0 && seq.frame() == 0
                         ? juce::String("empty")
                         : juce::String(seq.frame()) + " / " +
                               juce::String(juce::jmax(last, seq.frame())),
                     juce::dontSendNotification);
  stepPrev_.setEnabled(seq.frame() > 1);
  stepNext_.setEnabled(capturing || seq.frame() < last);
  setter_.setToggleState(capturing, juce::dontSendNotification);
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
  live += "voices " + juce::String(stats.activeVoices);
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
    live += "  |  saving the samples for the next load";
    if (f >= 0.0) {
      live += ": " + juce::String(juce::roundToInt(f * 100.0)) + "%";
      const double elapsed = (now - cacheSaveStartMs_) / 1000.0;
      const double moved = f - cacheSaveStartFraction_;
      if (elapsed > 3.0 && moved > 0.01) {
        const int left = juce::roundToInt(elapsed / moved * (1.0 - f));
        live += left >= 90 ? ", about " + juce::String((left + 30) / 60) + " min left"
                           : ", about " + juce::String(juce::jmax(1, left)) + " s left";
      }
    } else {
      live += "...";
    }
  } else {
    cacheSaveStartMs_ = 0.0;
  }
  if (stats.startsDropped > 0)
    live += ", dropped " + juce::String(stats.startsDropped);
  if (stats.samplesMissing > 0)
    live += ", no audio " + juce::String(stats.samplesMissing);

  // Audio that did not keep time: each late block is a glitch the player
  // heard, and the log says how late, so a report can say why (#120).
  const auto load = proc_.takeAudioLoad();
  worstBlockSinceLog_ = juce::jmax(worstBlockSinceLog_, load.worstPercent);
  if (load.late > 0) live += ", late blocks " + juce::String(load.late);
  // A streamed release the disk did not deliver in time goes quiet partway.
  // Said here, with what to do about it: the Loading tab's count is out of
  // sight, and whether a machine can stream is only known by playing (#120).
  if (proc_.streamReleases())
    if (const auto under = proc_.streamUnderruns(); under > 0)
      live += ", streamed releases fell behind " + juce::String(under) +
              "x: turn off \"Stream release tails\" in Organ settings";
  const auto now = juce::Time::getMillisecondCounter();
  if (load.late > lateBlocksLogged_ && now - lateLoggedAt_ > 5000) {
    juce::Logger::writeToLog("audio: " + juce::String(load.late - lateBlocksLogged_) +
                             " late block(s) of " + juce::String(load.blocks) +
                             "; the slowest took " + juce::String(worstBlockSinceLog_, 0) +
                             "% of its time");
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
  return new ui::MasterpieceEditor(*this);
}
} // namespace mp
