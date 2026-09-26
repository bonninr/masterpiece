#include "GrandOrgueImport.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
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
  double pct = ini.real(section, prefix + "AmplitudeLevel", -1.0);
  if (pct < 0.0) pct = ini.real(section, prefix + "Amplitude", 100.0);
  if (pct > 0.0) db += 20.0 * std::log10(pct / 100.0);
  return db;
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

struct PipeRef {
  int pipeId = 0;
  int windchest = 0;
};

}  // namespace

bool isGrandOrgueDefinition(const std::string& path) {
  const auto dot = path.find_last_of('.');
  return dot != std::string::npos && lower(path.substr(dot)) == ".organ";
}

GrandOrgueImportReport convertGrandOrgueText(const std::string& rawText) {
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
  }
  const double organDb = levelDb(ini, "Organ");
  const double organCents = ini.real("Organ", "PitchTuning", 0.0);

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
    chestCents[w] = ini.real(sec, "PitchTuning", 0.0);
  }

  // ---- samples, pipes, ranks ------------------------------------------------
  std::map<std::string, int> sampleIds;  // relative file -> SampleID
  int nextPipeId = 1, nextUniqueId = 1;
  std::map<int, PipeRef> pipeOfRankPipe;  // rankId * 1000 + index -> pipe
  std::map<int, std::vector<int>> rankPipeIds;
  std::map<int, int> rankFirstMidi;

  auto sampleFor = [&](const std::string& file) {
    std::string rel = file;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    const auto it = sampleIds.find(rel);
    if (it != sampleIds.end()) return it->second;
    const int id = static_cast<int>(sampleIds.size()) + 1;
    sampleIds[rel] = id;
    auto s = out.row("Sample");
    Emitter::set(s, "SampleID", id);
    Emitter::set(s, "InstallationPackageID", 0);
    Emitter::set(s, "SampleFilename", rel);
    return id;
  };

  // One rank: a [RankNNN], or a stop that carries its pipes itself.
  auto buildRank = [&](const std::string& sec, int rankId) {
    const std::string name = ini.str(sec, "Name", sec);
    auto r = out.row("Rank");
    Emitter::set(r, "RankID", rankId);
    Emitter::set(r, "Name", name);
    const int firstMidi = ini.num(sec, "FirstMidiNoteNumber", 36);
    rankFirstMidi[rankId] = firstMidi;
    const int count = ini.num(sec, "NumberOfLogicalPipes", 0);
    const int rankHarmonic = ini.num(sec, "HarmonicNumber", 8);
    const int rankChest = ini.num(sec, "WindchestGroup", 1);
    const double rankDb = levelDb(ini, sec);
    const double rankCents = ini.real(sec, "PitchTuning", 0.0);
    const bool rankPercussive = ini.yes(sec, "Percussive", false);
    auto& ids = rankPipeIds[rankId];
    for (int p = 1; p <= count; ++p) {
      const std::string key = "Pipe" + n3(p);
      const std::string file = ini.str(sec, key);
      ids.push_back(0);
      if (file.empty() || lower(file) == "dummy") continue;
      if (lower(file).rfind("ref:", 0) == 0) {
        note("pipes that borrow another rank's (REF:) are not yet shared; they are silent");
        continue;
      }
      const int pipeId = nextPipeId++;
      ids.back() = pipeId;
      const int midi = firstMidi + p - 1;
      const int harmonic = ini.num(sec, key + "HarmonicNumber", rankHarmonic);
      const int chest = ini.num(sec, key + "WindchestGroup", rankChest);
      pipeOfRankPipe[rankId * 1000 + p] = {pipeId, chest};
      const double cents = organCents + chestCents[chest] + rankCents +
                           ini.real(sec, key + "PitchTuning", 0.0);
      auto pipe = out.row("Pipe_SoundEngine01");
      Emitter::set(pipe, "PipeID", pipeId);
      Emitter::set(pipe, "RankID", rankId);
      Emitter::set(pipe, "NormalMIDINoteNumber", midi);
      Emitter::set(pipe, "Pitch_Tempered_RankBasePitch64ftHarmonicNum", harmonic);
      if (cents != 0.0) Emitter::set(pipe, "Pitch_Tempered_BaseTuningDeviation", cents);

      auto layer = out.row("Pipe_SoundEngine01_Layer");
      Emitter::set(layer, "LayerID", pipeId);
      Emitter::set(layer, "PipeID", pipeId);
      const double db = organDb + chestDb[chest] + rankDb + levelDb(ini, sec, key);
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
        const int sampleId = sampleFor(attacks[i].file);
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
        Emitter::set(rel, "SampleID", sampleFor(f));
        const int maxMs = ini.num(sec, pre + "MaxKeyPressTime", -1);
        if (maxMs >= 0) Emitter::set(rel, "ReleaseSelCriteria_LatestKeyReleaseTimeMs", maxMs);
      }
      // An exact recorded pitch declared in the definition beats the file.
      const int keyNumber = ini.num(sec, key + "MIDIKeyNumber", -1);
      if (keyNumber >= 0) {
        const double fraction = ini.real(sec, key + "MIDIPitchFraction", 0.0);
        const double hz = 440.0 * std::pow(2.0, (keyNumber + fraction / 100.0 - 69.0) / 12.0);
        // Every sample of this pipe is at that pitch; the attack's row is
        // the last one written, so it is found by id and annotated.
        for (const auto& a : attacks) {
          std::string rel = a.file;
          std::replace(rel.begin(), rel.end(), '\\', '/');
          const int sid = sampleIds[rel];
          for (auto s : out.lists["Sample"].children("Sample"))
            if (s.child("SampleID").text().as_int() == sid && !s.child("Pitch_ExactSamplePitch")) {
              Emitter::set(s, "Pitch_SpecificationMethodCode", 4);
              Emitter::set(s, "Pitch_ExactSamplePitch", hz);
            }
        }
      }
    }
  };

  const int rankCount = ini.num("Organ", "NumberOfRanks", 0);
  for (int r = 1; r <= rankCount; ++r) buildRank("Rank" + n3(r), kRankBase + r);

  // ---- tremulants, enclosures --------------------------------------------
  const int tremCount = ini.num("Organ", "NumberOfTremulants", 0);
  for (int t = 1; t <= tremCount; ++t) {
    const std::string sec = "Tremulant" + n3(t);
    const int sw = kTremulantSwitchBase + t;
    auto s = out.row("Switch");
    Emitter::set(s, "SwitchID", sw);
    Emitter::set(s, "Name", ini.str(sec, "Name", sec));
    Emitter::yn(s, "DefaultToEngaged", ini.yes(sec, "DefaultToEngaged", false));
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
  for (int e = 1; e <= encCount; ++e) {
    const std::string sec = "Enclosure" + n3(e);
    auto cc = out.row("ContinuousControl");
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
  const int manuals = ini.num("Organ", "NumberOfManuals", 0);
  const bool pedals = ini.yes("Organ", "HasPedals", false);
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
    // Hauptwerk's channel convention, which is also GrandOrgue's order:
    // the pedal on 1, the first manual on 2.
    Emitter::set(kb, "DefaultInputOutputKeyboardAsgnCode", pedals ? m + 1 : m + 1);
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
      const std::string csec = "Coupler" + n3(ini.num(sec, "Coupler" + n3(c), 0));
      if (ini.yes(csec, "UnisonOff", false)) unisonOffSwitch = kUnisonOffSwitchBase + ini.num(sec, "Coupler" + n3(c), 0);
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
      const int sw = kStopSwitchBase + stopNo;
      auto swr = out.row("Switch");
      Emitter::set(swr, "SwitchID", sw);
      Emitter::set(swr, "Name", ini.str(ssec, "Name", ssec));
      Emitter::yn(swr, "DefaultToEngaged", ini.yes(ssec, "DefaultToEngaged", false));
      auto st = out.row("Stop");
      Emitter::set(st, "StopID", stopId);
      Emitter::set(st, "Name", ini.str(ssec, "Name", ssec));
      Emitter::set(st, "DivisionID", info.kb);
      Emitter::set(st, "ControllingSwitchID", sw);

      const int firstKey = ini.num(ssec, "FirstAccessiblePipeLogicalKeyNumber", 1);
      const int firstPipe = ini.num(ssec, "FirstAccessiblePipeLogicalPipeNumber", 1);
      const int accessible = ini.num(ssec, "NumberOfAccessiblePipes", info.keys);
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
      const int ranksInStop = ini.num(ssec, "NumberOfRanks", 0);
      if (ranksInStop == 0) {
        // The stop carries its pipes itself: it is its own rank.
        const int rankId = kStopRankBase + stopNo;
        if (!rankPipeIds.count(rankId)) {
          const size_t before = pipeOfRankPipe.size();
          buildRank(ssec, rankId);
          (void)before;
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
          const int keyStart = firstKey + ini.num(ssec, rk + "FirstAccessibleKeyNumber", 1) - 1;
          const int pipeStart = ini.num(ssec, rk + "FirstPipeNumber", firstPipe);
          const int count = ini.num(ssec, rk + "PipeCount", accessible);
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
      const int sw = unisonOff ? kUnisonOffSwitchBase + cNo : kCouplerSwitchBase + cNo;
      auto swr = out.row("Switch");
      Emitter::set(swr, "SwitchID", sw);
      Emitter::set(swr, "Name", ini.str(csec, "Name", csec));
      Emitter::yn(swr, "DefaultToEngaged", ini.yes(csec, "DefaultToEngaged", false));
      // A stop row makes the coupler a drawable, listed control like the
      // rest; it sounds nothing itself.
      auto st = out.row("Stop");
      Emitter::set(st, "StopID", kStopBase + 500 + cNo);
      Emitter::set(st, "Name", ini.str(csec, "Name", csec));
      Emitter::set(st, "DivisionID", info.kb);
      Emitter::set(st, "ControllingSwitchID", sw);
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
      if (tNo < 1 || tNo > tremCount) continue;
      auto st = out.row("Stop");
      Emitter::set(st, "StopID", kStopBase + 800 + tNo);
      Emitter::set(st, "Name", ini.str("Tremulant" + n3(tNo), "Name", "Tremulant"));
      Emitter::set(st, "DivisionID", info.kb);
      Emitter::set(st, "ControllingSwitchID", kTremulantSwitchBase + tNo);
    }
  }

  if (ini.num("Organ", "NumberOfGenerals", 0) > 0 || ini.num("Organ", "NumberOfSwitches", 0) > 0)
    note("generals, divisionals and logical switches are not imported yet");
  if (ini.has("Panel000") || ini.num("Organ", "NumberOfPanels", 0) > 0)
    note("the console panels are not drawn yet; the stops are on the plain jamb");

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
  return convertGrandOrgueText(ss.str());
}

}  // namespace mp
