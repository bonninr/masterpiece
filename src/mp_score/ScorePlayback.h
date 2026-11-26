// What a score asks of the playing beyond its notes (#1012): the order its
// measures are played in, with the repeats, the first and second endings,
// da capo, dal segno, fine and the coda followed; and the ornaments on its
// notes (trills, mordents, turns), which the engraver's MIDI leaves as plain
// notes.
//
// Read from the engraving copy ScoreMarks makes, where every note has an id
// of its own. The page shows the score as written; the playing goes through
// unrolled(), a copy whose measures come in the order they are played, a
// measure's notes played again named "<id>-mpr2", "<id>-mpr3"... so the
// cursor finds them on the page by originalId().
//
// JUCE-free, like the engraver.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mp {

struct ScoreOrnament {
  enum class Kind { Trill, Mordent, InvertedMordent, Turn, InvertedTurn };
  Kind kind = Kind::Trill;
  int pitch = 60;   // the note's own, as MIDI
  int upper = 2;    // semitones to the note above it in the key
  int lower = -2;   // and below it
};

class ScorePlayback {
public:
  ScorePlayback();
  ~ScorePlayback();
  ScorePlayback(const ScorePlayback&) = delete;
  ScorePlayback& operator=(const ScorePlayback&) = delete;

  // A MusicXML score with an id on every note. False when it is not XML.
  bool load(const std::string& musicXml);

  // The measures of the first part, from 0, in the order they are played.
  const std::vector<int>& order() const { return order_; }
  // True when that is not simply each measure once, in turn.
  bool repeats() const { return repeats_; }
  // The score with its measures in that order, every part alike, and the
  // repeat signs and jumps taken out; empty when repeats() is false.
  std::string unrolled() const;

  // The ornaments, by note id.
  const std::map<std::string, ScoreOrnament>& ornaments() const { return ornaments_; }

  // A note's id on the page, from its id in the playing.
  static std::string originalId(const std::string& id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<int> order_;
  bool repeats_ = false;
  std::map<std::string, ScoreOrnament> ornaments_;
};

}  // namespace mp
