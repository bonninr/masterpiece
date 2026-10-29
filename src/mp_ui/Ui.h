// The playable console.
//
// This is the working console, not the final one: a stop jamb grouped by
// division, expression shoes, a keyboard you can play with the mouse, and a
// load button. The full HW-parity console (DisplayPage/ImageSet artwork,
// alternate layouts, Touch Menu, MIDI learn) is M3/M4 and is specified in
// docs/screens/ — this exists so the organ can be played and heard now, which
// is what tells us whether the engine is right.
#pragma once
#include <array>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../mp_audio/MasterpieceProcessor.h"
#include "AccessibleToggle.h"
#include "CombinationsWindow.h"
#include "SplitKeyboardWindow.h"
#include "TuningPanel.h"
#include "Console.h"
#include "PageWindow.h"
#include "Panels.h"
#if MP_HAS_SCORE
#include "ScorePanel.h"
#endif
#include "Icons.h"
#include "UpdateCheck.h"

#include <memory>
#include <vector>

namespace mp::ui {

// One stop in the stop list: a tile a screen reader can read, whose state
// says whether the stop is drawn, and that the keyboard moves through.
//
// The tile carries the stop's own name; what a screen reader reads is the
// stop and the division it is in, "Principal 8, Great". The state is the
// stop's own: drawn or not.
class StopButton : public juce::TextButton, public AccessibleToggle {
public:
  explicit StopButton(const juce::String& text) : juce::TextButton(text) {}

  // What a screen reader reads: the stop's own name and the division it is
  // in.
  void setStopName(const juce::String& name, const juce::String& division);

  // Space and Enter draw and retire the stop, as a click does. The arrow
  // keys move through the list; the jamb tells the tile where the next stop
  // lies.
  bool keyPressed(const juce::KeyPress& key) override;
  std::function<void(StopButton&, const juce::KeyPress&)> onArrowKey;

  // A right-click opens the stop's MIDI menu and leaves the stop as it is.
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  std::function<void(StopButton&)> onMidiMenu;
  // A drag from this stop onto others: the jamb sets each one passed over.
  // True once the drag has left this stop and taken over from the click.
  std::function<bool(StopButton&, const juce::MouseEvent&)> onDragAcross;

  // What the accessibility layer reads: the stop's name, and whether it is
  // drawn.
  juce::String accessibleName() const override { return accessibleName_; }
  bool accessibleDrawn() const override { return getToggleState(); }
  void accessibleToggle() override;

