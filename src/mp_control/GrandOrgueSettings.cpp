#include "GrandOrgueSettings.h"
#include "GrandOrgueCmb.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_map>

namespace mp {

namespace {

using gocmb::lower;
using gocmb::Section;

std::string n3(int i) {
  char b[16];
  std::snprintf(b, sizeof b, "%03d", i);
  return b;
}

// Masterpiece's voicing range: a level from -24 dB (the control API's floor)
// to +12, tuning to 50 cents either way, a shelf to 12 dB either way.
constexpr double kMinGainDb = -24.0, kMaxGainDb = 12.0;
constexpr double kMaxCents = 50.0;
constexpr double kMaxShelfDb = 12.0;

class Reader {
public:
  Reader(const std::vector<Section>& sections, const OrganModel& model, GrandOrgueSettingsReport& report)
      : model_(model), report_(report) {
    for (const auto& s : sections) byName_[lower(s.name)] = &s;
    // Which manual each organ divisional belongs to: "manual002/divisional007".
    for (const auto& [key, object] : model.grandOrgueObjects) {
      const size_t slash = key.find('/');
      if (slash == std::string::npos || key.compare(slash + 1, 10, "divisional") != 0) continue;
      int m = -1;
      if (std::sscanf(key.c_str(), "manual%d/", &m) == 1) manualOfDivisional_[key.substr(slash + 1)] = m;
    }
  }

  const Section* section(const std::string& name) const {
    const auto it = byName_.find(lower(name));
    return it == byName_.end() ? nullptr : it->second;
  }

  // The switch a section of the .organ became, or 0.
  Id switchOf(const std::string& key) const {
    const auto it = model_.grandOrgueObjects.find(lower(key));
    return it != model_.grandOrgueObjects.end() && it->second.kind == OrganModel::GrandOrgueObject::Switch
               ? it->second.id
               : 0;
  }

  void add(GrandOrgueRegistration& r, Id sw, int signedNumber) {
    if (signedNumber == 0) return;
    if (sw == 0) {
      ++report_.unknownElements;
      return;
    }
    r.switches.emplace_back(sw, signedNumber > 0);
  }

  // A general: stops, couplers and switches named by manual and place,
  // tremulants and the organ's own switches by number (GOGeneralCombination).
  GrandOrgueRegistration general(const Section& s) {
    GrandOrgueRegistration r;
    r.full = s.yes("IsFull", false);
    const auto manual = [](int m) { return "Manual" + n3(m) + "/"; };
    for (int i = 1; i <= s.num("NumberOfStops", 0); ++i) {
      const int v = s.num("StopNumber" + n3(i), 0);
      add(r, switchOf(manual(s.num("StopManual" + n3(i), 1)) + "Stop" + n3(std::abs(v))), v);
    }
    for (int i = 1; i <= s.num("NumberOfCouplers", 0); ++i) {
      const int v = s.num("CouplerNumber" + n3(i), 0);
      add(r, switchOf(manual(s.num("CouplerManual" + n3(i), 1)) + "Coupler" + n3(std::abs(v))), v);
    }
    for (int i = 1; i <= s.num("NumberOfTremulants", 0); ++i) {
      const int v = s.num("TremulantNumber" + n3(i), 0);
      add(r, switchOf("Tremulant" + n3(std::abs(v))), v);
    }
    for (int i = 1; i <= s.num("NumberOfSwitches", 0); ++i) {
      const int v = s.num("SwitchNumber" + n3(i), 0);
      const int m = s.num("SwitchManual" + n3(i), -1);
      add(r, switchOf(m >= 0 ? manual(m) + "Switch" + n3(std::abs(v)) : "Switch" + n3(std::abs(v))), v);
    }
    report_.divisionalCouplers += s.num("NumberOfDivisionalCouplers", 0);
    return r;
  }

