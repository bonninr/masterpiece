// Settings and session tools: everything that is not the organ itself.
//
// One panel with tabs rather than a menu tree, because on a touch console the
// player is standing at a keyboard, not sitting at a mouse. The engine owns
// all the state; this only reads and writes it.
#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"

#include <array>
#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

namespace mp::ui {

// Engine: DSP switches and how much of each sample is preloaded.
// A tab that scrolls when the window is too short for its contents. The
// Engine tab has more in it than fits a small window -- reported on macOS,
// where the default window left the last rows off the bottom with no way to
// reach them but resizing.
class ScrollHost : public juce::Viewport {
public:
  // contentHeight, when given, is the height the contents need, for a tab
  // whose height changes with the organ (the MIDI tab has a row per keyboard).
  explicit ScrollHost(juce::Component& c, int minHeight, std::function<int()> contentHeight = {})
      : minHeight_(minHeight), contentHeight_(std::move(contentHeight)) {
    setViewedComponent(&c, false);
    setScrollBarsShown(true, false);
  }
  void resized() override {
    juce::Viewport::resized();
    fit();
  }
  // Sizes the contents to the window, or taller where they need it, leaving
  // room for the scroll bar when there is one.
  void fit() {
    auto* c = getViewedComponent();
    if (c == nullptr) return;
    const int needed = juce::jmax(minHeight_, contentHeight_ ? contentHeight_() : 0);
    const bool scrolls = needed > getHeight();
    c->setSize(getWidth() - (scrolls ? getScrollBarThickness() : 0), juce::jmax(needed, getHeight()));
  }

private:
  int minHeight_;
  std::function<int()> contentHeight_;
};

class EnginePanel : public juce::Component, private juce::Timer {
public:
  explicit EnginePanel(MasterpieceProcessor& p);
  ~EnginePanel() override;
  // Loads the organ again with what this panel now says. Given by a window
  // that can do that; without it, no button is offered.
  std::function<void()> onReload;
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  void timerCallback() override;
  void pushSwitches();
  // Apply one of the named profiles to all four memory settings at once.
  void applyProfile(int id);
  // Move the profile box to whichever profile the current settings match, or
  // to Custom when they match none.
  void syncProfile();
  // Put the engine, and the controls, back to the state captured when this
  // panel was built.
  void revert();
  void closeDialog();
  void showFasterUnit();
  void showRealtimeUnit();
  void layoutFooter(juce::Rectangle<int> footer);

  MasterpieceProcessor& proc_;

  // Settings here apply live, so leaving is not by itself a decision: the
  // player needs a way back that does not depend on remembering what was
  // there. Captured once, on open.
  EngineSwitch openSwitch_;
  int64_t openPreload_ = 0;
  SampleStorage openStorage_ = SampleStorage::Float32;
  bool openStream_ = false;
  int openStreamPercent_ = 0;
  bool openMono_ = false;
  // Whether the program couplers are offered. Off by default.
  bool openProgramCouplers_ = false;
  juce::ToggleButton programCouplers_{TRANS("Program couplers")};
  double openRate_ = 0.0;
  SampleLibrary::ReleaseChoice openReleases_ = SampleLibrary::ReleaseChoice::All;
  int openReleaseMs_ = 0;
  int openSparse_ = 1;
  SampleLibrary::CacheMode openCache_ = SampleLibrary::CacheMode::Single;
  // General config rather than per-organ state, but shown here because this
  // is the panel about loading. Reverted like the rest; each change writes
  // the global file at once.
  bool openLoadTicks_ = false;
  bool openReopen_ = false;
  bool openFaster_ = true;
  bool openRealtime_ = false;
  int openMemLimit_ = 0;
  int openPolyphony_ = 0;
  // Set while a profile is writing the individual controls, so their
  // onChange handlers do not bounce the profile straight back to Custom.
  bool applyingProfile_ = false;