  // The handler the accessibility layer talks to. The component hands this
  // out when the stop is on screen, which needs a window; this asks for it
  // without one.
  std::unique_ptr<juce::AccessibilityHandler> makeAccessibilityHandler() {
    return createAccessibilityHandler();
  }

protected:
  std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
  juce::String accessibleName_;
  bool draggedAcross_ = false;
};

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
  // Put the keyboard focus on the first stop: where a player who cannot see
  // the console starts reading the list.
  void focusFirstStop();
  // The stops in the order they are shown, for the keyboard and for the
  // tests.
  int stopCount() const { return static_cast<int>(entries_.size()); }
  StopButton* stopButtonAt(int index);

private:
  struct Entry {
    std::unique_ptr<StopButton> button;
    Id stopId = 0;
    // The switch a program coupler is. Zero for a stop.
    Id switchId = 0;
    Id divisionId = 0;
    bool playable = false;
    juce::String name;      // the stop's own name
    juce::String division;  // the division it is in
  };
  MasterpieceProcessor& proc_;
  std::vector<Entry> entries_;
  std::vector<std::unique_ptr<juce::Label>> headers_;
  // Move the keyboard focus from one stop to the next.
  void moveFocus(StopButton& from, const juce::KeyPress& key);
  // A drag across the list: the stop it began on and the state it sets
  // (1 drawn, 0 retired, -1 not yet off the first stop).
  bool dragAcross(StopButton& from, const juce::MouseEvent& e);
  StopButton* dragFrom_ = nullptr;
  juce::Time dragStart_;
  int dragState_ = -1;
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

// The system's Bluetooth MIDI pairing window, one at a time. On macOS each
// press of the selector's "Bluetooth MIDI" button opened another window (#565).
// openPairingOnce calls `open` only while no pairing window is open; `open`
// hands the callback to the window, which calls it when the window closes.
bool openPairingOnce(const std::function<bool(juce::ModalComponentManager::Callback*)>& open);
// Points the selector's "Bluetooth MIDI" button at openPairingOnce. False
// where the selector has no such button (no Bluetooth MIDI on the platform).
bool guardPairingButton(juce::AudioDeviceSelectorComponent& selector);

// The Audio and MIDI dialog: the device selector, and under it what belongs
// to the output rather than to any organ -- keeping portable speakers awake.
class AudioSettingsPanel : public juce::Component, private juce::ChangeListener {
public:
  AudioSettingsPanel(juce::AudioDeviceManager& devices, MasterpieceProcessor& p);
  ~AudioSettingsPanel() override;
  void resized() override;

private:
  // The delay from a key to the speaker as the open device has it: the
  // selector shows one buffer's length, which is only part of it.
  void changeListenerCallback(juce::ChangeBroadcaster*) override;
  void showLatency();
  MasterpieceProcessor& proc_;
  juce::AudioDeviceManager& devices_;
  juce::AudioDeviceSelectorComponent selector_;
  juce::Label latency_;
  juce::ToggleButton keepAwake_{TRANS("Keep portable speakers awake")};
  juce::Slider level_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  // The peak limiter on the output: the switch, the ceiling it holds the
  // signal under and the rate the gain comes back at.
  juce::ToggleButton limiterOn_{TRANS("Output limiter")};
  juce::Label ceilingLabel_;
  juce::Slider ceiling_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label releaseLabel_;
  juce::Slider release_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  void apply();
  // The limiter's two settings are only in use while it is on.
  void updateLimiterControls();
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
  int darkTicks_ = 0;
};

class TopBar : public juce::Component {
public:
  using Callback = std::function<void()>;
  explicit TopBar(MasterpieceProcessor& p);
  void resized() override;
  void setStatus(const juce::String& text);
  // The status line lives along the bottom of the window, where it has the
  // whole width; the editor places it there.
  juce::Label& statusLabel() { return status_; }
  // The fader at its narrowest: its 58-pixel readout and a track a hand can
  // still set a level on.
  static constexpr int kFaderMinWidth = 160;
  // The fader at its narrowest, with the bar's margins. Bypassing every
  // effect is set once, in Organ settings > Loading, and has no place here.
  static constexpr int kMinWidth = 8 + 6 + kFaderMinWidth;

private:
  MasterpieceProcessor& proc_;
  LevelMeter meter_;
  // In decibels, because that is the only scale a volume control feels linear
  // on: the useful part of a 0..16 gain range is all crowded below 1.
  juce::Slider volume_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label status_;
};

// The band along the top of the window: the menus, the volume bar, the
// sequencer, the tuning button, save and panic. Worked out
// from the widths alone, so a test can lay it out in every language with no
// window.
constexpr int kTopBandHeight = 36;
constexpr int kTuningWidth = 140;
constexpr int kFrameWidth = 96;
// Around a menu's name: the button's 2 px margin and the text's own inset
// on each side, with a little air.
constexpr int kMenuPadding = 20;
// The widest a menu button grows. A longer translated name is drawn
// narrower to fit, and past that shortened with an ellipsis: the row holds
// whatever length a language gives a word.
constexpr int kMenuMaxWidth = 88;

struct TopBandLayout {
  std::array<juce::Rectangle<int>, 5> menus;  // File, Organ, View, Settings, Help
  juce::Rectangle<int> bar;                   // the volume and the meter
  juce::Rectangle<int> setter, stepPrev, stepFrame, stepNext, tuning, save, panic;
  bool stacked = false;  // the menus and the bar took a row of their own
};

// The width of a button's text, in the font it is drawn in at the band's
// height.
int textButtonTextWidth(juce::TextButton& b);

// Takes the band's row, or two rows on a narrow window, from the top of
// `area`. `menuText`: the five menu names' widths.
TopBandLayout layoutTopBand(juce::Rectangle<int>& area, const std::array<int, 5>& menuText, bool mobile);

// The width a page tab gives the icon that opens the page in a window of its
// own. A finger needs a wider target than a mouse: on phones and tablets the
// icon takes 44 pixels of the tab and is drawn at the same size as elsewhere.
inline int popOutIconWidth(bool touch) { return touch ? 44 : 16; }

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
  // "Masterpiece 0.8.1 is out", at the right of the status bar for 30 s.
  void showUpdateNotice(const juce::String& latest);
  // The load itself, past the first-time settings.
  void startLoad(const juce::File& odf, bool graphicsOnly);
  // Ask for an organ file and load it. Shared with the first-run wizard.
  // `startIn` is where the chooser opens; empty is wherever it last was.
  void chooseAndLoadOrgan(const juce::File& startIn = {});
  // Open: the favourite organs first, then the file dialog (#250). With no
  // favourites it goes straight to the dialog.
  void showOpenMenu();
#if JUCE_IOS
  // A package that is one volume of a set: asks for the folder holding the
  // set, holds it, and opens the package again.
  void askForFolderOf(const juce::File& package, bool graphicsOnly);
#endif
  // Show one of the organ's console pages, counting from 1. A set with jambs
  // on their own pages cannot be photographed from a script otherwise, and
  // this is also what --console-page drives.
  void showConsolePage(int oneBased);

private:
  void showLoadingDialog(const juce::File& file);
  // The message-thread half of a load, run once the loader thread is done.
  void finishLoad(const juce::File& odf, bool graphicsOnly,
                  const MasterpieceProcessor::LoadResult& result);
  // Non-owning: the dialog window owns itself once launched async, and this
  // is how it gets closed when the load ends. A close from the system
  // (Alt+F4) deletes it sooner, and this then reads null.
  juce::Component::SafePointer<juce::DialogWindow> loadWindow_;
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
  // Supplied by the application, which owns the device manager the settings
  // panel needs.
  std::function<void()> onSettings;
  // The organ's own settings: how it is loaded and which stops.
  std::function<void()> onOrganSettings;
  // After Panic has silenced the notes: the application opens the MIDI inputs
  // again, for a keyboard that stopped sending (#637).
  std::function<void()> onPanic;
  // Settings > a section: the settings window, open on the tab of that name.
  std::function<void(const juce::String& tab)> onSettingsTab;
  // Load the organ that is loaded now again, with whatever its settings say.
  void reloadOrgan();
  // File > Import and Export, Organ > Copy settings (SettingsBackup.cpp): the
  // organ's settings or all of them to one file and back, and parts of another
  // organ's copied to this one.
  void exportSettings(bool all);
  void importSettings();
  void copySettingsFrom();
  // After settings were written behind the program's back: read them in.
  void adoptRestoredSettings(bool loadedOrganTouched, bool globalTouched);
  std::unique_ptr<juce::FileChooser> backupChooser_;
  // Ask whether the player holds the publisher's licence for this organ.
  void askForLicence();
  // The question itself; `answered` gets the player's answer.
  void askForPackage(const juce::String& package, const juce::String& name, const juce::File& start,
                     std::function<void(const juce::String&)> answered);
  std::unique_ptr<juce::FileChooser> packageChooser_;
  void askLicenceQuestion(const juce::String& organ, const juce::String& who,
                          std::function<void(bool)> answered);
  // Several organs in one set of packages -- a perspective each, or full and
  // light: ask which, by name, the one opened last at the top.
  void chooseDefinition(const juce::Array<juce::File>& definitions, bool graphicsOnly);
  // Asked before an organ's first load, with the load to run afterwards.
  // Unset, the load simply starts.
  std::function<void(std::function<void()>)> onBeforeFirstLoad;

private:
  void timerCallback() override;
  void changeListenerCallback(juce::ChangeBroadcaster* source) override;
  // When the background cache save was first seen, and how far along it was:
  // the rate its time-left estimate is taken from.
  double cacheSaveStartMs_ = 0.0;
  double cacheSaveStartFraction_ = 0.0;
  // The computer-keyboard shortcuts set in an object's MIDI window. Listened
  // for on the top-level window, so a key reaches them whatever has focus,
  // unless that thing uses the key itself.
  bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
  void parentHierarchyChanged() override;
  juce::Component::SafePointer<juce::Component> keyWindow_;

