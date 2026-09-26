#include "GrandOrgueImport.h"
#include "GrandOrgueStockImages.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace mp {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Valid UTF-8, or not. GrandOrgue files come in both UTF-8 and the Western
// code page of whoever wrote them, and a Latin-1 "é" read as UTF-8 is not a
// character at all.
bool isUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
    if (n < 0) return false;
    for (int k = 1; k <= n; ++k) {
      if (i + static_cast<size_t>(k) >= s.size()) return false;
      if ((static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0xC0) != 0x80) return false;
    }
    i += static_cast<size_t>(n) + 1;
  }
  return true;
}

std::string latin1ToUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (unsigned char c : s) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

// The file as GrandOrgue reads it: [Section] headers, Key=Value lines, ';'
// comments. Keys are matched without regard to case, which is what the sets
// in the wild need even where the reader is stricter.
class Ini {
public:
  explicit Ini(const std::string& text) {
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = trim(line);
      if (t.empty() || t[0] == ';' || t[0] == '#') continue;
      if (t.front() == '[') {
        const auto close = t.find(']');
        section = lower(trim(t.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
        continue;
      }
      const auto eq = t.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = lower(trim(t.substr(0, eq)));
      // A later line wins, as in GrandOrgue.
      sections_[section][key] = trim(t.substr(eq + 1));
    }
  }

  bool has(const std::string& section) const { return sections_.count(lower(section)) != 0; }

  std::string str(const std::string& section, const std::string& key,
                  const std::string& fallback = "") const {
    const auto s = sections_.find(lower(section));
    if (s == sections_.end()) return fallback;
    const auto k = s->second.find(lower(key));
    return k == s->second.end() ? fallback : k->second;
  }
  bool hasKey(const std::string& section, const std::string& key) const {
    const auto s = sections_.find(lower(section));
    return s != sections_.end() && s->second.count(lower(key)) != 0;
  }
  int num(const std::string& section, const std::string& key, int fallback) const {
    const std::string v = str(section, key);
    if (v.empty()) return fallback;
    try {
      return std::stoi(v);
    } catch (...) {
      return fallback;
    }
  }
  double real(const std::string& section, const std::string& key, double fallback) const {
    const std::string v = str(section, key);
    if (v.empty()) return fallback;
    try {
      return std::stod(v);
    } catch (...) {
      return fallback;
    }
  }
  bool yes(const std::string& section, const std::string& key, bool fallback) const {
    const std::string v = lower(str(section, key));
    if (v.empty()) return fallback;
    return v == "y" || v == "yes" || v == "true" || v == "1";
  }

private:
  std::map<std::string, std::map<std::string, std::string>> sections_;
};

std::string n3(int n) {
  char b[16];
  std::snprintf(b, sizeof b, "%03d", n);
  return b;
}

// GrandOrgue's level controls are a percentage and a gain in dB, at the
// organ, the windchest group, the rank and the pipe; they multiply.
double levelDb(const Ini& ini, const std::string& section, const std::string& prefix = "") {
  double db = ini.real(section, prefix + "Gain", 0.0);
  const double pct = ini.real(section, prefix + "AmplitudeLevel", 100.0);
  if (pct > 0.0) db += 20.0 * std::log10(pct / 100.0);
  return db;
}

// The pixel size of an image, from its header: PNG, BMP, GIF or JPEG. Zero
// when the file is missing or unreadable. GrandOrgue sizes and spaces keys by
// their bitmaps, so the layout cannot be worked out without them.
std::pair<int, int> imageSize(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {0, 0};
  unsigned char h[32] = {};
  f.read(reinterpret_cast<char*>(h), sizeof h);
  auto be32 = [&](int i) { return (h[i] << 24) | (h[i + 1] << 16) | (h[i + 2] << 8) | h[i + 3]; };
  auto le32 = [&](int i) { return h[i] | (h[i + 1] << 8) | (h[i + 2] << 16) | (h[i + 3] << 24); };
  if (h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G') return {be32(16), be32(20)};
  if (h[0] == 'B' && h[1] == 'M') return {le32(18), std::abs(le32(22))};
  if (h[0] == 'G' && h[1] == 'I' && h[2] == 'F') return {h[6] | (h[7] << 8), h[8] | (h[9] << 8)};
  if (h[0] == 0xFF && h[1] == 0xD8) {
    // JPEG: walk the segments to the frame header.
    f.clear();
    f.seekg(2);
    unsigned char m[9];
    while (f.read(reinterpret_cast<char*>(m), 4)) {
      if (m[0] != 0xFF) break;
      const int len = (m[2] << 8) | m[3];
      if (m[1] >= 0xC0 && m[1] <= 0xCF && m[1] != 0xC4 && m[1] != 0xC8 && m[1] != 0xCC) {
        if (!f.read(reinterpret_cast<char*>(m), 5)) break;
        return {(m[3] << 8) | m[4], (m[1] << 8) | m[2]};
      }
      f.seekg(len - 2, std::ios::cur);
    }
  }
  return {0, 0};
}

// GrandOrgue's colour names, or #RRGGBB.
bool parseColour(const std::string& text, int& r, int& g, int& b) {
  static const std::map<std::string, int> named = {
      {"black", 0x000000},      {"dark blue", 0x000080},    {"dark green", 0x008000},
      {"dark cyan", 0x008080},  {"dark red", 0x800000},     {"dark magenta", 0x800080},
      {"brown", 0x808000},      {"light grey", 0xC0C0C0},   {"dark grey", 0x808080},
      {"blue", 0x0000FF},       {"green", 0x00FF00},        {"cyan", 0x00FFFF},
      {"red", 0xFF0000},        {"magenta", 0xFF00FF},      {"yellow", 0xFFFF00},
      {"white", 0xFFFFFF}};
  int v = -1;
  const std::string t = lower(trim(text));
  const auto it = named.find(t);
  if (it != named.end()) v = it->second;
  else if (t.size() == 7 && t[0] == '#') v = static_cast<int>(std::strtol(t.c_str() + 1, nullptr, 16));
  if (v < 0) return false;
  r = (v >> 16) & 0xFF;
  g = (v >> 8) & 0xFF;
  b = v & 0xFF;
  return true;
}

// Pitch in cents at one level: the tuning and the correction add.
double centsAt(const Ini& ini, const std::string& section, const std::string& prefix = "") {
  return ini.real(section, prefix + "PitchTuning", 0.0) +
         ini.real(section, prefix + "PitchCorrection", 0.0);
}

// The organ id, from the file itself: GrandOrgue declares none. FNV-1a over
// the bytes, folded into the positive range a Hauptwerk id takes, and kept
// clear of the small numbers real sets use.
long long idFromBytes(const std::string& bytes) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return 1000000000ll + static_cast<long long>(h % 1000000000ull);
}

struct Emitter {
  pugi::xml_document doc;
  pugi::xml_node root;
  std::map<std::string, pugi::xml_node> lists;

  Emitter() {
    auto decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";
    root = doc.append_child("Hauptwerk");
    root.append_attribute("FileFormat") = "Organ";
    root.append_attribute("FileFormatVersion") = "7.00";
  }
  pugi::xml_node row(const std::string& table) {
    auto& list = lists[table];
    if (!list) {
      list = root.append_child("ObjectList");
      list.append_attribute("ObjectType") = table.c_str();
    }
    return list.append_child(table.c_str());
  }
  template <typename T>
  static void set(pugi::xml_node r, const char* field, const T& v) {
    std::ostringstream os;
    os << v;
    r.append_child(field).text() = os.str().c_str();
  }
  static void set(pugi::xml_node r, const char* field, const std::string& v) {
    r.append_child(field).text() = v.c_str();
  }
  static void set(pugi::xml_node r, const char* field, const char* v) {
    r.append_child(field).text() = v;
  }
  static void yn(pugi::xml_node r, const char* field, bool v) { set(r, field, v ? "Y" : "N"); }
};

// Ids, allocated per table in ranges that cannot collide and that read back
// as what they are when a log names one.
constexpr int kKeyboardBase = 0;       // keyboard = manual number + 1
constexpr int kRankBase = 1000;        // explicit ranks
constexpr int kStopRankBase = 5000;    // ranks built from a stop's own pipes
constexpr int kAltRankBase = 60000;    // + rank id: the take recorded with the tremulant
constexpr int kStopBase = 2000;
constexpr int kStopSwitchBase = 3000;
constexpr int kCouplerSwitchBase = 4000;
constexpr int kTremulantBase = 100;
constexpr int kTremulantSwitchBase = 6000;
constexpr int kEnclosureBase = 200;
constexpr int kEnclosureControlBase = 700;
constexpr int kUnisonOffSwitchBase = 7000;
constexpr int kGoSwitchBase = 8000;    // GrandOrgue's own [SwitchNNN]
constexpr int kGateBase = 9000;        // switches that compute a Function
constexpr int kGeneralBase = 10000;    // general pistons
constexpr int kDivisionalBase = 11000; // divisional pistons: + manual * 100
constexpr int kSetterGeneralBase = 12000;  // GrandOrgue's own programmable generals
constexpr int kSetterSwitch = 12900;       // its Set button
constexpr int kGeneralCancel = 12950;      // its GC button
constexpr int kReversibleBase = 12500;     // reversible pistons

struct PipeRef {
  int pipeId = 0;
  int windchest = 0;
};

}  // namespace

bool isGrandOrgueDefinition(const std::string& path) {
  const auto dot = path.find_last_of('.');
  return dot != std::string::npos && lower(path.substr(dot)) == ".organ";
}

