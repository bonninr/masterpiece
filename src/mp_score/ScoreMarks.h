// The player's marks on a score: notes, and registrations that change the
// organ as the score goes on. Kept in the MusicXML file itself.
//
// The page is engraved again whenever its size or zoom changes, so a mark is
// never placed on the page: it belongs to a note, and is drawn wherever that
// note is engraved. In the file each mark is a <direction> written just
// before its note, which is the place MusicXML gives a direction that starts
// with a note, holding an <other-direction> that other programs keep and do
// not print:
//
//   <direction placement="above"><direction-type>
//     <other-direction print-object="no">masterpiece-note
//   The text, on as many lines as it has.</other-direction>
//   </direction-type></direction>
//
// A registration's first line is "masterpiece-registration", its second the
// name shown on it, and the rest the organs' states, which the program reads
// (ScoreRegistration in mp_ui); this file keeps them as text. A jump mark
// ("masterpiece-jump") and a combination ("masterpiece-combination") are
// written the same way, the combination's piston on its third line.
//
// JUCE-free, like the engraver. Reading gives every note an id of its own in
// the copy handed to Verovio, so the engraved page and the marks name the
// same note; the file keeps the ids it had, and gets none.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mp {

struct ScoreMark {
  // A jump mark (#1012) is a place to go to, named, stepped to with the up
  // and down keys or a console's buttons. A combination is a registration
  // by piston: "general 3" or "step 12", the same on every organ.
  enum class Kind { Note, Registration, Jump, Combination, Manuals };
  Kind kind = Kind::Note;
  // The note it belongs to, by the id the engraved page gives it.
  std::string note;
  // A note's text; a registration's, a jump's or a combination's name.
  std::string text;
  // A registration's states, one organ per line; a combination's piston
  // ("general 3", "step 12"); a manual change's staves, "1:2 2:3" (each
  // staff and the assignment code of the manual it plays from here on, 0 for
  // none, #1273); empty for the others.
  std::string states;
};

class ScoreMarks {
public:
  ScoreMarks();
  ~ScoreMarks();
  ScoreMarks(const ScoreMarks&) = delete;
  ScoreMarks& operator=(const ScoreMarks&) = delete;

  // Reads a MusicXML score and takes its marks out. False when it is not
  // XML; the marks are then empty and engraving() is the text as given.
  bool load(const std::string& musicXml);

  // The score for the engraver: every note with an id of its own, and no
  // marks.
  const std::string& engraving() const { return engraving_; }

  // The notes in the order the file has them, by id.
  const std::vector<std::string>& notes() const { return noteIds_; }
  bool hasNote(const std::string& id) const;

  std::vector<ScoreMark> marks;

  // The player's settings for this score (#1012): the tempo, the manual and
  // the muting of each staff, the view. Kept as text in the file's
  // identification, a <miscellaneous-field name="masterpiece-settings">,
  // which other programs keep and do not print. Empty when there are none.
  std::string settings;

  // The score as read, with `marks` and `settings` written in, each mark
  // before its note: the text to save. Marks on a note the score does not
  // have are left out.
  std::string save() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string engraving_;
  std::vector<std::string> noteIds_;
};

}  // namespace mp
