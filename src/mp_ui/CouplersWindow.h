// The couplers in a window of their own (#722): the organ's couplers by the
// division they feed, then the extra couplers, each a button that draws and
// retires it. Opened from the View menu, it floats over the console and can
// stay open while playing; the buttons follow pistons, the console and MIDI.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

#include "../mp_audio/MasterpieceProcessor.h"
#include "Panels.h"

namespace mp::ui {

class StopButton;

class CouplersPanel : public juce::Component, private juce::Timer {
public:
  explicit CouplersPanel(MasterpieceProcessor& p);
  ~CouplersPanel() override;

  // The organ's couplers as they are now: after a load, and after the extra
  // couplers are turned on or off.
  void rebuild();
  // The height the content wants at a given width, for the window to size to.
  int preferredHeight(int width) const;
  // The extra couplers are built with the organ, so turning them on or off
  // loads it again; the window asks its owner to do that.
  std::function<void()> onExtraCouplers;

  // Follow the engine: a coupler drawn on the console, by a piston or from
  // MIDI shows here too. On the panel's own timer.
  void refresh();

  // The buttons in the order shown, for the tests.
  int couplerCount() const { return static_cast<int>(buttons_.size()); }
  StopButton* couplerButton(int index) {
    return index >= 0 && index < couplerCount() ? buttons_[static_cast<size_t>(index)].button.get() : nullptr;
  }

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  void timerCallback() override;
  int layOut(int width, bool place);

  MasterpieceProcessor& proc_;
  juce::ToggleButton extra_{TRANS("Extra couplers")};
  juce::Label note_;
  std::vector<std::unique_ptr<juce::Label>> headers_;
  struct Entry {
    PanelElement element;
    size_t section = 0;
    std::unique_ptr<StopButton> button;
  };
  std::vector<Entry> buttons_;
};

class CouplersWindow : public juce::DocumentWindow {
public:
  explicit CouplersWindow(MasterpieceProcessor& p);
  ~CouplersWindow() override;
  void closeButtonPressed() override;
  void resized() override;
  CouplersPanel& panel() { return *panel_; }

private:
  juce::Viewport viewport_;
  std::unique_ptr<CouplersPanel> panel_;
};

}  // namespace mp::ui
