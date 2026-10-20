// The authentic console: the organ's own artwork, drawn and clickable.
//
// A Hauptwerk set ships its console as bitmaps. A DisplayPage places
// ImageSetInstances at pixel positions in ScreenLayerNumber order; each
// instance shows one frame of an ImageSet; and a Switch binds to an instance,
// naming which frame means engaged and which means disengaged. Clicking a
// drawstop therefore does two things at once — it flips the switch and it
// changes the picture — and that is the whole mechanism.
//
// Images are loaded lazily and cached: Nancy declares 622 image sets, and
// decoding all of them up front would cost seconds for artwork that is mostly
// off-screen or never used.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"
#include "AccessibleToggle.h"

#include <filesystem>
#include <functional>
#include <set>
#include <unordered_map>
#include <vector>

namespace mp::ui {

// The right-click menu on a continuous control -- a drawn shoe, slider or
// knob, or a slider in the swell strip: what drives it now, and learning a
// pedal or knob for it. `area` is in screen coordinates. `after` runs once a
// choice is made, for the caller to repaint.
void showControlMidiMenu(MasterpieceProcessor& proc, Id controlId,
                         juce::Rectangle<int> area, std::function<void()> after);

// One drawn control, with an invisible component over it. The console paints
// its controls and takes their clicks as it always did; this is there for the
// screen reader, which finds the stop, the coupler or the piston there and
// reads whether it is drawn.
class ConsoleControl : public juce::Component, public AccessibleToggle {
public:
  ConsoleControl(juce::String name, Id switchId)
      : name_(std::move(name)), switchId_(switchId) {}

  // What the screen reader reads, and whether the control is drawn.
  juce::String accessibleName() const override { return name_; }
  bool accessibleDrawn() const override { return drawn_; }
  void accessibleToggle() override;

  // The switch the control draws.
  Id switchId() const { return switchId_; }
  // The state as it is now. `tell` is false when the control is made: the
  // screen reader is told of a change, not of the state a control starts in.
  void setDrawn(bool drawn, bool tell = true) {
    if (drawn == drawn_) return;
    drawn_ = drawn;
    if (!tell) return;
    // The screen reader is told that the control moved.
    if (auto* handler = getAccessibilityHandler())
      handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
  }
  // A piston is pressed and let go, not drawn and retired: the screen reader
  // is told of a button, not of a toggle.
  void setMomentary(bool momentary) { momentary_ = momentary; }
  // What happens when the control is drawn, retired or pressed.
  std::function<void()> onToggle;

  // The handler the accessibility layer talks to. The component hands this
  // out when the console is on screen, which needs a window; this asks for
  // it without one.
  std::unique_ptr<juce::AccessibilityHandler> makeAccessibilityHandler() {
    return createAccessibilityHandler();
  }

protected:
  std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override {
    return std::make_unique<ToggleAccessibilityHandler>(*this, *this,
                                                         momentary_ ? juce::AccessibilityRole::button
                                                                    : juce::AccessibilityRole::toggleButton);
  }

private:
  juce::String name_;
  Id switchId_ = 0;
  bool drawn_ = false;
  bool momentary_ = false;
};

class ConsoleView : public juce::Component, public juce::TooltipClient, private juce::Timer {
public:
  explicit ConsoleView(MasterpieceProcessor& p);
  // Over a stop that makes no sound: its name, and why.
  juce::String getTooltip() override;

  // Rebuild from the current model. Safe to call on every organ load.
  void rebuild();
  // Held as a picture of itself until the next rebuild: an organ being loaded
  // replaces the model on the loader's thread, and a console still reading it
  // drew the organ being left with the new one's artwork and switches, a
  // general turned square, its neighbours gone (#228).
  void freeze();
  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  // Position a dragged control from a mouse point inside its image.
  void setControlFromMouse(juce::Point<int> p);
  // Keeps the drawn controls in step with the engine: a piston, a
  // combination or MIDI moves a switch, and the screen reader is told.
  void refreshControlStates();
  void resized() override;

  // Which console layout to draw. A set that ships for more than one console
  // size gives every drawn thing up to three alternate positions, and often
  // different artwork with them: a stop knob drawn for a 1920-wide console is
  // not the one drawn for a 1024-wide one. 0 is the primary layout, which
  // every set has.
  void setLayout(int layout);
  int layout() const { return layout_; }
  // How many layouts this organ actually offers, counting the primary one.
  int layoutCount() const { return layoutCount_; }
  // A layout drawn for a screen taller than it is wide: a set's portrait
  // jambs. The definition names no layout; its size is all it says.
  bool isPortrait(int layout) const;
  // The layout for a view of this shape: the one shown when it suits,
  // else the first that does, else the one shown.
  int layoutForShape(bool tall) const;

