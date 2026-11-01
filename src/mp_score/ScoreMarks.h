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
// (ScoreRegistration in mp_ui); this file keeps them as text.
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
  enum class Kind { Note, Registration };
  Kind kind = Kind::Note;
  // The note it belongs to, by the id the engraved page gives it.
  std::string note;
  // A note's text; a registration's name.
  std::string text;
  // A registration's states, one organ per line; empty for a note.
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

  // The score as read, with `marks` written in, each before its note: the
  // text to save. Marks on a note the score does not have are left out.
  std::string save() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string engraving_;
  std::vector<std::string> noteIds_;
};

}  // namespace mp
