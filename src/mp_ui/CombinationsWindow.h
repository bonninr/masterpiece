// Everything to do with combinations, in one window the player can close.
//
// The organ's drawn console and the stop list stay exactly as they are; this
// floats beside them. It holds the player's own pistons (PlayerCombinations):
// the setter, the general cancel, the generals, the stepper, a row of
// divisionals per division, and the combination set in use. Every piston can
// be mapped to MIDI from its right-click menu, so on a real console none of
// this needs to be on screen at all.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../mp_audio/MasterpieceProcessor.h"

#include <functional>
#include <memory>
#include <vector>

namespace mp::ui {

// A button that also answers a right-click, which is where its MIDI mapping
// lives.
class PistonButton : public juce::TextButton {
public:
  using juce::TextButton::TextButton;
  std::function<void()> onRightClick;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
};

// The generals' and divisionals' text, grown with View > Piston size: the
// default look stops at 16 px however large the piston (mdehn38, #1221).
class PistonLook : public juce::LookAndFeel_V4 {
public:
  int percent = 100;
  juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override {
    const float most = 16.0f * static_cast<float>(percent) / 100.0f;
    return juce::Font(juce::FontOptions(juce::jmin(most, static_cast<float>(buttonHeight) * 0.6f)));
  }
};

// The sequencer's frame readout: a click on it asks for the frame to go to.
class FrameLabel : public juce::Label {
public:
  std::function<void()> onClick;
  // The number alone, as large as the readout is high, amber on black, to
  // read from the bench (fabiomancini67, #1271).
  bool big = false;
  void paint(juce::Graphics& g) override {
    if (!big) {
      juce::Label::paint(g);
      return;
    }
    const auto area = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0b0c0e));
    g.fillRoundedRectangle(area, 4.0f);
    g.setColour(juce::Colour(0xffffb340));
    g.setFont(juce::Font(juce::FontOptions(area.getHeight() * 0.86f, juce::Font::bold)));
    g.drawFittedText(getText(), getLocalBounds().reduced(3, 0), juce::Justification::centred, 1, 0.6f);
  }
  void mouseUp(const juce::MouseEvent& e) override {
    // Released on the readout is a click (#1047), held a moment or with the
    // pointer moved a few pixels, as a touchpad click moves it.
    if (onClick && getLocalBounds().contains(e.getPosition())) onClick();
  }
};

// Asks for a frame number and goes there (#298). A number past the last
// frame holding anything goes to that last frame, unless the setter is on.
void askForFrame(MasterpieceProcessor& p);

class CombinationsPanel : public juce::Component, private juce::Timer {
public:
  explicit CombinationsPanel(MasterpieceProcessor& p);
  ~CombinationsPanel() override;

  // The pistons are the organ's: its divisions, and how many of each the
  // player keeps. Called after a load and whenever a count changes.
  void rebuild();
  // The height the content wants at a given width, for the window to size to.
  int preferredHeight(int width) const;
  // The width every row needs to be seen whole: the steps, ten generals a
  // line, a division's pistons and its cancel.
  int preferredWidth() const;
  // After the rows change, a count or the view: the window widens to them.
  std::function<void()> onWidthChanged;
  // What the processor holds now, and the display mode: on the panel's timer.
  void refresh() { refreshState(); }
  // After the window's choice of what to show has changed.
  void showView() { applyView(); }
  // Sized again to its rows, after a name or the piston size changed; the
  // window follows.
  void relayout();

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override;
  void refreshSets();
  void refreshState();
  // Simple or Advanced, as the display mode is (#692). Simple shows the
  // sequencer as the generals: Set, GC, the ten generals, - and + through
  // them and -10 and +10 through their levels. Advanced shows the frames,
  // Insert and Delete, and the player's generals as well.
  void applyView();
  bool sequencerShown() const { return proc_.combinationsView() != 1; }
  bool combinationsShown() const { return proc_.combinationsView() != 2; }
  // Right-click: what a piston is mapped to, and how to map it.
  void showMidiMenu(PistonButton& b, MidiTargetKind kind, Id targetId,
                    bool offerHold);

  MasterpieceProcessor& proc_;
  // Before every piston, so it outlives them.
  PistonLook pistonLook_;

  PistonButton setter_{TRANS("Set")};
  PistonButton generalCancel_{TRANS("GC")};
  PistonButton stepPrev_{"<"};
  PistonButton stepNext_{">"};
  PistonButton stepFirst_{"|<"};
  PistonButton stepLast_{">|"};
  PistonButton stepBack10_{"-10"};
  // Frames 1 to 9 and 10 (the 0 key), in a row under the sequencer's own.
  std::array<std::unique_ptr<PistonButton>, 10> digits_;
  PistonButton stepForward10_{"+10"};
  FrameLabel frame_;
  // The level of ten generals in use, in the Simple view.
  juce::Label level_;
  bool simple_ = false;
  // The stepper as levels of generals (#1246): always in the simple view, and
  // by the style box in the other.
  bool levels_ = false;
  static constexpr int kStyleW = 170;
  juce::ComboBox style_;
  juce::TextButton fewerLevel_{"-"}, moreLevel_{"+"};
  juce::Label levelSize_;
  juce::TextButton insertFrame_{TRANS("Insert")};
  juce::TextButton deleteFrame_{TRANS("Delete")};

