// The score panel: a MusicXML score engraved by Verovio, played through the
// organ with a cursor that follows the playing.
//
// A tab after the console pages and the panels, like a panel of stops, and
// like one it can be detached into a window of its own (not on phones and
// tablets). The page is engraved to the panel's own size, so the staves fill
// its width: zoom makes the notation larger and the lines break again,
// which gives more pages, turned by themselves while the score plays.
//
// Each staff is played on the manual the player chooses for it (the
// Manuals... button), on the channel a click on that manual's drawn keys
// would use, so couplers and the drawn keys follow the playing.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_score/ScoreEngraver.h"
#include "../mp_audio/ScorePlayer.h"
#include "../mp_core/OrganModel.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mp {
class MasterpieceProcessor;
}

namespace mp::ui {

// The font and glyph data Verovio reads, written once to the app-data folder
// from the copy built into the program. An empty File when it cannot be written.
juce::File scoreResourceFolder();
// The same into any folder, which is emptied first. False when it fails.
bool unpackScoreFonts(const juce::File& folder);
// The fonts built into the program for the page's text, made known to JUCE by
// their family names: Bravura for music symbols inside text (the note of a
// metronome mark), Liberation Serif for the words. Once per run; true when
// every one loaded.
bool registerScoreFonts();
// The family the page's words are drawn in.
inline constexpr const char* kScoreTextFont = "Liberation Serif";

// Where the notes and systems of one engraved page are, in the page's pixels.
// Read from the SVG Verovio writes: each glyph is a <use> placed by a
// translate(), each staff line a horizontal path inside <g class="staff">.
struct ScorePageGeometry {
  struct Note {
    juce::Point<float> head;  // the notehead's left end, on its centre line
    int system = -1;
  };
  struct System {
    juce::Rectangle<float> area;  // from the top line of its first staff to the bottom of its last
    float lineGap = 8.0f;         // the space between two staff lines
  };
  std::map<std::string, Note> notes;
  std::vector<System> systems;
  float width = 0, height = 0;  // the page, in pixels
};

// Reads the geometry from a page's SVG, and turns the SVG into the form JUCE
// draws (Verovio's nested <svg> becomes a group, its viewBox moved to the
// root). Both from one parse.
std::unique_ptr<juce::Drawable> parseScorePage(const std::string& svg, ScorePageGeometry& geometry);

class ScorePanel : public juce::Component, private juce::Timer {
public:
  // The fonts come from `fonts` when given, otherwise from scoreResourceFolder().
  explicit ScorePanel(MasterpieceProcessor& proc, const juce::File& fonts = {});
  ~ScorePanel() override;

  // Loads a MusicXML (.musicxml, .xml) or compressed (.mxl) file. False, with
  // the reason shown in the panel, when it cannot be read.
  bool openFile(const juce::File& file);
  // The same for the bytes of such a file.
  bool openData(const juce::MemoryBlock& bytes, const juce::String& name);
  bool loaded() const { return loaded_; }

  // The first page shown, from 1, and how many there are.
  int page() const { return page_; }
  int pageCount() const;
  void showPage(int page);
  void setTwoPages(bool two);
  bool twoPages() const { return twoPages_; }
  // In percent of the starting size, 50..250.
  void setZoom(int percent);
  int zoom() const { return zoom_; }
  // The zoom the page is engraved at, shown on the button: less than zoom()
  // where the page's size stops it.
  int drawnZoom() const { return drawnZoom_; }
  void setInverted(bool inverted);
  bool inverted() const { return inverted_; }

  // The staves, by number from 1, with the name the score gives each part.
  struct Staff {
    int number = 0;
    juce::String name;
  };
  const std::vector<Staff>& staves() const { return staves_; }
  // The keyboard each staff plays, 0 for none. The defaults: the pedal for
  // the last of three or more staves, the organ's preferred manual for the
  // others.
  Id keyboardForStaff(int staff) const;
  void setKeyboardForStaff(int staff, Id keyboard);

  // Playing, from the cursor: where it was stopped, the note last clicked, or
  // the first note of the page turned to.
  void play();
  void stop();
  bool playing() const;
  double startSeconds() const { return startMs_ / 1000.0; }
  // Moves the cursor to a moment of the score, as a click on a note does.
  void setStart(double seconds);

  // The notes the cursor marks now, by id, for tests.
  const std::vector<std::string>& cursorNotes() const { return cursorNotes_; }
  // What is drawn on the shown pages, for tests: the page's geometry.
  const ScorePageGeometry* geometryOf(int page) const;
  // Where a note's head is drawn on the panel, for tests; {-1, -1} when the
  // note is on no shown page.
  juce::Point<float> notePosition(const std::string& id) const;
  // The panel as drawn now, for tests: engraves at once if a re-engraving is
  // pending, and moves the cursor to where the playing is.
  juce::Image render();

  // A window of its own: Detach and Attach. Never on phones and tablets.
  std::function<void()> onDetachToggle;
  void setDetachable(bool can);
  void setDetached(bool detached);

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& e) override;

private:
  void timerCallback() override;
  void requestEngrave();
  void engrave();
  void renderShown();
  void drawSlot(juce::Graphics& g, int slot);
  juce::AffineTransform pageToPanel(int slot) const;
  void followPlaying();
  void chooseFile();
  void chooseManuals();
  void refreshButtons();
  void readTimeline();
  std::vector<ScorePlayer::Event> eventsForPlaying() const;
  double firstNoteMsOnPage(int slot) const;
  juce::Rectangle<int> pageArea() const;
  juce::Rectangle<int> slotArea(int slot) const;
  int slotCount() const { return twoPages_ ? 2 : 1; }

  MasterpieceProcessor& proc_;
  juce::File fonts_;
  std::unique_ptr<mp::ScoreEngraver> engraver_;

  struct Shown {
    int page = 0;
    ScorePageGeometry geometry;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Image image;  // drawn from it at the screen's pixel density, inverted when asked
    float density = 0.0f;
  };
  Shown shown_[2];

  // The score as notes, by staff, from Verovio's MIDI.
  struct Note {
    double on = 0, off = 0;  // seconds
    int staff = 1;
    int pitch = 60;
  };
  std::vector<Note> notes_;
  std::vector<Staff> staves_;
  std::map<int, Id> keyboards_;
  // When each note starts, from the time map: for the start of a page and a
  // click on a note.
  std::map<std::string, double> noteStartMs_;

  juce::TextButton open_{TRANS("Open score...")};
  juce::TextButton prev_{"<"}, next_{">"};
  juce::Label pageLabel_;
  juce::TextButton zoomOut_{"-"}, zoomIn_{"+"}, zoomReset_{"100%"};
  juce::TextButton twoUp_{TRANS("Two pages")};
  juce::TextButton play_{TRANS("Play")};
  juce::TextButton manuals_{TRANS("Manuals...")};
  juce::TextButton invert_{TRANS("Invert")};
  juce::TextButton detach_{TRANS("Detach")};
  std::unique_ptr<juce::FileChooser> chooser_;

  int page_ = 1;
  int zoom_ = 100;
  int drawnZoom_ = 100;
  bool twoPages_ = false;
  bool inverted_ = false;
  bool loaded_ = false;
  bool detachable_ = false;
  bool needsEngrave_ = false;
  juce::uint32 engraveDue_ = 0;  // Time::getMillisecondCounter() when the pending engraving runs
  bool wasPlaying_ = false;
  double startMs_ = 0;
  std::vector<std::string> cursorNotes_;
  juce::String message_;
};

}  // namespace mp::ui
