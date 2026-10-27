// The organ's own console pages, for the web console: each page drawn by
// the program as its window draws it, sent as a picture, with the parts that
// change sent again as they change, and taps and drags from the browser
// carried out on the page as clicks are.
//
// Every call is made on the message thread.
#pragma once

#include "ControlProtocol.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>

namespace mp {
class MasterpieceProcessor;
namespace ui {
class ConsoleView;
}
}  // namespace mp

namespace mp::api {

class ConsolePages {
public:
  explicit ConsolePages(MasterpieceProcessor& p);
  ~ConsolePages();

  // {session, pages: [{index, name, width, height, layouts: [{layout, portrait}]}]}
  juce::var list(Error& error);

  // The page's picture, or the part `area` of it (in the page's own pixels;
  // empty for the whole page). `serial` is the page's change count as drawn,
  // for changes() to go on from.
  juce::Image picture(int page, int layout, juce::Rectangle<int> area, int64_t& serial, Error& error);

  // What was drawn anew since `since`:
  // {serial, full: true} when the page must be fetched whole again, else
  // {serial, areas: [{x, y, w, h}]} in the page's pixels.
  juce::var changes(int page, int layout, int64_t since, Error& error);

  enum class Phase { Down, Move, Up };
  // A tap or a drag at a point in the page's pixels, as a mouse makes it.
  // Drawn manuals are not played from a browser.
  Error pointer(int page, int layout, Phase phase, juce::Point<int> at);

  // Pages kept drawn at once; the one longest unused makes room.
  static constexpr int kMaxViews = 4;
  // A press held this long with no release is let go: a browser that went
  // away mid-press leaves no piston held.
  static constexpr juce::uint32 kHeldMs = 10000;
  // Changes remembered per page; a client further behind fetches it whole.
  static constexpr size_t kMaxLog = 256;

  struct View;

private:
  View* view(int page, int layout, Error& error);
  bool current(Error& error);
  void releaseStale(View& v);

  MasterpieceProcessor& proc_;
  int builtFor_ = -1;
  juce::var list_;
  std::map<int, std::unique_ptr<View>> views_;
};

}  // namespace mp::api