  MasterpieceProcessor& proc_;
  TopBar top_;
  UpdateNotice updateNotice_{proc_};
  ConsoleView console_;
  juce::Viewport consoleView_;
  // The room the console page is given in the window, for "Fit window to page".
  juce::Rectangle<int> consoleArea_;
  PageTabs pageTabs_;
  // Pages in windows of their own, to spread a console over several screens.
  std::vector<std::unique_ptr<PageWindow>> pageWindows_;
  // `layout` is the console layout the window shows, -1 for the main window's.
  void openPageWindow(int page, juce::Rectangle<int> bounds = {}, int layout = -1,
                        bool borderless = false);
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
  // Three menus at the left, named in words (#276): File for organs and
  // recordings, View for what the window shows, Settings for the windows that
  // change how things work.
  juce::TextButton fileMenu_{TRANS("File")};
  juce::TextButton organMenu_{TRANS("Organ")};
  juce::TextButton viewMenu_{TRANS("View")};
  juce::TextButton settingsButton_{TRANS("Settings")};
  juce::TextButton helpMenu_{TRANS("Help")};
  void showFileMenu();
  void showOrganMenu();
  void showViewMenu();
  void showHelpMenu();
  void checkForUpdatesNow();
  void showAbout();
  // Size the window so the console's page fills it, with no margin.
  void fitWindowToPage();
  void showSettingsMenu();
  void toggleKeyboard();
  void toggleSwell();
  // The plain stop list, for a player who cannot see the console: every stop
  // by division, as buttons a screen reader can read. The key that opens it
  // is named in the View menu.
  void toggleStopList();
  // View > Program couplers: turns them on or off for this organ and loads it
  // again; turned on, the stop list opens on them.
  void toggleProgramCouplers();
  // What shows the tooltips the controls carry. Without one, none of them ever
  // appeared. On the desktop rather than in the editor, so the combinations
  // window's are shown too.
  juce::TooltipWindow tooltips_{nullptr, 700};
  // Opens and closes the combinations window: the player's own pistons, on
  // every organ, floating beside the console rather than drawn over it.

