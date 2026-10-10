// Open: the player's favourite organs, then any other (#276).
//
// What the Open button shows once there is a favourite: the list, in slot
// order, to open with one touch; a search over it; each one moved up or down
// the list, renamed or removed; the organ now loaded added; and the file
// dialog for an organ that is not a favourite. Everything to do with getting
// an organ on the console is here, in the window the player opens to do it.
#pragma once

#include "../mp_audio/MasterpieceProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace mp::ui {

class OpenPanel : public juce::Component {
public:
  // `open` loads an organ as Open does; `browse` asks for one in the file
  // dialog. Either closes the window.
  OpenPanel(MasterpieceProcessor& p, std::function<void(const juce::File&)> open,
            std::function<void()> browse);
  void resized() override;
  void paint(juce::Graphics& g) override;
  void visibilityChanged() override;
  // Show the window, or go straight to the file dialog when there is no
  // favourite to show.
  static void show(MasterpieceProcessor& p, std::function<void(const juce::File&)> open,
                   std::function<void()> browse);

private:
  class Row;
  void refresh();
  void openSlot(int slot);
  void move(int slot, int by);
  void rename(int slot);
  void close();

  MasterpieceProcessor& proc_;
  std::function<void(const juce::File&)> open_;
  std::function<void()> browse_;
  juce::TextEditor search_;
  juce::Viewport viewport_;
  juce::Component rows_;
  std::vector<std::unique_ptr<Row>> rowViews_;
  juce::Label empty_;
  juce::Label status_;
  juce::TextButton addCurrent_{"Add the organ now loaded"};
  juce::TextButton another_{"Open another organ..."};
};

}  // namespace mp::ui
