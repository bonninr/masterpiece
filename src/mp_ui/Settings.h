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
class EnginePanel : public juce::Component, private juce::Timer {
public:
  explicit EnginePanel(MasterpieceProcessor& p);
  ~EnginePanel() override;
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  void timerCallback() override;
  void pushSwitches();
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
  juce::ToggleButton stream_{"Stream release tails from disk"};
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
  ReverbPanel reverb_;
  MetronomePanel metronome_;
  RecorderPanel recorder_;
  MidiPanel midi_;
  DisplayPanel display_;
};

} // namespace mp::ui