  // A loading setting changed since the panel opened: none of them reach the
  // organ already loaded, and nothing said so (#120: streaming switched off,
  // and the glitches went on until Masterpiece was restarted).
  bool loadingChanged() const;
  juce::TextButton reload_{TRANS("Load the organ again to apply")};
  juce::TextButton revert_{TRANS("Revert changes")};
  juce::TextButton keep_{TRANS("Keep changes")};
  juce::TextButton saveOrgan_{TRANS("Save for this organ")};
  juce::TextButton saveGlobal_{TRANS("Save as default")};
  // A save leaves the panel open, so the organ can be loaded again with what
  // was saved (#120). The button says it was saved, for a moment.
  void showSaved(juce::TextButton& button);
  juce::uint32 savedUntilMs_ = 0;
  juce::ToggleButton simpleWav_{TRANS("Simple WAV only (bypass all DSP)")};
  juce::ToggleButton wind_{TRANS("Wind model")};
  juce::ToggleButton tremulant_{TRANS("Tremulants")};
  juce::ToggleButton enclosure_{TRANS("Enclosures (swell shades)")};
  juce::ToggleButton voicing_{TRANS("Voicing")};
  juce::ToggleButton originalPitch_{TRANS("Play at the original organ's pitch")};
  juce::Label preloadLabel_;
  juce::ComboBox preload_;
  juce::Label storageLabel_;
  juce::ComboBox storage_;
  juce::Label rateLabel_;
  juce::ComboBox rate_;
  juce::Label releasesLabel_;
  juce::ComboBox releases_;
  juce::Label releaseLengthLabel_;
  juce::ComboBox releaseLength_;
  juce::Label releaseCutoffLabel_;
  juce::ComboBox releaseCutoff_;
  void showReleaseLength(int ms);
  // How many notes of a rank are loaded.
  juce::Label sparseLabel_;
  juce::ComboBox sparse_;
  juce::Label cacheLabel_;
  juce::ComboBox cache_;
  juce::Label cacheWhenLabel_;
  juce::ComboBox cacheWhen_;
  juce::Label rebuildNote_;
public:
  // Whether closing now should rebuild the loaded organ's cache: a setting
  // that changes what it holds differs from what the organ was loaded with.
  bool cacheRebuildPending() const;
private:
  juce::Label cacheDirLabel_;
  juce::Label cacheDirValue_;
  juce::TextButton cacheDirChoose_{TRANS("Choose...")};
  juce::TextButton cacheDirDefault_{TRANS("Default")};
  std::unique_ptr<juce::FileChooser> cacheDirChooser_;
  void showCacheDir();
  juce::Label packageDirLabel_;
  juce::Label packageDirValue_;
  juce::TextButton packageDirChoose_{TRANS("Choose...")};
  juce::TextButton packageDirClear_{TRANS("Clear")};
  std::unique_ptr<juce::FileChooser> packageDirChooser_;
  void showPackageDir();
  juce::Label organRootLabel_;
  juce::Label organRootValue_;
  juce::TextButton organRootChoose_{TRANS("Choose...")};
  juce::TextButton organRootDefault_{TRANS("Default")};
  std::unique_ptr<juce::FileChooser> organRootChooser_;
  void showOrganRoot();
  juce::Label profileLabel_;
  juce::ComboBox profile_;
  juce::ToggleButton stream_{TRANS("Stream release tails from disk")};
  juce::ComboBox streamHead_;
  void showStreamPercent(int percent);
  void showPreload(int64_t frames);
  juce::ToggleButton mono_{TRANS("Load in mono (halves memory, gives up the stereo image)")};
  juce::ToggleButton loadTicks_{TRANS("Tap at each 10% while loading an organ")};
  juce::ToggleButton reopen_{TRANS("Reopen the last organ when Masterpiece starts")};
  juce::ToggleButton faster_{TRANS("Faster audio engine")};
  juce::ToggleButton realtime_{TRANS("Real-time render threads (trial)")};
  juce::ToggleButton portable_{TRANS("Keep this organ playable without its installation files")};
  juce::Label memLimitLabel_;
  juce::ComboBox memLimit_;
  // Fill the memory-limit choices around the current setting.
  void fillMemLimit();
  juce::Label polyphonyLabel_;
  juce::ComboBox polyphony_;
  void fillPolyphony();
  juce::Label memory_;
  juce::Label note_;
};

// Room: impulse-response convolution.
class ReverbPanel : public juce::Component {
public:
  explicit ReverbPanel(MasterpieceProcessor& p);
  void resized() override;

private:
  MasterpieceProcessor& proc_;
  juce::ToggleButton enabled_{TRANS("Impulse-response reverb")};
  juce::TextButton load_{TRANS("Load IR...")};
  juce::TextButton clear_{TRANS("Clear")};
  juce::Label irName_;
  juce::Label mixLabel_;
  juce::Slider mix_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label note_;
  std::unique_ptr<juce::FileChooser> chooser_;
};

// Practice: metronome.
class MetronomePanel : public juce::Component, private juce::Timer {
public:
  explicit MetronomePanel(MasterpieceProcessor& p);
  ~MetronomePanel() override;
  void resized() override;

private:
  void timerCallback() override;