  // Which display page to show, and how many there are.
  void setPage(int index);
  int pageCount() const { return static_cast<int>(pageIds_.size()); }
  juce::String pageName(int index) const;
  int currentPage() const { return pageIndex_; }

  // The console draws at the artwork's own pixel size; the viewport scrolls it.
  // Returns false when the organ ships no usable console artwork, which is
  // when the caller should fall back to the plain jamb.
  bool hasArtwork() const { return hasArtwork_; }
  // The artwork's own pixel size, which is what the editor scales to fit.
  juce::Rectangle<int> artworkBounds() const { return extent_; }
  // The scale at which this page fits an area. A page far smaller than the
  // organ's first page (the GrandOrgue demo's Noises panel is 300 by 160) is
  // magnified as the first page is, and no further (#299).
  // The scale that fits the artwork in the area. `fillSmall` lets a page
  // smaller than half the first one fill the window, as it does in a
  // window of its own.
  float fitScale(juce::Rectangle<int> area, bool fillSmall = false) const {
    return fitScale(extent_, firstExtent_, area, fillSmall);
  }
  static float fitScale(juce::Rectangle<int> art, juce::Rectangle<int> first,
                       juce::Rectangle<int> area, bool fillSmall = false);

private:
  // One drawn thing: an instance, the switch that owns it (if any), and where.
  struct Item {
    Id instanceId = 0;
    Id imageSetId = 0;
    Id switchId = 0;          // 0 = decoration, not a control
    // A shoe, wheel or slider: dragged rather than toggled, and its frame is
    // chosen from its value instead of from an on/off pair. Mutually
    // exclusive with switchId in every set seen so far, but not assumed to be.
    Id controlId = 0;
    bool controlHigherIsMore = true;
    int defaultIndex = 1;
    int engagedIndex = 0;
    int disengagedIndex = 0;
    int layer = 0;
    juce::Rectangle<int> bounds;
    // Where a click counts. A drawstop's knob is not its whole rectangle, and
    // the organ says so; when it does not, the whole image is live.
    juce::Rectangle<int> hitBounds;
    bool clickable = false;
    // When set, the image repeats to fill `bounds` rather than being drawn
    // once at its own size. Backdrops are authored this way.
    bool tiled = false;
    // The frame shown when the organ came up. A control showing another one
    // is painted over everything else (see paint()); -1 before it is known.
    int loadedIndex = -1;
    // Drawn where the layout shown puts it. An instance a set gives no place
    // in an alternate layout is drawn at its primary place, under that
    // layout's own artwork, and must stay there when its stop moves (#308).
    bool ownPlace = true;
    // The frame last seen by the timer, which repaints this item alone when
    // it differs; -1 before the first look.
    int shownIndex = -1;
  };

  // One drawn key of a drawn manual. Assembled from the KeyImageSet's
  // per-shape artwork, because Hauptwerk ships no picture of a whole manual.
  struct KeyItem {
    int midiNote = 0;
    // Which MIDI channel this key speaks on, so a click on a drawn manual
    // arrives exactly as it would from the console wired to that manual —
    // couplers and all. Refreshed from the keyboard's current assignment on
    // every tick, so a change in Settings shows at once.
    int channel = 1;
    // Which channel's notes light it: 0 when no channel reaches its keyboard,
    // so a pedal whose default channel went to a manual stays still while
    // that manual is played.
    int litChannel = 1;
    Id keyboardId = 0;
    Id imageSetId = 0;
    int engagedIndex = 1;
    int disengagedIndex = 2;
    juce::Rectangle<int> bounds;
    bool sharp = false;
    // True when the key's artwork is one of Hauptwerk's own standard images,
    // which ship with that application rather than with the sample set. The
    // key is then drawn from its measurements instead of from a bitmap: a
    // theatre console whose manuals are standard keys is otherwise invisible
    // and unplayable.
    bool synthetic = false;
    // Whether the timer last saw it down, to repaint this key alone when
    // that changes.
    bool shownDown = false;
  };

  // A piece of engraved console text. Many sets paint the stop names into
  // the artwork; the rest write them as text over it, and a console that
  // ignores that draws blank knobs.
  struct TextItem {
    juce::String text;
    juce::Rectangle<int> bounds;
    juce::Font font{juce::FontOptions{}};
    juce::Colour colour;
    juce::Justification justification{juce::Justification::centred};
    bool wrap = false;
  };

  // Lay out the text written over the current page.
  void buildTexts(const OrganModel& model, Id pageId);
  // The invisible components over the drawn controls, so a screen reader
  // finds them.
  void buildControls(const OrganModel& model);
  // The screen reader is offered the console's controls only while the
  // console is on the screen.
  void setControlsAccessible(bool accessible);
  // Draw the control or retire it, as a click on it does.
  void toggleDrawnSwitch(Id switchId);

