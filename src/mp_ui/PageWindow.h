// A console page in a window of its own, to put on another screen.
//
// A real console surrounds the player: jambs to either side, the key desk in
// front, a combination panel above. A sample set draws each on a page of its
// own, and one window shows one page at a time. With a window per page the
// player can spread them over several monitors -- the jambs left and right,
// the console in the middle -- and play every page at once.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"
#include "Console.h"

#include <functional>

namespace mp::ui {

// One page, drawn to fit whatever size the window is given, keeping the
// artwork's proportions: a console is a photograph, and stretching it would
// distort the case and the keys.
class PageView : public juce::Component {
public:
  PageView(MasterpieceProcessor& p, int page, int layout);
  void resized() override;
  void paint(juce::Graphics& g) override;
  int page() const { return console_.currentPage(); }
  juce::String pageName() const { return console_.pageName(console_.currentPage()); }
  juce::Rectangle<int> artworkBounds() const { return console_.artworkBounds(); }

private:
  ConsoleView console_;
  juce::Component holder_;
};

class PageWindow : public juce::DocumentWindow {
public:
  PageWindow(MasterpieceProcessor& p, int page, int layout, const juce::String& organName);
  void closeButtonPressed() override;
  void moved() override;
  void resized() override;
  int page() const { return view_->page(); }

  // Closed by the player: the owner drops the window. Moved or resized: the
  // owner writes down where it is, so it comes back there.
  std::function<void(PageWindow*)> onClosed;
  std::function<void()> onPlaced;

private:
  PageView* view_ = nullptr;  // owned by the window as its content
};

}  // namespace mp::ui
