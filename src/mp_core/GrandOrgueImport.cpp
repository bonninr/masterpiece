#include "GrandOrgueImport.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
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
  auto buildRank = [&](const std::string& rankSec, int rankId, int palletSwitch = 0) {
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
      const int midi = firstMidiOf(sec) + index - 1;
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
        std::string prefix;
      };
      std::vector<Attack> attacks;
      auto addAttack = [&](const std::string& prefix, const std::string& f) {
        if (f.empty()) return;
        if (ini.num(sec, prefix + "MaxTimeSinceLastRelease", -1) >= 0) {
          note("repetition attacks (MaxTimeSinceLastRelease) are left out; the normal attack is used");
          return;
        }
        if (ini.num(sec, prefix + "IsTremulant", -1) == 1) {
          note("recorded tremulant samples (IsTremulant=1) are left out; the tremulant is synthesised");
          return;
        }
        if (ini.num(sec, prefix + "LoopCount", 0) > 0)
          note("loops declared in the definition are not used; each file's own loop is");
        attacks.push_back({f, ini.num(sec, prefix + "AttackVelocity", 0), prefix});
      };
      addAttack(key, file);
      for (int a = 1; a <= ini.num(sec, key + "AttackCount", 0); ++a) {
        const std::string pre = key + "Attack" + n3(a);
        addAttack(pre, ini.str(sec, pre));
      }
      std::stable_sort(attacks.begin(), attacks.end(),
                       [](const Attack& a, const Attack& b) { return a.minVelocity < b.minVelocity; });
      for (size_t i = 0; i < attacks.size(); ++i) {
        const int sampleId = sampleFor(attacks[i].file, nominalHz);
        const int highest = (i + 1 < attacks.size()) ? std::max(0, attacks[i + 1].minVelocity - 1) : 127;
        auto at = out.row("Pipe_SoundEngine01_AttackSample");
        const int attackUid = nextUniqueId++;
        Emitter::set(at, "UniqueID", attackUid);
        Emitter::set(at, "LayerID", pipeId);
        Emitter::set(at, "SampleID", sampleId);
        Emitter::set(at, "AttackSelCriteria_HighestVelocity", highest);
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
        if (f.empty()) continue;
        if (ini.num(sec, pre + "IsTremulant", -1) == 1) continue;
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

  const int rankCount = ini.num("Organ", "NumberOfRanks", 0);
  for (int r = 1; r <= rankCount; ++r) buildRank("Rank" + n3(r), kRankBase + r);

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
  auto link = [&](int source, int dest, int condition, bool sourceEngaged) {
    auto l = out.row("SwitchLinkage");
    Emitter::set(l, "SourceSwitchID", source);
    Emitter::set(l, "DestSwitchID", dest);
    if (condition) {
      Emitter::set(l, "ConditionSwitchID", condition);
      Emitter::yn(l, "ConditionSwitchLinkIfEngaged", true);
    }
    Emitter::yn(l, "SourceSwitchLinkIfEngaged", sourceEngaged);
    Emitter::set(l, "EngageLinkActionCode", 1);
    Emitter::set(l, "DisengageLinkActionCode", 2);
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
    if (lower(ini.str(sec, "TremulantType", "Synth")) == "wave")
      note("recorded (wave) tremulants are played as synthesised ones");
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
      };
      if (ranksInStop == 0) {
        // The stop carries its pipes itself: it is its own rank.
        const int rankId = kStopRankBase + stopNo;
        if (!rankPipeIds.count(rankId)) {
          buildRank(ssec, rankId);
          for (const auto& [k, p] : pipeOfRankPipe)
            if (k / 1000 == rankId) wirePipe(p);
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
      auto ka = out.row("KeyAction");
      Emitter::set(ka, "SourceKeyboardID", info.kb);
      Emitter::yn(ka, "DestIsKeyboardNotDivision", false);
      Emitter::set(ka, "DestDivisionID", dIt->second.kb);
      Emitter::set(ka, "ActionTypeCode", 1);
      Emitter::set(ka, "ActionEffectCode", 1);
      Emitter::set(ka, "MIDINoteNumOfFirstSourceKey", info.firstMidi);
      Emitter::set(ka, "NumberOfKeys", info.keys);
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

  if (ini.num("Organ", "NumberOfGenerals", 0) > 0)
    note("generals and divisionals are not imported yet");

  // ---- the console --------------------------------------------------------
  // Each GrandOrgue panel is a display page. What is placed explicitly -- an
  // image, a drawstop with its pictures, a manual drawn key by key, a swell
  // shoe -- is drawn where the set puts it. GrandOrgue's automatic layout,
  // built from its own stock bitmaps, has no pictures to draw here, so a set
  // that relies on it keeps the plain jamb.
  if (ini.num("Panel000", "NumberOfGUIElements", -1) >= 0) {
    note("this set uses GrandOrgue's newer panel format, which is not drawn yet; the stops are on the plain jamb");
  } else {
    const std::string& root = organRoot;
    std::map<std::string, std::pair<int, int>> sizes;
    auto sizeOf = [&](const std::string& file) {
      const auto it = sizes.find(file);
      if (it != sizes.end()) return it->second;
      std::pair<int, int> wh{0, 0};
      if (!root.empty()) {
        std::string rel = file;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        wh = imageSize(root + "/" + rel);
      }
      return sizes[file] = wh;
    };
    auto rel = [](std::string f) {
      std::replace(f.begin(), f.end(), '\\', '/');
      return f;
    };

    int nextSet = 1, nextInstance = 1, nextText = 1, nextStyle = 1, nextMirror = 1;
    std::map<std::string, int> setMemo, styleMemo;
    auto imageSet = [&](const std::vector<std::string>& files, int clickL, int clickT, int clickW, int clickH) {
      std::string key;
      for (const auto& f : files) key += rel(f) + "|";
      key += std::to_string(clickL) + "," + std::to_string(clickT) + "," + std::to_string(clickW) + "," + std::to_string(clickH);
      const auto hit = setMemo.find(key);
      if (hit != setMemo.end()) return hit->second;
      const int id = nextSet++;
      auto set = out.row("ImageSet");
      Emitter::set(set, "ImageSetID", id);
      Emitter::set(set, "Name", rel(files.front()));
      Emitter::set(set, "InstallationPackageID", 0);
      const auto wh = sizeOf(files.front());
      if (wh.first > 0) {
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
        Emitter::set(el, "BitmapFilename", rel(files[i]));
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
    // A switch has one drawn instance. A second appearance -- the same stop
    // on the console and on a jamb -- is a mirror switch wired both ways.
    std::set<int> drawnSwitches;
    auto attach = [&](int switchId, int inst) {
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
      Emitter::yn(row, "Clickable", true);
    };
    auto label = [&](int page, int inst, const std::string& sec, const std::string& fallback, int w, int h) {
      const std::string text = ini.hasKey(sec, "DispLabelText") ? ini.str(sec, "DispLabelText") : fallback;
      if (text.empty() || w <= 0 || h <= 0) return;
      int r = 0x80, g = 0, b = 0;
      parseColour(ini.str(sec, "DispLabelColour", "Dark Red"), r, g, b);
      const std::string sizeText = lower(ini.str(sec, "DispLabelFontSize", "normal"));
      int px = sizeText == "small" ? 7 : sizeText == "large" ? 12 : sizeText == "normal" ? 9 : std::atoi(sizeText.c_str());
      if (px <= 0) px = 9;
      const std::string face = ini.str(sec, "DispLabelFontName", "Arial");
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
        Emitter::set(st, "Face_WindowsName", face);
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
      Emitter::set(t, "Text", text);
      Emitter::set(t, "TextStyleID", style);
      Emitter::set(t, "XPosPixels", 0);
      Emitter::set(t, "YPosPixels", 0);
      Emitter::set(t, "BoundingBoxWidthPixelsIfWordWrap", w);
      Emitter::set(t, "BoundingBoxHeightPixelsIfWordWrap", h);
      Emitter::yn(t, "AttachedToAnImageSetInstance", true);
      Emitter::set(t, "AttachedToImageSetInstanceID", inst);
      Emitter::yn(t, "PosRelativeToTopLeftOfImageSetInstance", true);
    };

    int drawn = 0;
    // An image, tiled when the set asks for more room than the bitmap has.
    auto drawImage = [&](int page, const std::string& sec) {
      const std::string file = ini.str(sec, "Image");
      if (file.empty()) return;
      const int x = ini.num(sec, "PositionX", 0), y = ini.num(sec, "PositionY", 0);
      const auto wh = sizeOf(file);
      const int w = ini.num(sec, "Width", wh.first), h = ini.num(sec, "Height", wh.second);
      const int set = imageSet({file}, 0, 0, 0, 0);
      const bool tile = wh.first > 0 && (w > wh.first || h > wh.second);
      instance(page, set, x, y, 0, tile ? x + w : 0, tile ? y + h : 0);
      ++drawn;
    };
    // A drawstop, piston or tab: its own pictures at its own place.
    auto drawButton = [&](int page, const std::string& sec, int switchId, const std::string& name) {
      if (!ini.hasKey(sec, "PositionX") || !ini.hasKey(sec, "ImageOn")) return;
      const std::string on = ini.str(sec, "ImageOn"), off = ini.str(sec, "ImageOff", on);
      const auto wh = sizeOf(off);
      const int w = ini.num(sec, "Width", wh.first), h = ini.num(sec, "Height", wh.second);
      const int ml = ini.num(sec, "MouseRectLeft", 0), mt = ini.num(sec, "MouseRectTop", 0);
      const int mw = ini.num(sec, "MouseRectWidth", w - ml), mh = ini.num(sec, "MouseRectHeight", h - mt);
      const int set = imageSet({off, on}, ml, mt, mw, mh);
      const int inst = instance(page, set, ini.num(sec, "PositionX", 0), ini.num(sec, "PositionY", 0), 2);
      attach(switchId, inst);
      label(page, inst, sec, name, w, h);
      ++drawn;
    };
    // A swell shoe: its frames, closed to open, on the enclosure's control.
    auto drawEnclosure = [&](int page, const std::string& sec, int e) {
      const int n = ini.num(sec, "BitmapCount", 0);
      if (n < 1 || !ini.hasKey(sec, "PositionX") || !enclosureControls.count(e)) return;
      std::vector<std::string> frames;
      for (int i = 1; i <= n; ++i) frames.push_back(ini.str(sec, "Bitmap" + n3(i)));
      const auto wh = sizeOf(frames.front());
      const int ml = ini.num(sec, "MouseRectLeft", 0), mt = ini.num(sec, "MouseRectTop", 0);
      const int set = imageSet(frames, ml, mt, ini.num(sec, "MouseRectWidth", wh.first - ml),
                               ini.num(sec, "MouseRectHeight", wh.second - mt));
      const int inst = instance(page, set, ini.num(sec, "PositionX", 0), ini.num(sec, "PositionY", 0), 2);
      auto cc = enclosureControls[e];
      if (!cc.child("ImageSetInstanceID")) {
        Emitter::set(cc, "ImageSetInstanceID", inst);
        Emitter::yn(cc, "Clickable", true);
        Emitter::yn(cc, "ClickingHigherIncreasesValue", true);
        for (int i = 1; i <= n; ++i) {
          auto st = out.row("ContinuousControlImageSetStage");
          Emitter::set(st, "ImageSetID", set);
          Emitter::set(st, "HighestContinuousControlValue", (i * 128) / n - 1);
          Emitter::set(st, "ImageSetIndex", i);
        }
      }
      ++drawn;
    };
    // A manual, key by key, laid out as GrandOrgue lays it out: each key at
    // the running position plus its offset, the position advancing by the
    // key's width -- a natural's bitmap, nothing for a sharp, which sits
    // centred on the join.
    int nextKeySwitch = 20000;
    auto drawManual = [&](int page, const std::string& sec, int m) {
      const auto mi = manualInfo.find(m);
      if (mi == manualInfo.end() || !ini.hasKey(sec, "PositionX")) return;
      static const char* names[12] = {"C", "Cis", "D", "Dis", "E", "F", "Fis", "G", "Gis", "A", "Ais", "B"};
      const std::string msec = "Manual" + n3(m);
      const int firstNote = ini.num(sec, "DisplayFirstNote", ini.num(msec, "FirstAccessibleKeyMIDINoteNumber", 36));
      const int count = ini.num(sec, "DisplayKeys", ini.num(msec, "NumberOfAccessibleKeys", 61));
      std::vector<int> midi(count), shown(count);
      std::vector<bool> sharp(count);
      for (int i = 0; i < count; ++i) {
        midi[i] = ini.num(sec, "DisplayKey" + n3(i + 1), firstNote + i);
        shown[i] = ini.num(sec, "DisplayKey" + n3(i + 1) + "Note", firstNote + i);
        const int k = shown[i];
        sharp[i] = !(((k % 12) < 5 && !(k & 1)) || ((k % 12) >= 5 && (k & 1)));
      }
      struct Key { std::string on, off; int x, y, ml, mt, mw, mh; };
      std::vector<Key> keys;
      int x = 0;
      for (int i = 0; i < count; ++i) {
        std::string base = names[shown[i] % 12];
        if (i == 0) base = "First" + base;
        else if (i + 1 == count) base = "Last" + base;
        std::string on = ini.str(sec, "ImageOn_" + base), off = ini.str(sec, "ImageOff_" + base);
        on = ini.str(sec, "Key" + n3(i + 1) + "ImageOn", on);
        off = ini.str(sec, "Key" + n3(i + 1) + "ImageOff", off);
        if (on.empty() || off.empty()) {
          note("a manual drawn with GrandOrgue's own key bitmaps is not drawn; it plays from the keyboard");
          return;
        }
        const auto wh = sizeOf(off);
        int width = wh.first, offset = 0, yoffset = 0;
        const bool nextSharp = i + 1 < count && sharp[i + 1];
        if (sharp[i] && m != 0) {
          width = 0;
          offset = -wh.first / 2;
        } else if (m == 0 && !nextSharp && !sharp[i]) {
          width *= 2;
        }
        width = ini.num(sec, "Width_" + base, width);
        offset = ini.num(sec, "Offset_" + base, offset);
        yoffset = ini.num(sec, "YOffset_" + base, yoffset);
        width = ini.num(sec, "Key" + n3(i + 1) + "Width", width);
        offset = ini.num(sec, "Key" + n3(i + 1) + "Offset", offset);
        yoffset = ini.num(sec, "Key" + n3(i + 1) + "YOffset", yoffset);
        if (wh.first <= 0 && !ini.hasKey(sec, "Key" + n3(i + 1) + "Width")) {
          note("key bitmaps could not be measured, so a manual is not drawn; it plays from the keyboard");
          return;
        }
        const std::string kp = "Key" + n3(i + 1) + "MouseRect";
        const int ml = ini.num(sec, kp + "Left", 0), mt = ini.num(sec, kp + "Top", 0);
        keys.push_back({on, off, x + offset, yoffset, ml, mt,
                        ini.num(sec, kp + "Width", wh.first - ml), ini.num(sec, kp + "Height", wh.second - mt)});
        x += width;
      }
      const int left = ini.num(sec, "PositionX", 0), top = ini.num(sec, "PositionY", 0);
      for (int i = 0; i < count; ++i) {
        const Key& k = keys[i];
        const int sw = nextKeySwitch++;
        emitSwitch(sw, ini.str(msec, "Name", msec) + " key " + std::to_string(midi[i]), false);
        auto kk = out.row("KeyboardKey");
        Emitter::set(kk, "KeyboardID", mi->second.kb);
        Emitter::set(kk, "SwitchID", sw);
        Emitter::set(kk, "NormalMIDINoteNumber", midi[i]);
        const int set = imageSet({k.off, k.on}, k.ml, k.mt, k.mw, k.mh);
        // Sharps are painted over the naturals they overlap.
        const int inst = instance(page, set, left + k.x, top + k.y, sharp[i] ? 4 : 3);
        auto row = switchRows[sw];
        Emitter::set(row, "Disp_ImageSetInstanceID", inst);
        Emitter::set(row, "Disp_ImageSetIndexEngaged", 2);
        Emitter::set(row, "Disp_ImageSetIndexDisengaged", 1);
      }
      ++drawn;
    };
    auto page = [&](int id, const std::string& name) {
      auto pg = out.row("DisplayPage");
      Emitter::set(pg, "PageID", id);
      Emitter::set(pg, "Name", name);
    };

    // The main panel: the organ's own sections, where they are displayed.
    const int before = drawn;
    for (int i = 1; i <= ini.num("Organ", "NumberOfImages", 0); ++i) drawImage(1, "Image" + n3(i));
    for (int e = 1; e <= encCount; ++e)
      if (ini.yes("Enclosure" + n3(e), "Displayed", false)) drawEnclosure(1, "Enclosure" + n3(e), e);
    for (int t = 1; t <= tremCount; ++t) {
      const std::string sec = "Tremulant" + n3(t);
      if (ini.yes(sec, "Displayed", false)) drawButton(1, sec, controlFor(sec, kTremulantSwitchBase + t), ini.str(sec, "Name"));
    }
    for (int n = 1; n <= switchCount; ++n) {
      const std::string sec = "Switch" + n3(n);
      if (ini.yes(sec, "Displayed", false)) drawButton(1, sec, kGoSwitchBase + n, ini.str(sec, "Name"));
    }
    for (const auto& [m, info] : manualInfo) {
      const std::string msec = "Manual" + n3(m);
      if (ini.yes(msec, "Displayed", false)) drawManual(1, msec, m);
      for (int c = 1; c <= ini.num(msec, "NumberOfCouplers", 0); ++c) {
        const int cNo = ini.num(msec, "Coupler" + n3(c), 0);
        const std::string sec = "Coupler" + n3(cNo);
        if (ini.yes(sec, "Displayed", false))
          drawButton(1, sec, controlFor(sec, kCouplerSwitchBase + cNo), ini.str(sec, "Name"));
      }
      for (int st = 1; st <= ini.num(msec, "NumberOfStops", 0); ++st) {
        const int sNo = ini.num(msec, "Stop" + n3(st), 0);
        const std::string sec = "Stop" + n3(sNo);
        if (ini.yes(sec, "Displayed", false))
          drawButton(1, sec, controlFor(sec, kStopSwitchBase + sNo), ini.str(sec, "Name"));
      }
    }
    const bool mainDrawn = drawn > before;
    if (mainDrawn) page(1, ini.str("Organ", "ChurchName", "Console"));

    // The other panels list what they show; each element's picture and
    // place are in a section of the panel's own.
    for (int pn = 1; pn <= ini.num("Organ", "NumberOfPanels", 0); ++pn) {
      const std::string ps = "Panel" + n3(pn);
      const int id = pn + 1;
      const int start = drawn;
      for (int i = 1; i <= ini.num(ps, "NumberOfImages", 0); ++i) drawImage(id, ps + "Image" + n3(i));
      for (int i = 1; i <= ini.num(ps, "NumberOfEnclosures", 0); ++i) {
        const int e = ini.num(ps, "Enclosure" + n3(i), 0);
        drawEnclosure(id, ps + "Enclosure" + n3(e), e);
      }
      for (int i = 1; i <= ini.num(ps, "NumberOfTremulants", 0); ++i) {
        const int t = ini.num(ps, "Tremulant" + n3(i), 0);
        const std::string tsec = "Tremulant" + n3(t);
        drawButton(id, ps + tsec, controlFor(tsec, kTremulantSwitchBase + t), ini.str(tsec, "Name"));
      }
      for (int i = 1; i <= ini.num(ps, "NumberOfSwitches", 0); ++i) {
        const int n = ini.num(ps, "Switch" + n3(i), 0);
        drawButton(id, ps + "Switch" + n3(n), kGoSwitchBase + n, ini.str("Switch" + n3(n), "Name"));
      }
      const bool pedalsHere = ini.yes(ps, "HasPedals", false);
      for (int i = pedalsHere ? 0 : 1; i <= ini.num(ps, "NumberOfManuals", 0); ++i) {
        const int m = ini.num(ps, "Manual" + n3(i), i);
        drawManual(id, ps + "Manual" + n3(m), m);
      }
      for (int i = 1; i <= ini.num(ps, "NumberOfCouplers", 0); ++i) {
        const int m = ini.num(ps, "Coupler" + n3(i) + "Manual", 1);
        const int cNo = ini.num("Manual" + n3(m), "Coupler" + n3(ini.num(ps, "Coupler" + n3(i), 1)), 0);
        const std::string csec = "Coupler" + n3(cNo);
        drawButton(id, ps + "Coupler" + n3(i), controlFor(csec, kCouplerSwitchBase + cNo), ini.str(csec, "Name"));
      }
      for (int i = 1; i <= ini.num(ps, "NumberOfStops", 0); ++i) {
        const int m = ini.num(ps, "Stop" + n3(i) + "Manual", 1);
        const int sNo = ini.num("Manual" + n3(m), "Stop" + n3(ini.num(ps, "Stop" + n3(i), 1)), 0);
        const std::string ssec = "Stop" + n3(sNo);
        drawButton(id, ps + "Stop" + n3(i), controlFor(ssec, kStopSwitchBase + sNo), ini.str(ssec, "Name"));
      }
      if (drawn > start) page(id, ini.str(ps, "Name", ps));
    }
    if (drawn == 0 && (ini.num("Organ", "NumberOfPanels", 0) > 0 || ini.num("Organ", "NumberOfImages", 0) > 0))
      note("the console places its controls automatically, which is not drawn yet; the stops are on the plain jamb");
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
