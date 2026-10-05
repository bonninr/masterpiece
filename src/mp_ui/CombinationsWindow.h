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

class CombinationsPanel : public juce::Component, private juce::Timer {
public:
  explicit CombinationsPanel(MasterpieceProcessor& p);
  ~CombinationsPanel() override;

  // The pistons are the organ's: its divisions, and how many of each the
  // player keeps. Called after a load and whenever a count changes.
  void rebuild();
  // The height the content wants at a given width, for the window to size to.
  int preferredHeight(int width) const;

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override;
  void refreshSets();
  void refreshState();
  // Right-click: what a piston is mapped to, and how to map it.
  void showMidiMenu(PistonButton& b, MidiTargetKind kind, Id targetId,
                    bool offerHold);

  MasterpieceProcessor& proc_;

  PistonButton setter_{"Set"};
  PistonButton generalCancel_{"GC"};
  PistonButton stepPrev_{"<"};
  PistonButton stepNext_{">"};
  juce::Label frame_;
  juce::TextButton insertFrame_{"Insert"};
  juce::TextButton deleteFrame_{"Delete"};

  juce::Label generalsLabel_;
  juce::TextButton fewerGenerals_{"-"};
  juce::TextButton moreGenerals_{"+"};
  std::vector<std::unique_ptr<PistonButton>> generals_;

  juce::Label divisionalsLabel_;
  juce::TextButton fewerDivisionals_{"-"};
  juce::TextButton moreDivisionals_{"+"};
  struct DivisionRow {
    Id divisionId = 0;
    std::unique_ptr<juce::Label> name;
    std::vector<std::unique_ptr<PistonButton>> pistons;
    std::unique_ptr<PistonButton> cancel;
  };
  std::vector<DivisionRow> divisions_;

  juce::Label setLabel_;
  juce::ComboBox setBox_;
  juce::TextButton setNew_{"Save as..."};
  juce::TextButton setDelete_{"Delete"};
  std::vector<std::string> setNames_;
  std::unique_ptr<juce::AlertWindow> setPrompt_;

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
  void remember();
  MasterpieceProcessor& proc_;
  // The checkbox above the pistons, then the pistons.
  struct Content : juce::Component {
    std::function<void()> layout;
    void resized() override {
      if (layout) layout();
    }
  } content_;
  juce::ToggleButton onTop_{"Always on top"};
  juce::Viewport viewport_;
  std::unique_ptr<CombinationsPanel> panel_;
  bool placing_ = true;  // until constructed: sizing is not the player moving it
  // Whether the player has the window open, which is not the same as whether
  // it is visible: shutting the app down hides it, and reading that back as
  // "closed" meant a window left open never came back.
  bool open_ = false;
};

} // namespace mp::ui