  MasterpieceProcessor& proc_;
  juce::ToggleButton enabled_{TRANS("Metronome")};
  juce::Label tempoLabel_, beatsLabel_, levelLabel_;
  juce::Slider tempo_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Slider beats_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Slider level_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label beat_;
};

// A mapping's message, such as "Note 36 ch 2", for a MIDI menu's header.
juce::String describeBinding(const MidiBinding& b);

// Session: MIDI recorder and player.
class RecorderPanel : public juce::Component, private juce::Timer {
public:
  explicit RecorderPanel(MasterpieceProcessor& p);
  ~RecorderPanel() override;
  void resized() override;

  // The width the panel needs for its widest row of buttons. The window
  // around it is that wide, so a label is never squeezed.
  int preferredWidth();

private:
  void timerCallback() override;
  // The Record button. A take that was never saved is worth keeping, so the
  // player is asked what to do with it before it is thrown away.
  void askBeforeRecording();

  // One row of buttons, each as wide as its own text. A row that is wider
  // than the panel wraps to a second line; the height it took is returned.
  int placeRow(juce::Rectangle<int> row, std::initializer_list<juce::TextButton*> buttons);

  MasterpieceProcessor& proc_;
  juce::TextButton record_{TRANS("Record")};
  juce::TextButton play_{TRANS("Play")};
  juce::TextButton stop_{TRANS("Stop")};
  juce::TextButton save_{TRANS("Save MIDI...")};
  juce::TextButton load_{TRANS("Load MIDI...")};
  juce::TextButton clear_{TRANS("Clear")};
  // Learn and Clear for the console's Record and Play buttons.
  juce::TextButton midi_{TRANS("MIDI...")};
  // The take as a file for a sequencer or a notation program.
  juce::TextButton saveForSequencer_{TRANS("Save for a sequencer...")};
  juce::ToggleButton markers_{TRANS("Write the registration as markers")};
  void showMidiMenu();
  juce::Label status_;
  juce::Label note_;
  std::unique_ptr<juce::FileChooser> chooser_;

  // Audio capture is a separate recording from the MIDI one and they are
  // useful together: the MIDI file is the performance and can be replayed
  // through a different registration, the WAV is what it sounded like.
  juce::Label audioHeading_;
  juce::TextButton audioRecord_{TRANS("Record audio...")};
  juce::TextButton audioStop_{TRANS("Stop")};
  juce::Label audioStatus_;
  juce::Label audioNote_;
  std::unique_ptr<juce::FileChooser> audioChooser_;
};

// MIDI: what the console sends and what comes back, plus the learned mapping.
class MidiPanel : public juce::Component, private juce::Timer {
public:
  MidiPanel(MasterpieceProcessor& p, juce::AudioDeviceManager& devices);
  ~MidiPanel() override;
  void resized() override;
  void refresh();
  // The height the panel's rows and note need at its present width.
  int contentHeight() const { return contentHeight_; }

private:
  void timerCallback() override;

