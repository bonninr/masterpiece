// Engraves a MusicXML score into SVG pages with Verovio, and gives the same
// score as MIDI and as a time map for playing it. JUCE-free; the Verovio
// headers stay in the .cpp.
#pragma once
#include <memory>
#include <string>

namespace mp {

class ScoreEngraver {
public:
  // resourceDir holds the SMuFL font data Verovio reads at run time (see
  // scoreResourceFolder() in mp_ui for the copy the program carries).
  explicit ScoreEngraver(const std::string& resourceDir);
  ~ScoreEngraver();
  ScoreEngraver(const ScoreEngraver&) = delete;
  ScoreEngraver& operator=(const ScoreEngraver&) = delete;

  // False when the resource folder is unusable.
  bool ready() const { return ready_; }

  // Takes MusicXML text. A compressed .mxl is unpacked by the caller. On
  // failure the previous score is dropped and error() says why.
  bool load(const std::string& musicXml);
  bool loaded() const { return loaded_; }
  const std::string& error() const { return error_; }

  // Page size in pixels, and the size of the notation in percent of
  // Verovio's own. Engraves again when a loaded score meets a changed
  // setting: the lines break again to the new width.
  void setPage(int widthPx, int heightPx, int scalePercent);

  int pageCount() const;

  // Page 1..pageCount() as a standalone SVG document; empty when there is no
  // such page. Its width and height are the page size in pixels.
  std::string renderPage(int page);

  // The score as a standard MIDI file, base64: staff n plays on track n,
  // at the tempo the score asks for. Empty when nothing is loaded.
  std::string midiBase64();
  // When each note starts and stops, by element id, in milliseconds: a JSON
  // array of {"tstamp": ms, "on": [ids], "off": [ids], ...}.
  std::string timemapJson();
  // The page an element is engraved on, 0 if none. Searches the whole
  // score: for the odd lookup, never per note.
  int pageWithElement(const std::string& id);
  // What sounds at a moment of the playing: a JSON object with "notes" (ids)
  // and "page" (-1 between measures). Cheap enough for every frame of the
  // cursor.
  std::string elementsAtTime(int milliseconds);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool ready_ = false;
  bool loaded_ = false;
  std::string error_;
};

} // namespace mp
