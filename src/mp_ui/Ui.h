// The playable console.
//
// This is the working console, not the final one: a stop jamb grouped by
// division, expression shoes, a keyboard you can play with the mouse, and a
// load button. The full HW-parity console (DisplayPage/ImageSet artwork,
// alternate layouts, Touch Menu, MIDI learn) is M3/M4 and is specified in
// docs/screens/ — this exists so the organ can be played and heard now, which
// is what tells us whether the engine is right.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../mp_audio/MasterpieceProcessor.h"
#include "Console.h"

#include <memory>
#include <vector>

namespace mp::ui {

// One division's stops, as a column of latching buttons. A stop whose ranks
// ship no pipes is drawn dimmed and cannot be drawn: demo sets are full of
// them, and a jamb that hides that is lying to the player.
class StopJamb : public juce::Component {
public:
  explicit StopJamb(MasterpieceProcessor& p);
  void rebuild();
  void resized() override;
  void paint(juce::Graphics& g) override;

private:
  struct Entry {
    std::unique_ptr<juce::TextButton> button;
    Id stopId = 0;
    Id divisionId = 0;
    bool playable = false;
  };
  MasterpieceProcessor& proc_;
  std::vector<Entry> entries_;
  std::vector<std::unique_ptr<juce::Label>> headers_;
};

// One shoe per enclosure, plus whatever else the organ exposes as a
// continuous control that an enclosure actually uses.
class ExpressionBar : public juce::Component {
public:
  explicit ExpressionBar(MasterpieceProcessor& p);
  void rebuild();
  void resized() override;

private:
  MasterpieceProcessor& proc_;
  std::vector<std::unique_ptr<juce::Slider>> shoes_;
  std::vector<std::unique_ptr<juce::Label>> labels_;
};

// Organ name, load button, audio settings, and what the load actually found.
class TopBar : public juce::Component {
public:
  using Callback = std::function<void()>;
  TopBar(MasterpieceProcessor& p, Callback onLoad, Callback onAudioSettings);
  void resized() override;
  void setStatus(const juce::String& text);

private:
  MasterpieceProcessor& proc_;
  juce::TextButton load_{"Load organ..."};
  juce::TextButton audio_{"Audio/MIDI..."};
  juce::ToggleButton simple_{"Simple (no DSP)"};
  // In decibels, because that is the only scale a volume control feels linear
  // on: the useful part of a 0..16 gain range is all crowded below 1.
  juce::Slider volume_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label volumeLabel_;
  juce::Label status_;
};

class MasterpieceEditor : public juce::AudioProcessorEditor,
                          private juce::Timer,
                          private juce::ChangeListener {
public:
  explicit MasterpieceEditor(MasterpieceProcessor& p);
  ~MasterpieceEditor() override;

  void paint(juce::Graphics& g) override;
  void resized() override;

  // Load an organ and refresh every panel from the new model. `graphicsOnly`
  // draws the console without reading any audio — see
  // MasterpieceProcessor::loadOrgan.
  void loadOrgan(const juce::File& odf, bool graphicsOnly = false);
  // Fired once an organ is on screen, with its name. The host puts it in the
  // window title, which is the only load-progress signal visible from outside
  // the process — the status bar cannot be read, and a fixed wait is a guess
  // that a slow disk turns into a screenshot of a half-built console.
  std::function<void(const juce::String&)> onOrganLoaded;
  // Supplied by the host application, which owns the device manager; the
  // editor must not reach for hardware itself.
  std::function<void()> onAudioSettings;
  // Supplied by the application, which owns the device manager the settings
  // panel needs.
  std::function<void()> onSettings;

private:
  void timerCallback() override;
  void changeListenerCallback(juce::ChangeBroadcaster* source) override;

  MasterpieceProcessor& proc_;
  TopBar top_;
  ConsoleView console_;
  juce::Viewport consoleView_;
  juce::TabbedButtonBar pageTabs_{juce::TabbedButtonBar::TabsAtTop};
  juce::TextButton toggleView_{"Stop list"};
  juce::TextButton settingsButton_{"Settings"};
  juce::TextButton keysButton_{"Keys"};
  // The registration sequencer. Two thumb pistons and a frame number, which is
  // all an organist wants from it: the point of a sequencer is that you press
  // one button without looking. Also mappable to a real console's pistons —
  // see Settings -> MIDI.
  // Which console layout the set is drawn at. Only shown when the organ
  // declares more than one, which most do not.
  juce::ComboBox layout_;
  // Which manual the on-screen keyboard plays. It used to be hardcoded to
  // channel 1, which on an organ with separated key flow is the PEDAL — so on
  // Nancy the keys played the pedal division and the manuals were unreachable,
  // because her manuals are part of the backdrop photo and have no drawn keys
  // to click.
  juce::ComboBox manual_;
  juce::TextButton stepPrev_{"<"};
  juce::TextButton stepNext_{">"};
  juce::ToggleButton setter_{"Set"};
  juce::Label stepFrame_;
  // The console already shows the organ's own manuals; a second giant
  // keyboard underneath is duplicate furniture, so it is off by default and
  // there for a machine with no MIDI console attached.
  bool showingKeyboard_ = false;
  bool showingConsole_ = true;
  StopJamb jamb_;
  ExpressionBar expression_;
  juce::MidiKeyboardComponent keyboard_;
  juce::Viewport jambView_;
  std::unique_ptr<juce::FileChooser> chooser_;
  juce::String status_;
};

} // namespace mp::ui
