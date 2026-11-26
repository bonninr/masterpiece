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
//
// The player's marks (ScoreMarks) are icons on their notes: a note, opened
// and edited with a click, and a registration, which holds the organ's state
// and sets it as the playing reaches it, as the pages are turned forward, or
// with the registration buttons. A right click on a note adds one; an icon is
// dragged to another note. Each change is saved into the score's file, and
// Ctrl+Z (or Undo in the Mark... menu) takes it back.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_score/ScoreEngraver.h"
#include "../mp_score/ScoreMarks.h"
#include "../mp_score/ScorePlayback.h"
#include "../mp_audio/ScorePlayer.h"
#include "../mp_core/OrganModel.h"

#include <map>
#include <memory>
#include <set>
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
// root). Both from one parse. Staff lines drawn in `lines` when it is not
// transparent; otherwise in the ink's colour.
std::unique_ptr<juce::Drawable> parseScorePage(const std::string& svg, ScorePageGeometry& geometry,
                                               juce::Colour lines = {});

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
  // In percent of the starting size, 50..250.
  void setZoom(int percent);
  int zoom() const { return zoom_; }
  // The zoom the page is engraved at, shown on the button: less than zoom()
  // where the page's size stops it.
  int drawnZoom() const { return drawnZoom_; }
  void setInverted(bool inverted);
  bool inverted() const { return inverted_; }
  // The whole score on one line that scrolls with the playing, instead of
  // pages (#1012).
  void setLineView(bool line);
  bool lineView() const { return line_; }
  // Where the line is scrolled to, in the page's pixels.
  float lineScroll() const { return scrollX_; }
  // The paper: white, or cream, a little sepia; and staff lines in blue.
  enum class Paper { White, Cream };
  void setPaper(Paper paper);
  Paper paper() const { return paper_; }
  void setBlueLines(bool blue);
  bool blueLines() const { return blueLines_; }

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
  // A staff left silent, for the player to play it (#1012); its notes still
  // shown and followed.
  void setMuted(int staff, bool muted);
  // A manual change (#1273): from its note on, each staff it names plays the
  // manual it names. By the keyboard's place on a console, so the same on
  // another organ; on a repeat, again on each pass.
  Id markKeyboardForStaff(int markIndex, int staff) const;
  void setMarkKeyboardForStaff(int markIndex, int staff, Id keyboard);
  // The keyboard a staff plays at a moment of the playing: the last manual
  // change before it, or the score's own.
  Id keyboardForStaffAt(int staff, double ms) const;
  bool muted(int staff) const { return muted_.count(staff) != 0; }

  // The score's own settings (#1012), saved into its file with the marks:
  // the tempo, each staff's manual (by its place on a console, so the same
  // on another organ) and muting, and the view. As text, for tests.
  std::string settingsText() const;
  void applySettings(const std::string& text);

  // Playing, from the cursor: where it was stopped, the note last clicked, or
  // the first note of the page turned to.
  void play();
  void stop();
  bool playing() const;
  double startSeconds() const { return startMs_ / 1000.0; }
  // Moves the cursor to a moment of the score, as a click on a note does.
  void setStart(double seconds);
  // The tempo now, in beats a minute, and set (the score's own to start with).
  double tempoBpm() const;
  void setTempoBpm(double bpm);

  // A measure back or on (the arrow keys, a console's buttons): from where
  // the playing is, or from the start point while stopped. False at an end.
  bool stepMeasure(int step);
  // The number of the measure the playing is in (the start point while
  // stopped), as the score writes it; empty with no score.
  juce::String currentBar() const;
  // When each measure starts, in milliseconds of the playing, in order.
  const std::vector<double>& measureStarts() const { return measureMs_; }
  // The jump marks in the order they come, and a step to the one after or
  // before where the playing is (the up and down keys, a console's buttons).
  std::vector<int> jumpOrder() const;
  bool stepJump(int step);
  // The ornaments and repeats as played, for tests: the events handed to the
  // player.
  std::vector<ScorePlayer::Event> playedEvents() const { return eventsForPlaying(); }
  // Takes the console's score buttons now (as the panel's timer does).
  void takeConsoleCommands();

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

  // --- marks ----------------------------------------------------------
  // The score's marks, in the file's order.
  const std::vector<ScoreMark>& marks() const { return marks_.marks; }
  // Adds one on a note; its index, or -1 when the score has no such note.
  // A registration takes the organ's state at once.
  int addMark(ScoreMark::Kind kind, const std::string& note, const juce::String& text = {});
  void setMarkText(int index, const juce::String& text);
  // A combination's piston, "general 3" or "step 12".
  void setMarkStates(int index, const std::string& states);
  // To a jump mark's note: the start moves there, and the playing goes on
  // from it.
  void goToMark(int index);
  void moveMark(int index, const std::string& note);
  void deleteMark(int index);
  // A registration: the organ's state now, for this organ, beside the
  // others'; or this organ's taken out.
  void storeRegistration(int index);
  void forgetRegistration(int index);
  // Opens a mark's window, as a click on its icon does. `fresh` for one just
  // added: closed empty, a note goes without a step to take back.
  void openMark(int index, bool fresh = false);
  // A note whose window closed with no text goes.
  void dropEmptyNote(int index, bool fresh);
  // Takes back the last change to the marks. False when there is none.
  bool undo();
  // Where a mark's icon is drawn on the panel; empty when its note is on no
  // shown page.
  juce::Rectangle<float> markBounds(int index) const;

  // The registration marks (and the combination marks, which press a
  // general or go to a sequencer step) in the order they are played, and the
  // one set last (-1 for none). Stepping sets the next or the one before and shows
  // its page; a page turned forward sets the first one on it.
  std::vector<int> registrationOrder() const;
  int currentRegistration() const { return currentReg_; }
  bool stepRegistration(int step);
  void recallRegistration(int index);

  // Writes the marks into it now; false when there is no file or the write
  // fails. A change is written by itself a moment after it is made.
  bool saveMarks();

  // A window of its own: Detach and Attach. Never on phones and tablets.
  std::function<void()> onDetachToggle;
  void setDetachable(bool can);
  void setDetached(bool detached);
  // Its window kept over the others (#1012): chosen in View, while detached.
  // Kept with the program's settings.
  std::function<void(bool)> onKeepOnTop;
  bool keptOnTop() const;
  void setKeptOnTop(bool top);
  // The file chooser, as Open score... does.
  void openScoreDialog() { chooseFile(); }

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;
  bool keyPressed(const juce::KeyPress& key) override;

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
  std::vector<ScorePlayer::Event> eventsForPlaying(double fromSeconds = 0.0) const;
  double firstNoteMsOnPage(int slot) const;
  juce::Rectangle<int> pageArea() const;
  juce::Rectangle<int> slotArea(int slot) const;
  int slotCount() const { return twoPages_ && !line_ ? 2 : 1; }
  std::string noteAt(juce::Point<float> at, float within) const;
  int markAt(juce::Point<float> at) const;
  double markMs(int index) const;
  void drawMarks(juce::Graphics& g);
  void marksChanged();
  void remember();
  void addMarkMenu(const std::string& note, juce::Rectangle<int> screenArea);
  void registrationsReached(double fromMs, double toMs);
  void pageTurnedTo(int page);
  void settingsChanged();
  void restart();
  void moveTo(double ms);
  void keepInView();
  void scrollLine(float pages);
  float lineScale() const;
  void chooseView();
  void chooseConsoleButtons();

  MasterpieceProcessor& proc_;
  juce::File fonts_;
  std::unique_ptr<mp::ScoreEngraver> engraver_;

  struct Shown {
    int page = 0;
    ScorePageGeometry geometry;
    std::unique_ptr<juce::Drawable> drawable;
    juce::Image image;  // drawn from it at the screen's pixel density, inverted when asked
    float density = 0.0f;
    float scroll = -1.0f;  // on one line: where the image starts, in the page's pixels
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
  // click on a note. The first time; noteTimes_ has every time a repeat
  // plays it.
  std::map<std::string, double> noteStartMs_;
  std::map<std::string, std::vector<double>> noteTimes_;
  std::vector<double> measureMs_;
  std::map<size_t, ScoreOrnament> ornamented_;  // by index in notes_
  std::set<int> muted_;
  mp::ScorePlayback playback_;
  // A staff's manual saved by its place on the console (the keyboard's
  // assignment code), applied once the staves are known.
  std::map<int, int> savedCodes_;

  juce::TextButton open_{TRANS("Open score...")};
  juce::TextButton prev_{"<"}, next_{">"};
  juce::Label pageLabel_;
  juce::TextButton zoomOut_{"-"}, zoomIn_{"+"}, zoomReset_{"100%"};
  juce::TextButton twoUp_{TRANS("Two pages")};
  juce::TextButton play_{TRANS("Play")};
  // The playing's tempo (#1012): the score's own, from its first tempo mark,
  // and any number typed or stepped. The tempo marks that follow keep their
  // proportion: the whole score plays at the same ratio to what is written.
  juce::Slider tempo_{juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft};
  double scoreBpm_ = 120.0;
  void showTempo();
  juce::TextButton manuals_{TRANS("Manuals...")};
  juce::TextButton invert_{TRANS("Invert")};
  juce::TextButton view_{TRANS("View...")};
  juce::TextButton console_{TRANS("Buttons...")};
  juce::TextButton detach_{TRANS("Detach")};
  juce::TextButton mark_{TRANS("Mark...")};
  juce::TextButton regPrev_{"<"}, regNext_{">"};
  juce::Label regLabel_;
  std::unique_ptr<juce::FileChooser> chooser_;

  mp::ScoreMarks marks_;
  std::vector<std::vector<ScoreMark>> undo_;
  juce::File file_;
  bool zipped_ = false;
  bool marksDirty_ = false;
  juce::uint32 saveDue_ = 0;
  int currentReg_ = -1;
  double lastPlayMs_ = -1;
  int dragging_ = -1;           // the mark being dragged
  bool dragMoved_ = false;
  juce::Point<float> dragAt_;

  int page_ = 1;
  int zoom_ = 100;
  int drawnZoom_ = 100;
  bool twoPages_ = false;
  bool inverted_ = false;
  bool line_ = false;
  float scrollX_ = 0.0f;
  Paper paper_ = Paper::White;
  bool blueLines_ = false;
  bool applyingSettings_ = false;
  bool loaded_ = false;
  bool detachable_ = false;
  bool detached_ = false;
  bool needsEngrave_ = false;
  juce::uint32 engraveDue_ = 0;  // Time::getMillisecondCounter() when the pending engraving runs
  bool wasPlaying_ = false;
  double startMs_ = 0;
  std::vector<std::string> cursorNotes_;
  juce::String message_;
};

}  // namespace mp::ui