  // A divisional: everything named by its place in its manual
  // (GODivisionalCombination).
  GrandOrgueRegistration divisional(const Section& s, int m) {
    GrandOrgueRegistration r;
    r.full = s.yes("IsFull", false);
    const std::string prefix = "Manual" + n3(m) + "/";
    for (const char* what : {"Stop", "Coupler", "Tremulant", "Switch"}) {
      const std::string kind(what);
      const int count = s.num("NumberOf" + kind + "s", 0);
      for (int i = 1; i <= count; ++i) {
        const int v = s.num(kind + n3(i), 0);
        add(r, switchOf(prefix + kind + n3(std::abs(v))), v);
      }
    }
    return r;
  }

  int manualOfDivisional(const std::string& sectionName) const {
    const auto it = manualOfDivisional_.find(lower(sectionName));
    return it == manualOfDivisional_.end() ? -1 : it->second;
  }

  // The division a GrandOrgue manual plays, as the player's divisionals know
  // divisions: its keyboard's own hint, or the division that lists it.
  Id divisionOfManual(int m) const {
    const auto kb = model_.grandOrgueObjects.find("manual" + n3(m));
    if (kb == model_.grandOrgueObjects.end() || kb->second.kind != OrganModel::GrandOrgueObject::Keyboard) return 0;
    const Id keyboard = kb->second.id;
    if (const auto it = model_.keyboards.find(keyboard); it != model_.keyboards.end() && it->second.primaryDivisionHint)
      return it->second.primaryDivisionHint;
    for (const auto& [id, d] : model_.divisions)
      if (std::find(d.keyboardIds.begin(), d.keyboardIds.end(), keyboard) != d.keyboardIds.end()) return id;
    return 0;
  }

  // One level's voicing, as a change from the definition's own.
  struct Node {
    double gainDb = 0.0, cents = 0.0;
    int tone = 0;
    std::string group;
  };
  Node node(const std::string& sectionName, const std::string& prefix, const std::string& defaultKey) {
    Node n;
    const Section* s = section(sectionName);
    if (s == nullptr) return n;
    OrganModel::GrandOrgueVoicing def;
    if (const auto it = model_.grandOrgueVoicing.find(lower(defaultKey)); it != model_.grandOrgueVoicing.end())
      def = it->second;
    const double amp = s->real(prefix + "Amplitude", def.amplitude);
    const double gain = s->real(prefix + "UserGain", def.gain);
    if (amp != def.amplitude)
      n.gainDb += amp > 0.0 && def.amplitude > 0.0 ? 20.0 * std::log10(amp / def.amplitude) : kMinGainDb;
    n.gainDb += gain - def.gain;
    // ManualTuning is already a change; an older file's Tuning is the pipe's
    // whole tuning, the definition's included.
    if (s->has(prefix + "ManualTuning")) n.cents += s->real(prefix + "ManualTuning", 0.0);
    else if (s->has(prefix + "Tuning")) n.cents += s->real(prefix + "Tuning", def.pitchTuning) - def.pitchTuning;
    n.cents += s->real(prefix + "AutoTuningCorrection", 0.0);
    n.tone = std::clamp(s->num(prefix + "ToneBalance", 0), -100, 100);
    n.group = gocmb::trim(s->str(prefix + "AudioGroup"));
    const std::string where = lower(sectionName + "/" + prefix);
    if (s->num(prefix + "Delay", 0) > 0 && counted_.insert("d:" + where).second) ++report_.delays;
    if (s->num(prefix + "ReleaseTail", 0) > 0 && counted_.insert("t:" + where).second) ++report_.releaseTails;
    return n;
  }

  double clampTo(double v, double lo, double hi) {
    if (v < lo || v > hi) ++report_.voicingClamped;
    return std::clamp(v, lo, hi);
  }

  GrandOrgueVoice voice(double gainDb, double cents, double brightness, double bass) {
    GrandOrgueVoice v;
    v.gainDb = clampTo(gainDb, kMinGainDb, kMaxGainDb);
    v.tuningCents = clampTo(cents, -kMaxCents, kMaxCents);
    v.brightnessDb = clampTo(brightness, -kMaxShelfDb, kMaxShelfDb);
    v.bassDb = clampTo(bass, -kMaxShelfDb, kMaxShelfDb);
    return v;
  }

