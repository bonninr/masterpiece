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
  explicit ScrollHost(juce::Component& c, int minHeight) : minHeight_(minHeight) {
    setViewedComponent(&c, false);
    setScrollBarsShown(true, false);
  }
  void resized() override {
    juce::Viewport::resized();
    if (auto* c = getViewedComponent())
      c->setSize(getMaximumVisibleWidth(),
                 juce::jmax(minHeight_, getMaximumVisibleHeight()));
  }

private:
  int minHeight_;
};

class EnginePanel : public juce::Component, private juce::Timer {
public:
  explicit EnginePanel(MasterpieceProcessor& p);
  ~EnginePanel() override;
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

  MasterpieceProcessor& proc_;

  // Settings here apply live, so leaving is not by itself a decision: the
  // player needs a way back that does not depend on remembering what was
  // there. Captured once, on open.
  EngineSwitch openSwitch_;
  int64_t openPreload_ = 0;
  SampleStorage openStorage_ = SampleStorage::Float32;
  bool openStream_ = false;
  bool openMono_ = false;
  double openRate_ = 0.0;
  SampleLibrary::CacheMode openCache_ = SampleLibrary::CacheMode::Single;
  // General config rather than per-organ state, but shown here because this
  // is the panel about loading. Reverted like the rest; each change writes
  // the global file at once.
  bool openLoadTicks_ = false;
  // Set while a profile is writing the individual controls, so their
  // onChange handlers do not bounce the profile straight back to Custom.
  bool applyingProfile_ = false;

  juce::TextButton revert_{"Revert changes"};
  juce::TextButton keep_{"Keep changes"};
  juce::TextButton saveOrgan_{"Save for this organ"};
  juce::TextButton saveGlobal_{"Save as default"};
  juce::ToggleButton simpleWav_{"Simple WAV only (bypass all DSP)"};
  juce::ToggleButton wind_{"Wind model"};
  juce::ToggleButton tremulant_{"Tremulants"};
  juce::ToggleButton enclosure_{"Enclosures (swell shades)"};
  juce::ToggleButton voicing_{"Voicing"};
  juce::ToggleButton originalPitch_{"Play at the original organ's pitch"};
  juce::Label preloadLabel_;
  juce::ComboBox preload_;
  juce::Label storageLabel_;
  juce::ComboBox storage_;
  juce::Label rateLabel_;
  juce::ComboBox rate_;
  juce::Label cacheLabel_;
  juce::ComboBox cache_;
  juce::Label cacheDirLabel_;
  juce::Label cacheDirValue_;
  juce::TextButton cacheDirChoose_{"Choose..."};
  juce::TextButton cacheDirDefault_{"Default"};
  std::unique_ptr<juce::FileChooser> cacheDirChooser_;
  void showCacheDir();
  juce::Label organRootLabel_;
  juce::Label organRootValue_;
  juce::TextButton organRootChoose_{"Choose..."};
  juce::TextButton organRootDefault_{"Default"};
  std::unique_ptr<juce::FileChooser> organRootChooser_;
  void showOrganRoot();
  juce::Label profileLabel_;
  juce::ComboBox profile_;
  juce::ToggleButton stream_{"Stream release tails from disk"};
  juce::ToggleButton mono_{"Load in mono (halves memory, gives up the stereo image)"};
  juce::ToggleButton loadTicks_{"Tap at each 10% while loading an organ"};
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
  juce::ToggleButton enabled_{"Impulse-response reverb"};
  juce::TextButton load_{"Load IR..."};
  juce::TextButton clear_{"Clear"};
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
  juce::ToggleButton enabled_{"Metronome"};
  juce::Label tempoLabel_, beatsLabel_, levelLabel_;
  juce::Slider tempo_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Slider beats_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Slider level_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label beat_;
};

// Session: MIDI recorder and player.
class RecorderPanel : public juce::Component, private juce::Timer {
public:
  explicit RecorderPanel(MasterpieceProcessor& p);
  ~RecorderPanel() override;
  void resized() override;

private:
  void timerCallback() override;

  MasterpieceProcessor& proc_;
  juce::TextButton record_{"Record"};
  juce::TextButton play_{"Play"};
  juce::TextButton stop_{"Stop"};
  juce::TextButton save_{"Save MIDI..."};
  juce::TextButton load_{"Load MIDI..."};
  juce::TextButton clear_{"Clear"};
  juce::Label status_;
  juce::Label note_;
  std::unique_ptr<juce::FileChooser> chooser_;