  MasterpieceProcessor& proc_;
  juce::AudioDeviceManager& devices_;
  int contentHeight_ = 0;
  juce::Label inputsLabel_, outputsLabel_, keyboardsLabel_;
  std::vector<std::unique_ptr<juce::ToggleButton>> inputs_;
  // One row per playable keyboard: which MIDI channel plays it. This is the
  // setting that decides whether the manual under your hands sounds the Great
  // or the Pedal, and no organ can guess it for you.
  std::vector<std::unique_ptr<juce::Label>> keyboardLabels_;
  std::vector<std::unique_ptr<juce::ComboBox>> keyboardChannels_;
  // Which physical console plays this manual. Two keyboards both sending on
  // channel 1 is the ordinary case for anyone with more than one plugged in,
  // and the channel alone cannot separate them.
  std::vector<std::unique_ptr<juce::ComboBox>> keyboardDevices_;
  // Everything the two boxes cannot say: key range, transpose, velocity
  // window, tracker action, short octave, debounce.
  std::vector<std::unique_ptr<juce::TextButton>> keyboardMore_;
  // Which manuals share a channel, said out loud. Sharing one is allowed --
  // it is how a single keyboard plays two divisions -- but a player who did
  // it by accident would otherwise hear two divisions and not know why.
  juce::Label sharedNote_;
  void showSharedChannels();
  // The player's own console: these channels for every organ that has none
  // saved for it.
  juce::TextButton useAsDefault_{TRANS("Use as default for all organs")};
  juce::TextButton clearDefault_{TRANS("Clear default")};
  juce::Label defaultNote_;
  void showDefaultConsole();
  juce::ComboBox output_;
  juce::ToggleButton feedback_{TRANS("Send stop changes back to the console")};
  juce::ToggleButton cancelResets_{
      TRANS("General Cancel also puts the keyboards back on their own manuals")};
  juce::ToggleButton leaveHeld_{TRANS("Stop changes leave the notes already held alone")};
  juce::ToggleButton sustain_{TRANS("Sustain pedal: controller 64 holds the notes let go")};
  juce::ToggleButton noiseFilter_{TRANS("Noise filter: when learning, a key counts once held a quarter second")};
  juce::TextButton saveMap_{TRANS("Save mapping")};
  juce::TextButton clearMap_{TRANS("Clear mapping")};
  // The sequencer pistons have nothing on the console to right-click, because
  // the organ does not declare them — Hauptwerk provides the sequencer and the
  // player maps it. So they get their own learn buttons.
  juce::Label stepperLabel_;
  juce::TextButton learnNext_{TRANS("Learn sequencer +")};
  juce::TextButton learnPrev_{TRANS("Learn sequencer -")};
  // Pistons that switch which manual a keyboard plays (#90).
  juce::TextButton learnManuals_{TRANS("Learn manual buttons...")};
  // Console actions a real console's thumb pistons would do. A player whose
  // hands are on the keys cannot reach for a mouse to turn a page.
  juce::Label consoleHeading_;
  juce::Label keyboardsHeading_;
  juce::TextButton learnPageNext_{TRANS("Learn page +")};
  juce::TextButton learnPagePrev_{TRANS("Learn page -")};
  juce::TextButton learnLayout_{TRANS("Learn console size")};
  juce::TextButton learnStopList_{TRANS("Learn panel")};
  juce::TextButton learnKeyboard_{TRANS("Learn keyboard")};
  juce::TextButton learnCombinations_{TRANS("Learn combinations")};
  juce::TextButton learnTransposeDown_{TRANS("Learn transpose -")};
  juce::TextButton learnTransposeUp_{TRANS("Learn transpose +")};
  juce::TextButton learnTemperamentPrev_{TRANS("Learn temperament -")};
  juce::TextButton learnTemperamentNext_{TRANS("Learn temperament +")};
  // One temperament on a button of its own.
  juce::Label temperamentLabel_;
  juce::ComboBox temperamentPick_;
  std::vector<std::string> temperamentNames_;
  juce::TextButton learnTemperament_{TRANS("Learn")};
  // A console with no screen: buttons that quit the program or shut the
  // computer down, and a lamp that lights when the organ is ready.
  juce::Label powerHeading_;
  juce::TextButton learnQuit_{TRANS("Learn quit")};
  juce::TextButton learnPowerOff_{TRANS("Learn power off")};
  juce::Label lampLabel_;
  juce::ComboBox lampKind_, lampChannel_, lampNumber_;
  void pushReadyLamp();
  // Page turns for a score app on a tablet or another program: a piston
  // learned here sends the message chosen beside it on the MIDI output.
  juce::Label scoreHeading_;
  juce::TextButton learnScoreBack_{TRANS("Learn score page -")};
  juce::TextButton learnScoreForward_{TRANS("Learn score page +")};
  juce::ComboBox scoreKind_, scoreChannel_;
  juce::TextEditor scoreForward_, scoreBack_;
  void pushScorePage();
  juce::TextButton learnRecord_{TRANS("Learn record")};
  juce::TextButton learnPlay_{TRANS("Learn play")};
  juce::Label mapStatus_;
  juce::Label note_;
};

// Combination sets: the registration books of the organ loaded. The favourite
// organs that shared this tab are in Open (OpenWindow.h).
class FavouritesPanel : public juce::Component {
public:
  explicit FavouritesPanel(MasterpieceProcessor& p);
  void resized() override;
  void refresh();
  // Loads an organ the way Open does: its packages, the progress window, the
  // questions a load can ask. Without it Load started a bare background load
  // the window never heard of, and nothing seemed to happen (#90).
  std::function<void(const juce::File&)> onLoad;

private:
  MasterpieceProcessor& proc_;
  void refreshSets();

