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

#include <filesystem>
#include <set>
#include <unordered_map>
#include <vector>

namespace mp::ui {

class ConsoleView : public juce::Component, private juce::Timer {
public:
  explicit ConsoleView(MasterpieceProcessor& p);

  // Rebuild from the current model. Safe to call on every organ load.
  void rebuild();
  void paint(juce::Graphics& g) override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  // Position a dragged control from a mouse point inside its image.
  void setControlFromMouse(juce::Point<int> p);
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
  };

  // One drawn key of a drawn manual. Assembled from the KeyImageSet's
  // per-shape artwork, because Hauptwerk ships no picture of a whole manual.
  struct KeyItem {
    int midiNote = 0;
    // Which MIDI channel this key speaks on, so a click on a drawn manual
    // arrives exactly as it would from the console wired to that manual —
    // couplers and all.
    int channel = 1;
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
  std::vector<Id> pageIds_;
  int pageIndex_ = 0;
  int layout_ = 0;
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
  juce::Rectangle<int> heldControlBounds_;
  bool heldControlHigherIsMore_ = true;
  // Where the drag began, and the value it began from: the gesture is
  // relative, so both are needed for its whole duration.
  int heldControlStartY_ = 0;
  int heldControlStartValue_ = 0;
  // Last seen sounding set, to repaint only when it actually changed.
  uint64_t keyStateHash_ = 0;
  // Same trick for the drawstops, so a registration set by anything other than
  // a mouse click still shows on the jamb.
  uint64_t stopStateHash_ = 0;
  bool hasArtwork_ = false;
  juce::Rectangle<int> extent_;
  std::string organRoot_;

  // Cache key is (imageSetId, frame index). A miss is remembered too, as a
  // null image, so a missing bitmap is not re-resolved on every repaint.
  std::unordered_map<int64_t, juce::Image> cache_;
};

} // namespace mp::ui
