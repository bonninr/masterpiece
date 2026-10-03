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
#include "CombinationsWindow.h"
#include "TuningPanel.h"
#include "Console.h"
#include "PageWindow.h"

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
  // Follow the engine: a stop drawn on the console, by a piston or from MIDI
  // shows here too. Called on the editor's timer.
  void refresh();
  // The height the grid needs at a given width: the columns follow the width.
  int heightFor(int width) const;
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
  // Follow the engine: a pedal moved over MIDI moves its slider too.
  void refresh();

private:
  // A slider that offers MIDI learn on a right-click.
  class Shoe : public juce::Slider {
  public:
    Shoe(MasterpieceProcessor& p, Id controlId)
        : juce::Slider(juce::Slider::LinearVertical, juce::Slider::NoTextBox),
          proc_(p), controlId_(controlId) {}
    Id controlId() const { return controlId_; }
    void mouseDown(const juce::MouseEvent& e) override;

  private:
    MasterpieceProcessor& proc_;
    Id controlId_;
  };
  MasterpieceProcessor& proc_;
  std::vector<std::unique_ptr<Shoe>> shoes_;
  std::vector<std::unique_ptr<juce::Label>> labels_;
};

// The Audio and MIDI dialog: the device selector, and under it what belongs
// to the output rather than to any organ -- keeping portable speakers awake.
class AudioSettingsPanel : public juce::Component {
public:
  AudioSettingsPanel(juce::AudioDeviceManager& devices, MasterpieceProcessor& p);
  void resized() override;

private:
  MasterpieceProcessor& proc_;
  juce::AudioDeviceSelectorComponent selector_;
  juce::ToggleButton keepAwake_{"Keep portable speakers awake"};
  juce::Slider level_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  void apply();
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
  // The status line lives along the bottom of the window, where it has the
  // whole width; the editor places it there.
  juce::Label& statusLabel() { return status_; }

private:
  MasterpieceProcessor& proc_;
  // Terse on purpose: everything the console offers has to share one row, and
  // a slider that reads out in dB does not also need a label saying "Volume".
  juce::TextButton load_{"Open"};
  juce::TextButton audio_{"Audio"};
  juce::ToggleButton simple_{"No DSP"};
  LevelMeter meter_;
  // In decibels, because that is the only scale a volume control feels linear
  // on: the useful part of a 0..16 gain range is all crowded below 1.
  juce::Slider volume_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label status_;
};

// The page tabs, with a right-click that offers the page a window of its own.
class PageTabs : public juce::TabbedButtonBar {
public:
  PageTabs() : juce::TabbedButtonBar(juce::TabbedButtonBar::TabsAtTop) {}
  std::function<void(int)> onPopup;
  void popupMenuClickOnTab(int tabIndex, const juce::String&) override {
    if (onPopup) onPopup(tabIndex);
  }
};

class MasterpieceEditor : public juce::AudioProcessorEditor,
                          private juce::Timer,
                          private juce::ChangeListener,
                          private juce::KeyListener {
public:
  explicit MasterpieceEditor(MasterpieceProcessor& p);
  MasterpieceProcessor& organProcessor() { return proc_; }
  ~MasterpieceEditor() override;

  void paint(juce::Graphics& g) override;
  void resized() override;

  // Load an organ and refresh every panel from the new model. `graphicsOnly`
  // draws the console without reading any audio — see
  // MasterpieceProcessor::loadOrgan.
  void loadOrgan(const juce::File& odf, bool graphicsOnly = false);
  // The load itself, past the first-time settings.
  void startLoad(const juce::File& odf, bool graphicsOnly);
  // Ask for an organ file and load it. Shared with the first-run wizard.
  // `startIn` is where the chooser opens; empty is wherever it last was.
  void chooseAndLoadOrgan(const juce::File& startIn = {});
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
  // Run once the load in progress has finished: a first load reads the
  // organ without its audio, then asks for Organ settings, then loads.
  std::function<void()> afterLoad_;

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
  // The organ's own settings: how it is loaded and which stops.
  std::function<void()> onOrganSettings;
  // Load the organ that is loaded now again, with whatever its settings say.
  void reloadOrgan();
  // Ask whether the player holds the publisher's licence for this organ.
  void askForLicence();
  // Several organs in one set of packages -- a perspective each, or full and
  // light: ask which, by name, the one opened last at the top.
  void chooseDefinition(const juce::Array<juce::File>& definitions, bool graphicsOnly);
  // Asked before an organ's first load, with the load to run afterwards.
  // Unset, the load simply starts.
  std::function<void(std::function<void()>)> onBeforeFirstLoad;

private:
  void timerCallback() override;
  void changeListenerCallback(juce::ChangeBroadcaster* source) override;
  // The computer-keyboard shortcuts set in an object's MIDI window. Listened
  // for on the top-level window, so a key reaches them whatever has focus,
  // unless that thing uses the key itself.
  bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
  void parentHierarchyChanged() override;
  juce::Component::SafePointer<juce::Component> keyWindow_;

  MasterpieceProcessor& proc_;
  TopBar top_;
  ConsoleView console_;
  juce::Viewport consoleView_;
  PageTabs pageTabs_;
  // Pages in windows of their own, to spread a console over several screens.
  std::vector<std::unique_ptr<PageWindow>> pageWindows_;
  // `layout` is the console layout the window shows, -1 for the main window's.
  void openPageWindow(int page, juce::Rectangle<int> bounds = {}, int layout = -1);
  // "main layout", "alternate layout 2 (portrait)": how a layout is offered.
  juce::String layoutName(int layout) const;
  void closePageWindow(PageWindow* window);
  PageWindow* pageWindowFor(int page) const;
  // Where they are now, handed to the processor to be saved with the organ.
  void rememberPageWindows();
  // A load replaces the model the windows draw from: they close first, and
  // come back where they were once the organ is in.
  void closePageWindowsForLoad();
  void restorePageWindows();
  bool pagesCanFloat() const;
  // The pop-out icon on each tab: filled while its page has a window.
  void addPopOutIcons();
  void refreshPopOutIcons();
  juce::TextButton toggleView_{"Stop list"};
  juce::TextButton settingsButton_{"Settings"};
  juce::TextButton keysButton_{"Keys"};
  juce::TextButton swellButton_{"Swell"};
  // What shows the tooltips the controls carry. Without one, none of them ever
  // appeared. On the desktop rather than in the editor, so the combinations
  // window's are shown too.
  juce::TooltipWindow tooltips_{nullptr, 700};
  // Opens and closes the combinations window: the player's own pistons, on
  // every organ, floating beside the console rather than drawn over it.
  juce::TextButton combinationsButton_{"Combinations"};
  std::unique_ptr<CombinationsWindow> combinations_;
  // The recorder, in a small window of its own above the console (#90): in
  // a settings dialog it took the console away while it recorded.
  std::unique_ptr<juce::DocumentWindow> recorderWindow_;
  void toggleRecorder();
  void toggleCombinations();
  // Temperament, pitch and transposer. Labelled with what is in force, so the
  // panel behind it only has to open to change something.
  juce::TextButton tuningButton_;
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
  // Late audio blocks already written to the log, when, and the slowest since.
  int64_t lateBlocksLogged_ = 0;
  juce::uint32 lateLoggedAt_ = 0;
  double worstBlockSinceLog_ = 0.0;
};

} // namespace mp::ui