  const OrganModel& model_;
  GrandOrgueSettingsReport& report_;
  std::unordered_map<std::string, const Section*> byName_;
  std::unordered_map<std::string, int> manualOfDivisional_;
  std::set<std::string> counted_;
};

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

void toneBalanceShelves(int toneBalance, double& brightnessDb, double& bassDb) {
  brightnessDb = bassDb = 0.0;
  if (toneBalance < 0) {
    const double fc = 16000.0 * std::pow(20.0 / 16000.0, std::min(99, -toneBalance) / 99.0);
    brightnessDb = -10.0 * std::log10(1.0 + std::pow(6000.0 / fc, 2.0));
  } else if (toneBalance > 0) {
    const double fc = 20.0 * std::pow(800.0, std::min(99, toneBalance) / 99.0);
    const double r = std::pow(100.0 / fc, 2.0);
    bassDb = 10.0 * std::log10(r / (1.0 + r));
  }
}

std::optional<std::string> temperamentFromGrandOrgue(const std::string& goName) {
  const std::string n = lower(gocmb::trim(goName));
  if (n.empty() || n == "original temperament") return std::string();
  static const std::pair<const char*, const char*> kNames[] = {
      {"equal temperament", "Equal"},
      {"equal", "Equal"},
      {"1/4 comma meantone (aaron 1523)", "Meantone 1/4 comma"},
      {"meantone 1/4 comma", "Meantone 1/4 comma"},
      {"werckmeister iii", "Werckmeister III"},
      {"kirnberger iii", "Kirnberger III"},
      {"vallotti", "Vallotti"},
      {"young ii", "Young II"},
      {"pythagorean", "Pythagorean"},
      {"silbermann 1/6 comma", "Silbermann 1/6 comma"},
  };
  for (const auto& [go, ours] : kNames)
    if (n == go) return std::string(ours);
  return std::nullopt;
}

GrandOrgueSettings readGrandOrgueSettings(const std::string& text, const OrganModel& model,
                                          GrandOrgueSettingsReport& report) {
  report = {};
  GrandOrgueSettings out;
  const auto sections = gocmb::readSections(text);
  report.ok = !sections.empty();
  if (!report.ok) return out;
  Reader r(sections, model, report);

  // ---- registrations ---------------------------------------------------------
  // A combination is in the file when it says how many stops it holds, as
  // GrandOrgue's own reader decides (is_cmb_on_file).
  for (const auto& s : sections) {
    if (!s.has("NumberOfStops")) continue;
    const std::string name = lower(s.name);
    int a = 0, b = 0;
    char tail = 0;
    if (std::sscanf(name.c_str(), "setter%ddivisional%d%c", &a, &b, &tail) == 2) {
      // The divisional setter's memory: manual a, divisional b from 0, ten to
      // a bank.
      const Id division = r.divisionOfManual(a);
      if (division == 0 || b + 1 > 20) {
        ++report.divisionalsLeftOut;
        continue;
      }
      out.divisionals[{division, b + 1}] = r.divisional(s, a);
      ++report.divisionals;
    } else if (std::sscanf(name.c_str(), "settercrescendo%d_%d%c", &a, &b, &tail) == 2) {
      ++report.crescendoStages;
    } else if (std::sscanf(name.c_str(), "settergeneral%d%c", &a, &tail) == 1) {
      if (a < 1 || a > 100) {
        ++report.generalsLeftOut;
        continue;
      }
      out.generals[a] = r.general(s);
      ++report.generals;
    } else if (std::sscanf(name.c_str(), "framegeneral%d%c", &a, &tail) == 1) {
      if (a < 1 || a > 999) {
        ++report.framesLeftOut;
        continue;
      }
      out.frames[a] = r.general(s);
      ++report.frames;
    } else if (std::sscanf(name.c_str(), "general%d%c", &a, &tail) == 1) {
      // The organ's own general, as the player set it.
      const Id piston = r.switchOf(s.name);
      if (piston == 0) continue;
      out.organPistons[piston] = r.general(s);
      ++report.organPistons;
    } else if (std::sscanf(name.c_str(), "divisional%d%c", &a, &tail) == 1) {
      const Id piston = r.switchOf(s.name);
      const int m = r.manualOfDivisional(s.name);
      if (piston == 0 || m < 0) continue;
      out.organPistons[piston] = r.divisional(s, m);
      ++report.organPistons;
    }
  }

  // ---- voicing ---------------------------------------------------------------
  const auto organ = r.node("Organ", "", "Organ");
  std::map<int, Reader::Node> chests;
  for (const auto& [rankSection, ranks] : model.grandOrgueRanks) {
    if (ranks.empty()) continue;
    const int w = ranks.front().windchest;
    if (chests.count(w) == 0) {
      const std::string chest = "WindchestGroup" + n3(w);
      chests[w] = r.node(chest, "", chest);
    }
    const auto& chest = chests[w];
    const auto rank = r.node(rankSection, "", rankSection);
    const int tone = rank.tone != 0 ? rank.tone : chest.tone != 0 ? chest.tone : organ.tone;
    double bright = 0.0, bass = 0.0;
    toneBalanceShelves(tone, bright, bass);
    const auto rankVoice =
        r.voice(organ.gainDb + chest.gainDb + rank.gainDb, organ.cents + chest.cents + rank.cents, bright, bass);
    const std::string group = !rank.group.empty() ? rank.group : !chest.group.empty() ? chest.group : organ.group;
    bool voicedPipe = false;
    const bool covered = r.section(rankSection) != nullptr;
    for (const auto& gr : ranks) {
      if (covered || !rankVoice.neutral()) out.ranks[gr.rankId] = rankVoice;
      if (!group.empty()) out.audioGroups[gr.rankId] = group;
      const auto it = model.ranks.find(gr.rankId);
      if (it == model.ranks.end()) continue;
      for (const auto& pipe : it->second.pipes) {
        const int p = pipe.midiNote - gr.firstMidi + 1;
        if (p < 1) continue;
        const std::string prefix = "Pipe" + n3(p);
        const auto own = r.node(rankSection, prefix, rankSection + "/" + prefix);
        double pb = 0.0, pbass = 0.0;
        if (own.tone != 0 && own.tone != tone) {
          toneBalanceShelves(own.tone, pb, pbass);
          pb -= bright;
          pbass -= bass;
        }
        const auto v = r.voice(own.gainDb, own.cents, pb, pbass);
        if (v.neutral()) continue;
        out.pipes[pipe.pipeId] = v;
        if (&gr == &ranks.front()) {
          ++report.voicedPipes;
          voicedPipe = true;
        }
      }
    }
    if (!rankVoice.neutral() || voicedPipe) ++report.voicedRanks;
  }

  // ---- the organ -------------------------------------------------------------
  if (const Section* o = r.section("Organ")) {
    if (o->has("Volume")) {
      const int v = o->num("Volume", -15);
      out.volumeChangeDb = (v > 20 ? 0 : v) + 15.0;  // GrandOrgue's own default is -15 dB
      report.volume = true;
    }
    out.temperament = gocmb::trim(o->str("Temperament"));
    report.temperament = out.temperament;
  }
  for (const auto& [key, object] : model.grandOrgueObjects) {
    if (key.find('/') != std::string::npos) continue;
    const Section* s = r.section(key);
    if (s == nullptr) continue;
    if (object.kind == OrganModel::GrandOrgueObject::Control && startsWith(key, "enclosure") && s->has("Value")) {
      out.swells[object.id] = std::clamp(s->num("Value", 127), 0, 127);
      ++report.swells;
    } else if (object.kind == OrganModel::GrandOrgueObject::Switch && s->has("DefaultToEngaged") &&
               (startsWith(key, "stop") || startsWith(key, "coupler") || startsWith(key, "tremulant") ||
                startsWith(key, "switch"))) {
      out.drawn[object.id] = s->yes("DefaultToEngaged", false);
      ++report.drawn;
    }
  }
  if (const Section* m = r.section("Metronome")) {
    if (m->has("BPM")) out.metronomeBpm = std::clamp(m->num("BPM", 80), 1, 500);
    if (m->has("MeasureLength")) out.metronomeBeats = std::clamp(m->num("MeasureLength", 4), 0, 32);
    report.metronome = out.metronomeBpm.has_value() || out.metronomeBeats.has_value();
  }
  return out;
}

}  // namespace mp