  juce::Label setsHeading_;
  juce::Label setLabel_;
  juce::ComboBox setBox_;
  juce::TextButton setNew_{TRANS("Save as new set...")};
  juce::TextButton setDelete_{TRANS("Delete set")};
  juce::Label setStatus_;
  std::vector<std::string> setNames_;
  std::unique_ptr<juce::AlertWindow> setPrompt_;
  juce::Label note_;
};

// Voicing: the player's own adjustments to a rank or a single pipe, the way a
// voicer goes round an organ with a knife and a tuning cone. Rank level here;
// the per-pipe rows are reached by picking a rank and a note.
class VoicingPanel : public juce::Component {
public:
  explicit VoicingPanel(MasterpieceProcessor& p);
  void resized() override;
  void refresh();

private:
  void pushCurrent();
  void loadCurrentIntoSliders();
  void updateStatus();

  MasterpieceProcessor& proc_;
  juce::Label heading_;
  juce::Label rankLabel_;
  juce::ComboBox rank_;
  juce::Label scopeLabel_;
  juce::ComboBox scope_;   // the whole rank, or one note of it
  juce::Label noteLabel_;
  juce::Slider note_{juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft};
  juce::Label gainLabel_, tuneLabel_;
  juce::Slider gain_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Slider tune_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::TextButton abSwap_{"A / B"};
  juce::TextButton abCopy_{TRANS("Copy to other")};
  juce::TextButton resetOne_{TRANS("Reset this")};
  juce::TextButton resetAll_{TRANS("Reset everything")};
  juce::TextButton save_{TRANS("Save for this organ")};
  juce::Label status_;
  juce::Label note2_;
  std::vector<Id> rankIds_;
};

// The mixer: how many output pairs the player has, and which one each rank
// speaks through. Its own tab because the rank list is long — a real organ has
// dozens — and it is the one settings page that needs to scroll.
class MixerPanel : public juce::Component {
public:
  explicit MixerPanel(MasterpieceProcessor& p);
  void resized() override;
  // Rebuild the rank rows. The rank list only exists once an organ is loaded,
  // so this cannot happen in the constructor.
  void refresh();

private:
  void pushRouting();
  void setBusCount(int buses);

  MasterpieceProcessor& proc_;
  juce::Label heading_;
  juce::Label busesLabel_;
  juce::ComboBox busCount_;
  juce::Label status_;
  juce::TextButton save_{TRANS("Save for this organ")};
  juce::TextButton spread_{TRANS("Spread ranks evenly")};
  juce::TextButton reset_{TRANS("All to pair 1")};
  // One row per rank, inside a viewport: 51 ranks does not fit a dialog.
  juce::Viewport viewport_;
  juce::Component rankHolder_;
  std::vector<Id> rankIds_;
  std::vector<std::unique_ptr<juce::Label>> rankLabels_;
  std::vector<std::unique_ptr<juce::ComboBox>> rankBuses_;
  juce::Label note_;
};

// The console's own text display: the little 32-character panel on the jamb
// that tells the player what the keys cannot. Its own tab rather than a corner
// of the MIDI page, because the framing bytes belong to the player's hardware
// and typing them in needs room to see what you are doing.
// Where the log is, and the two detailed logs a report may ask for, without
// the command line (#53: starting a program with flags is awkward on macOS).
// Both last until Masterpiece is closed.
// A phone or a tablet has no command line and no folder to open, so the tab
// also checks what a report needs and gives it as text to copy or send: the
// program and the device, each folder Masterpiece writes to, the audio, the
// organ, and every document the Files app has lent (#197).
juce::String diagnosticsReport(MasterpieceProcessor& p);

class LogPanel : public juce::Component {
public:
  explicit LogPanel(MasterpieceProcessor& p);
  void resized() override;

private:
  void runChecks();
  // The checks, then the end of the log.
  juce::String reportWithLog();

