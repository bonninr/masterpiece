#include "JOrganHauptwerk.h"

#include "mp_core/GrandOrgueStockImages.h"
#include "mp_core/OrganModel.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace mp {
namespace {

// Rows of a Hauptwerk definition, one object list per table.
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
  static void set(pugi::xml_node r, const char* field, const std::string& v) {
    r.append_child(field).text() = v.c_str();
  }
  static void set(pugi::xml_node r, const char* field, const char* v) { r.append_child(field).text() = v; }
  static void set(pugi::xml_node r, const char* field, int v) { set(r, field, std::to_string(v)); }
  static void set(pugi::xml_node r, const char* field, long long v) { set(r, field, std::to_string(v)); }
  static void set(pugi::xml_node r, const char* field, double v) {
    std::ostringstream os;
    os << v;
    set(r, field, os.str());
  }
  static void yn(pugi::xml_node r, const char* field, bool v) { set(r, field, v ? "Y" : "N"); }
  std::string text() const {
    std::ostringstream xml;
    doc.save(xml, "  ", pugi::format_indent, pugi::encoding_utf8);
    return xml.str();
  }
};

void general(Emitter& out, const std::string& name, long long id) {
  auto g = out.row("_General");
  Emitter::set(g, "Identification_Name", name);
  Emitter::set(g, "Identification_UniqueOrganID", id);
  Emitter::set(g, "OrganInfo_Comments", "Converted from a jOrgan disposition");
  Emitter::set(g, "AudioEngine_BasePitchHz", 440);
}

// Ids, per table, in ranges that read back as what they are in a log.
constexpr int kStopSwitchBase = 3000;   // + drawstop
constexpr int kKeySwitchBase = 20000;   // + keys in order
constexpr int kKnobSwitchBase = 40000;  // + a skin's pictures of drawstops
constexpr int kStopBase = 100000;       // + stop
constexpr int kCouplerStopBase = 110000;// + coupler: listed with the stops
constexpr int kRankBase = 1000;         // + rank
constexpr int kAltRankBase = 61000;     // + rank: its tremulant takes
constexpr int kTremulantBase = 100;
constexpr int kEnclosureBase = 200;
constexpr int kEnclosureControlBase = 700;

// The stock pictures: the program's own drawstops, keys, shoes and wood,
// in the standard component package.
constexpr int kStockPackage = 1;
std::pair<int, int> stockSize(const std::string& name) {
  for (const auto& img : kGrandOrgueStockImages)
    if (name == img.name) return {img.width, img.height};
  return {0, 0};
}
std::string stockFile(const std::string& name) {
  return std::string("GrandOrgueStandIns/" MP_GRANDORGUE_STOCK_PREFIX) + name + ".png";
}

}  // namespace

std::string jorganHauptwerkHeader(const std::string& name, long long id) {
  Emitter out;
  general(out, name, id);
  return out.text();
}