  // The right-click menu on a drawstop: what it is mapped to now, and the
  // behaviours it can be taught.
  void showMidiMenu(Id switchId, juce::Rectangle<int> bounds);
  const juce::Image* imageFor(Id imageSetId, int index);
  // A bitmap from "Hauptwerk Standard Components" (package ids 1 to 10),
  // which ships with Hauptwerk rather than with the set. When the set's own
  // root does not have it, a Hauptwerk installation among the known sample
  // libraries may; the path there, or `found` unchanged.
  std::filesystem::path standardComponent(const std::filesystem::path& found,
                                          const std::string& fileName, Id packageId) const;
  // Image sets drawn with generated wood because their pictures are missing.
  std::set<Id> generatedWood_;
  int frameIndexFor(const Item& item) const;
  // Lay out every drawn manual on the current page.
  void buildKeyboards(const OrganModel& model, Id pageId);
  // Which key is under this point, or -1. Sharps win, being on top.
  int keyAt(juce::Point<int> p, int* outChannel = nullptr) const;
  void timerCallback() override;
  // Repaint bounds covering every drawn key, so a note on or off does not
  // repaint the whole console picture.
  juce::Rectangle<int> keysBounds() const;

  MasterpieceProcessor& proc_;
  std::vector<Item> items_;      // sorted by layer: painter's order
  std::vector<TextItem> texts_;  // drawn over the artwork, under the keys
  // The invisible components over the drawn controls, for the screen reader.
  std::vector<std::unique_ptr<ConsoleControl>> controls_;
  // Whether the screen reader is offered the controls: only while the console
  // is on the screen.
  bool controlsAccessible_ = true;
  std::vector<Id> pageIds_;
  int pageIndex_ = 0;
  int layout_ = 0;
  // The layout the page on show is drawn in: layout_, or the primary one
  // for a page that declares nothing for it.
  int drawLayout_ = 0;
  int layoutCount_ = 1;
  // Naturals first, then sharps: painter's order, since a sharp overlaps the
  // naturals on either side of it.
  std::vector<KeyItem> keys_;
  // Keys found while walking the page's instances, held until the assembled
  // manuals have been built so the paint order can be sorted out once.
  std::vector<KeyItem> drawnKeys_;
  // The key the mouse is currently holding down, so it can be released on
  // mouse-up even if the pointer has wandered off it.
  int heldKey_ = -1;
  int heldChannel_ = 1;
  // The control under the mouse for the duration of a drag. Its geometry is
  // kept here so a drag that wanders outside the image still moves the right
  // thing along the right axis.
  Id heldControl_ = 0;
  // A momentary button held down with the mouse: released with it.
  Id heldButton_ = 0;
  juce::Rectangle<int> heldButtonBounds_;
  juce::Rectangle<int> heldControlBounds_;
  bool heldControlHigherIsMore_ = true;
  // Where the drag began, and the value it began from: the gesture is
  // relative, so both are needed for its whole duration.
  int heldControlStartY_ = 0;
  int heldControlStartX_ = 0;
  int heldControlStartValue_ = 0;
  // The timer looks 30 times a second and asks where each manual's keys are
  // routed on every eighth look.
  static constexpr int kRoutingEvery = 8;
  int routingTick_ = kRoutingEvery;
  // How often the timer looks, in milliseconds: while the organ is played,
  // once nothing has moved for kQuietTicks looks, and while the console is
  // not on screen.
  static constexpr int kPlayingMs = 33;
  static constexpr int kRestingMs = 100;
  static constexpr int kHiddenMs = 250;
  static constexpr int kQuietTicks = 60;
  int quietTicks_ = 0;
  // Counts the timer's looks, for the keys' every other one.
  unsigned keyTick_ = 0;
  // This many changes in one look are repainted as the whole console.
  static constexpr int kRepaintWhole = 24;
  // The control being learned when the timer last looked, -1 for none.
  int64_t shownLearning_ = -1;
  bool hasArtwork_ = false;
  // The picture shown while frozen; invalid otherwise.
  juce::Image frozen_;
  juce::Rectangle<int> extent_;
  // The first page's extent, and the organ and layout it was measured for.
  juce::Rectangle<int> firstExtent_;
  std::string firstOf_;
  std::string organRoot_;

  // Cache key is (imageSetId, frame index). A miss is remembered too, as a
  // null image, so a missing bitmap is not re-resolved on every repaint.
  std::unordered_map<int64_t, juce::Image> cache_;
};

} // namespace mp::ui
