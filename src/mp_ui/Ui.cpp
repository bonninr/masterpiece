#include "Ui.h"
#include "../mp_archive/OrganArchive.h"

#include "LoadingDialog.h"

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
  const int volumeW = juce::jlimit(90, 130, room);
  const int meterW = juce::jlimit(0, 112, room - volumeW - 8);
  volume_.setBounds(r.removeFromLeft(volumeW));
  r.removeFromLeft(8);
  // Beside the fader it answers for: the two are read together.
  meter_.setVisible(meterW >= 40);
  meter_.setBounds(r.removeFromLeft(meterW).reduced(0, 5));
  r.removeFromLeft(10);
  // Whatever is left. The status line is the one thing here that can be
  // shortened without losing a control, so it takes the squeeze.
  status_.setVisible(r.getWidth() >= 60);
  status_.setBounds(r);
}

// ---------------------------------------------------------------- editor

// The organ file dialog. Its own method rather than a lambda in the member
// list, because the first-run wizard needs the same door.
void MasterpieceEditor::chooseAndLoadOrgan(const juce::File& startIn) {
  // The extension pattern names the format because that IS the file name on
  // disk; the prompt does not, because the player is choosing an organ.
  chooser_ = std::make_unique<juce::FileChooser>(
      "Choose an organ definition file", startIn,
      "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.organ;*.rar;*.orgue");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                            juce::FileBrowserComponent::canSelectFiles,
                        [this](const juce::FileChooser& fc) {
                          const auto f = fc.getResult();
                          if (f.existsAsFile()) loadOrgan(f);
                        });
}

MasterpieceEditor::MasterpieceEditor(MasterpieceProcessor& p)
    : juce::AudioProcessorEditor(p),
      proc_(p),
      top_(p, [this] { chooseAndLoadOrgan(); },
           [this] { if (onAudioSettings) onAudioSettings(); }),
      console_(p),
      jamb_(p),
      expression_(p),
      keyboard_(p.keyboardState(),
                juce::MidiKeyboardComponent::horizontalKeyboard) {
  addAndMakeVisible(top_);

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
    if (open) menu.addItem(2, "Close its window");
    menu.showMenuAsync(juce::PopupMenu::Options(), [this, page](int choice) {
      if (choice == 1) openPageWindow(page);
      if (choice == 2) closePageWindow(pageWindowFor(page));
    });
  };
  addAndMakeVisible(settingsButton_);
  settingsButton_.onClick = [this] {
    // Two windows: what belongs to this organ, and what belongs to the
    // program and the player's console.
    juce::PopupMenu menu;
    menu.addItem(1, "Organ settings...", !proc_.loadedOrganFile().getFullPathName().isEmpty());
    menu.addItem(2, "General settings...");
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
  // Where the page windows are, for the next time; then they go, before the
  // console they share a processor with.
  rememberPageWindows();
  proc_.saveSettingsIfDirty();
  pageWindows_.clear();
  // A level moved in the last second before quitting.
  proc_.saveRememberedStateIfPending();
}

void MasterpieceEditor::toggleCombinations() {
  if (combinations_ == nullptr) return;
  combinations_->showOrHide(!combinations_->isVisible());
}