std::string jorganHauptwerkXml(const JOrganOrgan& o) {
  Emitter out;
  general(out, o.name, o.id);

  // ---- switches: one per drawstop --------------------------------------
  std::map<int, pugi::xml_node> switchRows;
  auto emitSwitch = [&](int id, const std::string& name, bool engaged) {
    auto sw = out.row("Switch");
    switchRows[id] = sw;
    Emitter::set(sw, "SwitchID", id);
    Emitter::set(sw, "Name", name);
    Emitter::yn(sw, "DefaultToEngaged", engaged);
    return sw;
  };
  for (size_t d = 0; d < o.drawstops.size(); ++d)
    emitSwitch(kStopSwitchBase + static_cast<int>(d), o.drawstops[d].name, o.drawstops[d].engaged);

  // ---- keyboards and divisions ---------------------------------------
  // A keyboard and a division per manual, joined by a key action that a
  // unison off, when the manual has one, opens.
  auto kb = [](size_t m) { return static_cast<int>(m) + 1; };
  for (size_t m = 0; m < o.manuals.size(); ++m) {
    const auto& man = o.manuals[m];
    auto div = out.row("Division");
    Emitter::set(div, "DivisionID", kb(m));
    Emitter::set(div, "Name", man.name);
    auto k = out.row("Keyboard");
    Emitter::set(k, "KeyboardID", kb(m));
    Emitter::set(k, "Name", man.name);
    // The usual channel convention: the pedal on 1, the manuals from 2.
    Emitter::set(k, "DefaultInputOutputKeyboardAsgnCode", static_cast<int>(m) + (o.manuals.front().pedal ? 1 : 2));
    Emitter::set(k, "Hint_PrimaryAssociatedDivisionID", kb(m));
    Emitter::set(k, "KeyGen_NumberOfKeys", man.keys);
    Emitter::set(k, "KeyGen_MIDINoteNumberOfFirstKey", man.firstMidi);
    auto own = out.row("KeyAction");
    Emitter::set(own, "SourceKeyboardID", kb(m));
    Emitter::yn(own, "DestIsKeyboardNotDivision", false);
    Emitter::set(own, "DestDivisionID", kb(m));
    Emitter::set(own, "ActionTypeCode", 1);
    Emitter::set(own, "ActionEffectCode", 1);
    Emitter::set(own, "MIDINoteNumOfFirstSourceKey", man.firstMidi);
    Emitter::set(own, "NumberOfKeys", man.keys);
    Emitter::set(own, "MIDINoteNumberIncrement", 0);
    for (const auto& c : o.couplers)
      if (c.unisonOff && c.manual == static_cast<int>(m)) {
        Emitter::set(own, "ConditionSwitchID", kStopSwitchBase + c.drawstop);
        Emitter::yn(own, "ConditionSwitchLinkIfEngaged", false);
        break;
      }
  }

  // ---- couplers: key actions on their drawstop -------------------------
  for (size_t i = 0; i < o.couplers.size(); ++i) {
    const auto& c = o.couplers[i];
    const int sw = kStopSwitchBase + c.drawstop;
    auto st = out.row("Stop");
    Emitter::set(st, "StopID", kCouplerStopBase + static_cast<int>(i));
    Emitter::set(st, "Name", c.name);
    Emitter::set(st, "DivisionID", kb(c.manual));
    Emitter::set(st, "ControllingSwitchID", sw);
    if (c.unisonOff) continue;
    const auto& man = o.manuals[c.manual];
    auto ka = out.row("KeyAction");
    Emitter::set(ka, "SourceKeyboardID", kb(c.manual));
    Emitter::yn(ka, "DestIsKeyboardNotDivision", false);
    Emitter::set(ka, "DestDivisionID", kb(c.dest));
    Emitter::set(ka, "ActionTypeCode", 1);
    Emitter::set(ka, "ActionEffectCode", 1);
    Emitter::set(ka, "MIDINoteNumOfFirstSourceKey", man.firstMidi);
    Emitter::set(ka, "NumberOfKeys", man.keys);
    Emitter::set(ka, "MIDINoteNumberIncrement", c.shift);
    Emitter::set(ka, "ConditionSwitchID", sw);
    Emitter::yn(ka, "ConditionSwitchLinkIfEngaged", true);
  }

  // ---- tremulants, enclosures ------------------------------------------
  for (size_t t = 0; t < o.tremulants.size(); ++t) {
    const auto& tr = o.tremulants[t];
    auto row = out.row("Tremulant");
    Emitter::set(row, "TremulantID", kTremulantBase + static_cast<int>(t));
    Emitter::set(row, "Name", tr.name);
    Emitter::set(row, "ControllingSwitchID", kStopSwitchBase + tr.drawstop);
    Emitter::set(row, "FrequencyWhenEngagedHz", 1000.0 / std::max(20.0, tr.periodMs));
    Emitter::set(row, "StartRatePercent", 10);
    Emitter::set(row, "StopRatePercent", 10);
    auto wf = out.row("TremulantWaveform");
    Emitter::set(wf, "TremulantWaveformID", kTremulantBase + static_cast<int>(t));
    Emitter::set(wf, "TremulantID", kTremulantBase + static_cast<int>(t));
  }
  std::map<int, pugi::xml_node> enclosureControls;
  for (size_t e = 0; e < o.enclosures.size(); ++e) {
    auto cc = out.row("ContinuousControl");
    enclosureControls[static_cast<int>(e)] = cc;
    Emitter::set(cc, "ControlID", kEnclosureControlBase + static_cast<int>(e));
    Emitter::set(cc, "Name", o.enclosures[e].name);
    Emitter::set(cc, "DefaultValue", 127);
    auto en = out.row("Enclosure");
    Emitter::set(en, "EnclosureID", kEnclosureBase + static_cast<int>(e));
    Emitter::set(en, "Name", o.enclosures[e].name);
    Emitter::set(en, "ShutterPositionContinuousControlID", kEnclosureControlBase + static_cast<int>(e));
  }

  // ---- ranks and pipes ---------------------------------------------------
  // Each sample is declared at its pipe's nominal pitch, and the pipe's
  // tuning carries the retuning from the region's root: the engine plays the
  // file at exactly the ratio the SoundFont would.
  std::map<std::string, int> sampleIds;
  auto sampleFor = [&](const std::string& file, double hz) {
    char tag[32];
    std::snprintf(tag, sizeof tag, "@%.4f", hz);
    const std::string key = file + tag;
    const auto it = sampleIds.find(key);
    if (it != sampleIds.end()) return it->second;
    const int id = static_cast<int>(sampleIds.size()) + 1;
    sampleIds[key] = id;
    auto s = out.row("Sample");
    Emitter::set(s, "SampleID", id);
    Emitter::set(s, "InstallationPackageID", 0);
    Emitter::set(s, "SampleFilename", file);
    Emitter::set(s, "Pitch_SpecificationMethodCode", 4);
    Emitter::set(s, "Pitch_ExactSamplePitch", hz);
    return id;
  };
  int nextPipe = 1, nextUnique = 1;
  std::vector<std::vector<int>> pipesOfRank(o.ranks.size());
  auto buildRank = [&](size_t r, bool tremulantTake) {
    const auto& rank = o.ranks[r];
    const int rankId = (tremulantTake ? kAltRankBase : kRankBase) + static_cast<int>(r);
    auto row = out.row("Rank");
    Emitter::set(row, "RankID", rankId);
    Emitter::set(row, "Name", rank.name + (tremulantTake ? " (tremulant)" : ""));
    auto g = out.row("MasterpieceRankWindchest");
    Emitter::set(g, "RankID", rankId);
    Emitter::set(g, "Name", "Windchest " + std::to_string(rank.chest + 1));
    for (const auto& p : rank.pipes) {
      const std::string& file = tremulantTake ? p.tremulantFile : p.file;
      if (file.empty()) continue;
      const int pipeId = nextPipe++;
      if (!tremulantTake) pipesOfRank[r].push_back(pipeId);
      auto pipe = out.row("Pipe_SoundEngine01");
      Emitter::set(pipe, "PipeID", pipeId);
      Emitter::set(pipe, "RankID", rankId);
      Emitter::set(pipe, "NormalMIDINoteNumber", p.midi);
      Emitter::set(pipe, "Pitch_Tempered_RankBasePitch64ftHarmonicNum", 8);
      if (p.cents != 0.0) Emitter::set(pipe, "Pitch_Tempered_BaseTuningDeviation", p.cents);
      auto layer = out.row("Pipe_SoundEngine01_Layer");
      Emitter::set(layer, "LayerID", pipeId);
      Emitter::set(layer, "PipeID", pipeId);
      if (rank.db + p.db != 0.0) Emitter::set(layer, "AmpLvl_LevelAdjustDecibels", rank.db + p.db);
      if (p.percussive) Emitter::yn(layer, "MasterpiecePercussive", true);
      const double hz = 440.0 * std::pow(2.0, (p.midi - 69.0) / 12.0);
      const int sampleId = sampleFor(file, hz);
      auto at = out.row("Pipe_SoundEngine01_AttackSample");
      const int attack = nextUnique++;
      Emitter::set(at, "UniqueID", attack);
      Emitter::set(at, "LayerID", pipeId);
      Emitter::set(at, "SampleID", sampleId);
      Emitter::set(at, "AttackSelCriteria_HighestVelocity", 127);
      // The release: the file's own tail, from its release marker.
      if (!p.percussive) {
        auto rel = out.row("Pipe_SoundEngine01_ReleaseSample");
        Emitter::set(rel, "UniqueID", nextUnique++);
        Emitter::set(rel, "LayerID", pipeId);
        Emitter::set(rel, "SampleID", sampleId);
        Emitter::set(rel, "LoadSampleRange_StartPositionTypeCode", 1);
        Emitter::set(rel, "AttackSelCriteria_HighestVelocity", 127);
        Emitter::set(rel, "ReleaseSelCriteria_PreferThisRelForAttackID", attack);
        Emitter::yn(rel, "PhaseAlignAutomatically", true);
      }
      // The windchest's enclosures and level tremulants.
      const auto& chest = o.chests[rank.chest];
      for (int e : chest.enclosures) {
        auto ep = out.row("EnclosurePipe");
        Emitter::set(ep, "EnclosureID", kEnclosureBase + e);
        Emitter::set(ep, "PipeID", pipeId);
        Emitter::set(ep, "FiltParamWhenClsd_OverallAttnDb",
                     -20.0 * std::log10(std::clamp(o.enclosures[e].closedPct, 0.1, 100.0) / 100.0));
        Emitter::set(ep, "FiltParamWhenClsd_MaxFreqHz", 20000);
        Emitter::set(ep, "FiltParamWhenOpen_MaxFreqHz", 20000);
      }
      for (int t : chest.tremulants) {
        const auto& tr = o.tremulants[t];
        if (tr.wave) continue;
        auto tp = out.row("TremulantWaveformPipe");
        Emitter::set(tp, "PipeID", pipeId);
        Emitter::set(tp, "TremulantWaveformID", kTremulantBase + t);
        // Swings in the units of the waveform's own: 15% of level, 8.4 cents.
        Emitter::set(tp, "AmplitudeModDepthAdjustDecibels",
                     20.0 * std::log10(std::max(0.1, tr.ampPct) / 100.0 / kTremulantWaveformAmpDepth));
        Emitter::set(tp, "PitchModDepthAdjustPercent", tr.pitchCents / kTremulantWaveformPitchCents * 100.0);
      }
    }
  };
  std::vector<int> hasTremulantTakes(o.ranks.size(), 0);
  for (size_t r = 0; r < o.ranks.size(); ++r) {
    buildRank(r, false);
    for (const auto& p : o.ranks[r].pipes)
      if (!p.tremulantFile.empty()) hasTremulantTakes[r] = 1;
    if (hasTremulantTakes[r]) buildRank(r, true);
  }

  // ---- stops -------------------------------------------------------------
  for (size_t s = 0; s < o.stops.size(); ++s) {
    const auto& stop = o.stops[s];
    const int stopId = kStopBase + static_cast<int>(s);
    auto st = out.row("Stop");
    Emitter::set(st, "StopID", stopId);
    Emitter::set(st, "Name", stop.name);
    Emitter::set(st, "DivisionID", kb(stop.manual));
    Emitter::set(st, "ControllingSwitchID", kStopSwitchBase + stop.drawstop);
    for (const auto& sr : stop.ranks) {
      if (sr.count <= 0) continue;
      auto row = out.row("StopRank");
      Emitter::set(row, "StopID", stopId);
      Emitter::set(row, "RankID", kRankBase + sr.rank);
      Emitter::set(row, "MIDINoteNumOfFirstMappedDivisionInputNode", sr.firstKey);
      Emitter::set(row, "NumberOfMappedDivisionInputNodes", sr.count);
      Emitter::set(row, "MIDINoteNumIncrementFromDivisionToRank", sr.increment);
      // The tremulant takes, swapped in by the wave tremulant on the rank's
      // windchest, re-sounding held notes.
      if (!hasTremulantTakes[sr.rank]) continue;
      for (int t : o.chests[o.ranks[sr.rank].chest].tremulants)
        if (o.tremulants[t].wave) {
          Emitter::set(row, "AlternateRankID", kAltRankBase + sr.rank);
          Emitter::set(row, "SwitchIDToSwitchToAlternateRank", kStopSwitchBase + o.tremulants[t].drawstop);
          Emitter::yn(row, "RetriggerNotesWhenSwitchingBetweenNormalAndAlternateRanks", true);
          break;
        }
    }
  }

  // ---- the console -------------------------------------------------------
  // The skin's consoles, one page each, then the stock page: wood, the
  // drawstops in two jambs, the manuals between them with the swell shoes
  // above the pedal.
  int page = 1;
  int nextSet = 1, nextInstance = 1, nextText = 1;
  std::map<std::string, int> setMemo;
  auto imageSet = [&](const std::vector<std::string>& stock, bool wood) {
    std::string key;
    for (const auto& f : stock) key += f + "|";
    const auto hit = setMemo.find(key);
    if (hit != setMemo.end()) return hit->second;
    const int id = nextSet++;
    auto set = out.row("ImageSet");
    Emitter::set(set, "ImageSetID", id);
    Emitter::set(set, "InstallationPackageID", kStockPackage);
    if (wood) {
      Emitter::set(set, "Name", stock.front());
    } else {
      Emitter::set(set, "Name", stockFile(stock.front()));
      const auto wh = stockSize(stock.front());
      Emitter::set(set, "ImageWidthPixels", wh.first);
      Emitter::set(set, "ImageHeightPixels", wh.second);
    }
    for (size_t i = 0; i < stock.size(); ++i) {
      auto el = out.row("ImageSetElement");
      Emitter::set(el, "ImageSetID", id);
      Emitter::set(el, "ImageIndexWithinSet", static_cast<int>(i) + 1);
      Emitter::set(el, "BitmapFilename", wood ? stock[i] : stockFile(stock[i]));
    }
    return setMemo[key] = id;
  };
  auto instance = [&](int set, int x, int y, int layer, int right = 0, int bottom = 0) {
    const int id = nextInstance++;
    auto in = out.row("ImageSetInstance");
    Emitter::set(in, "ImageSetInstanceID", id);
    Emitter::set(in, "DisplayPageID", page);
    Emitter::set(in, "ImageSetID", set);
    Emitter::set(in, "DefaultImageIndexWithinSet", 1);
    Emitter::set(in, "ScreenLayerNumber", layer);
    Emitter::set(in, "LeftXPosPixels", x);
    Emitter::set(in, "TopYPosPixels", y);
    if (right > 0) {
      Emitter::set(in, "RightXPosPixelsIfTiling", right);
      Emitter::set(in, "BottomYPosPixelsIfTiling", bottom);
    }
    return id;
  };
  {
    auto st = out.row("TextStyle");
    Emitter::set(st, "StyleID", 1);
    Emitter::set(st, "Name", "Knob");
    Emitter::set(st, "Face_WindowsName", "Arial");
    Emitter::set(st, "Font_SizePixels", 9);
    Emitter::set(st, "Colour_Red", 0x80);
    Emitter::set(st, "Colour_Green", 0);
    Emitter::set(st, "Colour_Blue", 0);
    Emitter::set(st, "HorizontalAlignmentCode", 0);
    Emitter::set(st, "VerticalAlignmentCode", 0);
  }
  auto text = [&](int inst, const std::string& words, int x, int y, int w, int h) {
    if (words.empty()) return;
    auto t = out.row("TextInstance");
    Emitter::set(t, "TextInstanceID", nextText++);
    Emitter::set(t, "DisplayPageID", page);
    Emitter::set(t, "Text", words);
    Emitter::set(t, "TextStyleID", 1);
    Emitter::set(t, "XPosPixels", x);
    Emitter::set(t, "YPosPixels", y);
    Emitter::set(t, "BoundingBoxWidthPixelsIfWordWrap", w);
    Emitter::set(t, "BoundingBoxHeightPixelsIfWordWrap", h);
    Emitter::yn(t, "AttachedToAnImageSetInstance", true);
    Emitter::set(t, "AttachedToImageSetInstanceID", inst);
    Emitter::yn(t, "PosRelativeToTopLeftOfImageSetInstance", true);
  };

  // ---- the skin's consoles ---------------------------------------------
  // Each element's pictures, one per state, in the work folder. The first
  // picture of a swell moves it; every picture of a drawstop is a knob of
  // its own, wired both ways to the drawstop's switch.
  auto pictureSet = [&](const std::vector<JOrganOrgan::Picture>& frames) {
    const int id = nextSet++;
    auto set = out.row("ImageSet");
    Emitter::set(set, "ImageSetID", id);
    Emitter::set(set, "InstallationPackageID", 0);
    Emitter::set(set, "Name", frames.front().file);
    Emitter::set(set, "ImageWidthPixels", frames.front().width);
    Emitter::set(set, "ImageHeightPixels", frames.front().height);
    for (size_t i = 0; i < frames.size(); ++i) {
      auto el = out.row("ImageSetElement");
      Emitter::set(el, "ImageSetID", id);
      Emitter::set(el, "ImageIndexWithinSet", static_cast<int>(i) + 1);
      Emitter::set(el, "BitmapFilename", frames[i].file);
    }
    return id;
  };
  auto link = [&](int source, int dest) {
    auto l = out.row("SwitchLinkage");
    Emitter::set(l, "SourceSwitchID", source);
    Emitter::set(l, "DestSwitchID", dest);
    Emitter::yn(l, "SourceSwitchLinkIfEngaged", true);
    Emitter::set(l, "EngageLinkActionCode", 1);
    Emitter::set(l, "DisengageLinkActionCode", 2);
  };
  std::set<int> skinnedShoes;
  int nextKnob = kKnobSwitchBase;
  for (const auto& console : o.consoles) {
    // The background over the elements' extent, or over the whole picture
    // when it is a little larger: drawn for the screen the console fills.
    int w = console.width, h = console.height;
    const auto& bg = console.background;
    if (!bg.file.empty() && bg.width <= w * 3 / 2 && bg.height <= h * 3 / 2) {
      w = std::max(w, bg.width);
      h = std::max(h, bg.height);
    }
    if (!bg.file.empty()) instance(pictureSet({bg}), 0, 0, 0, w, h);
    for (const auto& v : console.views) {
      const int set = pictureSet(v.frames);
      const int inst = instance(set, v.x, v.y, 2);
      out.lists["ImageSetInstance"].last_child().child("DefaultImageIndexWithinSet").text() = v.initial + 1;
      if (v.drawstop >= 0) {
        const int knob = nextKnob++;
        auto sw = emitSwitch(knob, o.drawstops[v.drawstop].name + " (" + console.name + ")",
                             o.drawstops[v.drawstop].engaged);
        Emitter::set(sw, "Disp_ImageSetInstanceID", inst);
        Emitter::set(sw, "Disp_ImageSetIndexEngaged", 2);
        Emitter::set(sw, "Disp_ImageSetIndexDisengaged", 1);
        Emitter::yn(sw, "Clickable", true);
        link(knob, kStopSwitchBase + v.drawstop);
        link(kStopSwitchBase + v.drawstop, knob);
      } else if (v.enclosure >= 0 && skinnedShoes.insert(v.enclosure).second) {
        const int n = static_cast<int>(v.frames.size());
        for (int f = 0; f < n; ++f) {
          // The frame jOrgan shows at a position: the nearest of the slider's.
          auto st = out.row("ContinuousControlImageSetStage");
          Emitter::set(st, "ImageSetID", set);
          const int highest =
              n == 2 ? (f == 0 ? 0 : 127)
                     : (f == n - 1 ? 127 : static_cast<int>(std::ceil(127.0 * (f + 0.5) / (n - 1))) - 1);
          Emitter::set(st, "HighestContinuousControlValue", highest);
          Emitter::set(st, "ImageSetIndex", f + 1);
        }
        auto cc = enclosureControls[v.enclosure];
        Emitter::set(cc, "ImageSetInstanceID", inst);
        Emitter::yn(cc, "Clickable", true);
        Emitter::yn(cc, "ClickingHigherIncreasesValue", true);
      }
    }
    auto pg = out.row("DisplayPage");
    Emitter::set(pg, "PageID", page);
    Emitter::set(pg, "Name", console.name.empty() ? o.name : console.name);
    ++page;
  }

  // The geometry.
  constexpr int kCellW = 78, kCellH = 69, kMargin = 30;
  const auto drawstopWH = stockSize("drawstop01_off");
  const auto shoeWH = stockSize("enclosureA00");
  const int controls = static_cast<int>(o.drawstops.size());
  const int cols = std::clamp(2 * static_cast<int>(std::ceil(std::sqrt(std::max(1, controls) / 2.0))), 2, 12);
  const int rows = std::max(1, (controls + cols - 1) / cols);
  const int jambW = cols / 2 * kCellW;
  auto isSharp = [](int midi) {
    const int n = ((midi % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
  };
  struct Key { int midi; std::string off, on; int x; bool sharp; };
  std::vector<std::vector<Key>> keysOf(o.manuals.size());
  std::vector<int> widthOf(o.manuals.size(), 0);
  for (size_t m = 0; m < o.manuals.size(); ++m) {
    const auto& man = o.manuals[m];
    const std::string type = man.pedal ? "Pedal01" : "Manual01";
    int x = 0;
    for (int k = 0; k < man.keys; ++k) {
      const int midi = man.firstMidi + k;
      const bool sharp = isSharp(midi);
      const bool prevSharp = k > 0 && isSharp(midi - 1);
      const bool nextSharp = k + 1 < man.keys && isSharp(midi + 1);
      std::string shape;
      if (man.pedal) shape = sharp ? "Sharp" : "Natural";
      else if (sharp) shape = "Sharp";
      else if (!prevSharp && nextSharp) shape = "C";
      else if (prevSharp && nextSharp) shape = "D";
      else if (prevSharp && !nextSharp) shape = "E";
      else shape = "Natural";
      const std::string off = type + "Off_" + shape, on = type + "On_" + shape;
      const int w = stockSize(off).first;
      int advance = w, offset = 0;
      if (sharp && !man.pedal) {
        advance = 0;
        offset = -w / 2;
      } else if (man.pedal && !sharp && !nextSharp) {
        advance = 2 * w;
      }
      keysOf[m].push_back({midi, off, on, x + offset, sharp});
      x += advance;
    }
    widthOf[m] = x;
  }
  int centerW = 2 * shoeWH.first * static_cast<int>(o.enclosures.size()) + 80;
  for (int w : widthOf) centerW = std::max(centerW, w + 80);
  centerW = std::max(centerW, 520);
  const int manualRowH = 32 + 24, pedalRowH = 40 + 24, shoeRowH = o.enclosures.empty() ? 0 : shoeWH.second + 24;
  int centerH = 80 + shoeRowH;
  for (const auto& man : o.manuals) centerH += man.pedal ? pedalRowH : manualRowH;
  const int screenW = 2 * (kMargin + jambW) + centerW;
  const int screenH = std::max({400, rows * kCellH + 2 * kMargin + 40, centerH});

  instance(imageSet({"SeamlessWoodBkgnds - HGrain001.bmp"}, true), 0, 0, 0, screenW, screenH);

  // The drawstops, row by row across both jambs.
  const int jambY = (screenH - rows * kCellH) / 2;
  const int drawstopSet = imageSet({"drawstop01_off", "drawstop01_on"}, false);
  for (int d = 0; d < controls; ++d) {
    const int row = d / cols, col = d % cols;
    const int half = cols / 2;
    const int x = col < half ? kMargin + col * kCellW + 6 : screenW - kMargin - jambW + (col - half) * kCellW + 6;
    const int y = jambY + row * kCellH + 2;
    const int inst = instance(drawstopSet, x, y, 2);
    auto sw = switchRows[kStopSwitchBase + d];
    Emitter::set(sw, "Disp_ImageSetInstanceID", inst);
    Emitter::set(sw, "Disp_ImageSetIndexEngaged", 2);
    Emitter::set(sw, "Disp_ImageSetIndexDisengaged", 1);
    Emitter::yn(sw, "Clickable", true);
    // The label inside the knob, wrapped a little narrower, as GrandOrgue does.
    const int w = drawstopWH.first, h = drawstopWH.second;
    text(inst, o.drawstops[d].label, 1 + 7, 1, w - 1 - 14, h - 1);
  }

  // The manuals, highest at the top; the shoes; the pedal at the bottom.
  const int centerX = kMargin + jambW;
  int y = (screenH - centerH) / 2 + 40;
  int nextKey = 0;
  auto drawKeys = [&](size_t m, int top) {
    const int left = centerX + (centerW - widthOf[m]) / 2;
    for (const auto& key : keysOf[m]) {
      const int sw = kKeySwitchBase + nextKey++;
      auto srow = emitSwitch(sw, o.manuals[m].name + " key " + std::to_string(key.midi), false);
      auto kk = out.row("KeyboardKey");
      Emitter::set(kk, "KeyboardID", kb(m));
      Emitter::set(kk, "SwitchID", sw);
      Emitter::set(kk, "NormalMIDINoteNumber", key.midi);
      // Sharps over the naturals they overlap.
      const int inst = instance(imageSet({key.off, key.on}, false), left + key.x, top, key.sharp ? 4 : 3);
      Emitter::set(srow, "Disp_ImageSetInstanceID", inst);
      Emitter::set(srow, "Disp_ImageSetIndexEngaged", 2);
      Emitter::set(srow, "Disp_ImageSetIndexDisengaged", 1);
    }
  };
  for (size_t m = o.manuals.size(); m-- > 0;) {
    if (o.manuals[m].pedal) continue;
    drawKeys(m, y);
    y += manualRowH;
  }
  if (!o.enclosures.empty()) {
    std::vector<std::string> frames;
    for (int f = 0; f < 16; ++f) {
      char name[32];
      std::snprintf(name, sizeof name, "enclosureA%02d", f);
      frames.push_back(name);
    }
    const int set = imageSet(frames, false);
    for (int f = 1; f <= 16; ++f) {
      auto st = out.row("ContinuousControlImageSetStage");
      Emitter::set(st, "ImageSetID", set);
      Emitter::set(st, "HighestContinuousControlValue", (f * 128) / 16 - 1);
      Emitter::set(st, "ImageSetIndex", f);
    }
    const int n = static_cast<int>(o.enclosures.size());
    const int left = centerX + (centerW - n * 2 * shoeWH.first) / 2 + shoeWH.first / 2;
    for (int e = 0; e < n; ++e) {
      if (skinnedShoes.count(e)) continue;
      const int inst = instance(set, left + e * 2 * shoeWH.first, y, 2);
      auto cc = enclosureControls[e];
      Emitter::set(cc, "ImageSetInstanceID", inst);
      Emitter::yn(cc, "Clickable", true);
      Emitter::yn(cc, "ClickingHigherIncreasesValue", true);
      text(inst, o.enclosures[e].name, 0, 0, shoeWH.first, shoeWH.second);
    }
    y += shoeRowH;
  }
  for (size_t m = 0; m < o.manuals.size(); ++m)
    if (o.manuals[m].pedal) drawKeys(m, y);

  auto pg = out.row("DisplayPage");
  Emitter::set(pg, "PageID", page);
  Emitter::set(pg, "Name", o.consoles.empty() ? o.name : std::string("Stock console"));
  return out.text();
}

}  // namespace mp