GrandOrgueImportReport convertGrandOrgueText(const std::string& rawText, const std::string& organRoot) {
  GrandOrgueImportReport rep;
  std::string text = rawText;
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    text.erase(0, 3);
  if (!isUtf8(text)) text = latin1ToUtf8(text);

  const Ini ini(text);
  if (!ini.has("Organ")) {
    rep.error = "not a GrandOrgue organ definition: no [Organ] section";
    return rep;
  }

  Emitter out;
  auto note = [&rep](const std::string& s) {
    if (std::find(rep.notes.begin(), rep.notes.end(), s) == rep.notes.end()) rep.notes.push_back(s);
  };

  // ---- the organ ----------------------------------------------------------
  const std::string church = ini.str("Organ", "ChurchName", "GrandOrgue organ");
  {
    auto g = out.row("_General");
    Emitter::set(g, "Identification_Name", church);
    Emitter::set(g, "Identification_UniqueOrganID", idFromBytes(rawText));
    Emitter::set(g, "OrganInfo_Location", ini.str("Organ", "ChurchAddress"));
    Emitter::set(g, "OrganInfo_Builder", ini.str("Organ", "OrganBuilder"));
    Emitter::set(g, "OrganInfo_BuildDate", ini.str("Organ", "OrganBuildDate"));
    Emitter::set(g, "OrganInfo_Comments", ini.str("Organ", "OrganComments"));
    Emitter::set(g, "AudioEngine_BasePitchHz", 440);
    // The organ's own level is its output trim, as a producer's calibration
    // is elsewhere, not a gain on every pipe.
    const double trim = levelDb(ini, "Organ");
    if (trim != 0.0) Emitter::set(g, "AudioOut_AmplitudeLevelAdjustDecibels", trim);
  }
  const double organCents = centsAt(ini, "Organ");

  // ---- windchest groups: which enclosures and tremulants reach a pipe ------
  const int windchests = ini.num("Organ", "NumberOfWindchestGroups", 0);
  std::map<int, std::vector<int>> chestEnclosures, chestTremulants;
  std::map<int, double> chestDb, chestCents;
  for (int w = 1; w <= windchests; ++w) {
    const std::string sec = "WindchestGroup" + n3(w);
    for (int e = 1; e <= ini.num(sec, "NumberOfEnclosures", 0); ++e)
      chestEnclosures[w].push_back(ini.num(sec, "Enclosure" + n3(e), 0));
    for (int t = 1; t <= ini.num(sec, "NumberOfTremulants", 0); ++t)
      chestTremulants[w].push_back(ini.num(sec, "Tremulant" + n3(t), 0));
    chestDb[w] = levelDb(ini, sec);
    chestCents[w] = centsAt(ini, sec);
  }

  // ---- samples, pipes, ranks ------------------------------------------------
  std::map<std::string, int> sampleIds;  // relative file -> SampleID
  int nextPipeId = 1, nextUniqueId = 1;
  std::map<int, PipeRef> pipeOfRankPipe;  // rankId * 1000 + index -> pipe
  std::map<int, std::vector<int>> rankPipeIds;
  std::map<int, int> rankFirstMidi;

  // GrandOrgue plays a sample at the pitch it was recorded at, moved only by
  // the definition's PitchTuning; the engine here retunes every sample to its
  // pipe. So each sample is declared to BE its pipe's untuned pitch, which
  // makes the engine's ratio exactly the definition's tuning -- the same
  // playback GrandOrgue gives. A file shared by pipes of different pitch is
  // a sample per pitch.
  auto sampleFor = [&](const std::string& file, double pitchHz) {
    std::string rel = file;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    char tag[32];
    std::snprintf(tag, sizeof tag, "@%.4f", pitchHz);
    const std::string key = rel + tag;
    const auto it = sampleIds.find(key);
    if (it != sampleIds.end()) return it->second;
    const int id = static_cast<int>(sampleIds.size()) + 1;
    sampleIds[key] = id;
    auto s = out.row("Sample");
    Emitter::set(s, "SampleID", id);
    Emitter::set(s, "InstallationPackageID", 0);
    Emitter::set(s, "SampleFilename", rel);
    Emitter::set(s, "Pitch_SpecificationMethodCode", 4);
    Emitter::set(s, "Pitch_ExactSamplePitch", pitchHz);
    return id;
  };

  // A stop that carries its own pipes has no FirstMidiNoteNumber of its own:
  // GrandOrgue places its first accessible pipe under the stop's first key.
  const int manualCount = ini.num("Organ", "NumberOfManuals", 0);
  const bool hasPedals = ini.yes("Organ", "HasPedals", false);
  std::map<std::string, int> implicitFirstMidi;  // lower-case stop section
  for (int m = hasPedals ? 0 : 1; m <= manualCount; ++m) {
    const std::string msec = "Manual" + n3(m);
    const int manualFirst = ini.num(msec, "FirstAccessibleKeyMIDINoteNumber", 36) -
                            ini.num(msec, "FirstAccessibleKeyLogicalKeyNumber", 1) + 1;
    for (int st = 1; st <= ini.num(msec, "NumberOfStops", 0); ++st) {
      const std::string ssec = "Stop" + n3(ini.num(msec, "Stop" + n3(st), 0));
      if (ini.num(ssec, "NumberOfRanks", 0) == 0)
        implicitFirstMidi[lower(ssec)] =
            manualFirst - ini.num(ssec, "FirstAccessiblePipeLogicalPipeNumber", 1) +
            ini.num(ssec, "FirstAccessiblePipeLogicalKeyNumber", 1);
    }
  }
  auto firstMidiOf = [&](const std::string& sec) {
    const auto it = implicitFirstMidi.find(lower(sec));
    return ini.num(sec, "FirstMidiNoteNumber", it == implicitFirstMidi.end() ? 36 : it->second);
  };

  // Where a pipe's sound is defined. Usually its own line; a REF:MMM:SSS:PPP
  // pipe is pipe PPP of the first rank of stop SSS on manual MMM, and plays
  // that pipe as it is -- its samples, level, tuning and windchest.
  auto pipeSource = [&](std::string sec, std::string key, int& index) {
    for (int depth = 0; depth < 8; ++depth) {
      const std::string v = lower(ini.str(sec, key));
      if (v.rfind("ref:", 0) != 0) return std::make_pair(sec, key);
      int manual = -1, stop = 0, pipe = 0;
      if (std::sscanf(v.c_str() + 4, "%d:%d:%d", &manual, &stop, &pipe) != 3)
        return std::make_pair(std::string(), std::string());
      const std::string ssec = "Stop" + n3(ini.num("Manual" + n3(manual), "Stop" + n3(stop), 0));
      sec = ini.num(ssec, "NumberOfRanks", 0) > 0 ? "Rank" + n3(ini.num(ssec, "Rank001", 0)) : ssec;
      key = "Pipe" + n3(pipe);
      index = pipe;
    }
    return std::make_pair(std::string(), std::string());
  };

  // One rank: a [RankNNN], or a stop that carries its pipes itself.
  // A rank whose pipes were also recorded with a tremulant running keeps
  // those takes apart: `tremulantTake` 0 builds the rank from the takes
  // without (IsTremulant=0) and the unmarked ones, 1 the twin from the takes
  // with it (IsTremulant=1) and the unmarked ones. The twin is swapped in by
  // the tremulant's switch.
  auto buildRank = [&](const std::string& rankSec, int rankId, int palletSwitch = 0,
                       int tremulantTake = 0) {
    auto r = out.row("Rank");
    Emitter::set(r, "RankID", rankId);
    Emitter::set(r, "Name", ini.str(rankSec, "Name", rankSec));
    rankFirstMidi[rankId] = firstMidiOf(rankSec);
    const int count = ini.num(rankSec, "NumberOfLogicalPipes", 0);
    auto& ids = rankPipeIds[rankId];
    for (int p = 1; p <= count; ++p) {
      ids.push_back(0);
      int index = p;
      const auto source = pipeSource(rankSec, "Pipe" + n3(p), index);
      const std::string sec = source.first, key = source.second;
      const std::string file = sec.empty() ? std::string() : ini.str(sec, key);
      if (sec.empty()) note("a REF: pipe that points nowhere is silent");
      if (file.empty() || lower(file) == "dummy") continue;
      if (sec != rankSec) note("REF: pipes play the pipe they name as a pipe of their own");
      const int pipeId = nextPipeId++;
      ids.back() = pipeId;
      // The pipe's place is in THIS rank, whatever it borrows: a REF pipe
      // takes the slot it is written in and only sounds like the pipe it
      // names. Its pitch is unaffected -- the sample is declared at this
      // same nominal pitch below, so it plays as recorded plus its tuning.
      const int midi = firstMidiOf(rankSec) + p - 1;
      const int harmonic = ini.num(sec, key + "HarmonicNumber", ini.num(sec, "HarmonicNumber", 8));
      const int chest = ini.num(sec, key + "WindchestGroup", ini.num(sec, "WindchestGroup", 1));
      const double rankDb = levelDb(ini, sec);
      const bool rankPercussive = ini.yes(sec, "Percussive", false);
      pipeOfRankPipe[rankId * 1000 + p] = {pipeId, chest};
      const double cents = organCents + chestCents[chest] + centsAt(ini, sec) + centsAt(ini, sec, key);
      auto pipe = out.row("Pipe_SoundEngine01");
      Emitter::set(pipe, "PipeID", pipeId);
      Emitter::set(pipe, "RankID", rankId);
      Emitter::set(pipe, "NormalMIDINoteNumber", midi);
      Emitter::set(pipe, "Pitch_Tempered_RankBasePitch64ftHarmonicNum", harmonic);
      // An effect stop's pipe sounds while its switch is engaged, not by key.
      if (palletSwitch) Emitter::set(pipe, "ControllingPalletSwitchID", palletSwitch);
      if (cents != 0.0) Emitter::set(pipe, "Pitch_Tempered_BaseTuningDeviation", cents);
      const double nominalHz =
          440.0 * std::pow(2.0, (midi - 69.0) / 12.0) * (harmonic > 0 ? harmonic / 8.0 : 1.0);

      auto layer = out.row("Pipe_SoundEngine01_Layer");
      Emitter::set(layer, "LayerID", pipeId);
      Emitter::set(layer, "PipeID", pipeId);
      const double db = chestDb[chest] + rankDb + levelDb(ini, sec, key);
      if (db != 0.0) Emitter::set(layer, "AmpLvl_LevelAdjustDecibels", db);

      const bool percussive = ini.yes(sec, key + "Percussive", rankPercussive);
      // The attacks: the pipe's own file, then PipeNNNAttackNNN. A velocity
      // threshold in GrandOrgue is the lowest velocity an attack is used
      // for; in Hauptwerk it is the highest, so they are sorted and each one
      // runs up to where the next begins.
      struct Attack {
        std::string file;
        int minVelocity = 0;
        int maxRestMs = -1;  // GrandOrgue's MaxTimeSinceLastRelease; -1 any
        std::string prefix;
        int highestVelocity = 127;
        int minRestMs = 0;
      };
      std::vector<Attack> attacks;
      // Whether a take belongs in this rank: the other tremulant state's
      // takes are the twin's.
      auto inThisTake = [&](const std::string& prefix) {
        const int t = ini.num(sec, prefix + "IsTremulant", -1);
        return t < 0 || t == tremulantTake;
      };
      auto addAttack = [&](const std::string& prefix, const std::string& f) {
        if (f.empty() || !inThisTake(prefix)) return;
        if (ini.num(sec, prefix + "LoopCount", 0) > 0)
          note("loops declared in the definition are not used; each file's own loop is");
        attacks.push_back({f, ini.num(sec, prefix + "AttackVelocity", 0),
                           ini.num(sec, prefix + "MaxTimeSinceLastRelease", -1), prefix});
      };
      addAttack(key, file);
      for (int a = 1; a <= ini.num(sec, key + "AttackCount", 0); ++a) {
        const std::string pre = key + "Attack" + n3(a);
        addAttack(pre, ini.str(sec, pre));
      }
      // GrandOrgue picks an attack by the lowest velocity it is for and the
      // longest rest it may follow. The attack table is read first-match,
      // each row with the highest velocity and the shortest rest it takes:
      // so by velocity band, lowest first, and within a band the longest rest
      // first, each row taking only rests longer than the next row allows.
      std::stable_sort(attacks.begin(), attacks.end(), [](const Attack& a, const Attack& b) {
        if (a.minVelocity != b.minVelocity) return a.minVelocity < b.minVelocity;
        const long long ra = a.maxRestMs < 0 ? LLONG_MAX : a.maxRestMs;
        const long long rb = b.maxRestMs < 0 ? LLONG_MAX : b.maxRestMs;
        return ra > rb;
      });
      for (size_t i = 0; i < attacks.size(); ++i) {
        size_t next = i + 1;
        while (next < attacks.size() && attacks[next].minVelocity == attacks[i].minVelocity) ++next;
        attacks[i].highestVelocity =
            next < attacks.size() ? std::max(0, attacks[next].minVelocity - 1) : 127;
        const bool sameBandFollows =
            i + 1 < attacks.size() && attacks[i + 1].minVelocity == attacks[i].minVelocity;
        attacks[i].minRestMs = sameBandFollows && attacks[i + 1].maxRestMs >= 0
                                   ? attacks[i + 1].maxRestMs + 1
                                   : 0;
      }
      for (size_t i = 0; i < attacks.size(); ++i) {
        const int sampleId = sampleFor(attacks[i].file, nominalHz);
        const int highest = attacks[i].highestVelocity;
        auto at = out.row("Pipe_SoundEngine01_AttackSample");
        const int attackUid = nextUniqueId++;
        Emitter::set(at, "UniqueID", attackUid);
        Emitter::set(at, "LayerID", pipeId);
        Emitter::set(at, "SampleID", sampleId);
        Emitter::set(at, "AttackSelCriteria_HighestVelocity", highest);
        if (attacks[i].minRestMs > 0)
          Emitter::set(at, "AttackSelCriteria_MinTimeSincePrevPipeCloseMs", attacks[i].minRestMs);
        const int xfade = ini.num(sec, attacks[i].prefix + "LoopCrossfadeLength", 0);
        if (xfade > 0) Emitter::set(at, "LoopCrossfadeLengthInSrcSampleMs", xfade);
        // Unless the pipe is percussive, its release is the end of the same
        // file, from the cue point on.
        if (!percussive && ini.yes(sec, attacks[i].prefix + "LoadRelease", true)) {
          auto rel = out.row("Pipe_SoundEngine01_ReleaseSample");
          Emitter::set(rel, "UniqueID", nextUniqueId++);
          Emitter::set(rel, "LayerID", pipeId);
          Emitter::set(rel, "SampleID", sampleId);
          Emitter::set(rel, "LoadSampleRange_StartPositionTypeCode", 1);
          Emitter::set(rel, "AttackSelCriteria_HighestVelocity", highest);
          Emitter::set(rel, "ReleaseSelCriteria_PreferThisRelForAttackID", attackUid);
          const int rx = ini.num(sec, attacks[i].prefix + "ReleaseCrossfadeLength", 0);
          if (rx > 0) Emitter::set(rel, "ReleaseCrossfadeLengthMs", rx);
        }
      }
      // Separate release files.
      for (int r = 1; r <= ini.num(sec, key + "ReleaseCount", 0); ++r) {
        const std::string pre = key + "Release" + n3(r);
        const std::string f = ini.str(sec, pre);
        if (f.empty() || !inThisTake(pre)) continue;
        auto rel = out.row("Pipe_SoundEngine01_ReleaseSample");
        Emitter::set(rel, "UniqueID", nextUniqueId++);
        Emitter::set(rel, "LayerID", pipeId);
        Emitter::set(rel, "SampleID", sampleFor(f, nominalHz));
        const int maxMs = ini.num(sec, pre + "MaxKeyPressTime", -1);
        if (maxMs >= 0) Emitter::set(rel, "ReleaseSelCriteria_LatestKeyReleaseTimeMs", maxMs);
      }
      if (ini.num(sec, key + "MIDIKeyNumber", -1) >= 0)
        note("MIDIKeyNumber only matters to retuned temperaments; pipes play as recorded plus their tuning");
    }
  };

  // Whether a rank (or a stop carrying its pipes) has takes recorded with a
  // tremulant, which makes it a twin of ranks.
  auto hasTremulantTakes = [&](const std::string& rankSec) {
    for (int p = 1; p <= ini.num(rankSec, "NumberOfLogicalPipes", 0); ++p) {
      const std::string key = "Pipe" + n3(p);
      if (ini.num(rankSec, key + "IsTremulant", -1) == 1) return true;
      for (int a = 1; a <= ini.num(rankSec, key + "AttackCount", 0); ++a)
        if (ini.num(rankSec, key + "Attack" + n3(a) + "IsTremulant", -1) == 1) return true;
      for (int r = 1; r <= ini.num(rankSec, key + "ReleaseCount", 0); ++r)
        if (ini.num(rankSec, key + "Release" + n3(r) + "IsTremulant", -1) == 1) return true;
    }
    return false;
  };
  std::map<int, int> alternateOf;             // rank id -> its tremulant twin
  std::map<int, std::string> rankSectionOf;   // for finding its windchest
  auto buildWithTwin = [&](const std::string& rankSec, int rankId, int palletSwitch = 0) {
    buildRank(rankSec, rankId, palletSwitch);
    rankSectionOf[rankId] = rankSec;
    if (hasTremulantTakes(rankSec)) {
      buildRank(rankSec, kAltRankBase + rankId, palletSwitch, 1);
      alternateOf[rankId] = kAltRankBase + rankId;
    }
  };
  const int rankCount = ini.num("Organ", "NumberOfRanks", 0);
  for (int r = 1; r <= rankCount; ++r) buildWithTwin("Rank" + n3(r), kRankBase + r);

  // ---- switches -------------------------------------------------------------
  // GrandOrgue drives a stop, coupler or tremulant either from its own
  // drawstop or from a Function (And, Or, Not...) of [SwitchNNN] objects. A
  // function becomes a gate: a switch set by linkages, which re-evaluate when
  // any of their inputs moves. Several linkages into one switch are an Or.
  std::map<int, pugi::xml_node> switchRows;
  auto emitSwitch = [&](int id, const std::string& name, bool engaged) {
    auto sw = out.row("Switch");
    switchRows[id] = sw;
    Emitter::set(sw, "SwitchID", id);
    Emitter::set(sw, "Name", name);
    Emitter::yn(sw, "DefaultToEngaged", engaged);
  };
  auto link = [&](int source, int dest, int condition, bool sourceEngaged, int engageCode = 1,
                  int disengageCode = 2) {
    auto l = out.row("SwitchLinkage");
    Emitter::set(l, "SourceSwitchID", source);
    Emitter::set(l, "DestSwitchID", dest);
    if (condition) {
      Emitter::set(l, "ConditionSwitchID", condition);
      Emitter::yn(l, "ConditionSwitchLinkIfEngaged", true);
    }
    Emitter::yn(l, "SourceSwitchLinkIfEngaged", sourceEngaged);
    Emitter::set(l, "EngageLinkActionCode", engageCode);
    Emitter::set(l, "DisengageLinkActionCode", disengageCode);
  };
  int gates = 0;
  std::map<std::string, int> gateMemo;
  std::function<int(const std::string&, const std::vector<int>&)> gate =
      [&](const std::string& fn, const std::vector<int>& in) -> int {
    std::string key = fn;
    for (int i : in) key += ":" + std::to_string(i);
    const auto hit = gateMemo.find(key);
    if (hit != gateMemo.end()) return hit->second;
    auto fresh = [&] {
      const int id = kGateBase + ++gates;
      emitSwitch(id, "logic " + key, false);
      return id;
    };
    int g = 0;
    if ((fn == "and" || fn == "or") && in.size() == 1) {
      g = in[0];
    } else if (fn == "and") {
      g = in[0];
      for (size_t i = 1; i < in.size(); ++i) {
        const int next = fresh();
        link(g, next, in[i], true);
        g = next;
      }
    } else if (fn == "not" || fn == "nand" || fn == "nor") {
      const int inner = in.size() == 1 ? in[0] : gate(fn == "nor" ? "or" : "and", in);
      g = fresh();
      link(inner, g, 0, false);
    } else {
      if (fn != "or") note("switch function '" + fn + "' is treated as Or");
      g = fresh();
      for (int i : in) link(i, g, 0, true);
    }
    gateMemo[key] = g;
    return g;
  };
  auto functionOf = [&](const std::string& sec, std::vector<int>& in) {
    const int n = ini.num(sec, "SwitchCount", 0);
    for (int i = 1; i <= n; ++i) in.push_back(kGoSwitchBase + ini.num(sec, "Switch" + n3(i), 0));
    return lower(ini.str(sec, "Function", "And"));
  };
  const int switchCount = ini.num("Organ", "NumberOfSwitches", 0);
  for (int n = 1; n <= switchCount; ++n) {
    const std::string sec = "Switch" + n3(n);
    emitSwitch(kGoSwitchBase + n, ini.str(sec, "Name", sec), ini.yes(sec, "DefaultToEngaged", false));
    std::vector<int> in;
    const std::string fn = functionOf(sec, in);
    if (!in.empty()) link(gate(fn, in), kGoSwitchBase + n, 0, true);
  }
  // The switch that controls an object: its own, or its function's gate.
  std::map<std::string, int> controlMemo;
  auto controlFor = [&](const std::string& sec, int ownId) {
    const auto hit = controlMemo.find(lower(sec));
    if (hit != controlMemo.end()) return hit->second;
    std::vector<int> in;
    const std::string fn = functionOf(sec, in);
    int id = ownId;
    if (in.empty()) emitSwitch(ownId, ini.str(sec, "Name", sec), ini.yes(sec, "DefaultToEngaged", false));
    else id = gate(fn, in);
    controlMemo[lower(sec)] = id;
    return id;
  };
  auto hasOwnSwitch = [&](const std::string& sec) { return ini.num(sec, "SwitchCount", 0) == 0; };

  // ---- tremulants, enclosures --------------------------------------------
  const int tremCount = ini.num("Organ", "NumberOfTremulants", 0);
  for (int t = 1; t <= tremCount; ++t) {
    const std::string sec = "Tremulant" + n3(t);
    const int sw = controlFor(sec, kTremulantSwitchBase + t);
    auto tr = out.row("Tremulant");
    Emitter::set(tr, "TremulantID", kTremulantBase + t);
    Emitter::set(tr, "Name", ini.str(sec, "Name", sec));
    Emitter::set(tr, "ControllingSwitchID", sw);
    const double period = std::max(20.0, ini.real(sec, "Period", 250.0));
    Emitter::set(tr, "FrequencyWhenEngagedHz", 1000.0 / period);
    auto wf = out.row("TremulantWaveform");
    Emitter::set(wf, "TremulantWaveformID", kTremulantBase + t);
    Emitter::set(wf, "TremulantID", kTremulantBase + t);
    // A wave tremulant is the recordings made with it running, swapped in
    // by its switch; it modulates nothing itself.
  }
  const int encCount = ini.num("Organ", "NumberOfEnclosures", 0);
  std::map<int, pugi::xml_node> enclosureControls;
  for (int e = 1; e <= encCount; ++e) {
    const std::string sec = "Enclosure" + n3(e);
    auto cc = out.row("ContinuousControl");
    enclosureControls[e] = cc;
    Emitter::set(cc, "ControlID", kEnclosureControlBase + e);
    Emitter::set(cc, "Name", ini.str(sec, "Name", sec));
    Emitter::set(cc, "DefaultValue", 127);
    auto en = out.row("Enclosure");
    Emitter::set(en, "EnclosureID", kEnclosureBase + e);
    Emitter::set(en, "Name", ini.str(sec, "Name", sec));
    Emitter::set(en, "ShutterPositionContinuousControlID", kEnclosureControlBase + e);
  }
  // Every pipe on an enclosed or tremulated windchest group, now that the
  // pipes of the explicit ranks exist; a stop's own pipes add theirs below.
  std::set<std::pair<int, int>> enclosed;  // (enclosure, pipe)
  auto wirePipe = [&](const PipeRef& p) {
    for (int e : chestEnclosures[p.windchest]) {
      if (e < 1 || e > encCount || !enclosed.insert({e, p.pipeId}).second) continue;
      const double minPct = std::clamp(ini.real("Enclosure" + n3(e), "AmpMinimumLevel", 1.0), 0.1, 100.0);
      auto ep = out.row("EnclosurePipe");
      Emitter::set(ep, "EnclosureID", kEnclosureBase + e);
      Emitter::set(ep, "PipeID", p.pipeId);
      Emitter::set(ep, "FiltParamWhenClsd_OverallAttnDb", -20.0 * std::log10(minPct / 100.0));
      // No filter: GrandOrgue's box lowers the level only.
      Emitter::set(ep, "FiltParamWhenClsd_MaxFreqHz", 20000);
      Emitter::set(ep, "FiltParamWhenOpen_MaxFreqHz", 20000);
    }
    for (int t : chestTremulants[p.windchest]) {
      if (t < 1 || t > tremCount) continue;
      if (lower(ini.str("Tremulant" + n3(t), "TremulantType", "Synth")) == "wave") continue;
      const double depth = ini.real("Tremulant" + n3(t), "AmpModDepth", 0.0);
      auto tp = out.row("TremulantWaveformPipe");
      Emitter::set(tp, "PipeID", p.pipeId);
      Emitter::set(tp, "TremulantWaveformID", kTremulantBase + t);
      Emitter::set(tp, "AmplitudeModDepthAdjustDecibels", 20.0 * std::log10(1.0 + depth / 100.0));
      Emitter::set(tp, "PitchModDepthAdjustPercent", 0);
    }
  };
  for (const auto& [key, p] : pipeOfRankPipe) wirePipe(p);

  // ---- manuals, stops, couplers -------------------------------------------
  const int manuals = manualCount;
  const bool pedals = hasPedals;
  struct ManualInfo {
    int kb = 0;
    int firstMidi = 36;  // MIDI note of logical key 1
    int keys = 61;
  };
  std::map<int, ManualInfo> manualInfo;
  for (int m = pedals ? 0 : 1; m <= manuals; ++m) {
    const std::string sec = "Manual" + n3(m);
    if (!ini.has(sec)) continue;
    ManualInfo info;
    info.kb = kKeyboardBase + m + 1;
    const int firstAccessibleMidi = ini.num(sec, "FirstAccessibleKeyMIDINoteNumber", 36);
    const int firstAccessibleKey = ini.num(sec, "FirstAccessibleKeyLogicalKeyNumber", 1);
    info.firstMidi = firstAccessibleMidi - (firstAccessibleKey - 1);
    info.keys = ini.num(sec, "NumberOfLogicalKeys", ini.num(sec, "NumberOfAccessibleKeys", 61));
    manualInfo[m] = info;
    const std::string name = ini.str(sec, "Name", sec);

    auto div = out.row("Division");
    Emitter::set(div, "DivisionID", info.kb);
    Emitter::set(div, "Name", name);
    auto kb = out.row("Keyboard");
    Emitter::set(kb, "KeyboardID", info.kb);
    Emitter::set(kb, "Name", name);
    // The usual channel convention: the pedal on 1, the first manual on 2,
    // whether or not the organ has a pedal.
    Emitter::set(kb, "DefaultInputOutputKeyboardAsgnCode", m + 1);
    Emitter::set(kb, "Hint_PrimaryAssociatedDivisionID", info.kb);
    Emitter::set(kb, "KeyGen_NumberOfKeys", ini.num(sec, "NumberOfAccessibleKeys", info.keys));
    Emitter::set(kb, "KeyGen_MIDINoteNumberOfFirstKey", firstAccessibleMidi);
  }

  for (const auto& [m, info] : manualInfo) {
    const std::string sec = "Manual" + n3(m);

    // A unison-off coupler on this manual silences its own division while
    // it is drawn, so the manual's own key action is conditional on it.
    int unisonOffSwitch = 0;
    for (int c = 1; c <= ini.num(sec, "NumberOfCouplers", 0); ++c) {
      const int cNo = ini.num(sec, "Coupler" + n3(c), 0);
      const std::string csec = "Coupler" + n3(cNo);
      if (ini.yes(csec, "UnisonOff", false)) unisonOffSwitch = controlFor(csec, kUnisonOffSwitchBase + cNo);
    }
    auto own = out.row("KeyAction");
    Emitter::set(own, "SourceKeyboardID", info.kb);
    Emitter::yn(own, "DestIsKeyboardNotDivision", false);
    Emitter::set(own, "DestDivisionID", info.kb);
    Emitter::set(own, "ActionTypeCode", 1);
    Emitter::set(own, "ActionEffectCode", 1);
    Emitter::set(own, "MIDINoteNumOfFirstSourceKey", info.firstMidi);
    Emitter::set(own, "NumberOfKeys", info.keys);
    Emitter::set(own, "MIDINoteNumberIncrement", 0);
    if (unisonOffSwitch) {
      Emitter::set(own, "ConditionSwitchID", unisonOffSwitch);
      Emitter::yn(own, "ConditionSwitchLinkIfEngaged", false);
    }

    // Stops.
    for (int s = 1; s <= ini.num(sec, "NumberOfStops", 0); ++s) {
      const int stopNo = ini.num(sec, "Stop" + n3(s), 0);
      const std::string ssec = "Stop" + n3(stopNo);
      if (!ini.has(ssec)) continue;
      const int stopId = kStopBase + stopNo;
      const int sw = controlFor(ssec, kStopSwitchBase + stopNo);
      const int firstKey = ini.num(ssec, "FirstAccessiblePipeLogicalKeyNumber", 1);
      const int firstPipe = ini.num(ssec, "FirstAccessiblePipeLogicalPipeNumber", 1);
      const int accessible = ini.num(ssec, "NumberOfAccessiblePipes", info.keys);
      const int ranksInStop = ini.num(ssec, "NumberOfRanks", 0);

      // A stop of a single pipe is an effect in GrandOrgue: not played from
      // the keys, it sounds while the stop is on -- a blower, a stop action,
      // a coupler's clack. Its pipe opens with the switch, like a pallet.
      if (ranksInStop == 0 && accessible == 1 && ini.num(ssec, "NumberOfLogicalPipes", 0) == 1) {
        const int rankId = kStopRankBase + stopNo;
        if (!rankPipeIds.count(rankId)) {
          buildRank(ssec, rankId, sw);
          for (const auto& [k, p] : pipeOfRankPipe)
            if (k / 1000 == rankId) wirePipe(p);
        }
        continue;
      }

      auto st = out.row("Stop");
      Emitter::set(st, "StopID", stopId);
      Emitter::set(st, "Name", ini.str(ssec, "Name", ssec));
      Emitter::set(st, "DivisionID", info.kb);
      Emitter::set(st, "ControllingSwitchID", sw);

      auto mapRank = [&](int rankId, int keyStart, int pipeStart, int count) {
        auto sr = out.row("StopRank");
        Emitter::set(sr, "StopID", stopId);
        Emitter::set(sr, "RankID", rankId);
        const int keyMidi = info.firstMidi + keyStart - 1;
        const int pipeMidi = rankFirstMidi[rankId] + pipeStart - 1;
        Emitter::set(sr, "MIDINoteNumOfFirstMappedDivisionInputNode", keyMidi);
        Emitter::set(sr, "NumberOfMappedDivisionInputNodes", count);
        Emitter::set(sr, "MIDINoteNumIncrementFromDivisionToRank", pipeMidi - keyMidi);
        // The tremulant take, swapped in by the wave tremulant on the rank's
        // windchest, re-sounding held notes as GrandOrgue does.
        const auto alt = alternateOf.find(rankId);
        if (alt != alternateOf.end()) {
          const int chest = ini.num(rankSectionOf[rankId], "WindchestGroup", 1);
          for (int t : chestTremulants[chest])
            if (t >= 1 && t <= tremCount &&
                lower(ini.str("Tremulant" + n3(t), "TremulantType", "Synth")) == "wave") {
              Emitter::set(sr, "AlternateRankID", alt->second);
              Emitter::set(sr, "SwitchIDToSwitchToAlternateRank",
                           controlFor("Tremulant" + n3(t), kTremulantSwitchBase + t));
              Emitter::yn(sr, "RetriggerNotesWhenSwitchingBetweenNormalAndAlternateRanks", true);
              break;
            }
        }
      };
      if (ranksInStop == 0) {
        // The stop carries its pipes itself: it is its own rank.
        const int rankId = kStopRankBase + stopNo;
        if (!rankPipeIds.count(rankId)) {
          buildWithTwin(ssec, rankId);
          for (const auto& [k, p] : pipeOfRankPipe)
            if (k / 1000 == rankId || k / 1000 == kAltRankBase + rankId) wirePipe(p);
        }
        mapRank(rankId, firstKey, firstPipe, accessible);
      } else {
        for (int r = 1; r <= ranksInStop; ++r) {
          const std::string rk = "Rank" + n3(r);
          const int rankNo = ini.num(ssec, rk, 0);
          const int rankId = kRankBase + rankNo;
          if (!rankPipeIds.count(rankId)) continue;
          // GrandOrgue's defaults: the rank from its first pipe, as far as it
          // goes, starting under the stop's first key.
          const int rankKey = ini.num(ssec, rk + "FirstAccessibleKeyNumber", 1);
          const int keyStart = firstKey + rankKey - 1;
          const int pipeStart = ini.num(ssec, rk + "FirstPipeNumber", 1);
          const int rankPipes = static_cast<int>(rankPipeIds[rankId].size());
          const int count = std::min(ini.num(ssec, rk + "PipeCount", rankPipes - pipeStart + 1),
                                     accessible - rankKey + 1);
          mapRank(rankId, keyStart, pipeStart, count);
        }
      }
    }

    // Couplers: to another manual's division, or this one's at an interval.
    for (int c = 1; c <= ini.num(sec, "NumberOfCouplers", 0); ++c) {
      const int cNo = ini.num(sec, "Coupler" + n3(c), 0);
      const std::string csec = "Coupler" + n3(cNo);
      if (!ini.has(csec)) continue;
      const bool unisonOff = ini.yes(csec, "UnisonOff", false);
      const int sw = controlFor(csec, unisonOff ? kUnisonOffSwitchBase + cNo : kCouplerSwitchBase + cNo);
      // A stop row makes a coupler with its own drawstop a listed control
      // like the rest; it sounds nothing itself. One driven by switches is
      // drawn as those switches.
      if (hasOwnSwitch(csec)) {
        auto st = out.row("Stop");
        Emitter::set(st, "StopID", kStopBase + 500 + cNo);
        Emitter::set(st, "Name", ini.str(csec, "Name", csec));
        Emitter::set(st, "DivisionID", info.kb);
        Emitter::set(st, "ControllingSwitchID", sw);
      }
      if (unisonOff) continue;
      const int dest = ini.num(csec, "DestinationManual", m);
      const auto dIt = manualInfo.find(dest);
      if (dIt == manualInfo.end()) continue;
      const std::string type = lower(ini.str(csec, "CouplerType", "Normal"));
      if (type == "bass" || type == "melody")
        note("bass and melody couplers couple every key, like normal ones");
      // A coupler that feeds the destination's own couplers reaches its
      // keyboard, whose key flow carries on; one that does not reaches its
      // pipes alone. GrandOrgue names five kinds of coupler it may pass
      // through; the key flow here has no such kinds, so any of them lets
      // the coupled notes on. A manual coupled to itself never does, or the
      // notes would come round again.
      const bool feedsOn =
          dest != m && (ini.yes(csec, "CoupleToSubsequentUnisonIntermanualCouplers", false) ||
                        ini.yes(csec, "CoupleToSubsequentUpwardIntermanualCouplers", false) ||
                        ini.yes(csec, "CoupleToSubsequentDownwardIntermanualCouplers", false) ||
                        ini.yes(csec, "CoupleToSubsequentUpwardIntramanualCouplers", false) ||
                        ini.yes(csec, "CoupleToSubsequentDownwardIntramanualCouplers", false));
      // Only the keys the coupler covers.
      const int first = std::max(info.firstMidi, ini.num(csec, "FirstMIDINoteNumber", 0));
      const int last = std::min(info.firstMidi + info.keys, ini.num(csec, "FirstMIDINoteNumber", 0) +
                                                                ini.num(csec, "NumberOfKeys", 127));
      if (last <= first) continue;
      auto ka = out.row("KeyAction");
      Emitter::set(ka, "SourceKeyboardID", info.kb);
      Emitter::yn(ka, "DestIsKeyboardNotDivision", feedsOn);
      if (feedsOn) Emitter::set(ka, "DestKeyboardID", dIt->second.kb);
      else Emitter::set(ka, "DestDivisionID", dIt->second.kb);
      Emitter::set(ka, "ActionTypeCode", 1);
      Emitter::set(ka, "ActionEffectCode", 1);
      Emitter::set(ka, "MIDINoteNumOfFirstSourceKey", first);
      Emitter::set(ka, "NumberOfKeys", last - first);
      Emitter::set(ka, "MIDINoteNumberIncrement", ini.num(csec, "DestinationKeyshift", 0));
      Emitter::set(ka, "ConditionSwitchID", sw);
      Emitter::yn(ka, "ConditionSwitchLinkIfEngaged", true);
    }

    // Tremulants drawn on this manual are listed with its stops.
    for (int t = 1; t <= ini.num(sec, "NumberOfTremulants", 0); ++t) {
      const int tNo = ini.num(sec, "Tremulant" + n3(t), 0);
      if (tNo < 1 || tNo > tremCount || !hasOwnSwitch("Tremulant" + n3(tNo))) continue;
      auto st = out.row("Stop");
      Emitter::set(st, "StopID", kStopBase + 800 + tNo);
      Emitter::set(st, "Name", ini.str("Tremulant" + n3(tNo), "Name", "Tremulant"));
      Emitter::set(st, "DivisionID", info.kb);
      Emitter::set(st, "ControllingSwitchID", kTremulantSwitchBase + tNo);
    }
  }

  // ---- combinations ---------------------------------------------------------
  // A general or a divisional is a piston that sets some drawstops on (a
  // positive number) and some off (a negative one) and leaves the rest as
  // they are -- exactly a combination whose elements are the ones it names.
  // Stops, couplers and switches are counted per manual; tremulants in a
  // general are the organ's, in a divisional the manual's.
  auto manualStopSwitch = [&](int m, int k) {
    const int sNo = ini.num("Manual" + n3(m), "Stop" + n3(k), 0);
    return sNo > 0 ? controlFor("Stop" + n3(sNo), kStopSwitchBase + sNo) : 0;
  };
  auto manualCouplerSwitch = [&](int m, int k) {
    const int cNo = ini.num("Manual" + n3(m), "Coupler" + n3(k), 0);
    const std::string sec = "Coupler" + n3(cNo);
    return cNo > 0 ? controlFor(sec, ini.yes(sec, "UnisonOff", false) ? kUnisonOffSwitchBase + cNo
                                                                       : kCouplerSwitchBase + cNo)
                   : 0;
  };
  auto tremulantSwitch = [&](int t) {
    return t >= 1 && t <= tremCount ? controlFor("Tremulant" + n3(t), kTremulantSwitchBase + t) : 0;
  };
  auto manualSwitch = [&](int m, int k) {
    const int n = ini.num("Manual" + n3(m), "Switch" + n3(k), 0);
    return n > 0 ? kGoSwitchBase + n : 0;
  };
  int nextCombination = 1;
  auto combination = [&](const std::string& sec, int piston, const std::string& name,
                         const std::vector<std::pair<int, bool>>& elements) {
    emitSwitch(piston, name, false);
    Emitter::yn(switchRows[piston], "Latching", false);
    const int id = nextCombination++;
    auto c = out.row("Combination");
    Emitter::set(c, "CombinationID", id);
    Emitter::set(c, "Name", name);
    Emitter::set(c, "CombinationTypeCode", 1);
    Emitter::set(c, "ActivatingSwitchID", piston);
    Emitter::yn(c, "CanEngageControlledSwitches", true);
    Emitter::yn(c, "CanDisengageControlledSwitches", true);
    Emitter::yn(c, "AllowsCapture", !ini.yes(sec, "Protected", false));
    for (const auto& [sw, on] : elements) {
      if (sw == 0) continue;
      auto e = out.row("CombinationElement");
      Emitter::set(e, "CombinationID", id);
      Emitter::set(e, "ControlledSwitchID", sw);
      Emitter::yn(e, "InitialStoredStateIsEngaged", on);
    }
  };
  const int generalCount = ini.num("Organ", "NumberOfGenerals", 0);
  for (int g = 1; g <= generalCount; ++g) {
    const std::string sec = "General" + n3(g);
    std::vector<std::pair<int, bool>> el;
    for (int i = 1; i <= ini.num(sec, "NumberOfStops", 0); ++i) {
      const int v = ini.num(sec, "StopNumber" + n3(i), 0);
      el.emplace_back(manualStopSwitch(ini.num(sec, "StopManual" + n3(i), 1), std::abs(v)), v > 0);
    }
    for (int i = 1; i <= ini.num(sec, "NumberOfCouplers", 0); ++i) {
      const int v = ini.num(sec, "CouplerNumber" + n3(i), 0);
      el.emplace_back(manualCouplerSwitch(ini.num(sec, "CouplerManual" + n3(i), 1), std::abs(v)), v > 0);
    }
    for (int i = 1; i <= ini.num(sec, "NumberOfTremulants", 0); ++i) {
      const int v = ini.num(sec, "TremulantNumber" + n3(i), 0);
      el.emplace_back(tremulantSwitch(std::abs(v)), v > 0);
    }
    for (int i = 1; i <= ini.num(sec, "NumberOfSwitches", 0); ++i) {
      const int v = ini.num(sec, "SwitchNumber" + n3(i), 0);
      const int m = ini.num(sec, "SwitchManual" + n3(i), -1);
      el.emplace_back(m >= 0 ? manualSwitch(m, std::abs(v)) : kGoSwitchBase + std::abs(v), v > 0);
    }
    if (ini.num(sec, "NumberOfDivisionalCouplers", 0) > 0)
      note("divisional couplers inside generals are left out");
    combination(sec, kGeneralBase + g, ini.str(sec, "Name", "General " + std::to_string(g)), el);
  }
  for (int m = hasPedals ? 0 : 1; m <= manualCount; ++m) {
    const std::string msec = "Manual" + n3(m);
    for (int d = 1; d <= ini.num(msec, "NumberOfDivisionals", 0); ++d) {
      const std::string sec = "Divisional" + n3(ini.num(msec, "Divisional" + n3(d), 0));
      std::vector<std::pair<int, bool>> el;
      for (int i = 1; i <= ini.num(sec, "NumberOfStops", 0); ++i) {
        const int v = ini.num(sec, "Stop" + n3(i), 0);
        el.emplace_back(manualStopSwitch(m, std::abs(v)), v > 0);
      }
      for (int i = 1; i <= ini.num(sec, "NumberOfCouplers", 0); ++i) {
        const int v = ini.num(sec, "Coupler" + n3(i), 0);
        el.emplace_back(manualCouplerSwitch(m, std::abs(v)), v > 0);
      }
      for (int i = 1; i <= ini.num(sec, "NumberOfTremulants", 0); ++i) {
        const int v = ini.num(sec, "Tremulant" + n3(i), 0);
        el.emplace_back(tremulantSwitch(ini.num(msec, "Tremulant" + n3(std::abs(v)), 0)), v > 0);
      }
      for (int i = 1; i <= ini.num(sec, "NumberOfSwitches", 0); ++i) {
        const int v = ini.num(sec, "Switch" + n3(i), 0);
        el.emplace_back(manualSwitch(m, std::abs(v)), v > 0);
      }
      combination(sec, kDivisionalBase + m * 100 + d,
                  ini.str(sec, "Name", ini.str(msec, "Name", msec) + " " + std::to_string(d)), el);
    }
  }
  // GrandOrgue's own setter -- its programmable generals, Set and General
  // Cancel -- act on every drawstop the player controls that is stored in a
  // general. They are made when a panel shows them.
  std::vector<int> storable;
  {
    std::set<int> seen;
    auto keep = [&](const std::string& sec, int sw) {
      if (sw != 0 && ini.num(sec, "SwitchCount", 0) == 0 && ini.yes(sec, "StoreInGeneral", true) &&
          seen.insert(sw).second)
        storable.push_back(sw);
    };
    for (int m = hasPedals ? 0 : 1; m <= manualCount; ++m) {
      const std::string msec = "Manual" + n3(m);
      for (int k = 1; k <= ini.num(msec, "NumberOfStops", 0); ++k)
        keep("Stop" + n3(ini.num(msec, "Stop" + n3(k), 0)), manualStopSwitch(m, k));
      for (int k = 1; k <= ini.num(msec, "NumberOfCouplers", 0); ++k)
        keep("Coupler" + n3(ini.num(msec, "Coupler" + n3(k), 0)), manualCouplerSwitch(m, k));
    }
    for (int t = 1; t <= tremCount; ++t) keep("Tremulant" + n3(t), tremulantSwitch(t));
    for (int n = 1; n <= switchCount; ++n) keep("Switch" + n3(n), kGoSwitchBase + n);
  }
  std::set<int> setterMade;
  auto setterControl = [&](const std::string& type) -> int {
    const std::string t = lower(type);
    if (t == "set") {
      if (setterMade.insert(kSetterSwitch).second) {
        emitSwitch(kSetterSwitch, "Set", false);
        Emitter::set(switchRows[kSetterSwitch], "DefaultInputOutputSwitchAsgnCode", 12);
      }
      return kSetterSwitch;
    }
    if (t == "gc") {
      if (setterMade.insert(kGeneralCancel).second) {
        std::vector<std::pair<int, bool>> off;
        for (int sw : storable) off.emplace_back(sw, false);
        combination("GC", kGeneralCancel, "General cancel", off);
        for (auto c : out.lists["Combination"].children("Combination"))
          if (c.child("ActivatingSwitchID").text().as_int() == kGeneralCancel) {
            c.child("CombinationTypeCode").text() = 6;
            c.child("AllowsCapture").text() = "N";
          }
      }
      return kGeneralCancel;
    }
    if (t.rfind("general", 0) == 0 && t.size() > 7 &&
        std::all_of(t.begin() + 7, t.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
      const int n = std::atoi(t.c_str() + 7);
      const int id = kSetterGeneralBase + n;
      if (setterMade.insert(id).second) {
        // Empty until set: nothing is stored on, so recalling one before it
        // has been set clears the stops, as a fresh general does.
        std::vector<std::pair<int, bool>> off;
        for (int sw : storable) off.emplace_back(sw, false);
        combination("General" + n3(n), id, "General " + std::to_string(n), off);
      }
      return id;
    }
    return 0;
  };

  // A reversible piston flips one drawstop each time it is pressed: a
  // momentary switch wired to it by a toggling link (3/7), as the reversible
  // pistons of the sets built for the other format are.
  const int reversibleCount = ini.num("Organ", "NumberOfReversiblePistons", 0);
  for (int r = 1; r <= reversibleCount; ++r) {
    const std::string sec = "ReversiblePiston" + n3(r);
    const std::string type = lower(ini.str(sec, "ObjectType"));
    const int m = ini.num(sec, "ManualNumber", 1), k = ini.num(sec, "ObjectNumber", 1);
    const int target = type == "stop"        ? manualStopSwitch(m, k)
                       : type == "coupler"   ? manualCouplerSwitch(m, k)
                       : type == "tremulant" ? tremulantSwitch(k)
                       : type == "switch"    ? kGoSwitchBase + k
                                             : 0;
    if (target == 0) continue;
    const int piston = kReversibleBase + r;
    emitSwitch(piston, ini.str(sec, "Name", "Reversible " + std::to_string(r)), false);
    Emitter::yn(switchRows[piston], "Latching", false);
    link(piston, target, 0, true, 3, 7);
  }
  if (ini.num("Organ", "NumberOfDivisionalCouplers", 0) > 0)
    note("divisional couplers are not imported yet");

  // ---- the console --------------------------------------------------------
  // Each GrandOrgue panel becomes a display page of the definition, with
  // every picture at a fixed place: what the set places itself stays where it
  // is, and what it leaves to GrandOrgue's automatic layout is placed here by
  // the same arithmetic. Pictures the set does not ship -- GrandOrgue's stock
  // drawstops, pistons, keys, shoes, nameplates and wood -- are written as
  // images of a standard component package, which the console covers with
  // stand-ins of its own, as it does for the standard components of any set.
  {
    const bool newFormat = ini.num("Panel000", "NumberOfGUIElements", -1) >= 0;
    const std::string& root = organRoot;
    constexpr int kStockPackage = 1;
    const std::string kStock = "stock:";
    auto isStock = [&](const std::string& f) { return f.rfind(kStock, 0) == 0; };
    std::map<std::string, std::pair<int, int>> sizes;
    auto sizeOf = [&](const std::string& file) -> std::pair<int, int> {
      const auto it = sizes.find(file);
      if (it != sizes.end()) return it->second;
      std::pair<int, int> wh{0, 0};
      if (isStock(file)) {
        const std::string name = file.substr(kStock.size());
        for (const auto& img : kGrandOrgueStockImages)
          if (name == img.name) wh = {img.width, img.height};
      } else if (!root.empty()) {
        std::string rel = file;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        wh = imageSize(root + "/" + rel);
      }
      return sizes[file] = wh;
    };
    // Where the definition says a picture is. A stock picture is a standard
    // component file; wood is the seamless background the console tiles.
    auto bitmapFile = [&](const std::string& f) {
      if (isStock(f)) {
        const std::string name = f.substr(kStock.size());
        if (name.rfind("wood", 0) == 0) {
          const int n = std::atoi(name.c_str() + 4);
          char buf[64];
          std::snprintf(buf, sizeof buf, "SeamlessWoodBkgnds - %sGrain%03d.bmp",
                        n % 2 == 0 ? "V" : "H", (n + 1) / 2);
          return std::string(buf);
        }
        return std::string("GrandOrgueStandIns/" MP_GRANDORGUE_STOCK_PREFIX) + name + ".png";
      }
      std::string rel = f;
      std::replace(rel.begin(), rel.end(), '\\', '/');
      return rel;
    };
    auto stockName = [](const char* fmt, int n) {
      char buf[64];
      std::snprintf(buf, sizeof buf, fmt, n);
      return std::string("stock:") + buf;
    };

    int nextSet = 1, nextInstance = 1, nextText = 1, nextStyle = 1, nextMirror = 1;
    std::map<std::string, int> setMemo, styleMemo;
    auto imageSet = [&](const std::vector<std::string>& files, int clickL, int clickT, int clickW, int clickH) {
      std::string key;
      for (const auto& f : files) key += f + "|";
      key += std::to_string(clickL) + "," + std::to_string(clickT) + "," + std::to_string(clickW) + "," + std::to_string(clickH);
      const auto hit = setMemo.find(key);
      if (hit != setMemo.end()) return hit->second;
      const int id = nextSet++;
      auto set = out.row("ImageSet");
      Emitter::set(set, "ImageSetID", id);
      Emitter::set(set, "Name", bitmapFile(files.front()));
      Emitter::set(set, "InstallationPackageID", isStock(files.front()) ? kStockPackage : 0);
      const auto wh = sizeOf(files.front());
      if (wh.first > 0 && files.front().rfind("stock:wood", 0) != 0) {
        Emitter::set(set, "ImageWidthPixels", wh.first);
        Emitter::set(set, "ImageHeightPixels", wh.second);
      }
      if (clickW > 0 && clickH > 0) {
        Emitter::set(set, "ClickableAreaLeftRelativeXPosPixels", clickL);
        Emitter::set(set, "ClickableAreaRightRelativeXPosPixels", clickL + clickW);
        Emitter::set(set, "ClickableAreaTopRelativeYPosPixels", clickT);
        Emitter::set(set, "ClickableAreaBottomRelativeYPosPixels", clickT + clickH);
      }
      for (size_t i = 0; i < files.size(); ++i) {
        auto el = out.row("ImageSetElement");
        Emitter::set(el, "ImageSetID", id);
        Emitter::set(el, "ImageIndexWithinSet", static_cast<int>(i) + 1);
        Emitter::set(el, "BitmapFilename", bitmapFile(files[i]));
      }
      return setMemo[key] = id;
    };
    auto instance = [&](int page, int set, int x, int y, int layer, int tileRight = 0, int tileBottom = 0) {
      const int id = nextInstance++;
      auto in = out.row("ImageSetInstance");
      Emitter::set(in, "ImageSetInstanceID", id);
      Emitter::set(in, "DisplayPageID", page);
      Emitter::set(in, "ImageSetID", set);
      Emitter::set(in, "DefaultImageIndexWithinSet", 1);
      Emitter::set(in, "ScreenLayerNumber", layer);
      Emitter::set(in, "LeftXPosPixels", x);
      Emitter::set(in, "TopYPosPixels", y);
      if (tileRight > 0) {
        Emitter::set(in, "RightXPosPixelsIfTiling", tileRight);
        Emitter::set(in, "BottomYPosPixelsIfTiling", tileBottom);
      }
      return id;
    };
    // Wood, tiled over a rectangle, behind everything else on the page.
    auto wood = [&](int page, int num, int x, int y, int w, int h) {
      if (w <= 0 || h <= 0 || num <= 0) return;
      instance(page, imageSet({stockName("wood%02d", num)}, 0, 0, 0, 0), x, y, 0, x + w, y + h);
    };
    // A switch has one drawn instance. A second appearance -- the same stop
    // on the console and on a jamb -- is a mirror switch wired both ways.
    std::set<int> drawnSwitches;
    auto attach = [&](int switchId, int inst, bool clickable) {
      int id = switchId;
      if (!drawnSwitches.insert(switchId).second) {
        id = 30000 + nextMirror++;
        const auto src = switchRows.find(switchId);
        const bool on = src != switchRows.end() &&
                        std::string(src->second.child("DefaultToEngaged").text().get()) == "Y";
        emitSwitch(id, "mirror of " + std::to_string(switchId), on);
        link(switchId, id, 0, true);
        link(id, switchId, 0, true);
      }
      auto row = switchRows[id];
      if (!row) return;
      Emitter::set(row, "Disp_ImageSetInstanceID", inst);
      Emitter::set(row, "Disp_ImageSetIndexEngaged", 2);
      Emitter::set(row, "Disp_ImageSetIndexDisengaged", 1);
      Emitter::yn(row, "Clickable", clickable);
    };
    // GrandOrgue sizes type in points; the definition's styles are pixels.
    auto fontPx = [&](const std::string& sec, const char* keyName, const std::string& fallback) {
      const std::string v = lower(ini.str(sec, keyName, fallback));
      const int pt = v == "small" ? 6 : v == "normal" ? 7 : v == "large" ? 10 : std::atoi(v.c_str());
      return std::max(6, (pt > 0 ? pt : 7) * 4 / 3);
    };
    auto text = [&](int page, int inst, const std::string& words, int r, int g, int b, int px,
                    const std::string& face, int x, int y, int w, int h) {
      if (words.empty() || w <= 0 || h <= 0) return;
      const std::string key = face + "/" + std::to_string(px) + "/" + std::to_string((r << 16) | (g << 8) | b);
      int style = 0;
      const auto hit = styleMemo.find(key);
      if (hit != styleMemo.end()) {
        style = hit->second;
      } else {
        style = styleMemo[key] = nextStyle++;
        auto st = out.row("TextStyle");
        Emitter::set(st, "StyleID", style);
        Emitter::set(st, "Name", key);
        Emitter::set(st, "Face_WindowsName", face.empty() ? std::string("Arial") : face);
        Emitter::set(st, "Font_SizePixels", px);
        Emitter::set(st, "Colour_Red", r);
        Emitter::set(st, "Colour_Green", g);
        Emitter::set(st, "Colour_Blue", b);
        Emitter::set(st, "HorizontalAlignmentCode", 0);
        Emitter::set(st, "VerticalAlignmentCode", 0);
      }
      auto t = out.row("TextInstance");
      Emitter::set(t, "TextInstanceID", nextText++);
      Emitter::set(t, "DisplayPageID", page);
      Emitter::set(t, "Text", words);
      Emitter::set(t, "TextStyleID", style);
      Emitter::set(t, "XPosPixels", x);
      Emitter::set(t, "YPosPixels", y);
      Emitter::set(t, "BoundingBoxWidthPixelsIfWordWrap", w);
      Emitter::set(t, "BoundingBoxHeightPixelsIfWordWrap", h);
      if (inst) {
        Emitter::yn(t, "AttachedToAnImageSetInstance", true);
        Emitter::set(t, "AttachedToImageSetInstanceID", inst);
        Emitter::yn(t, "PosRelativeToTopLeftOfImageSetInstance", true);
      }
    };

    // GrandOrgue's display metrics for one panel, and the layout that places
    // what the panel does not place itself (GOGUIHW1DisplayMetrics and
    // GOGUILayoutEngine).
    struct Metrics {
      int screenW = 800, screenH = 500;
      int drawstopBg = 1, consoleBg = 1, keyHorizBg = 1, keyVertBg = 1, insetBg = 1;
      int drawstopCols = 2, drawstopRows = 1, extraDrawstopRows = 0, extraDrawstopCols = 0;
      int buttonCols = 1, extraButtonRows = 0;
      bool colsOffset = false, outerColOffsetUp = false, pairCols = false;
      bool extraPedalButtonRow = false, extraPedalButtonRowOffset = false;
      bool buttonsAboveManuals = false, trimAboveManuals = false, trimBelowManuals = false;
      bool trimAboveExtraRows = false, extraDrawstopRowsAboveExtraButtonRows = false;
      int drawstopW = 78, drawstopH = 69, buttonW = 44, buttonH = 40;
      int enclosureW = 52, enclosureH = 63, pedalH = 40, pedalKeyW = 7, manualH = 32, manualKeyW = 12;
      std::string controlFont;
    };
    auto screenSize = [&](const std::string& g, const char* keyName, int axis) {
      static const int named[2][4] = {{800, 1007, 1263, 1583}, {500, 663, 855, 1095}};
      std::string v = ini.str(g, keyName, "SMALL");
      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::toupper(c); });
      if (v == "SMALL") return named[axis][0];
      if (v == "MEDIUM") return named[axis][1];
      if (v == "MEDIUM LARGE") return named[axis][2];
      if (v == "LARGE") return named[axis][3];
      const int n = std::atoi(v.c_str());
      return n > 0 ? n : named[axis][0];
    };
    auto metricsOf = [&](const std::string& g) {
      Metrics m;
      m.screenW = screenSize(g, "DispScreenSizeHoriz", 0);
      m.screenH = screenSize(g, "DispScreenSizeVert", 1);
      m.drawstopBg = ini.num(g, "DispDrawstopBackgroundImageNum", 1);
      m.consoleBg = ini.num(g, "DispConsoleBackgroundImageNum", 1);
      m.keyHorizBg = ini.num(g, "DispKeyHorizBackgroundImageNum", 1);
      m.keyVertBg = ini.num(g, "DispKeyVertBackgroundImageNum", 1);
      m.insetBg = ini.num(g, "DispDrawstopInsetBackgroundImageNum", 1);
      m.controlFont = ini.str(g, "DispControlLabelFont");
      m.drawstopCols = ini.num(g, "DispDrawstopCols", 2);
      m.drawstopRows = ini.num(g, "DispDrawstopRows", 1);
      m.colsOffset = ini.yes(g, "DispDrawstopColsOffset", false);
      m.outerColOffsetUp = ini.yes(g, "DispDrawstopOuterColOffsetUp", m.colsOffset);
      m.pairCols = ini.yes(g, "DispPairDrawstopCols", false);
      m.extraDrawstopRows = ini.num(g, "DispExtraDrawstopRows", 0);
      m.extraDrawstopCols = ini.num(g, "DispExtraDrawstopCols", 0);
      m.buttonCols = ini.num(g, "DispButtonCols", 1);
      m.extraButtonRows = ini.num(g, "DispExtraButtonRows", 0);
      m.extraPedalButtonRow = ini.yes(g, "DispExtraPedalButtonRow", false);
      m.extraPedalButtonRowOffset = ini.yes(g, "DispExtraPedalButtonRowOffset", m.extraPedalButtonRow);
      m.buttonsAboveManuals = ini.yes(g, "DispButtonsAboveManuals", false);
      m.trimAboveManuals = ini.yes(g, "DispTrimAboveManuals", false);
      m.trimBelowManuals = ini.yes(g, "DispTrimBelowManuals", false);
      m.trimAboveExtraRows = ini.yes(g, "DispTrimAboveExtraRows", false);
      m.extraDrawstopRowsAboveExtraButtonRows = ini.yes(g, "DispExtraDrawstopRowsAboveExtraButtonRows", false);
      m.drawstopW = ini.num(g, "DispDrawstopWidth", 78);
      m.drawstopH = ini.num(g, "DispDrawstopHeight", 69);
      m.buttonW = ini.num(g, "DispPistonWidth", 44);
      m.buttonH = ini.num(g, "DispPistonHeight", 40);
      m.enclosureW = ini.num(g, "DispEnclosureWidth", 52);
      m.enclosureH = ini.num(g, "DispEnclosureHeight", 63);
      m.pedalH = ini.num(g, "DispPedalHeight", 40);
      m.pedalKeyW = ini.num(g, "DispPedalKeyWidth", 7);
      m.manualH = ini.num(g, "DispManualHeight", 32);
      m.manualKeyW = ini.num(g, "DispManualKeyWidth", 12);
      return m;
    };
    struct ManualRow {
      int x = 0, y = 0, width = 0, height = 0, keysY = 0, pistonY = 0;
    };
    struct Layout {
      Metrics m;
      std::vector<std::vector<bool>> manuals;  // per registered manual: its keys' sharpness; empty = none
      std::vector<bool> present;
      int enclosures = 0;
      std::vector<ManualRow> rows;
      int centerY = 0, centerW = 0, hackY = 0, enclosureY = 0;

      int jambLRHeight() const { return (m.drawstopRows + 1) * m.drawstopH; }
      int jambLRY() const {
        return (m.screenH - jambLRHeight() - (m.colsOffset ? m.drawstopH / 2 : 0)) / 2;
      }
      int jambLRWidth() const {
        int w = m.drawstopCols * m.drawstopW / 2;
        if (m.pairCols) w += ((m.drawstopCols >> 2) * (m.drawstopW / 4)) - 8;
        return w;
      }
      int jambTopHeight() const { return m.extraDrawstopRows * m.drawstopH; }
      int jambTopWidth() const { return m.extraDrawstopCols * m.drawstopW; }
      int jambTopX() const { return (m.screenW - jambTopWidth()) >> 1; }
      int pistonTopHeight() const { return m.extraButtonRows * m.buttonH; }
      int pistonWidth() const { return m.buttonCols * m.buttonW; }
      int pistonX() const { return (m.screenW - pistonWidth()) >> 1; }
      int centerX() const { return (m.screenW - centerW) >> 1; }
      int enclosuresWidth() const { return m.enclosureW * enclosures; }
      int enclosureX(int index) const {
        return ((m.screenW - enclosuresWidth() + 6) >> 1) + index * m.enclosureW;
      }
      int jambLeftX() const {
        int x = (centerX() - jambLRWidth()) >> 1;
        if (m.pairCols) x += 5;
        return x;
      }
      int jambRightX() const {
        int x = jambLeftX() + centerX() + centerW;
        if (m.pairCols) x += 5;
        return x;
      }
      int jambTopY() const { return m.trimAboveExtraRows ? centerY + 8 : centerY; }
      void drawstop(int row, int col, int& x, int& y) const {
        if (row > 99) {
          x = jambTopX() + (col - 1) * m.drawstopW + 6;
          y = jambTopY() + (row - 100) * m.drawstopH + 2 +
              (m.extraDrawstopRowsAboveExtraButtonRows ? 0 : m.extraButtonRows * m.buttonH);
          return;
        }
        int i = m.drawstopCols >> 1;
        if (col <= i) x = jambLeftX() + (col - 1) * m.drawstopW + 6;
        else x = jambRightX() + (col - 1 - i) * m.drawstopW + 6;
        y = jambLRY() + (row - 1) * m.drawstopH + 32;
        if (m.pairCols && i > 0) x += (((col - 1) % i) >> 1) * (m.drawstopW / 4);
        i = col <= i ? col : m.drawstopCols - col + 1;
        if (m.colsOffset && ((i & 1) ^ (m.outerColOffsetUp ? 1 : 0))) y += m.drawstopH / 2;
      }
      void piston(int row, int col, int& x, int& y) const {
        x = pistonX() + (col - 1) * m.buttonW + 6;
        if (row > 99) {
          y = jambTopY() + (row - 100) * m.buttonH + 5 +
              (m.extraDrawstopRowsAboveExtraButtonRows ? m.extraDrawstopRows * m.drawstopH : 0);
          return;
        }
        const int i = row == 99 ? 0 : row;
        if (i >= static_cast<int>(rows.size()))
          y = hackY - (i + 1 - static_cast<int>(rows.size())) * (m.manualH + m.buttonH) + m.manualH + 5;
        else
          y = rows[static_cast<size_t>(i)].pistonY + 5;
        if (m.extraPedalButtonRow && row == 0) y += m.buttonH;
        if (m.extraPedalButtonRowOffset && row == 99) x -= m.buttonW / 2 + 2;
      }
      void update() {
        if (manuals.empty()) {
          manuals.emplace_back();
          present.push_back(false);
        }
        rows.assign(manuals.size(), ManualRow{});
        centerY = m.screenH - m.pedalH;
        centerW = std::max(jambTopWidth(), pistonWidth());
        for (size_t i = 0; i < manuals.size(); ++i) {
          if (i == 0 && present[0]) {
            rows[0].height = m.pedalH;
            rows[0].keysY = rows[0].y = centerY;
            centerY -= m.pedalH;
            if (m.extraPedalButtonRow) centerY -= m.buttonH;
            rows[0].pistonY = centerY;
            centerW = std::max(centerW, enclosuresWidth());
            centerY -= 12 + m.enclosureH;
            enclosureY = centerY;
            centerY -= 12;
          }
          if (i == 0 && !present[0] && enclosures) {
            centerY -= 12 + m.enclosureH;
            enclosureY = centerY;
            centerY -= 12;
          }
          if (!present[i]) continue;
          if (i) {
            if (!m.buttonsAboveManuals) {
              centerY -= m.buttonH;
              rows[i].pistonY = centerY;
            }
            rows[i].height = m.manualH;
            if (m.trimBelowManuals && i == 1) {
              rows[i].height += 8;
              centerY -= 8;
            }
            centerY -= m.manualH;
            rows[i].keysY = centerY;
            if (m.trimAboveManuals && i + 1 == manuals.size()) {
              centerY -= 8;
              rows[i].height += 8;
            }
            if (m.buttonsAboveManuals) {
              centerY -= m.buttonH;
              rows[i].pistonY = centerY;
            }
            rows[i].y = centerY;
          }
          int width = 1;
          const auto& keys = manuals[i];
          for (size_t j = 0; j < keys.size(); ++j) {
            if (i) {
              if (!keys[j]) width += m.manualKeyW;
            } else {
              width += m.pedalKeyW;
              if (j && !keys[j - 1] && !keys[j]) width += m.pedalKeyW;
            }
          }
          rows[i].x = (m.screenW - width) >> 1;
          rows[i].width = width + 16;
          centerW = std::max(centerW, rows[i].width);
        }
        hackY = centerY;
        if (centerW + jambLRWidth() * 2 < m.screenW)
          centerW += (m.screenW - centerW - jambLRWidth() * 2) / 3;
        centerY -= pistonTopHeight() + jambTopHeight();
        if (m.trimAboveExtraRows) centerY -= 8;
      }
    };

    // What a panel shows, in the order GrandOrgue loads it: that order is
    // what numbers its manual rows and lines its swell shoes up.
    enum class Kind { Image, Button, Enclosure, Manual, Label };
    struct Element {
      Element(Kind k, std::string s) : kind(k), sec(std::move(s)) {}
      Kind kind;
      std::string sec;       // where its own settings are
      int switchId = 0;
      bool piston = false;
      bool readOnly = false;
      std::string name;
      int manual = -1;       // Kind::Manual
      int enclosure = 0;     // Kind::Enclosure
    };
    auto isReadOnly = [&](const std::string& objectSec) { return ini.num(objectSec, "SwitchCount", 0) > 0; };
    auto button = [&](const std::string& sec, const std::string& objectSec, int sw, bool piston) {
      Element e{Kind::Button, sec};
      e.switchId = sw;
      e.piston = piston;
      e.readOnly = isReadOnly(objectSec);
      e.name = ini.str(objectSec, "Name");
      return e;
    };
    auto stopSwitchOf = [&](int m, int local, std::string& objectSec) {
      const int sNo = ini.num("Manual" + n3(m), "Stop" + n3(local), 0);
      objectSec = "Stop" + n3(sNo);
      return manualStopSwitch(m, local);
    };
    auto couplerSwitchOf = [&](int m, int local, std::string& objectSec) {
      const int cNo = ini.num("Manual" + n3(m), "Coupler" + n3(local), 0);
      objectSec = "Coupler" + n3(cNo);
      return manualCouplerSwitch(m, local);
    };
    auto divisionalSec = [&](int m, int local) {
      return "Divisional" + n3(ini.num("Manual" + n3(m), "Divisional" + n3(local), 0));
    };

    int drawn = 0;
    auto buildPanel = [&](int page, const std::string& group, const std::string& prefix,
                          bool isMain, const std::string& pageName) {
      std::vector<Element> els;
      Layout lay;
      lay.m = metricsOf(group);
      const bool pedalsHere = ini.yes(group, "HasPedals", false);
      auto addManual = [&](int m, const std::string& sec) {
        Element e{Kind::Manual, sec};
        e.manual = m;
        els.push_back(e);
      };
      for (int i = 1; i <= ini.num(group, "NumberOfImages", 0); ++i)
        els.push_back({Kind::Image, prefix + "Image" + n3(i)});
      // The main panel draws every object marked Displayed, in either
      // format; the newer format then adds the panel's own elements.
      if (isMain) {
        for (int e = 1; e <= encCount; ++e)
          if (ini.yes("Enclosure" + n3(e), "Displayed", !newFormat)) {
            Element el{Kind::Enclosure, "Enclosure" + n3(e)};
            el.enclosure = e;
            els.push_back(el);
          }
        for (int t = 1; t <= tremCount; ++t)
          if (ini.yes("Tremulant" + n3(t), "Displayed", false))
            els.push_back(button("Tremulant" + n3(t), "Tremulant" + n3(t), tremulantSwitch(t), false));
        for (int g = 1; g <= generalCount; ++g)
          if (ini.yes("General" + n3(g), "Displayed", false))
            els.push_back(button("General" + n3(g), "General" + n3(g), kGeneralBase + g, true));
        for (int r = 1; r <= reversibleCount; ++r)
          if (ini.yes("ReversiblePiston" + n3(r), "Displayed", false))
            els.push_back(button("ReversiblePiston" + n3(r), "ReversiblePiston" + n3(r),
                                 kReversibleBase + r, true));
        for (int n = 1; n <= switchCount; ++n)
          if (ini.yes("Switch" + n3(n), "Displayed", false))
            els.push_back(button("Switch" + n3(n), "Switch" + n3(n), kGoSwitchBase + n, false));
        for (int m = hasPedals ? 0 : 1; m <= manualCount; ++m) {
          const std::string msec = "Manual" + n3(m);
          if (ini.yes(msec, "Displayed", false)) addManual(m, msec);
          for (int c = 1; c <= ini.num(msec, "NumberOfCouplers", 0); ++c) {
            std::string objectSec;
            const int sw = couplerSwitchOf(m, c, objectSec);
            if (ini.yes(objectSec, "Displayed", false)) els.push_back(button(objectSec, objectSec, sw, false));
          }
          for (int st = 1; st <= ini.num(msec, "NumberOfStops", 0); ++st) {
            std::string objectSec;
            const int sw = stopSwitchOf(m, st, objectSec);
            if (ini.yes(objectSec, "Displayed", false)) els.push_back(button(objectSec, objectSec, sw, false));
          }
          for (int d = 1; d <= ini.num(msec, "NumberOfDivisionals", 0); ++d) {
            const std::string dsec = divisionalSec(m, d);
            if (ini.yes(dsec, "Displayed", false))
              els.push_back(button(dsec, dsec, kDivisionalBase + m * 100 + d, true));
          }
        }
      }
      if (newFormat) {
        for (int i = 1; i <= ini.num(group, "NumberOfGUIElements", 0); ++i) {
          const std::string sec = group + "Element" + n3(i);
          const std::string type = ini.str(sec, "Type");
          std::string objectSec;
          if (type == "Stop") {
            const int sw = stopSwitchOf(ini.num(sec, "Manual", 1), ini.num(sec, "Stop", 1), objectSec);
            els.push_back(button(sec, objectSec, sw, false));
          } else if (type == "Coupler") {
            const int sw = couplerSwitchOf(ini.num(sec, "Manual", 1), ini.num(sec, "Coupler", 1), objectSec);
            els.push_back(button(sec, objectSec, sw, false));
          } else if (type == "Switch") {
            const int n = ini.num(sec, "Switch", 1);
            els.push_back(button(sec, "Switch" + n3(n), kGoSwitchBase + n, false));
          } else if (type == "Tremulant") {
            const int t = ini.num(sec, "Tremulant", 1);
            els.push_back(button(sec, "Tremulant" + n3(t), tremulantSwitch(t), false));
          } else if (type == "General") {
            const int g = ini.num(sec, "General", 1);
            els.push_back(button(sec, "General" + n3(g), kGeneralBase + g, true));
          } else if (type == "Divisional") {
            const int m = ini.num(sec, "Manual", 1), d = ini.num(sec, "Divisional", 1);
            els.push_back(button(sec, divisionalSec(m, d), kDivisionalBase + m * 100 + d, true));
          } else if (type == "Enclosure") {
            Element e{Kind::Enclosure, sec};
            e.enclosure = ini.num(sec, "Enclosure", 1);
            els.push_back(e);
          } else if (type == "Manual") {
            addManual(ini.num(sec, "Manual", 1), sec);
          } else if (type == "Label") {
            els.push_back({Kind::Label, sec});
          } else if (type == "ReversiblePiston") {
            const int r = ini.num(sec, "ReversiblePiston", 1);
            els.push_back(button(sec, "ReversiblePiston" + n3(r), kReversibleBase + r, true));
          } else if (type == "DivisionalCoupler") {
            note("divisional couplers are not imported yet");
          } else if (const int sw = setterControl(type)) {
            Element e = button(sec, sec, sw, true);
            e.name = type;
            e.readOnly = false;
            els.push_back(e);
          }
          // The setter's other buttons -- memory banks, crescendo, transpose
          // and the like -- are GrandOrgue's own interface, not the organ's.
        }
      } else if (!isMain) {
        for (int i = 1; i <= ini.num(group, "NumberOfEnclosures", 0); ++i) {
          const int e = ini.num(group, "Enclosure" + n3(i), 0);
          Element el{Kind::Enclosure, prefix + "Enclosure" + n3(e)};
          el.enclosure = e;
          els.push_back(el);
        }
        for (int i = 1; i <= ini.num(group, "NumberOfTremulants", 0); ++i) {
          const int t = ini.num(group, "Tremulant" + n3(i), 0);
          els.push_back(button(prefix + "Tremulant" + n3(t), "Tremulant" + n3(t), tremulantSwitch(t), false));
        }
        for (int i = 1; i <= ini.num(group, "NumberOfGenerals", 0); ++i) {
          const int g = ini.num(group, "General" + n3(i), 0);
          els.push_back(button(prefix + "General" + n3(g), "General" + n3(g), kGeneralBase + g, true));
        }
        for (int i = 1; i <= ini.num(group, "NumberOfReversiblePistons", 0); ++i) {
          const int r = ini.num(group, "ReversiblePiston" + n3(i), 0);
          els.push_back(button(prefix + "ReversiblePiston" + n3(r), "ReversiblePiston" + n3(r),
                               kReversibleBase + r, true));
        }
        for (int i = 1; i <= ini.num(group, "NumberOfSwitches", 0); ++i) {
          const int n = ini.num(group, "Switch" + n3(i), 0);
          els.push_back(button(prefix + "Switch" + n3(n), "Switch" + n3(n), kGoSwitchBase + n, false));
        }
        for (int i = pedalsHere ? 0 : 1; i <= ini.num(group, "NumberOfManuals", 0); ++i) {
          const int m = ini.num(group, "Manual" + n3(i), i);
          addManual(m, prefix + "Manual" + n3(m));
        }
        for (int i = 1; i <= ini.num(group, "NumberOfCouplers", 0); ++i) {
          std::string objectSec;
          const int sw = couplerSwitchOf(ini.num(group, "Coupler" + n3(i) + "Manual", 1),
                                         ini.num(group, "Coupler" + n3(i), 1), objectSec);
          els.push_back(button(prefix + "Coupler" + n3(i), objectSec, sw, false));
        }
        for (int i = 1; i <= ini.num(group, "NumberOfStops", 0); ++i) {
          std::string objectSec;
          const int sw = stopSwitchOf(ini.num(group, "Stop" + n3(i) + "Manual", 1),
                                      ini.num(group, "Stop" + n3(i), 1), objectSec);
          els.push_back(button(prefix + "Stop" + n3(i), objectSec, sw, false));
        }
        for (int i = 1; i <= ini.num(group, "NumberOfDivisionals", 0); ++i) {
          const int m = ini.num(group, "Divisional" + n3(i) + "Manual", 1);
          const int d = ini.num(group, "Divisional" + n3(i), 1);
          els.push_back(button(prefix + "Divisional" + n3(i), divisionalSec(m, d),
                               kDivisionalBase + m * 100 + d, true));
        }
      }
      if (!newFormat) {
        for (int i = 1; i <= ini.num(group, "NumberOfSetterElements", 0); ++i) {
          const std::string sec = prefix + "SetterElement" + n3(i);
          if (const int sw = setterControl(ini.str(sec, "Type"))) {
            Element e = button(sec, sec, sw, true);
            e.name = ini.str(sec, "Type");
            e.readOnly = false;
            els.push_back(e);
          }
        }
        for (int i = 1; i <= ini.num(group, "NumberOfLabels", 0); ++i)
          els.push_back({Kind::Label, prefix + "Label" + n3(i)});
      }
      if (els.empty()) return;

      // The manuals' key patterns and the shoes, registered as GrandOrgue does.
      static const char* names[12] = {"C", "Cis", "D", "Dis", "E", "F", "Fis", "G", "Gis", "A", "Ais", "B"};
      auto keysOf = [&](int m, const std::string& sec, std::vector<int>& midi, std::vector<int>& shown) {
        const std::string msec = "Manual" + n3(m);
        const int firstNote = ini.num(sec, "DisplayFirstNote", ini.num(msec, "FirstAccessibleKeyMIDINoteNumber", 36));
        const int count = ini.num(sec, "DisplayKeys", ini.num(msec, "NumberOfAccessibleKeys", 61));
        std::vector<bool> sharp(static_cast<size_t>(std::max(0, count)));
        midi.assign(sharp.size(), 0);
        shown.assign(sharp.size(), 0);
        for (int i = 0; i < count; ++i) {
          midi[i] = ini.num(sec, "DisplayKey" + n3(i + 1), firstNote + i);
          shown[i] = ini.num(sec, "DisplayKey" + n3(i + 1) + "Note", firstNote + i);
          const int k = shown[i];
          sharp[i] = !(((k % 12) < 5 && !(k & 1)) || ((k % 12) >= 5 && (k & 1)));
        }
        return sharp;
      };
      if (!newFormat && !pedalsHere) {
        lay.manuals.emplace_back();
        lay.present.push_back(false);
      }
      std::map<size_t, size_t> rowOf;  // element index -> manual row
      std::map<size_t, int> shoeIndex;
      for (size_t i = 0; i < els.size(); ++i) {
        if (els[i].kind == Kind::Manual && manualInfo.count(els[i].manual)) {
          std::vector<int> midi, shown;
          rowOf[i] = lay.manuals.size();
          lay.manuals.push_back(keysOf(els[i].manual, els[i].sec, midi, shown));
          lay.present.push_back(true);
        } else if (els[i].kind == Kind::Enclosure) {
          shoeIndex[i] = lay.enclosures++;
        }
      }
      lay.update();
      const Metrics& m = lay.m;

      // The wood behind it all: the jambs, the centre, the insets and the
      // rails above the keys (GOGUIHW1Background).
      wood(page, m.drawstopBg, 0, 0, lay.centerX(), m.screenH);
      wood(page, m.drawstopBg, lay.centerX() + lay.centerW, 0,
           m.screenW - (lay.centerX() + lay.centerW), m.screenH);
      wood(page, m.consoleBg, lay.centerX(), 0, lay.centerW, m.screenH);
      if (m.pairCols)
        for (int i = 0; i < (m.drawstopCols >> 2); ++i) {
          wood(page, m.insetBg, i * (2 * m.drawstopW + 18) + lay.jambLeftX() - 5, lay.jambLRY(),
               2 * m.drawstopW + 10, lay.jambLRHeight());
          wood(page, m.insetBg, i * (2 * m.drawstopW + 18) + lay.jambRightX() - 5, lay.jambLRY(),
               2 * m.drawstopW + 10, lay.jambLRHeight());
        }
      if (m.trimAboveExtraRows) wood(page, m.keyVertBg, lay.centerX(), lay.centerY, lay.centerW, 8);
      if (lay.jambTopHeight() + lay.pistonTopHeight())
        wood(page, m.keyHorizBg, lay.centerX(), lay.jambTopY(), lay.centerW,
             lay.jambTopHeight() + lay.pistonTopHeight());

      int nextKeySwitch = 20000 + page * 1000;
      for (size_t i = 0; i < els.size(); ++i) {
        const Element& e = els[i];
        const std::string& sec = e.sec;
        if (e.kind == Kind::Image) {
          const std::string file = ini.str(sec, "Image");
          if (file.empty()) continue;
          const int x = ini.num(sec, "PositionX", 0), y = ini.num(sec, "PositionY", 0);
          const auto wh = sizeOf(file);
          const int w = ini.num(sec, "Width", wh.first), h = ini.num(sec, "Height", wh.second);
          const bool tile = wh.first > 0 && (w > wh.first || h > wh.second);
          instance(page, imageSet({file}, 0, 0, 0, 0), x, y, 1, tile ? x + w : 0, tile ? y + h : 0);
          ++drawn;
        } else if (e.kind == Kind::Button) {
          if (e.switchId == 0) continue;
          const bool piston = ini.yes(sec, "DisplayAsPiston", e.piston);
          const int image = ini.num(sec, "DispImageNum", piston ? (e.readOnly ? 3 : 1) : (e.readOnly ? 4 : 1));
          std::string on = ini.str(sec, "ImageOn", stockName(piston ? "piston%02d_on" : "drawstop%02d_on", image));
          std::string off = ini.str(sec, "ImageOff", stockName(piston ? "piston%02d_off" : "drawstop%02d_off", image));
          if (ini.yes(sec, "DisplayInInvertedState", false)) std::swap(on, off);
          const auto wh = sizeOf(off);
          const int w = ini.num(sec, "Width", wh.first), h = ini.num(sec, "Height", wh.second);
          int x = ini.num(sec, "PositionX", -1), y = ini.num(sec, "PositionY", -1);
          if (x < 0 || y < 0) {
            int lx = 0, ly = 0;
            if (piston) {
              lay.piston(ini.num(sec, "DispButtonRow", 1), ini.num(sec, "DispButtonCol", 1), lx, ly);
              if (!ini.yes(sec, "DispKeyLabelOnLeft", true)) lx -= 13;
            } else {
              lay.drawstop(ini.num(sec, "DispDrawstopRow", 1), ini.num(sec, "DispDrawstopCol", 1), lx, ly);
            }
            if (x < 0) x = lx;
            if (y < 0) y = ly;
          }
          const int ml = ini.num(sec, "MouseRectLeft", 0), mt = ini.num(sec, "MouseRectTop", 0);
          const int set = imageSet({off, on}, ml, mt, ini.num(sec, "MouseRectWidth", w - ml),
                                   ini.num(sec, "MouseRectHeight", h - mt));
          const int inst = instance(page, set, x, y, 2);
          attach(e.switchId, inst, !e.readOnly);
          int r = 0x80, g = 0, b = 0;
          parseColour(ini.str(sec, "DispLabelColour", "Dark Red"), r, g, b);
          const int tl = ini.num(sec, "TextRectLeft", 1), tt = ini.num(sec, "TextRectTop", 1);
          text(page, inst, ini.hasKey(sec, "DispLabelText") ? ini.str(sec, "DispLabelText") : e.name, r, g, b,
               fontPx(sec, "DispLabelFontSize", "normal"), ini.str(sec, "DispLabelFontName", m.controlFont),
               tl, tt, ini.num(sec, "TextRectWidth", w - tl), ini.num(sec, "TextRectHeight", h - tt));
          ++drawn;
        } else if (e.kind == Kind::Enclosure) {
          if (!enclosureControls.count(e.enclosure)) continue;
          const std::string esec = "Enclosure" + n3(e.enclosure);
          const char style = static_cast<char>('A' + ini.num(sec, "EnclosureStyle", 1));
          const int count = ini.num(sec, "BitmapCount", 16);
          std::vector<std::string> frames;
          for (int f = 1; f <= count; ++f) {
            char stock[32];
            std::snprintf(stock, sizeof stock, "stock:enclosure%c%02d", style, f - 1);
            frames.push_back(ini.str(sec, "Bitmap" + n3(f), stock));
          }
          const auto wh = sizeOf(frames.front());
          const int w = ini.num(sec, "Width", wh.first), h = ini.num(sec, "Height", wh.second);
          const int x = ini.num(sec, "PositionX", lay.enclosureX(shoeIndex[i]));
          const int y = ini.num(sec, "PositionY", lay.enclosureY);
          const int ml = ini.num(sec, "MouseRectLeft", 0), mt = ini.num(sec, "MouseRectTop", 0);
          const int set = imageSet(frames, ml, mt, ini.num(sec, "MouseRectWidth", w - ml),
                                   ini.num(sec, "MouseRectHeight", h - mt));
          const int inst = instance(page, set, x, y, 2);
          auto cc = enclosureControls[e.enclosure];
          if (!cc.child("ImageSetInstanceID")) {
            Emitter::set(cc, "ImageSetInstanceID", inst);
            Emitter::yn(cc, "Clickable", true);
            Emitter::yn(cc, "ClickingHigherIncreasesValue", true);
            for (int f = 1; f <= count; ++f) {
              auto st = out.row("ContinuousControlImageSetStage");
              Emitter::set(st, "ImageSetID", set);
              Emitter::set(st, "HighestContinuousControlValue", (f * 128) / count - 1);
              Emitter::set(st, "ImageSetIndex", f);
            }
          }
          int r = 0x80, g = 0, b = 0;
          parseColour(ini.str(sec, "DispLabelColour", "Dark Red"), r, g, b);
          text(page, inst, ini.str(sec, "DispLabelText", ini.str(esec, "Name")), r, g, b,
               fontPx(sec, "DispLabelFontSize", "normal"), ini.str(sec, "DispLabelFontName", m.controlFont),
               ini.num(sec, "TextRectLeft", 0), ini.num(sec, "TextRectTop", 0),
               ini.num(sec, "TextRectWidth", w), ini.num(sec, "TextRectHeight", h));
          ++drawn;
        } else if (e.kind == Kind::Manual) {
          const auto mi = manualInfo.find(e.manual);
          if (mi == manualInfo.end()) continue;
          const ManualRow& row = lay.rows[rowOf[i]];
          const bool pedal = rowOf[i] == 0 && e.manual == 0;
          // The rails of wood the manual sits on (GOGUIManualBackground).
          wood(page, m.keyVertBg, lay.centerX(), row.y, lay.centerW, row.height);
          wood(page, m.keyHorizBg, lay.centerX(), row.pistonY, lay.centerW,
               (pedal && m.extraPedalButtonRow) ? 2 * m.buttonH : m.buttonH);
          const std::string msec = "Manual" + n3(e.manual);
          std::vector<int> midi, shown;
          const std::vector<bool> sharp = keysOf(e.manual, sec, midi, shown);
          const int count = static_cast<int>(sharp.size());
          std::string type = e.manual ? "Manual" : "Pedal";
          if (ini.yes(sec, "DispKeyColourInverted", false)) type += "Inverted";
          if (ini.yes(sec, "DispKeyColourWooden", false) && e.manual) type += "Wood";
          const int imageNum = ini.num(sec, "DispImageNum", 1);
          struct Key { std::string on, off; int x, y, ml, mt, mw, mh; };
          std::vector<Key> keys;
          int x = 0;
          for (int k = 0; k < count; ++k) {
            std::string base = names[shown[k] % 12];
            if (k == 0) base = "First" + base;
            else if (k + 1 == count) base = "Last" + base;
            const bool prevSharp = k > 0 && sharp[k - 1];
            const bool nextSharp = k + 1 < count && sharp[k + 1];
            std::string shape;
            if (!e.manual) shape = sharp[k] ? "Sharp" : "Natural";
            else if (sharp[k]) shape = "Sharp";
            else if (!prevSharp && nextSharp) shape = "C";
            else if (prevSharp && nextSharp) shape = "D";
            else if (prevSharp && !nextSharp) shape = "E";
            else shape = "Natural";
            char stockOff[64], stockOn[64];
            std::snprintf(stockOff, sizeof stockOff, "stock:%s%02dOff_%s", type.c_str(), imageNum, shape.c_str());
            std::snprintf(stockOn, sizeof stockOn, "stock:%s%02dOn_%s", type.c_str(), imageNum, shape.c_str());
            std::string on = ini.str(sec, "ImageOn_" + base, stockOn);
            std::string off = ini.str(sec, "ImageOff_" + base, stockOff);
            on = ini.str(sec, "Key" + n3(k + 1) + "ImageOn", on);
            off = ini.str(sec, "Key" + n3(k + 1) + "ImageOff", off);
            const auto wh = sizeOf(off);
            int width = wh.first, offset = 0, yoffset = 0;
            if (sharp[k] && e.manual) {
              width = 0;
              offset = -wh.first / 2;
            } else if (!e.manual && !nextSharp && !sharp[k]) {
              width *= 2;
            }
            width = ini.num(sec, "Width_" + base, width);
            offset = ini.num(sec, "Offset_" + base, offset);
            yoffset = ini.num(sec, "YOffset_" + base, yoffset);
            width = ini.num(sec, "Key" + n3(k + 1) + "Width", width);
            offset = ini.num(sec, "Key" + n3(k + 1) + "Offset", offset);
            yoffset = ini.num(sec, "Key" + n3(k + 1) + "YOffset", yoffset);
            const std::string kp = "Key" + n3(k + 1) + "MouseRect";
            const int ml = ini.num(sec, kp + "Left", 0), mt = ini.num(sec, kp + "Top", 0);
            keys.push_back({on, off, x + offset, yoffset, ml, mt,
                            ini.num(sec, kp + "Width", wh.first - ml), ini.num(sec, kp + "Height", wh.second - mt)});
            x += width;
          }
          const int left = ini.num(sec, "PositionX", row.x + 1), top = ini.num(sec, "PositionY", row.keysY);
          for (int k = 0; k < count; ++k) {
            const Key& key = keys[k];
            const int sw = nextKeySwitch++;
            emitSwitch(sw, ini.str(msec, "Name", msec) + " key " + std::to_string(midi[k]), false);
            auto kk = out.row("KeyboardKey");
            Emitter::set(kk, "KeyboardID", mi->second.kb);
            Emitter::set(kk, "SwitchID", sw);
            Emitter::set(kk, "NormalMIDINoteNumber", midi[k]);
            const int set = imageSet({key.off, key.on}, key.ml, key.mt, key.mw, key.mh);
            // Sharps are painted over the naturals they overlap.
            const int inst = instance(page, set, left + key.x, top + key.y, sharp[k] ? 4 : 3);
            auto srow = switchRows[sw];
            Emitter::set(srow, "Disp_ImageSetInstanceID", inst);
            Emitter::set(srow, "Disp_ImageSetIndexEngaged", 2);
            Emitter::set(srow, "Disp_ImageSetIndexDisengaged", 1);
          }
          ++drawn;
        } else if (e.kind == Kind::Label) {
          // A nameplate: free, or at the top or bottom of a drawstop column
          // (GOGUILabel).
          int x = -1, y = -1;
          if (!ini.yes(sec, "FreeXPlacement", true)) {
            const int col = ini.num(sec, "DispDrawstopCol", 1);
            const int half = m.drawstopCols >> 1;
            int dx = ini.yes(sec, "DispSpanDrawstopColToRight", false) ? 39 : 0;
            if (col <= half) x = lay.jambLeftX() + dx + (col - 1) * 78 + 1;
            else x = lay.jambRightX() + dx + (col - 1 - half) * 78 + 1;
          } else {
            x = ini.num(sec, "DispXpos", 0);
          }
          if (!ini.yes(sec, "FreeYPlacement", true))
            y = ini.yes(sec, "DispAtTopOfDrawstopCol", false)
                    ? lay.jambLRY() + 1
                    : lay.jambLRY() + 1 + lay.jambLRHeight() - 32;
          else
            y = ini.num(sec, "DispYpos", 0);
          x = ini.num(sec, "PositionX", x);
          y = ini.num(sec, "PositionY", y);
          std::string image = ini.str(sec, "Image");
          const int imageNum = ini.num(sec, "DispImageNum", 1);
          if (image.empty() && imageNum > 0) image = stockName("label%02d", imageNum);
          const auto wh = image.empty() ? std::pair<int, int>{80, 25} : sizeOf(image);
          const int w = ini.num(sec, "Width", wh.first > 0 ? wh.first : 80);
          const int h = ini.num(sec, "Height", wh.second > 0 ? wh.second : 25);
          int inst = 0;
          if (!image.empty()) inst = instance(page, imageSet({image}, 0, 0, 0, 0), x, y, 1);
          int r = 0, g = 0, b = 0;
          parseColour(ini.str(sec, "DispLabelColour", "BLACK"), r, g, b);
          const int tl = ini.num(sec, "TextRectLeft", 1), tt = ini.num(sec, "TextRectTop", 1);
          text(page, inst, ini.str(sec, "Name"), r, g, b, fontPx(sec, "DispLabelFontSize", "normal"),
               ini.str(sec, "DispLabelFontName", m.controlFont), inst ? tl : x + tl, inst ? tt : y + tt,
               ini.num(sec, "TextRectWidth", w - tl), ini.num(sec, "TextRectHeight", h - tt));
          ++drawn;
        }
      }
      auto pg = out.row("DisplayPage");
      Emitter::set(pg, "PageID", page);
      Emitter::set(pg, "Name", pageName);
    };

    buildPanel(1, newFormat ? "Panel000" : "Organ", newFormat ? "Panel000" : "", true,
               ini.str(newFormat ? "Panel000" : "Organ", newFormat ? "Name" : "ChurchName",
                       ini.str("Organ", "ChurchName", "Console")));
    for (int pn = 1; pn <= ini.num("Organ", "NumberOfPanels", 0); ++pn) {
      const std::string ps = "Panel" + n3(pn);
      buildPanel(pn + 1, ps, ps, false, ini.str(ps, "Name", ps));
    }
  }

  std::ostringstream xml;
  out.doc.save(xml, "  ", pugi::format_indent, pugi::encoding_utf8);
  rep.xml = xml.str();
  rep.ok = true;
  return rep;
}

GrandOrgueImportReport convertGrandOrgue(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    GrandOrgueImportReport rep;
    rep.error = "cannot open " + path;
    return rep;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  const auto slash = path.find_last_of("/\\");
  return convertGrandOrgueText(ss.str(), slash == std::string::npos ? "." : path.substr(0, slash));
}

}  // namespace mp