  std::unique_ptr<CombinationsWindow> combinations_;
  // Panels (#237): up to two per organ, each a tab after the console pages or
  // a window of its own. panelWindows_ runs beside panels_, null while that
  // panel is a tab; panelTabs_ maps the tabs after the pages to panels.
  std::vector<std::unique_ptr<PanelView>> panels_;
  std::vector<std::unique_ptr<PanelWindow>> panelWindows_;
  std::vector<int> panelTabs_;
  int showingPanel_ = -1;
#if MP_HAS_SCORE
  // The score panel: made when it is first opened from the View menu, then
  // kept for the session, a tab after the panels' or a window of its own.
  // Never a window on phones and tablets. The window is declared after the
  // panel so it goes first.
  std::unique_ptr<ScorePanel> score_;
  std::unique_ptr<juce::DocumentWindow> scoreWindow_;
  juce::Rectangle<int> scoreWindowBounds_;
  bool showingScore_ = false;
  int scoreTab_ = -1;  // its tab, -1 while it has none
  void showScore();
  void setScoreDetached(bool detached);
#endif

  void loadPanels();
  void savePanels();
  void addPanel();
  void deletePanel(int index);
  // A panel's callbacks, for the place it has now.
  void wirePanel(int index);
  void setPanelDetached(int index, bool detached);
  void rebuildPageTabs();
  // The panels part of the View menu: each panel, the console, and new,
  // rename and delete. Item ids from 1000.
  void addPanelItems(juce::PopupMenu& menu);
  void panelItemChosen(int id);
  // A panel on screen: its tab, or its window brought to the front.
  void showPanel(int index);
  // The first panel and back to the console; a panel made if there is none.
  void togglePanel();
  // The recorder, in a small window of its own above the console (#90): in
  // a settings dialog it took the console away while it recorded.
  std::unique_ptr<juce::DocumentWindow> recorderWindow_;
  void toggleRecorder();
  // One keyboard playing the whole organ (#358).
  void toggleSplitKeyboard();
  std::unique_ptr<SplitKeyboardWindow> splitWindow_;
  void toggleCombinations();
  // Temperament, pitch and transposer. Labelled with what is in force, so the
  // panel behind it only has to open to change something.
  juce::TextButton tuningButton_;
  // Releases every key. An organ pipe does not decay, so one stuck note goes
  // on sounding until something stops it -- and the usual causes (a coupler
  // changed mid-chord, a MIDI note-off lost on the cable) leave the player
  // with no key to lift.
  IconButton panicButton_{Icon::Panic, TRANS("Panic"), TRANS("Panic: silence every note still sounding"),
                          juce::Colour(0xffe8a09a)};
  // Records the stops as they are now as the organ's starting state.
  IconButton saveStateButton_{Icon::Save, TRANS("Save state"),
                              TRANS("Save the stops as they are now: the organ starts with them"),
                              juce::Colour(0xffa8d8a0)};
  // The registration sequencer. Two thumb pistons and a frame number, which is
  // all an organist wants from it: the point of a sequencer is that you press
  // one button without looking. Also mappable to a real console's pistons —
  // see Settings -> MIDI.
  // The names of the console's layouts, for View > Console layout: shown
  // when the organ declares more than one, which most do not.
  juce::StringArray layoutNames_;
  // Draws the console at layout `index`, as a choice the player made: it
  // holds until the window's shape turns.
  void chooseLayout(int index);
  // Whether the console last had a tall room (1), a wide one (0), or no
  // shape yet (-1): the layout is chosen again only when this turns.
  int shapeTall_ = -1;
  // Which manual the on-screen keyboard plays. It used to be hardcoded to
  // channel 1, which on an organ with separated key flow is the PEDAL — so on
  // Nancy the keys played the pedal division and the manuals were unreachable,
  // because her manuals are part of the backdrop photo and have no drawn keys
  // to click.
  juce::ComboBox manual_;
  juce::TextButton stepPrev_{"<"};
  juce::TextButton stepNext_{">"};
  // Momentary (#276): stores what is drawn into the piston or step chosen
  // last. Lit while capture is on from elsewhere -- the console's own Set,
  // the combinations window -- and then a press lets capture go.
  juce::TextButton setter_{TRANS("Set")};
  FrameLabel stepFrame_;
  // The console already shows the organ's own manuals; a second giant
  // keyboard underneath is duplicate furniture, so it is off by default and
  // there for a machine with no MIDI console attached.
  bool showingKeyboard_ = false;
  bool showingConsole_ = true;
  // The plain stop list is on screen, over the console. An organ that ships
  // no console artwork shows it anyway, and then this is never set.
  bool showingStopList_ = false;
  // Set by View > Program couplers when it turns them on, for the load it
  // starts: the stop list then opens on them.
  bool showCouplersAfterLoad_ = false;
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
  bool realtimeRefusalLogged_ = false;
};

} // namespace mp::ui
