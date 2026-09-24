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

  // How many enclosures this organ actually has. An unenclosed organ should
  // not be offered a swell control at all, rather than an empty strip.
  int shoeCount() const { return static_cast<int>(shoes_.size()); }

private:
  MasterpieceProcessor& proc_;
  std::vector<std::unique_ptr<juce::Slider>> shoes_;
  std::vector<std::unique_ptr<juce::Label>> labels_;
};

// Organ name, load button, audio settings, and what the load actually found.
// A row of lamps per channel, lit up to the current level. Discrete segments
// rather than a continuous bar because the eye reads a count at a glance and
// has to measure a length; on an instrument whose loud registrations sit
// pinned near the top, "how many lamps" is the useful question.
class LevelMeter : public juce::Component, private juce::Timer {
public:
  explicit LevelMeter(MasterpieceProcessor& p);
  ~LevelMeter() override;
  void paint(juce::Graphics& g) override;

private:
  void timerCallback() override;
  MasterpieceProcessor& proc_;
  // What was last painted, so a still meter does not repaint 30 times a
  // second behind a console that is doing real work.
  int lit_[2] = {-1, -1};
};

class TopBar : public juce::Component {
public:
  using Callback = std::function<void()>;
  TopBar(MasterpieceProcessor& p, Callback onLoad, Callback onAudioSettings);
  void resized() override;
  void setStatus(const juce::String& text);

private:
  MasterpieceProcessor& proc_;
  // Terse on purpose: everything the console offers has to share one row, and
  // a slider that reads out in dB does not also need a label saying "Volume".
  juce::TextButton load_{"Organs"};
  juce::TextButton audio_{"Audio"};
  juce::ToggleButton simple_{"No DSP"};
  LevelMeter meter_;
  // In decibels, because that is the only scale a volume control feels linear
  // on: the useful part of a 0..16 gain range is all crowded below 1.
  juce::Slider volume_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
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
  void unloadOrgan();
  // Show the Organs dialog (installed organs list, ODF picker, package installer).
  void showOrganDialog();
  // Ask for an organ file and load it. Shared with the first-run wizard.
  void chooseAndLoadOrgan();
  // Show one of the organ's console pages, counting from 1. A set with jambs
  // on their own pages cannot be photographed from a script otherwise, and
  // this is also what --console-page drives.
  void showConsolePage(int oneBased);

private:
  // The message-thread half of a load, run once the loader thread is done.
  void finishLoad(const juce::File& odf, bool graphicsOnly,
                  const MasterpieceProcessor::LoadResult& result);
  // Non-owning: the dialog window owns itself once launched async, and this
  // is how it gets closed when the load ends.
  juce::DialogWindow* loadWindow_ = nullptr;
  bool loading_ = false;

public:
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
  juce::TextButton swellButton_{"Swell"};
  // Releases every key. An organ pipe does not decay, so one stuck note goes
  // on sounding until something stops it -- and the usual causes (a coupler
  // changed mid-chord, a MIDI note-off lost on the cable) leave the player
  // with no key to lift.
  juce::TextButton panicButton_{"Panic"};
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
  // The expression shoes as a strip of faders down the side. Off by default:
  // it took 120px of console width permanently, and on a set that draws its
  // own shoes it was showing the same control twice. Kept because a set whose
  // shoes are not drawn has no other way to work them with a mouse.
  bool showingSwell_ = false;
  StopJamb jamb_;
  ExpressionBar expression_;
  juce::MidiKeyboardComponent keyboard_;
  juce::Viewport jambView_;
  std::unique_ptr<juce::FileChooser> chooser_;
  juce::String status_;
};

} // namespace mp::ui