  MasterpieceProcessor& proc_;
  juce::Label about_;
  juce::TextButton open_{TRANS("Open the log folder")};
  juce::TextButton check_{TRANS("Run the checks")};
  juce::TextButton copy_{TRANS("Copy the report")};
  juce::TextButton send_{TRANS("Send the report and the log")};
  juce::TextEditor report_;
  juce::ScopedMessageBox sending_;
  juce::ToggleButton midi_{TRANS("Log every MIDI message and what became of it")};
  juce::ToggleButton releases_{TRANS("Log every key release: the release played, its length and start")};
  juce::ToggleButton updates_{TRANS("Check once a day for a newer version")};
};

// The control API and the web console it serves (src/mp_api): on or off, the
// port, whether the network may change anything, and a token.
class RemotePanel : public juce::Component, private juce::Timer {
public:
  explicit RemotePanel(MasterpieceProcessor& p);
  void resized() override;

private:
  void apply();
  void timerCallback() override;
  MasterpieceProcessor& proc_;
  juce::Label heading_, portLabel_, tokenLabel_, status_, note_, oscPortLabel_;
  juce::ToggleButton enable_{TRANS("Serve the web console and the control API on this network")};
  juce::ToggleButton write_{TRANS("Allow changes from the network")};
  juce::ToggleButton power_{TRANS("Allow quitting Masterpiece and shutting the computer down")};
  juce::ToggleButton local_{TRANS("This computer only")};
  juce::ToggleButton sysex_{TRANS("Answer the same requests over MIDI (System Exclusive)")};
  juce::ToggleButton osc_{TRANS("Answer the same requests over OSC")};
  juce::TextEditor port_, token_, oscPort_;
  juce::TextButton open_{TRANS("Open in the browser")};
};

class DisplayPanel : public juce::Component, private juce::Timer {
public:
  explicit DisplayPanel(MasterpieceProcessor& p);
  ~DisplayPanel() override;
  void resized() override;

private:
  void timerCallback() override;
  void rebuild();

  MasterpieceProcessor& proc_;
  juce::Label heading_;
  juce::ToggleButton enable_{TRANS("Drive a console display")};
  juce::Label idLabel_;
  juce::Slider id_{juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft};
  juce::Label widthLabel_;
  juce::ComboBox width_;
  juce::Label headerLabel_;
  juce::TextEditor header_;
  juce::Label linesLabel_;
  std::array<std::unique_ptr<juce::ComboBox>, 4> lines_;
  // What the hardware would read, shown before it is sent: the truncation is
  // the part a player needs to see.
  juce::Label previewLabel_;
  juce::Label preview_;
  juce::TextButton send_{TRANS("Send to display")};
  juce::Label note_;
};

// The language the program speaks. Applies at the next start: every window is
// built with its words already in place, and rebuilding the open ones to
// follow a change would lose where the player was.
class LanguagePanel : public juce::Component {
public:
  explicit LanguagePanel(MasterpieceProcessor& p);
  void resized() override;

private:
  MasterpieceProcessor& proc_;
  juce::Label heading_;
  juce::ComboBox language_;
  juce::Label note_;
  // The codes behind the list, by item id minus 3. Ids 1 and 2 are the system
  // default and English.
  std::vector<juce::String> codes_;
};

class SettingsWindow : public juce::Component {
public:
  // Without a device manager (a plugin) there are no Audio and MIDI tabs.
  SettingsWindow(MasterpieceProcessor& p, juce::AudioDeviceManager* devices);
  ~SettingsWindow() override;
  // Open on the tab of this name ("Audio"), when there is one.
  void showTab(const juce::String& name);
  void resized() override;

  // Run once the window has closed, on the message thread: an organ's first
  // load waits here for its engine settings.
  std::function<void()> onClosed;
  // For Favourites: load this organ as Open would.
  void setOrganLoader(std::function<void(const juce::File&)> load) { favourites_.onLoad = std::move(load); }
  void paint(juce::Graphics& g) override;

private:
  // General settings: the program and the player's console. What belongs to
  // the organ -- how it is loaded, which stops -- is OrganSettingsWindow.
  juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
  // The output device and MIDI inputs: the toolbar's Audio window, which is
  // set up once and seldom visited again (#276).
  std::unique_ptr<juce::Component> audio_;
  ReverbPanel reverb_;
  MetronomePanel metronome_;
  std::unique_ptr<MidiPanel> midi_;
  std::unique_ptr<ScrollHost> midiScroll_;  // after midi_: it goes first
  MixerPanel mixer_;
  VoicingPanel voicing_;
  FavouritesPanel favourites_;
  DisplayPanel display_;
  LanguagePanel language_;
  LogPanel log_;
  std::unique_ptr<RemotePanel> remote_;
};

} // namespace mp::ui
