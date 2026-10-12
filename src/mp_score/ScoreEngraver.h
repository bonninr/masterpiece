// Engraves a MusicXML score (plain or compressed) into SVG pages with Verovio.
// JUCE-free; the Verovio headers stay in the .cpp.
#pragma once
#include <memory>
#include <string>

namespace mp {

class ScoreEngraver {
public:
  // resourceDir holds the SMuFL font data Verovio reads at run time (see
  // ScoreResources in mp_ui for the copy the app ships).
  explicit ScoreEngraver(const std::string& resourceDir);
  ~ScoreEngraver();
  ScoreEngraver(const ScoreEngraver&) = delete;
  ScoreEngraver& operator=(const ScoreEngraver&) = delete;

  // False when the resource folder is unusable.
  bool ready() const { return ready_; }

  // Takes MusicXML text or the bytes of a compressed .mxl file. On failure
  // the previous score is dropped and error() says why.
  bool load(const std::string& bytes);
  const std::string& error() const { return error_; }

  // Page size in pixels at the given scale in percent (the notation grows
  // with it). Engraves again when a loaded score meets a changed setting.
  void setPage(int widthPx, int heightPx, int scalePercent);

  int pageCount() const;

  // Page 1..pageCount() as a standalone SVG document; empty when there is no
  // such page. Its width and height are the page size in pixels.
  std::string renderPage(int page);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool ready_ = false;
  bool loaded_ = false;
  std::string error_;
};

} // namespace mp