  // Audio capture is a separate recording from the MIDI one and they are
  // useful together: the MIDI file is the performance and can be replayed
  // through a different registration, the WAV is what it sounded like.
  juce::Label audioHeading_;
  juce::TextButton audioRecord_{"Record audio..."};
  juce::TextButton audioStop_{"Stop"};
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

private:
  void timerCallback() override;

  MasterpieceProcessor& proc_;
  juce::AudioDeviceManager& devices_;
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
  juce::ComboBox output_;
  juce::ToggleButton feedback_{"Send stop changes back to the console"};
  juce::TextButton saveMap_{"Save mapping"};
  juce::TextButton clearMap_{"Clear mapping"};
  // The sequencer pistons have nothing on the console to right-click, because
  // the organ does not declare them — Hauptwerk provides the sequencer and the
  // player maps it. So they get their own learn buttons.
  juce::Label stepperLabel_;
  juce::TextButton learnNext_{"Learn sequencer +"};
  juce::TextButton learnPrev_{"Learn sequencer -"};
  // Console actions a real console's thumb pistons would do. A player whose
  // hands are on the keys cannot reach for a mouse to turn a page.
  juce::Label consoleHeading_;
  juce::TextButton learnPageNext_{"Learn page +"};
  juce::TextButton learnPagePrev_{"Learn page -"};
  juce::TextButton learnLayout_{"Learn console size"};
  juce::TextButton learnStopList_{"Learn stop list"};
  juce::TextButton learnKeyboard_{"Learn keyboard"};
  juce::Label mapStatus_;
  juce::Label note_;
  std::unique_ptr<juce::MidiOutput> openedOutput_;
};

// Favourites: numbered slots for the organs a player actually uses, so getting
// back to one does not mean finding a 19 GB set in a file browser.
class FavouritesPanel : public juce::Component {
public:
  explicit FavouritesPanel(MasterpieceProcessor& p);
  void resized() override;
  void refresh();

private:
  MasterpieceProcessor& proc_;
  void refreshSets();

  juce::Label heading_;
  juce::TextButton addCurrent_{"Add the organ now loaded"};
  // Combination sets live here rather than on their own tab: a set IS a
  // favourite registration, and the two are reached at the same moment.
  juce::Label setsHeading_;
  juce::Label setLabel_;
  juce::ComboBox setBox_;
  juce::TextButton setNew_{"Save as new set..."};
  juce::TextButton setDelete_{"Delete set"};
  juce::Label setStatus_;
  std::vector<std::string> setNames_;
  std::unique_ptr<juce::AlertWindow> setPrompt_;
  juce::Label status_;
  juce::Viewport viewport_;
  juce::Component rows_;
  std::vector<int> slots_;
  std::vector<std::unique_ptr<juce::Label>> labels_;
  std::vector<std::unique_ptr<juce::TextButton>> loads_;
  std::vector<std::unique_ptr<juce::TextButton>> removes_;
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
  juce::TextButton abCopy_{"Copy to other"};
  juce::TextButton resetOne_{"Reset this"};
  juce::TextButton resetAll_{"Reset everything"};
  juce::TextButton save_{"Save for this organ"};
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
  juce::TextButton save_{"Save for this organ"};
  juce::TextButton spread_{"Spread ranks evenly"};
  juce::TextButton reset_{"All to bus 1"};
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
  juce::ToggleButton enable_{"Drive a console display"};
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
  juce::TextButton send_{"Send to display"};
  juce::Label note_;
};

class SettingsWindow : public juce::Component {
public:
  SettingsWindow(MasterpieceProcessor& p, juce::AudioDeviceManager& devices);
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
  EnginePanel engine_;
  // Tall enough for every row of the Engine tab; see ScrollHost.
  ScrollHost engineScroll_{engine_, 720};
  ReverbPanel reverb_;
  MetronomePanel metronome_;
  RecorderPanel recorder_;
  MidiPanel midi_;
  MixerPanel mixer_;
  VoicingPanel voicing_;
  FavouritesPanel favourites_;
  DisplayPanel display_;
};

} // namespace mp::ui
