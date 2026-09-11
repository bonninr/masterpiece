#include "LcdPanel.h"

#include <cstdio>

namespace mp {
namespace {

// Latin-1 and Latin Extended-A, folded to the letters a console panel can
// actually draw. Organ names come from all over Europe and every one of these
// has turned up in a real sample set's title.
const char* foldLatin(uint32_t cp) {
  switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC5: return "A";
    case 0xC4: return "Ae";
    case 0xC6: return "AE";
    case 0xC7: return "C";
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
    case 0xD1: return "N";
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD8: return "O";
    case 0xD6: return "Oe";
    case 0xD9: case 0xDA: case 0xDB: return "U";
    case 0xDC: return "Ue";
    case 0xDD: return "Y";
    case 0xDF: return "ss";
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5: return "a";
    case 0xE4: return "ae";
    case 0xE6: return "ae";
    case 0xE7: return "c";
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
    case 0xF1: return "n";
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF8: return "o";
    case 0xF6: return "oe";
    case 0xF9: case 0xFA: case 0xFB: return "u";
    case 0xFC: return "ue";
    case 0xFD: case 0xFF: return "y";
    // Latin Extended-A, the Polish and Czech letters. "Kraków" is the one
    // that has already cost us an hour once, in a different tool.
    case 0x0104: case 0x0102: return "A";
    case 0x0105: case 0x0103: return "a";
    case 0x0106: case 0x010C: return "C";
    case 0x0107: case 0x010D: return "c";
    case 0x010E: case 0x0110: return "D";
    case 0x010F: case 0x0111: return "d";
    case 0x0118: case 0x011A: return "E";
    case 0x0119: case 0x011B: return "e";
    case 0x0141: return "L";
    case 0x0142: return "l";
    case 0x0143: case 0x0147: return "N";
    case 0x0144: case 0x0148: return "n";
    case 0x0150: return "O";
    case 0x0151: return "o";
    case 0x0158: return "R";
    case 0x0159: return "r";
    case 0x015A: case 0x0160: return "S";
    case 0x015B: case 0x0161: return "s";
    case 0x0164: return "T";
    case 0x0165: return "t";
    case 0x016E: case 0x0170: return "U";
    case 0x016F: case 0x0171: return "u";
    case 0x0179: case 0x017B: case 0x017D: return "Z";
    case 0x017A: case 0x017C: case 0x017E: return "z";
    default: return nullptr;
  }
}

}  // namespace

std::string LcdPanels::toDisplayAscii(const std::string& utf8) {
  std::string out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80) {
      // Control characters would be read as command bytes by some panels and
      // as garbage by the rest.
      out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : ' ');
      ++i;
      continue;
    }
    // Decode one UTF-8 sequence. A malformed byte is dropped rather than
    // passed through, since passing it through is what breaks the message.
    uint32_t cp = 0;
    int len = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
    else { ++i; continue; }

    if (i + static_cast<size_t>(len) > utf8.size()) break;
    bool ok = true;
    for (int k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
      if ((cc & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    i += static_cast<size_t>(len);
    if (!ok) continue;

    if (const char* folded = foldLatin(cp)) out += folded;
    else out.push_back('?');
  }
  return out;
}

std::string LcdPanels::renderLine(const LcdLine& line, const LcdState& state,
                                  int width) {
  if (width < 1) width = 1;
  std::string value;
  char buf[64];
  switch (line.field) {
    case LcdField::Literal:
      break;
    case LcdField::OrganName:
      value = state.organName;
      break;
    case LcdField::Temperament:
      value = state.temperament;
      break;
    case LcdField::CombinationSet:
      value = state.combinationSet;
      break;
    case LcdField::PitchHz:
      std::snprintf(buf, sizeof buf, "A=%.1f Hz", state.pitchHz);
      value = buf;
      break;
    case LcdField::Transpose:
      // The sign is the information. "+2" and "-2" are opposite instructions
      // and a bare "2" is ambiguous at a glance.
      std::snprintf(buf, sizeof buf, "%+d", state.transpose);
      value = state.transpose == 0 ? "0" : buf;
      break;
    case LcdField::StopsDrawn:
      std::snprintf(buf, sizeof buf, "%d", state.stopsDrawn);
      value = buf;
      break;
    case LcdField::CrescendoStep:
      std::snprintf(buf, sizeof buf, "%d", state.crescendoStep);
      value = buf;
      break;
  }

  std::string text = toDisplayAscii(line.label) + toDisplayAscii(value);
  // Truncate, then pad: a panel does not clear what it is not sent, so a
  // shorter value must overwrite the tail of the longer one it replaces.
  if (static_cast<int>(text.size()) > width)
    text.resize(static_cast<size_t>(width));
  text.append(static_cast<size_t>(width) - text.size(), ' ');
  return text;
}

bool LcdPanels::setHeader(const std::vector<uint8_t>& bytes) {
  if (bytes.empty() || bytes.size() > 8) return false;
  for (uint8_t b : bytes)
    if (b > 0x7F) return false;
  header_ = bytes;
  rendered_.clear();  // the console will need telling again
  return true;
}

SysexMessage LcdPanels::encode(int hardwareId, int lineIndex,
                               const std::string& text) const {
  SysexMessage m;
  m.reserve(header_.size() + text.size() + 4);
  m.push_back(0xF0);
  for (uint8_t b : header_) m.push_back(b);
  m.push_back(static_cast<uint8_t>(hardwareId & 0x7F));
  m.push_back(static_cast<uint8_t>(lineIndex & 0x7F));
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    m.push_back(u < 0x80 ? u : static_cast<uint8_t>('?'));
  }
  m.push_back(0xF7);
  return m;
}

std::vector<SysexMessage> LcdPanels::update(const LcdState& state) {
  std::vector<SysexMessage> out;
  rendered_.resize(panels_.size());
  for (size_t p = 0; p < panels_.size(); ++p) {
    const auto& panel = panels_[p];
    auto& cache = rendered_[p];
    cache.resize(panel.lines.size());
    for (size_t l = 0; l < panel.lines.size(); ++l) {
      const std::string text = renderLine(panel.lines[l], state, panel.lineWidth);
      if (cache[l] == text) continue;
      cache[l] = text;
      out.push_back(encode(panel.hardwareId, static_cast<int>(l), text));
    }
  }
  return out;
}

std::vector<SysexMessage> LcdPanels::refreshAll(const LcdState& state) {
  rendered_.clear();
  return update(state);
}

}  // namespace mp
