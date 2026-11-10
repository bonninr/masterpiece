// Learn tabs in turn (#866): the organ's stops, couplers and tremulants in
// the order of the stop list, each armed for MIDI learn in its turn. The
// player presses each tab on the console as its name shows, and the window
// moves on by itself. A tab already learned in this run for another row is
// refused and the row asks again, so a tab pressed twice to put it back up
// does not map the next stop. The first step toward a console profile: the
// console's tabs mapped in one pass.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>
#include <vector>

#include "../mp_audio/MasterpieceProcessor.h"

namespace mp::ui {

class LearnInTurnPanel : public juce::Component, private juce::Timer, private juce::ListBoxModel {
public:
  explicit LearnInTurnPanel(MasterpieceProcessor& p);
  ~LearnInTurnPanel() override;

  struct Row {
    std::string section;  // "Great"
    std::string name;     // "Principal 8'"
    Id switchId = 0;      // the switch MIDI learns, after midiSwitchFor
    bool latching = true;
    bool piston = false;
  };

  // The rows as the organ is now: after a load, and when Pistons is ticked.
  void rebuild();
  // Arm the row `from`, or the first one after it still to learn.
  void start(int from);
  void skip();
  void back();
  void stop();
  // What the timer does: a learn taken moves on, a repeated tab asks again.
  void poll();

  int rowCount() const { return static_cast<int>(rows_.size()); }
  const Row& row(int i) const { return rows_[static_cast<size_t>(i)]; }
  // The row armed now, -1 when not running.
  int currentRow() const { return current_; }
  int learnedCount() const { return learned_; }
  juce::String promptText() const { return prompt_.getText(); }
  juce::ToggleButton& unmappedOnlyButton() { return unmappedOnly_; }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override { poll(); }
  int getNumRows() override { return rowCount(); }
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
  void listBoxItemClicked(int row, const juce::MouseEvent&) override;

  void arm();
  // Leave the armed row: its learn cancelled, its mapping put back when
  // nothing new was learned.
  void release();
  void finish();
  void showState();
  bool mapped(const Row& r) const;
  juce::String rowLabel(const Row& r) const;

  MasterpieceProcessor& proc_;
  std::vector<Row> rows_;
  int current_ = -1;
  bool armed_ = false;
  int learned_ = 0;
  // What the armed row had before it was armed: put back when its learn is
  // refused.
  std::vector<MidiBinding> before_;
  // The tabs learned in this run, by row.
  std::vector<std::pair<MidiSource, int>> taken_;
  juce::String refused_;

  juce::Label prompt_;
  juce::Label note_;
  juce::ListBox list_;
  juce::TextButton start_{TRANS("Start")};
  juce::TextButton skip_{TRANS("Skip")};
  juce::TextButton back_{TRANS("Back")};
  juce::TextButton stop_{TRANS("Stop")};
  juce::ToggleButton pistons_{TRANS("Pistons")};
  juce::ToggleButton unmappedOnly_{TRANS("Only tabs not mapped")};
};

class LearnInTurnWindow : public juce::DocumentWindow {
public:
  explicit LearnInTurnWindow(MasterpieceProcessor& p);
  ~LearnInTurnWindow() override;
  void closeButtonPressed() override;
  LearnInTurnPanel& panel() { return *panel_; }

private:
  std::unique_ptr<LearnInTurnPanel> panel_;
};

}  // namespace mp::ui