  juce::Label generalsLabel_;
  juce::TextButton fewerGenerals_{"-"};
  juce::TextButton moreGenerals_{"+"};
  // Ready-made registrations from the stop names (#1221).
  juce::TextButton readyMade_{TRANS("Ready-made...")};
  std::vector<std::unique_ptr<PistonButton>> generals_;

  juce::Label divisionalsLabel_;
  juce::TextButton fewerDivisionals_{"-"};
  juce::TextButton moreDivisionals_{"+"};
  // "Pedal combinations coupled": the chosen manual's divisionals take the
  // Pedal's of the same number with them. Shown when the organ has both.
  PistonButton pedalCoupler_;
  juce::ComboBox pedalManual_;
  std::vector<Id> pedalManuals_;
  struct DivisionRow {
    Id divisionId = 0;
    std::unique_ptr<juce::Label> name;
    std::vector<std::unique_ptr<PistonButton>> pistons;
    std::unique_ptr<PistonButton> cancel;
  };
  std::vector<DivisionRow> divisions_;

  juce::Label setLabel_;
  juce::ComboBox setBox_;
  juce::TextButton setNew_{TRANS("Save as...")};
  juce::TextButton setDelete_{TRANS("Delete")};
  std::vector<std::string> setNames_;
  std::unique_ptr<juce::AlertWindow> setPrompt_;
  // A general's name (#1214): asked for from its menu, and shown on the
  // piston in place of its number, the number in the tooltip.
  std::unique_ptr<juce::AlertWindow> namePrompt_;
  void nameGeneral(int n);
  void labelGeneral(juce::Button& b, int n) const;
  // The generals' pistons: wide, on two lines, once any is named (#1248).
  bool namedGenerals() const;
  int generalW() const;
  int generalH() const;
  int generalsPerRow() const;
  int generalRows(int count) const;
  // Divisionals named the same way (JPIL04, #1221): once any has a name, all
  // their pistons are as wide as named generals, five to a line.
  void nameDivisional(Id divisionId, int n);
  void labelDivisional(juce::Button& b, Id divisionId, int n) const;
  // The pistons' size from View > Piston size: 1, 1.4 or 1.8 times.
  int scaled(int px) const;
  int divisionalW() const;
  int divisionalH() const;
  int divisionalsPerRow() const;
  int divisionalRows() const;

  juce::Label note_;
};

class CombinationsWindow : public juce::DocumentWindow {
public:
  explicit CombinationsWindow(MasterpieceProcessor& p);
  void closeButtonPressed() override;
  void moved() override;
  void resized() override;

  CombinationsPanel& panel() { return *panel_; }
  // Shown or hidden, remembered per organ.
  void showOrHide(bool show);
  // Back where the player left it on this organ. The first time, beside the
  // main window, and open only when the organ has no pistons of its own.
  void place(juce::Rectangle<int> besideThis, bool openFirstTime);

  // Above every window: always when the player asks, otherwise while this
  // program is in front.
  bool onTopNow() const {
    return proc_.combinationsOnTop() || juce::Process::isForegroundProcess();
  }

private:
  // The three boxes above the pistons, from what the processor holds now.
  void refreshToggles();
  void remember();
  // The width the boxes and the pistons need, as far as the screen allows;
  // a window narrower than that is widened, and kept at least that wide.
  int neededWidth() const;
  void widenToFit();
  MasterpieceProcessor& proc_;
  // The checkbox above the pistons, then the pistons.
  struct Content : juce::Component {
    std::function<void()> layout;
    void resized() override {
      if (layout) layout();
    }
  } content_;
  juce::ToggleButton onTop_{TRANS("Always on top")};
  juce::ToggleButton setOff_{TRANS("Set turns off after storing")};
  juce::ToggleButton leaveBass_{TRANS("Pistons leave the bass coupler")};
  juce::ToggleButton stayLit_{TRANS("Pistons stay lit until another is pressed")};
  juce::ComboBox show_;
  juce::Viewport viewport_;
  std::unique_ptr<CombinationsPanel> panel_;
  bool placing_ = true;  // until constructed: sizing is not the player moving it
  // Whether the player has the window open, which is not the same as whether
  // it is visible: shutting the app down hides it, and reading that back as
  // "closed" meant a window left open never came back.
  bool open_ = false;
};

} // namespace mp::ui