void MasterpieceEditor::loadOrgan(const juce::File& odf, bool graphicsOnly) {
  if (loading_) return;  // one load at a time; the dialog is the interlock

  // An organ still in its packages is opened first, on its own thread, since
  // indexing a solid archive means decompressing it. What comes out is an
  // ordinary organ definition, loaded like any other. Packages that hold
  // several, as demo sets often do, leave the choice to the player in the
  // same file chooser, opened where they were unpacked.
  if (mp::isOrganArchive(odf.getFullPathName().toStdString())) {
    loading_ = true;
    top_.setStatus("Opening " + odf.getFileName() + "...");
    juce::Thread::launch([this, odf, graphicsOnly] {
      juce::String error;
      const auto definitions = proc_.openPackagedOrgan(odf, error);
      juce::MessageManager::callAsync([this, odf, graphicsOnly, definitions, error] {
        loading_ = false;
        if (definitions.size() == 1) {
          loadOrgan(definitions.getFirst(), graphicsOnly);
        } else if (definitions.size() > 1) {
          chooseDefinition(definitions, graphicsOnly);
        } else {
          status_ = "Failed to open " + odf.getFileName();
          top_.setStatus(status_);
          // Said where the player is looking, with where the details are:
          // a status line is easy to miss and says too little to act on.
          juce::String where;
          if (auto* file = dynamic_cast<juce::FileLogger*>(juce::Logger::getCurrentLogger()))
            where = "\n\nThe steps are in the log: " + file->getLogFile().getFullPathName();
          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                 "Could not open " + odf.getFileName(),
                                                 error + where);
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

void MasterpieceEditor::startLoad(const juce::File& odf, bool graphicsOnly) {
  if (loading_) return;
  loading_ = true;
  closePageWindowsForLoad();
  top_.setStatus("Loading " + odf.getFileName() + "...");

  // The load runs on its own thread and the message loop keeps running, so
  // the window paints and the Cancel button answers. Doing this work on the
  // message thread is what used to whiten the window for minutes and let
  // Windows offer to kill the program.
  auto dialog = std::make_unique<LoadingDialog>(proc_, odf.getFileNameWithoutExtension());
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
  loadWindow_ = opts.launchAsync();

  juce::Thread::launch([this, odf, graphicsOnly] {
    const auto result = proc_.loadOrgan(odf, /*maxFramesPerSample*/ 0, graphicsOnly);
    // Everything past here touches components, so it belongs to the message
    // thread. The lambda copies what it needs; the loader thread ends here.
    juce::MessageManager::callAsync(
        [this, odf, graphicsOnly, result] { finishLoad(odf, graphicsOnly, result); });
  });
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

PageWindow* MasterpieceEditor::pageWindowFor(int page) const {
  for (const auto& w : pageWindows_)
    if (w->page() == page) return w.get();
  return nullptr;
}

void MasterpieceEditor::openPageWindow(int page, juce::Rectangle<int> bounds) {
  if (auto* open = pageWindowFor(page)) {
    open->toFront(true);
    return;
  }
  const auto& m = proc_.organModel();
  auto window = std::make_unique<PageWindow>(proc_, page, console_.layout(),
                                             m.organName.empty() ? juce::String("Masterpiece")
                                                                 : juce::String(m.organName));
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
  window->setVisible(true);
  pageWindows_.push_back(std::move(window));
  rememberPageWindows();
}

void MasterpieceEditor::closePageWindow(PageWindow* window) {
  for (auto it = pageWindows_.begin(); it != pageWindows_.end(); ++it)
    if (it->get() == window) {
      pageWindows_.erase(it);
      break;
    }
  rememberPageWindows();
}

void MasterpieceEditor::rememberPageWindows() {
  std::vector<MasterpieceProcessor::PagePlace> places;
  for (const auto& w : pageWindows_) {
    const auto b = w->getBounds();
    places.push_back({w->page(), b.getX(), b.getY(), b.getWidth(), b.getHeight()});
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
      openPageWindow(p.page, {p.x, p.y, p.w, p.h});
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
              " GB).\n\nIn Settings, Engine: load 16-bit samples, stream the release "
              "tails, load in mono or preload less of each sample -- or raise the "
              "limit, if this computer has the memory to spare.");
    return;
  }

  // Timed separately from the model: this is where the console artwork is
  // actually decoded, and on a set with a thousand bitmaps it can dominate a
  // load that has no audio in it at all.
  const double artStart = juce::Time::getMillisecondCounterHiRes();
  jamb_.rebuild();
  expression_.rebuild();
  console_.rebuild();
  if (combinations_ != nullptr) {
    combinations_->panel().rebuild();
    // An organ with pistons of its own has somewhere to register already;
    // one without gets the window the first time, so it is found.
    combinations_->place(getScreenBounds(), proc_.organModel().combinations.empty());
  }
  juce::Logger::writeToLog(
      "load: artwork       " +
      juce::String(juce::Time::getMillisecondCounterHiRes() - artStart, 1) +
      " ms");

  pageTabs_.clearTabs();
  for (int i = 0; i < console_.pageCount(); ++i)
    pageTabs_.addTab(console_.pageName(i), juce::Colour(0xff2a2f3a), i);
  if (console_.pageCount() > 0) pageTabs_.setCurrentTabIndex(0, false);
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
    if (result.samples.missing > 0)
      status_ += ", " + juce::String(result.samples.missing) + " missing";
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
  // else's windows when it is not.
  if (combinations_ != nullptr && combinations_->isVisible()) {
    const bool front = juce::Process::isForegroundProcess();
    if (combinations_->isAlwaysOnTop() != front) combinations_->setAlwaysOnTop(front);
  }

  // The swell strip follows a pedal moved over MIDI.
  if (expression_.isVisible()) expression_.refresh();

  // Voice count is the honest health readout: it says whether drawing a stop
  // and pressing a key actually produced sound.
  const auto& stats = proc_.voiceStats();
  juce::String live = status_;
  if (live.isNotEmpty()) live += "  |  ";
  live += "voices " + juce::String(stats.activeVoices);
  if (stats.startsDropped > 0)
    live += ", dropped " + juce::String(stats.startsDropped);
  if (stats.samplesMissing > 0)
    live += ", no audio " + juce::String(stats.samplesMissing);
  top_.setStatus(live);
}

} // namespace mp::ui

// Processor -> editor hook (kept here to keep mp_audio UI-free).
namespace mp {
juce::AudioProcessorEditor* MasterpieceProcessor::createEditor() {
  return new ui::MasterpieceEditor(*this);
}
} // namespace mp
