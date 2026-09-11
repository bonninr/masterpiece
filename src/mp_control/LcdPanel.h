// Console LCD panels: the little text displays on a physical organ's jamb.
//
// A wired console often carries one or more 32-character displays that show
// what the player cannot see from the keys — which temperament is loaded, what
// the pitch is, how far the crescendo has stepped. They are driven by system
// exclusive messages sent to the console.
//
// Two things about that are worth stating plainly, because they shape the
// whole design:
//
// 1. **The framing is not in the organ file.** Which manufacturer id, which
//    command byte, how the display is addressed — all of that belongs to the
//    console hardware, not the instrument. So the framing is CONFIGURED, not
//    guessed (ADR-002). The default manufacturer id is 0x7D, which the MIDI
//    specification reserves for non-commercial use; that is the honest choice
//    for a message whose real vendor prefix we do not have.
//
// 2. **System exclusive is seven-bit.** Every byte between F0 and F7 must be
//    0x00..0x7F. A stray high byte does not produce a wrong character, it ends
//    the message and corrupts whatever follows. Organ and temperament names
//    are full of accented letters — "Kraków", "Müller" — so transliteration is
//    not a nicety here, it is what keeps the stream valid.
//
// Deliberately JUCE-free, so the fast loop tests it: this is text formatting
// and byte assembly, not audio.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mp {

// What a line shows. A line is one field, optionally behind a fixed label, so
// a panel reads "Temp: Valotti" rather than needing two lines for it.
enum class LcdField {
  Literal,       // the label alone: a title, or a blank spacer
  OrganName,
  Temperament,
  PitchHz,       // "A=440.0 Hz"
  Transpose,     // "+2" / "0" / "-3"
  StopsDrawn,    // how many stops are out
  CrescendoStep, // 0 when the shoe is closed
  CombinationSet,
};

struct LcdLine {
  LcdField field = LcdField::Literal;
  std::string label;  // printed before the value; may be empty
};

struct LcdPanel {
  int hardwareId = 0;   // which display on the console; sent as one byte
  int lineWidth = 32;   // the classic panel; 16 and 20 also exist
  std::vector<LcdLine> lines;
};

// Everything a panel can show, gathered in one place so the caller assembles
// it once per update rather than the panels reaching into the engine.
struct LcdState {
  std::string organName;
  std::string temperament;
  std::string combinationSet;
  double pitchHz = 440.0;
  int transpose = 0;
  int stopsDrawn = 0;
  int crescendoStep = 0;

  bool operator==(const LcdState& o) const {
    return organName == o.organName && temperament == o.temperament &&
           combinationSet == o.combinationSet && pitchHz == o.pitchHz &&
           transpose == o.transpose && stopsDrawn == o.stopsDrawn &&
           crescendoStep == o.crescendoStep;
  }
  bool operator!=(const LcdState& o) const { return !(*this == o); }
};

// One system exclusive message, F0 .. F7 inclusive.
using SysexMessage = std::vector<uint8_t>;

class LcdPanels {
 public:
  void addPanel(LcdPanel p) { panels_.push_back(std::move(p)); rendered_.clear(); }
  void clear() { panels_.clear(); rendered_.clear(); }
  bool empty() const { return panels_.empty(); }
  size_t panelCount() const { return panels_.size(); }
  const std::vector<LcdPanel>& panels() const { return panels_; }

  // The bytes between F0 and the payload. Console-specific; see the header
  // comment. Values above 0x7F are rejected, because they would terminate the
  // message they are supposed to introduce.
  bool setHeader(const std::vector<uint8_t>& bytes);
  const std::vector<uint8_t>& header() const { return header_; }

  // The messages needed to bring the displays up to date. Only lines whose
  // text actually changed are returned, so a panel showing a steady
  // temperament costs nothing per block — which matters, since this runs off
  // the audio thread's state and a console's MIDI in is 31250 baud: a full
  // 32-character refresh is about 12 ms of wire time.
  std::vector<SysexMessage> update(const LcdState& state);

  // Everything, changed or not. For connecting a console mid-session, and for
  // the settings page's "send test" button.
  std::vector<SysexMessage> refreshAll(const LcdState& state);

  // Forget what the displays are believed to show, so the next update() sends
  // everything. For a console plugged in after the organ loaded.
  void forgetDisplayed() { rendered_.clear(); }

  // What a line would read. Exposed for the settings-page preview, so the
  // player sees the truncation before the hardware does.
  static std::string renderLine(const LcdLine& line, const LcdState& state,
                                int width);

  // Seven-bit, printable ASCII. Accented Latin letters lose their accents
  // rather than the message losing its tail.
  static std::string toDisplayAscii(const std::string& utf8);

 private:
  SysexMessage encode(int hardwareId, int lineIndex, const std::string& text) const;

  std::vector<LcdPanel> panels_;
  std::vector<uint8_t> header_{0x7D};
  // Last text sent, per panel and line, so update() can say what moved.
  std::vector<std::vector<std::string>> rendered_;
};

}  // namespace mp
