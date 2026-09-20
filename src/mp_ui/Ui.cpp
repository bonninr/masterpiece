#include "Ui.h"

#include "LoadingDialog.h"
#include "OrganDialog.h"

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

constexpr int kStopHeight = 26;
constexpr int kHeaderHeight = 22;
constexpr int kJambWidth = 320;

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
    e.button->setEnabled(s.playable);
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
  const int rows = static_cast<int>(entries_.size());
  const int heads = static_cast<int>(headers_.size());
  setSize(kJambWidth, rows * kStopHeight + heads * kHeaderHeight + 8);
  resized();
}

void StopJamb::resized() {
  auto r = getLocalBounds().reduced(4, 4);
  size_t headerIndex = 0;
  Id lastDivision = -1;
  for (auto& e : entries_) {
    if (e.divisionId != lastDivision && headerIndex < headers_.size()) {
      headers_[headerIndex++]->setBounds(r.removeFromTop(kHeaderHeight));
      lastDivision = e.divisionId;
    }
    e.button->setBounds(r.removeFromTop(kStopHeight).reduced(1));
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
    shoes.emplace_back(enc.continuousControlId,
                       enc.name.empty() ? juce::String("Swell")
                                        : juce::String(enc.name));
  }
  std::sort(shoes.begin(), shoes.end());

  for (const auto& [controlId, name] : shoes) {
    auto label = std::make_unique<juce::Label>();
    label->setText(name, juce::dontSendNotification);
    label->setJustificationType(juce::Justification::centred);
    label->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(*label);
    labels_.push_back(std::move(label));

    auto slider = std::make_unique<juce::Slider>(
        juce::Slider::LinearVertical, juce::Slider::NoTextBox);
    slider->setRange(0.0, 127.0, 1.0);
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
  load_.setBounds(r.removeFromLeft(72));
  r.removeFromLeft(6);
  audio_.setBounds(r.removeFromLeft(64));
  r.removeFromLeft(6);
  simple_.setBounds(r.removeFromLeft(88));
  r.removeFromLeft(10);
  volume_.setBounds(r.removeFromLeft(130));
  r.removeFromLeft(8);
  // Beside the fader it answers for: the two are read together.
  meter_.setBounds(r.removeFromLeft(112).reduced(0, 5));
  r.removeFromLeft(10);
  // Whatever is left. The status line is the one thing here that can be
  // shortened without losing a control, so it takes the squeeze.
  status_.setBounds(r);
}

// ---------------------------------------------------------------- editor

// The organ file dialog. Its own method rather than a lambda in the member
// list, because the first-run wizard needs the same door.
void MasterpieceEditor::chooseAndLoadOrgan() {
  // The extension pattern names the format because that IS the file name on
  // disk; the prompt does not, because the player is choosing an organ.
  chooser_ = std::make_unique<juce::FileChooser>(
      "Choose an organ definition file", juce::File(),
      "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                            juce::FileBrowserComponent::canSelectFiles,
                        [this](const juce::FileChooser& fc) {
                          const auto f = fc.getResult();
                          if (f.existsAsFile()) loadOrgan(f);
                        });
}

void MasterpieceEditor::showOrganDialog() {
  OrganDialog::show(*this, proc_);
}

MasterpieceEditor::MasterpieceEditor(MasterpieceProcessor& p)
    : juce::AudioProcessorEditor(p),
      proc_(p),
      top_(p, [this] { showOrganDialog(); },
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
  addAndMakeVisible(settingsButton_);
  settingsButton_.onClick = [this] { if (onSettings) onSettings(); };

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
  setter_.setTooltip("Hold to store the registration into the frame you step "
                     "onto, instead of recalling it");
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

  setSize(1180, 760);
  setResizable(true, true);
  top_.setStatus("No organ loaded.");
  startTimerHz(4);
}

MasterpieceEditor::~MasterpieceEditor() { stopTimer(); }

void MasterpieceEditor::unloadOrgan() {
  proc_.unloadOrgan();
  jamb_.rebuild();
  expression_.rebuild();
  console_.rebuild();
  pageTabs_.clearTabs();
  layout_.clear(juce::dontSendNotification);
  status_ = "No organ loaded";
  top_.setStatus(status_);
  if (onOrganLoaded) onOrganLoaded("No organ loaded");
}

void MasterpieceEditor::loadOrgan(const juce::File& odf, bool graphicsOnly) {
  if (loading_) return;  // one load at a time; the dialog is the interlock
  loading_ = true;
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
    if (!graphicsOnly) {
      triggerBackgroundAudioStatPrecomputation(odf);
    }
    const auto result = proc_.loadOrgan(odf, /*maxFramesPerSample*/ 0, graphicsOnly);
    // Everything past here touches components, so it belongs to the message
    // thread. The lambda copies what it needs; the loader thread ends here.
    juce::MessageManager::callAsync(
        [this, odf, graphicsOnly, result] { finishLoad(odf, graphicsOnly, result); });
  });
}

// The message-thread half of a load: close the dialog, then either report the
// failure or build the console from the model that is now in place.
void MasterpieceEditor::finishLoad(const juce::File& odf, bool graphicsOnly,
                                   const MasterpieceProcessor::LoadResult& result) {
  loading_ = false;
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
    return;
  }

  // Timed separately from the model: this is where the console artwork is
  // actually decoded, and on a set with a thousand bitmaps it can dominate a
  // load that has no audio in it at all.
  const double artStart = juce::Time::getMillisecondCounterHiRes();
  jamb_.rebuild();
  expression_.rebuild();
  console_.rebuild();
  juce::Logger::writeToLog(
      "load: artwork       " +
      juce::String(juce::Time::getMillisecondCounterHiRes() - artStart, 1) +
      " ms");

  pageTabs_.clearTabs();
  for (int i = 0; i < console_.pageCount(); ++i)
    pageTabs_.addTab(console_.pageName(i), juce::Colour(0xff2a2f3a), i);
  if (console_.pageCount() > 0) pageTabs_.setCurrentTabIndex(0, false);

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
  settingsButton_.setBounds(bar.removeFromRight(90).reduced(2));
  layout_.setVisible(showingConsole_ && console_.layoutCount() > 1);
  if (layout_.isVisible())
    layout_.setBounds(bar.removeFromRight(130).reduced(2));
  keysButton_.setBounds(bar.removeFromRight(70).reduced(2));
  // No swell button on an organ with nothing to enclose.
  panicButton_.setBounds(bar.removeFromRight(64).reduced(2));
  swellButton_.setVisible(expression_.shoeCount() > 0);
  if (swellButton_.isVisible())
    swellButton_.setBounds(bar.removeFromRight(70).reduced(2));
  toggleView_.setBounds(bar.removeFromRight(110).reduced(2));
  // Sequencer, right to left: next, the frame it is on, previous, the setter.
  stepNext_.setBounds(bar.removeFromRight(30).reduced(2));
  stepFrame_.setBounds(bar.removeFromRight(64).reduced(2));
  stepPrev_.setBounds(bar.removeFromRight(30).reduced(2));
  setter_.setBounds(bar.removeFromRight(56).reduced(2));
  top_.setBounds(bar);

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
    jamb_.setSize(jambView_.getWidth() - 12, jamb_.getHeight());
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

  // The sequencer's frame, and whether it has anything to walk. An organ with
  // no generals says so rather than showing a dash that could mean anything.
  const auto& seq = proc_.stepper();
  if (seq.empty()) {
    stepFrame_.setText("no seq.", juce::dontSendNotification);
    stepPrev_.setEnabled(false);
    stepNext_.setEnabled(false);
  } else {
    stepFrame_.setText(juce::String(seq.frame()) + " / " +
                           juce::String(static_cast<int>(seq.frameCount())),
                       juce::dontSendNotification);
    stepPrev_.setEnabled(seq.frame() > 1);
    stepNext_.setEnabled(seq.frame() < static_cast<int>(seq.frameCount()));
  }
  setter_.setToggleState(proc_.captureMode(), juce::dontSendNotification);

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
