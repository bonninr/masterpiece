#include "Panels.h"

#include "../mp_audio/DataFolder.h"
#include "Console.h"
#include "MidiEventDialog.h"
#include "Mobile.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <sstream>

namespace mp::ui {

// ---- the organ as sections -------------------------------------------------

std::string PanelElement::key() const {
  const char* k = kind == Kind::Stop ? "s" : kind == Kind::Switch ? "w"
                : kind == Kind::Piston ? "p" : "c";
  return k + std::to_string(static_cast<long long>(id));
}

void splitFootage(const std::string& raw, std::string& name, std::string& footage) {
  // A curly apostrophe or a prime for the foot mark reads as a plain one, and
  // a jamb number in front ("51. Tremulant") is the console's, not the name's.
  static const std::regex number(R"(^\s*\d+[a-z]?\.\s+)", std::regex::icase);
  std::string label = std::regex_replace(raw, number, "");
  for (const char* mark : {"\xE2\x80\x99", "\xE2\x80\xB2"})
    for (size_t at; (at = label.find(mark)) != std::string::npos;)
      label.replace(at, 3, "'");
  // A footage at the end: 8', 2 2/3', 1 1/3, 16; a mixture's ranks: IV, III-IV,
  // 4f, 3-5f.
  static const std::regex tail(
      R"(^(.*?)[\s,]+((?:\d+\s+)?\d+(?:/\d+)?\s*(?:'|ft\.?)?|[IVX]+(?:\s*-\s*[IVX]+)?|\d+(?:\s*-\s*\d+)?\s*(?:f|fach|rangs?|rks?)\.?)\s*$)",
      std::regex::icase);
  std::smatch m;
  if (std::regex_match(label, m, tail) && !m[1].str().empty()) {
    name = m[1].str();
    footage = m[2].str();
  } else {
    name = label;
    footage.clear();
  }
}

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The division a keyboard plays: its own hint, the division that lists it, or
// the division its unconditional key action reaches.
Id keyboardDivision(const OrganModel& m, Id keyboard) {
  if (const auto kb = m.keyboards.find(keyboard);
      kb != m.keyboards.end() && kb->second.primaryDivisionHint != 0)
    return kb->second.primaryDivisionHint;
  for (const auto& [id, d] : m.divisions)
    if (std::find(d.keyboardIds.begin(), d.keyboardIds.end(), keyboard) != d.keyboardIds.end())
      return id;
  for (const auto& a : m.keyActions)
    if (a.conditionSwitchId == 0 && static_cast<Id>(a.sourceKeyboard) == keyboard &&
        !a.destIsKeyboard && a.destDivision != 0)
      return static_cast<Id>(a.destDivision);
  return 0;
}

// The division of manual `n` (0 the pedal): the keyboard with that default
// assignment, or a division numbered so.
Id manualDivision(const OrganModel& m, int n) {
  for (const auto& [id, kb] : m.keyboards)
    if (kb.assignmentCode == n + 1)
      if (const Id d = keyboardDivision(m, id); d != 0) return d;
  for (const auto& [id, d] : m.divisions)
    if (d.manualNumber == n && n > 0) return id;
  return 0;
}

// A coupler named for what it joins: "I/P", "II/I", "III / II". The part
// after the slash is the keyboard it couples onto. -1 when the name is not
// one.
int couplerTarget(const std::string& name) {
  static const std::regex coupler(R"(^\s*(I{1,3}|IV|V|VI)\s*/\s*(I{1,3}|IV|V|VI|P|Ped\.?|Pedal)\s*$)",
                                  std::regex::icase);
  std::smatch c;
  if (!std::regex_match(name, c, coupler)) return -1;
  const std::string to = c[2].str();
  if (std::tolower(static_cast<unsigned char>(to[0])) == 'p') return 0;
  std::string r = to;
  for (auto& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  static const std::map<std::string, int> roman{{"i", 1}, {"ii", 2}, {"iii", 3}, {"iv", 4}, {"v", 5}, {"vi", 6}};
  const auto it = roman.find(r);
  return it == roman.end() ? -1 : it->second;
}

// The division a coupler feeds: the key action it enables, followed through
// up to a few switch linkages, because the drawn knob is often upstream of the
// switch the key action names.
Id couplerDivision(const OrganModel& m, Id switchId) {
  std::vector<Id> frontier{switchId};
  std::set<Id> seen{switchId};
  for (int hop = 0; hop < 4 && !frontier.empty(); ++hop) {
    for (Id sw : frontier)
      for (const auto& a : m.keyActions) {
        if (a.conditionSwitchId != sw) continue;
        // On the jamb of the keyboard it couples onto: the key action's source.
        if (const Id d = keyboardDivision(m, static_cast<Id>(a.sourceKeyboard)); d != 0) return d;
      }
    std::vector<Id> next;
    for (Id sw : frontier)
      for (const auto& l : m.switchLinkages)
        if (l.sourceSwitchId == sw && seen.insert(l.destSwitchId).second)
          next.push_back(l.destSwitchId);
    frontier.swap(next);
  }
  return 0;
}

// Whether a switch, or a switch its linkages drive (a few hops on), is one of
// `targets`. The knob a player draws is often upstream of the switch that
// does the work.
bool reaches(const OrganModel& m, Id switchId, const std::set<Id>& targets) {
  std::vector<Id> frontier{switchId};
  std::set<Id> seen{switchId};
  for (int hop = 0; hop < 5 && !frontier.empty(); ++hop) {
    std::vector<Id> next;
    for (Id sw : frontier) {
      if (targets.count(sw) != 0) return true;
      for (const auto& l : m.switchLinkages)
        if (l.sourceSwitchId == sw && seen.insert(l.destSwitchId).second) next.push_back(l.destSwitchId);
    }
    frontier.swap(next);
  }
  return false;
}

int romanValue(const std::string& s) {
  static const std::map<std::string, int> r{{"i", 1}, {"ii", 2}, {"iii", 3}, {"iv", 4}, {"v", 5}, {"vi", 6}};
  const auto it = r.find(lower(s));
  return it == r.end() ? 0 : it->second;
}

// A tremulant's division, from its name: the division's own name in it, or a
// manual number in roman numerals ("Tremolo II").
Id tremulantDivision(const OrganModel& m, const std::string& name) {
  const std::string n = lower(name);
  for (const auto& [id, d] : m.divisions)
    if (!d.name.empty() && n.find(lower(d.name)) != std::string::npos) return id;
  std::istringstream words(name);
  std::string w, last;
  while (words >> w) last = w;
  int manual = romanValue(last);
  // "2Man", "Man 3", "Man. II": a manual number beside the word.
  static const std::regex man(R"((\d)\s*man|man\.?\s*(\d|[ivx]+))", std::regex::icase);
  std::smatch mm;
  if (manual == 0 && std::regex_search(name, mm, man))
    manual = mm[1].matched ? std::stoi(mm[1].str())
             : std::isdigit(static_cast<unsigned char>(mm[2].str()[0])) ? std::stoi(mm[2].str())
                                                                         : romanValue(mm[2].str());
  return manual > 0 ? manualDivision(m, manual) : 0;
}

}  // namespace

// A name the organ keeps for itself: "CustPg1_Coupler 1006",
// "CustPg1_EnclIn_220". A player's name has no underscore in it.
static bool technicalName(const std::string& name) { return name.find('_') != std::string::npos; }

// The category words a set writes in front of its names, "Stop: Ped:
// Principal 16", "Coupler: Swell to Great": each short word ending in a colon
// at the start goes. A name that would be left empty keeps them.
static std::string withoutCategory(const std::string& name) {
  std::string out = name;
  for (;;) {
    const auto colon = out.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 12) break;
    const std::string head = out.substr(0, colon);
    if (head.find(' ') != std::string::npos) break;
    size_t rest = colon + 1;
    while (rest < out.size() && out[rest] == ' ') ++rest;
    if (rest >= out.size()) break;
    out.erase(0, rest);
  }
  return out;
}

// What a name says once the organ's bookkeeping is taken off, to tell a
// stop's knob drawn again on another page from a control of its own:
// "11. HW  Principal 8'" and " GO: Montre 16 (Stop Screen)" are the stops
// "HW  Principal 8'" and "GO: Montre 16". The organ's number in front, the
// page in brackets (cut short or not), categories, case and spacing go.
static std::string nameKey(const std::string& name) {
  std::string n = name;
  const auto first = n.find_first_not_of(' ');
  n.erase(0, first == std::string::npos ? n.size() : first);
  size_t digits = 0;
  while (digits < n.size() && std::isdigit(static_cast<unsigned char>(n[digits])) != 0) ++digits;
  if (digits > 0 && digits < n.size() && n[digits] == '.') n.erase(0, digits + 1);
  n.erase(0, std::min(n.size(), n.find_first_not_of(' ')));
  n = withoutCategory(n);
  if (const auto bracket = n.find('('); bracket != std::string::npos && bracket > 0) n.erase(bracket);
  std::string key;
  for (const char ch : n)
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0)
      key += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return key;
}

std::vector<PanelSection> panelSections(const MasterpieceProcessor& p) {
  const auto& m = p.organModel();
  std::vector<PanelSection> divisions;
  std::map<Id, size_t> byDivision;
  auto sectionFor = [&](Id div) -> PanelSection& {
    if (const auto it = byDivision.find(div); it != byDivision.end()) return divisions[it->second];
    PanelSection s;
    s.kind = PanelSection::Kind::Division;
    s.divisionId = div;
    s.key = "d" + std::to_string(static_cast<long long>(div));
    const auto d = m.divisions.find(div);
    s.title = d != m.divisions.end() && !d->second.name.empty()
                  ? d->second.name
                  : TRANS("Division %1").replace("%1", juce::String(static_cast<int>(div))).toStdString();
    byDivision[div] = divisions.size();
    divisions.push_back(std::move(s));
    return divisions.back();
  };

  // Stops, in the order of the stop list: by division, then as the organ
  // numbers them.
  std::set<Id> stopSwitches;
  for (const auto& st : p.stopList()) {
    if (const auto s = m.stops.find(st.stopId); s != m.stops.end())
      stopSwitches.insert(s->second.controllingSwitchId);
    // A stop whose ranks ship no pipes would be a rectangle that does nothing;
    // a stop with no ranks at all is a coupler or a tremulant, and is kept.
    const auto model = m.stops.find(st.stopId);
    const bool control = model != m.stops.end() && model->second.ranks.empty();
    if (!st.playable && !control) continue;
    PanelElement e;
    e.kind = PanelElement::Kind::Stop;
    e.id = st.stopId;
    splitFootage(withoutCategory(st.name), e.name, e.footage);
    sectionFor(st.divisionId).elements.push_back(std::move(e));
  }

  // The program couplers, in a section of their own after the organ's own.
  if (!p.programCouplerList().empty()) {
    PanelSection program;
    program.kind = PanelSection::Kind::Program;
    program.key = "program";
    program.title = TRANS("Program couplers").toStdString();
    for (const auto& pc : p.programCouplerList()) {
      PanelElement e;
      e.kind = PanelElement::Kind::Switch;
      e.id = pc.switchId;
      const auto sw = m.switches.find(pc.switchId);
      e.name = sw != m.switches.end() ? sw->second.name : std::string();
      splitFootage(withoutCategory(e.name), e.name, e.footage);
      program.elements.push_back(std::move(e));
    }
    divisions.push_back(std::move(program));
  }

  // Couplers, then tremulants, each in the division it belongs to.
  std::vector<Id> switchIds;
  for (const auto& [id, sw] : m.switches) switchIds.push_back(id);
  std::sort(switchIds.begin(), switchIds.end());
  PanelSection other;
  other.kind = PanelSection::Kind::Other;
  other.key = "other";
  other.title = TRANS("Other").toStdString();
  // A coupler or tremulant is a drawn, latching knob, flagged as one (custom
  // organs) or wired to a key action or a tremulant (full definitions).
  std::set<Id> tremulantSwitches;
  for (const auto& [id, t] : m.tremulants)
    if (t.controllingSwitchId != 0) tremulantSwitches.insert(t.controllingSwitchId);

  // Copies: switches wired to each other both ways, so each follows the
  // other, are one control drawn in several places. Bethesda draws each
  // coupler twice, "CustPg1_Coupler 1006" and "CustPg2_Coupler 1006", and
  // the coupler itself, "Coupler: Great to Pedal 8", nowhere (#314). A group
  // is one button, named for the member with a player's name; a group that
  // holds a stop's knob is that stop, listed above.
  std::map<Id, Id> parent;
  std::function<Id(Id)> root = [&](Id x) -> Id {
    const auto it = parent.find(x);
    if (it == parent.end() || it->second == x) return x;
    return it->second = root(it->second);
  };
  {
    std::set<std::pair<Id, Id>> links;
    for (const auto& l : m.switchLinkages)
      if (l.conditionSwitchId == 0 && l.sourceWhenEngaged && l.engageAction == 1 && l.disengageAction == 2)
        links.insert({l.sourceSwitchId, l.destSwitchId});
    for (const auto& [a, b] : links)
      if (a < b && links.count({b, a}) != 0) parent[root(b)] = root(a);
  }
  std::map<Id, std::string> groupName;
  for (Id id : switchIds) {
    const auto& name = m.switches.at(id).name;
    auto& best = groupName[root(id)];
    if (best.empty() || (technicalName(best) && !technicalName(name) && !name.empty())) best = name;
  }
  std::set<Id> stopGroups;
  for (const auto& [sid, stop] : m.stops)
    if (stop.controllingSwitchId != 0) {
      stopGroups.insert(root(stop.controllingSwitchId));
      stopGroups.insert(root(p.playerSwitchFor(stop.controllingSwitchId)));
    }
  std::set<Id> groupsShown;

  for (const bool tremulants : {false, true})
    for (Id id : switchIds) {
      const auto& sw = m.switches.at(id);
      if (!sw.clickable || !sw.latching || stopSwitches.count(id) != 0) continue;
      // The program couplers have their section above (#637).
      if (sw.programProvided) continue;
      const bool drawn = sw.dispInstanceId != 0 || sw.isCoupler || sw.isTremulant;
      // A leading underscore marks a switch the organ keeps to itself.
      if (!drawn || sw.name.empty() || sw.name[0] == '_') continue;
      const Id group = root(id);
      if (stopGroups.count(group) != 0 || groupsShown.count(group) != 0) continue;
      PanelElement e;
      e.kind = PanelElement::Kind::Switch;
      e.id = id;
      splitFootage(withoutCategory(groupName[group]), e.name, e.footage);
      const bool isTrem = sw.isTremulant || reaches(m, id, tremulantSwitches);
      Id coupled = isTrem ? 0 : couplerDivision(m, id);
      if (!isTrem && coupled == 0)
        if (const int onto = couplerTarget(e.name); onto >= 0) coupled = manualDivision(m, onto);
      const bool isCoupler = sw.isCoupler || coupled != 0;
      if (tremulants ? !isTrem : !isCoupler) continue;
      const Id div = tremulants ? tremulantDivision(m, sw.name) : coupled;
      groupsShown.insert(group);
      if (div != 0 && m.divisions.count(div) != 0)
        sectionFor(div).elements.push_back(std::move(e));
      else
        other.elements.push_back(std::move(e));
    }

  // The organ's own pistons: the switch each combination is fired by.
  PanelSection presets;
  presets.kind = PanelSection::Kind::Presets;
  presets.key = "presets";
  presets.title = TRANS("Pistons").toStdString();
  std::vector<const Combination*> combos;
  for (const auto& [id, c] : m.combinations) combos.push_back(&c);
  std::sort(combos.begin(), combos.end(), [](const Combination* a, const Combination* b) {
    return a->type != b->type ? a->type < b->type : a->combinationId < b->combinationId;
  });
  auto pistonSwitch = [&](const Combination* c) {
    Id sw = c->activatingSwitchId;
    if (sw == 0 && c->type >= 100)
      for (const auto& [id, s] : m.switches)
        if (s.asgnCode == c->type) { sw = id; break; }
    return sw;
  };
  // An organ that draws its pistons has those: Bethesda declares 89 and
  // draws 29, and the panel listed them all (#314).
  auto drawnSwitch = [&](Id sw) {
    const auto s = m.switches.find(sw);
    return s != m.switches.end() && s->second.dispInstanceId != 0;
  };
  bool anyDrawn = false;
  for (const Combination* c : combos) anyDrawn = anyDrawn || drawnSwitch(pistonSwitch(c));
  std::set<Id> pistons;
  for (const Combination* c : combos) {
    const Id sw = pistonSwitch(c);
    if (sw == 0 || (anyDrawn && !drawnSwitch(sw)) || !pistons.insert(sw).second) continue;
    PanelElement e;
    e.kind = PanelElement::Kind::Piston;
    e.id = sw;
    const auto s = m.switches.find(sw);
    e.name = !c->name.empty() ? c->name
             : s != m.switches.end() && !s->second.name.empty() ? s->second.name
                                                               : TRANS("Piston").toStdString();
    // A divisional sits with its division's stops: several divisions' "1"
    // to "5" in one column could not be told apart (#489).
    if (c->divisionId != 0 && m.divisions.count(c->divisionId) != 0)
      sectionFor(c->divisionId).elements.push_back(std::move(e));
    else
      presets.elements.push_back(std::move(e));
  }

  // A drawn knob that sounds pipes of its own, a blower or a Zimbelstern
  // (#518): it opens a pipe's pallet, and is no stop's knob drawn again on
  // another page. A key and a piston have places of their own.
  {
    std::set<Id> pistonGroups;
    for (const Combination* c : combos)
      if (const Id sw = pistonSwitch(c); sw != 0) pistonGroups.insert(root(sw));
    std::map<Id, std::set<Id>> palletRanks;  // a pallet switch, and the ranks whose pipes it opens
    for (const auto& [rid, rank] : m.ranks)
      for (const auto& pipe : rank.pipes)
        if (pipe.palletSwitchId != 0) palletRanks[pipe.palletSwitchId].insert(rid);
    std::map<Id, std::vector<Id>> linkedTo;
    for (const auto& l : m.switchLinkages) linkedTo[l.sourceSwitchId].push_back(l.destSwitchId);
    // The switches within five linkages of one, itself included.
    const auto within = [&linkedTo](Id from) {
      std::vector<Id> frontier{from};
      std::set<Id> seen{from};
      for (int hop = 0; hop < 5 && !frontier.empty(); ++hop) {
        std::vector<Id> next;
        for (Id s : frontier)
          if (const auto l = linkedTo.find(s); l != linkedTo.end())
            for (Id d : l->second)
              if (seen.insert(d).second) next.push_back(d);
        frontier.swap(next);
      }
      return seen;
    };
    const auto ranksOpened = [&palletRanks](const std::set<Id>& switches) {
      std::set<Id> ranks;
      for (Id s : switches)
        if (const auto it = palletRanks.find(s); it != palletRanks.end())
          ranks.insert(it->second.begin(), it->second.end());
      return ranks;
    };
    const auto meets = [](const std::set<Id>& a, const std::set<Id>& b) {
      return std::any_of(a.begin(), a.end(), [&b](Id x) { return b.count(x) != 0; });
    };
    std::set<Id> stopKnobs = stopSwitches;
    std::set<std::string> named;  // what a stop or a button already shown is called
    for (const auto& [sid, stop] : m.stops) {
      if (stop.controllingSwitchId != 0) stopKnobs.insert(stop.controllingSwitchId);
      named.insert(nameKey(stop.name));
    }
    const auto nameShown = [&named](const PanelSection& s) {
      for (const auto& e : s.elements) {
        named.insert(nameKey(e.name + " " + e.footage));
        named.insert(nameKey(s.title + " " + e.name + " " + e.footage));
      }
    };
    for (const auto& s : divisions) nameShown(s);
    nameShown(other);
    nameShown(presets);
    // A stop's knob clicks its stop action: those noises are ranks of their
    // own, and a knob drawn for a stop the set leaves out opens only them.
    std::set<Id> actionRanks;
    std::map<Id, std::set<Id>> drawnReach;
    for (Id id : switchIds)
      if (m.switches.at(id).dispInstanceId != 0) {
        auto& reach = drawnReach[id] = within(id);
        if (meets(reach, stopKnobs))
          for (Id r : ranksOpened(reach)) actionRanks.insert(r);
      }
    for (const auto& [id, reach] : drawnReach) {
      const auto& sw = m.switches.at(id);
      if (!sw.clickable || !sw.latching || stopSwitches.count(id) != 0) continue;
      if (sw.name.empty() || sw.name[0] == '_' || m.keyboardKeys.count(id) != 0) continue;
      const Id group = root(id);
      if (stopGroups.count(group) != 0 || groupsShown.count(group) != 0 || pistonGroups.count(group) != 0)
        continue;
      if (meets(reach, stopKnobs)) continue;
      const std::set<Id> opened = ranksOpened(reach);
      if (std::none_of(opened.begin(), opened.end(), [&](Id r) { return actionRanks.count(r) == 0; })) continue;
      const std::string key = nameKey(groupName[group]);
      if (key.empty() || !named.insert(key).second) continue;
      groupsShown.insert(group);
      PanelElement e;
      e.kind = PanelElement::Kind::Switch;
      e.id = id;
      splitFootage(withoutCategory(groupName[group]), e.name, e.footage);
      other.elements.push_back(std::move(e));
    }
  }

  // Expression: the shoe each enclosure is worked by, and the crescendo.
  PanelSection controls;
  controls.kind = PanelSection::Kind::Controls;
  controls.key = "controls";
  controls.title = TRANS("Expression").toStdString();
  std::set<Id> shoes;
  auto addControl = [&](Id controlId, const std::string& fallback) {
    if (controlId == 0 || !shoes.insert(controlId).second) return;
    PanelElement e;
    e.kind = PanelElement::Kind::Control;
    e.id = controlId;
    const auto c = m.continuousControls.find(controlId);
    e.name = withoutCategory(c != m.continuousControls.end() && !c->second.name.empty() &&
                                     !technicalName(c->second.name)
                                 ? c->second.name
                                 : fallback);
    controls.elements.push_back(std::move(e));
  };
  for (const auto& [id, enc] : m.enclosures) {
    if (enc.continuousControlId == 0) continue;
    if (const auto c = m.continuousControls.find(enc.continuousControlId);
        c != m.continuousControls.end() && c->second.rememberState && c->second.imageSetInstanceId != 0)
      continue;  // a level set on its own page, not a shoe (see ExpressionBar)
    addControl(p.playerControlFor(enc.continuousControlId), enc.name.empty() ? TRANS("Swell").toStdString() : enc.name);
  }
  Id crescendo = 0;
  size_t steps = 0;
  for (Id id : p.stageSwitches().stagedControls())
    if (const size_t n = p.stageSwitches().stepCount(id); n > steps) {
      steps = n;
      crescendo = id;
    }
  if (steps >= 3) addControl(crescendo, TRANS("Crescendo").toStdString());

  // A division's own prefix, repeated on its stops ("HW Principal", "SW
  // Viola"), says nothing inside that division's column. The short capitalised
  // first word that most of the column's elements share is dropped from those
  // that carry it; a coupler named "III / I" keeps its name.
  for (auto& sec : divisions) {
    std::map<std::string, size_t> firsts;
    auto firstWord = [](const std::string& n) {
      const auto space = n.find(' ');
      if (space == std::string::npos || space == 0 || space > 3) return std::string();
      const std::string w = n.substr(0, space);
      return std::all_of(w.begin(), w.end(), [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) != 0; })
                 ? w : std::string();
    };
    for (const auto& e : sec.elements)
      if (const auto w = firstWord(e.name); !w.empty()) ++firsts[w];
    std::string prefix;
    size_t most = 0;
    for (const auto& [w, n] : firsts)
      if (n > most) { most = n; prefix = w; }
    if (prefix.empty() || most < 2 || most * 2 < sec.elements.size()) continue;
    for (auto& e : sec.elements)
      if (firstWord(e.name) == prefix) e.name = e.name.substr(prefix.size() + 1);
  }

  std::vector<PanelSection> out = std::move(divisions);
  if (!other.elements.empty()) out.push_back(std::move(other));
  if (!presets.elements.empty()) out.push_back(std::move(presets));
  if (!controls.elements.empty()) out.push_back(std::move(controls));
  return out;
}

// ---- saving ----------------------------------------------------------------

std::string PanelLayout::toText() const {
  std::ostringstream o;
  for (const auto& s : sectionsOff) o << "off " << s << "\n";
  for (const auto& h : hidden) o << "hide " << h << "\n";
  o << "detached " << (detached ? 1 : 0) << "\n";
  o << "window " << x << " " << y << " " << w << " " << h << "\n";
  o << "scheme " << scheme << "\n";
  o << "scale " << scale << "\n";
  o << "fit " << (fitToWindow ? 1 : 0) << "\n";
  // A name runs to the end of its line; a line break in it would end it.
  auto oneLine = [](std::string v) {
    for (char& ch : v)
      if (ch == '\n' || ch == '\r') ch = ' ';
    return v;
  };
  for (const auto& [key, name] : names) o << "name " << key << " " << oneLine(name) << "\n";
  if (!title.empty()) o << "title " << oneLine(title) << "\n";
  return o.str();
}

bool PanelLayout::fromText(const std::string& text) {
  *this = PanelLayout{};
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string key;
    if (!(l >> key)) continue;
    if (key == "off") { std::string v; if (l >> v) sectionsOff.insert(v); }
    else if (key == "hide") { std::string v; if (l >> v) hidden.insert(v); }
    else if (key == "detached") { int v = 0; l >> v; detached = v != 0; }
    else if (key == "window") { l >> x >> y >> w >> h; }
    else if (key == "scheme") { l >> scheme; scheme = std::clamp(scheme, 0, 3); }
    else if (key == "scale") { l >> scale; scale = std::clamp(scale, 50, 250); }
    else if (key == "fit") { int v = 0; l >> v; fitToWindow = v != 0; }
    else if (key == "name") {
      std::string k, v;
      if (l >> k && std::getline(l >> std::ws, v) && !v.empty()) names[k] = v;
    } else if (key == "title") {
      std::string v;
      if (std::getline(l >> std::ws, v)) title = v;
    }
  }
  return true;
}

bool PanelLayout::operator==(const PanelLayout& o) const {
  return sectionsOff == o.sectionsOff && hidden == o.hidden && detached == o.detached &&
         x == o.x && y == o.y && w == o.w && h == o.h && scheme == o.scheme &&
         scale == o.scale && fitToWindow == o.fitToWindow && names == o.names &&
         title == o.title;
}

namespace {
juce::File panelsFile(const MasterpieceProcessor& p) {
  return p.organFileForSaving("organs", ".mppanels");
}
}  // namespace

std::vector<PanelLayout> loadPanelLayouts(const MasterpieceProcessor& p) {
  std::vector<PanelLayout> out;
  const auto f = panelsFile(p);
  if (f == juce::File() || !f.existsAsFile()) return out;
  // One "panel" line opens each panel's block.
  std::string block;
  bool open = false;
  auto flush = [&] {
    if (!open || static_cast<int>(out.size()) >= kMaxPanels) return;
    PanelLayout l;
    l.fromText(block);
    out.push_back(std::move(l));
  };
  for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString())) {
    const auto t = line.trim();
    if (t.startsWith("#") || t.isEmpty()) continue;
    if (t == "panel") {
      flush();
      block.clear();
      open = true;
      continue;
    }
    block += t.toStdString() + "\n";
  }
  flush();
  return out;
}

bool savePanelLayouts(const MasterpieceProcessor& p, const std::vector<PanelLayout>& panels) {
  const auto f = panelsFile(p);
  if (f == juce::File() || p.savesHeld()) return false;
  std::string text = "# Masterpiece panels\n";
  for (const auto& l : panels) text += "panel\n" + l.toText();
  f.getParentDirectory().createDirectory();
  return mp::writeTextFile(f, juce::String(text));
}

// ---- colours -----------------------------------------------------------------

juce::StringArray panelSchemeNames() {
  return juce::StringArray(TRANS("Dark"), TRANS("Light"), TRANS("High contrast"), TRANS("By division"));
}

namespace {

struct Colours {
  juce::Colour background, cellOff, cellOn, textOff, textOn, border;
};

Colours schemeColours(int scheme, size_t sectionIndex) {
  switch (static_cast<PanelScheme>(scheme)) {
    case PanelScheme::Light:
      return {juce::Colour(0xffe9e5dc), juce::Colour(0xffd3ccbf), juce::Colour(0xff2f5d8c),
              juce::Colour(0xff26231f), juce::Colour(0xfff4f1ea), juce::Colour(0xffb3ab9c)};
    case PanelScheme::HighContrast:
      return {juce::Colours::black, juce::Colours::black, juce::Colour(0xffffd400),
              juce::Colours::white, juce::Colours::black, juce::Colours::white};
    case PanelScheme::ByDivision: {
      static const float hues[] = {0.08f, 0.58f, 0.33f, 0.78f, 0.0f, 0.15f, 0.48f};
      const float hue = hues[sectionIndex % (sizeof(hues) / sizeof(hues[0]))];
      return {juce::Colour(0xff15171c), juce::Colour::fromHSV(hue, 0.45f, 0.30f, 1.0f),
              juce::Colour::fromHSV(hue, 0.30f, 0.92f, 1.0f), juce::Colour(0xffdfe3ea),
              juce::Colour(0xff15171c), juce::Colour::fromHSV(hue, 0.40f, 0.45f, 1.0f)};
    }
    case PanelScheme::Dark:
    default:
      return {juce::Colour(0xff15171c), juce::Colour(0xff2a2f3a), juce::Colour(0xffeee3c6),
              juce::Colour(0xffb9c2d0), juce::Colour(0xff1b1e24), juce::Colour(0xff3a4150)};
  }
}

}  // namespace

// ---- one rectangle -----------------------------------------------------------

class PanelView::Cell : public juce::Component, public LongPressHolder {
public:
  Cell(PanelView& owner, PanelElement e, size_t sectionIndex)
      : owner_(owner), e_(std::move(e)), section_(sectionIndex) {}
  static constexpr float kTick = kMobile ? 34.0f : 26.0f;

  bool engaged() const {
    auto& p = owner_.proc_;
    switch (e_.kind) {
      case PanelElement::Kind::Stop: return p.shownStopEngaged(e_.id);
      case PanelElement::Kind::Switch: return p.shownSwitchEngaged(e_.id);
      case PanelElement::Kind::Piston: return pressed_;
      case PanelElement::Kind::Control: return false;
    }
    return false;
  }
  // Repaint only when what it shows has changed.
  void follow() {
    const bool now = engaged();
    if (now != shown_) {
      shown_ = now;
      repaint();
    }
  }

  void paint(juce::Graphics& g) override {
    const auto c = schemeColours(owner_.layout_.scheme, section_);
    const bool hidden = owner_.layout_.hidden.count(e_.key()) != 0;
    const bool on = engaged();
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(on ? c.cellOn : c.cellOff);
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(c.border);
    g.drawRoundedRectangle(r, 5.0f, 1.2f);
    g.setColour(on ? c.textOn : c.textOff);
    // The name, wrapped inside the button; the footage on a line of its own.
    //
    // A name too long for the button breaks onto two or more lines, as
    // GrandOrgue breaks it, rather than being shrunk until it fits on one.
    // The size drops only when the wrapped text still does not fit.
    const PanelElement e = owner_.shown(e_);
    auto text = r.reduced(6.0f, 4.0f);
    const float h = std::min(20.0f * owner_.scale() / 100.0f,
                           text.getHeight() * (e.footage.empty() ? 0.42f : 0.34f));
    juce::Font font(juce::FontOptions(h, juce::Font::bold));
    if (!e.footage.empty()) {
      auto foot = text.removeFromBottom(text.getHeight() * 0.36f);
      g.setFont(font);
      g.drawFittedText(juce::String::fromUTF8(e.footage.c_str()), foot.toNearestInt(),
                       juce::Justification::centredTop, 1, 0.7f);
    }
    {
      juce::AttributedString as(juce::String::fromUTF8(e.name.c_str()));
      as.setJustification(juce::Justification::centred);
      // The layout draws in its own colour, black unless told: the button's.
      as.setColour(on ? c.textOn : c.textOff);
      as.setFont(font);
      juce::TextLayout layout;
      layout.createLayout(as, text.getWidth());
      // Smaller until the lines fit the button's height, and until a single
      // word longer than the button ("Geigenprincipal" on a narrow one) fits
      // its width; never below 7 px. The layout breaks such a word inside
      // itself and stays within the width, so the word is measured alone, with
      // a margin: the layout breaks a word a little before the measure ends.
      const auto words = juce::StringArray::fromTokens(as.getText(), " ", "");
      const auto widestWord = [&words](const juce::Font& f) {
        float w = 0.0f;
        for (const auto& word : words) w = std::max(w, juce::GlyphArrangement::getStringWidth(f, word));
        return w;
      };
      for (int i = 0; i < 12 && font.getHeight() > 7.0f &&
                      (layout.getHeight() > text.getHeight() || widestWord(font) * 1.06f > text.getWidth());
           ++i) {
        font.setHeight(std::max(7.0f, font.getHeight() * 0.88f));
        as.setFont(font);
        layout.createLayout(as, text.getWidth());
      }
      auto area = text.withHeight(std::min(text.getHeight(), layout.getHeight()));
      area = area.withY(text.getY() + (text.getHeight() - area.getHeight()) * 0.5f);
      layout.draw(g, area);
    }
    if (owner_.editing_) {
      // The tick: shown, or hidden from the panel.
      auto box = tickArea().withSizeKeepingCentre(16.0f, 16.0f);
      g.setColour(juce::Colours::white);
      g.drawRect(box, 1.5f);
      if (!hidden) {
        juce::Path tick;
        tick.startNewSubPath(box.getX() + 2.0f, box.getCentreY());
        tick.lineTo(box.getX() + box.getWidth() * 0.42f, box.getBottom() - 2.0f);
        tick.lineTo(box.getRight() - 1.0f, box.getY() + 1.0f);
        g.strokePath(tick, juce::PathStrokeType(2.0f));
      }
      if (hidden) {
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(r, 5.0f);
      }
    }
  }

  // The tick box in edit mode, large enough for a finger.
  juce::Rectangle<float> tickArea() const {
    return getLocalBounds().toFloat().removeFromTop(kTick).removeFromRight(kTick);
  }

  void mouseDown(const juce::MouseEvent& ev) override {
    auto& p = owner_.proc_;
    if (owner_.editing_) {
      // The tick shows or hides it; anywhere else on it, or a right-click,
      // names it.
      if (!ev.mods.isPopupMenu() && tickArea().contains(ev.position)) {
        auto& hidden = owner_.layout_.hidden;
        if (!hidden.erase(e_.key())) hidden.insert(e_.key());
        repaint();
        owner_.changed();
      } else {
        owner_.askForName(e_);
      }
      return;
    }
    if (ev.mods.isPopupMenu()) {
      openMapping();
      return;
    }
    // Touch: a finger held still opens the mapping, and a stop or switch
    // changes as the finger lifts, so a held one stays as it was.
    if (owner_.touch_) {
      if (!longPress_.onHeld) longPress_.onHeld = [this] { letGo(); openMapping(); };
      longPress_.begin(ev.getPosition());
    }
    if (e_.kind == PanelElement::Kind::Piston) {
      // Fired on press, as on a console; let go with the finger.
      pressed_ = true;
      p.setSwitchEngaged(e_.id, true);
      repaint();
    } else if (!owner_.touch_) {
      owner_.dragState_ = engaged() ? 0 : 1;
      owner_.dragOn_ = this;
      toggle();
    }
  }
  void mouseDrag(const juce::MouseEvent& ev) override {
    longPress_.moved(ev.getPosition());
    // One drag across a row of stops draws or retires them all (#637).
    if (owner_.dragState_ < 0) return;
    auto* under = dynamic_cast<Cell*>(owner_.getComponentAt(ev.getEventRelativeTo(&owner_).getPosition()));
    if (under == nullptr || under->e_.kind == PanelElement::Kind::Piston) return;
    if (std::exchange(owner_.dragOn_, under) == under) return;
    if (under->engaged() != (owner_.dragState_ == 1)) under->toggle();
  }
  void mouseUp(const juce::MouseEvent& ev) override {
    owner_.dragState_ = -1;
    owner_.dragOn_ = nullptr;
    longPress_.end();
    if (owner_.touch_ && !longPress_.held() && e_.kind != PanelElement::Kind::Piston && !owner_.editing_ &&
        !ev.mods.isPopupMenu() && getLocalBounds().contains(ev.getPosition()))
      toggle();
    letGo();
  }
  LongPress& longPress() override { return longPress_; }

private:
  void toggle() {
    auto& p = owner_.proc_;
    if (e_.kind == PanelElement::Kind::Stop) p.setStopEngaged(e_.id, !p.shownStopEngaged(e_.id));
    if (e_.kind == PanelElement::Kind::Switch) p.setSwitchEngaged(e_.id, !p.shownSwitchEngaged(e_.id));
    repaint();
  }
  void letGo() {
    if (!pressed_) return;
    pressed_ = false;
    owner_.proc_.setSwitchEngaged(e_.id, false);
    repaint();
  }
  // The element's own mapping, on the knob the console draws for it: a stop
  // through its drawstop, a coupler or tremulant through the switch a player
  // moves. The menu is the console's, so a mapping learnt on either is seen
  // and cleared on both (#555).
  void openMapping() {
    auto& p = owner_.proc_;
    const Id target = e_.kind == PanelElement::Kind::Stop ? p.stopKnobFor(e_.id)
                                                           : p.playerSwitchFor(e_.id);
    if (target != 0)
      showSwitchMidiMenu(p, target, getScreenBounds(),
                         [safe = juce::Component::SafePointer<juce::Component>(this)] {
                           if (safe != nullptr) safe->repaint();
                         });
  }

  PanelView& owner_;
  PanelElement e_;
  size_t section_ = 0;
  bool shown_ = false;
  bool pressed_ = false;
  LongPress longPress_;
};

// ---- one fader -----------------------------------------------------------------

class PanelView::Fader : public juce::Component {
public:
  Fader(PanelView& owner, PanelElement e, size_t sectionIndex)
      : owner_(owner), e_(std::move(e)), section_(sectionIndex) {}

  void follow() {
    const int v = owner_.proc_.continuousControlValue(e_.id);
    if (v != shown_) {
      shown_ = v;
      repaint();
    }
  }
  void paint(juce::Graphics& g) override {
    const auto c = schemeColours(owner_.layout_.scheme, section_);
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(c.cellOff);
    g.fillRoundedRectangle(r, 5.0f);
    auto title = r.removeFromTop(22.0f);
    auto track = r.reduced(10.0f, 6.0f);
    const float frac = juce::jlimit(0.0f, 1.0f, owner_.proc_.continuousControlValue(e_.id) / 127.0f);
    g.setColour(c.border);
    g.drawRoundedRectangle(track, 4.0f, 1.2f);
    g.setColour(c.cellOn);
    g.fillRoundedRectangle(track.withTop(track.getBottom() - track.getHeight() * frac), 4.0f);
    g.setColour(c.textOff);
    g.setFont(juce::Font(juce::FontOptions(14.0f * owner_.scale() / 100.0f, juce::Font::bold)));
    g.drawFittedText(juce::String::fromUTF8(owner_.shown(e_).name.c_str()), title.toNearestInt(),
                     juce::Justification::centred, 1, 0.6f);
    if (owner_.editing_ && owner_.layout_.hidden.count(e_.key()) != 0) {
      g.setColour(juce::Colours::black.withAlpha(0.55f));
      g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 5.0f);
    }
  }
  void mouseDown(const juce::MouseEvent& ev) override {
    if (owner_.editing_) {
      // The title names it; the rest shows or hides it.
      if (ev.mods.isPopupMenu() || ev.position.y < 26.0f) {
        owner_.askForName(e_);
        return;
      }
      auto& hidden = owner_.layout_.hidden;
      if (!hidden.erase(e_.key())) hidden.insert(e_.key());
      repaint();
      owner_.changed();
      return;
    }
    if (ev.mods.isPopupMenu()) {
      openMapping();
      return;
    }
    // Touch: a finger held still on the name opens the mapping; the track
    // moves with the finger as before.
    if (owner_.touch_ && ev.position.y < 26.0f) {
      if (!longPress_.onHeld) longPress_.onHeld = [this] { openMapping(); };
      longPress_.begin(ev.getPosition());
      return;
    }
    mouseDrag(ev);
  }
  // The console's menu for a shoe or slider (#555).
  void openMapping() {
    showControlMidiMenu(owner_.proc_, e_.id, getScreenBounds(),
                        [safe = juce::Component::SafePointer<juce::Component>(this)] {
                          if (safe != nullptr) safe->repaint();
                        });
  }
  void mouseUp(const juce::MouseEvent&) override { longPress_.end(); }
  void mouseDrag(const juce::MouseEvent& ev) override {
    if (longPress_.waiting() && longPress_.moved(ev.getPosition())) return;
    if (owner_.editing_ || ev.mods.isPopupMenu()) return;
    const auto track = getLocalBounds().toFloat().reduced(2.0f).withTrimmedTop(22.0f).reduced(10.0f, 6.0f);
    const float frac = juce::jlimit(0.0f, 1.0f, (track.getBottom() - ev.position.y) / track.getHeight());
    owner_.proc_.setContinuousControl(e_.id, juce::roundToInt(frac * 127.0f));
    follow();
  }

private:
  PanelView& owner_;
  PanelElement e_;
  size_t section_ = 0;
  int shown_ = -1;
  LongPress longPress_;
};

// ---- the panel -------------------------------------------------------------------

PanelView::PanelView(MasterpieceProcessor& p, int index, PanelLayout layout)
    : proc_(p), index_(index), layout_(std::move(layout)), saved_(layout_) {
  addAndMakeVisible(edit_);
  edit_.setClickingTogglesState(true);
  edit_.setTooltip(TRANS("Tick the elements this panel shows; touch a name to change it"));
  addAndMakeVisible(rename_);
  rename_.setTooltip(TRANS("Give this panel a name of its own"));
  rename_.onClick = [this] { askForTitle(); };
  edit_.onClick = [this] { setEditing(edit_.getToggleState()); };
  addAndMakeVisible(save_);
  save_.setTooltip(TRANS("Keep the sections, hidden elements, window and colours"));
  save_.onClick = [this] { if (onSave) onSave(); };
  addAndMakeVisible(detach_);
  detach_.setTooltip(TRANS("Show this panel in a window of its own, or back among the tabs"));
  detach_.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
  detach_.setVisible(!kMobile);
  addAndMakeVisible(scheme_);
  scheme_.addItemList(panelSchemeNames(), 1);
  scheme_.setSelectedId(layout_.scheme + 1, juce::dontSendNotification);
  scheme_.onChange = [this] {
    layout_.scheme = scheme_.getSelectedId() - 1;
    repaint();
    for (auto* c : cells_) c->repaint();
    changed();
  };
  // The size, by hand or to fit.
  addAndMakeVisible(smaller_);
  smaller_.setTooltip(TRANS("Make the stops smaller"));
  smaller_.onClick = [this] { setScale(layout_.scale - 10); };
  addAndMakeVisible(bigger_);
  bigger_.setTooltip(TRANS("Make the stops bigger"));
  bigger_.onClick = [this] { setScale(layout_.scale + 10); };
  addAndMakeVisible(size_);
  size_.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(fit_);
  fit_.setTooltip(TRANS("Fit the stops to the window"));
  fit_.onClick = [this] {
    layout_.fitToWindow = fit_.getToggleState();
    changed();
    refreshSizeControls();
    layoutSections();
  };
  refreshSizeControls();
  setWantsKeyboardFocus(true);
  viewport_.setViewedComponent(&body_, false);
  viewport_.setScrollBarsShown(true, true);
  addAndMakeVisible(viewport_);
  rebuild();
  startTimerHz(15);
}

bool PanelView::keyPressed(const juce::KeyPress& key) {
  // Ctrl and +, Ctrl and -, Ctrl and 0, as a browser zooms. The + is on the =
  // key of most keyboards, so either counts, with or without Shift.
  if (!key.getModifiers().isCommandDown()) return false;
  const int code = key.getKeyCode();
  if (code == '+' || code == '=' || code == juce::KeyPress::numberPadAdd) {
    setScale(scale() + 10);
    return true;
  }
  if (code == '-' || code == '_' || code == juce::KeyPress::numberPadSubtract) {
    setScale(scale() - 10);
    return true;
  }
  if (code == '0' || code == juce::KeyPress::numberPad0) {
    setScale(100);
    return true;
  }
  return false;
}

PanelView::~PanelView() { stopTimer(); }

void PanelView::setDetached(bool detached) {
  layout_.detached = detached;
  detach_.setButtonText(detached ? TRANS("Attach") : TRANS("Detach"));
}

void PanelView::rebuild() {
  sections_ = panelSections(proc_);
  sectionButtons_.clear();
  for (const auto& s : sections_) {
    auto* b = sectionButtons_.add(new juce::TextButton(juce::String::fromUTF8(s.title.c_str())));
    b->setClickingTogglesState(true);
    b->setToggleState(layout_.sectionsOff.count(s.key) == 0, juce::dontSendNotification);
    b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff4a5a78));
    const std::string key = s.key;
    b->onClick = [this, b, key] {
      if (b->getToggleState()) layout_.sectionsOff.erase(key);
      else layout_.sectionsOff.insert(key);
      changed();
      layoutSections();
    };
    addAndMakeVisible(b);
  }
  setDetached(layout_.detached);
  resized();
}

PanelElement PanelView::shown(const PanelElement& e) const {
  const auto it = layout_.names.find(e.key());
  if (it == layout_.names.end()) return e;
  PanelElement out = e;
  // The player's name, footage and all: "Trumpet 8'" reads as the organ's
  // own names do.
  if (e.kind == PanelElement::Kind::Control) {
    out.name = it->second;
    out.footage.clear();
  } else {
    splitFootage(it->second, out.name, out.footage);
  }
  return out;
}

namespace {
// A name asked for in a small window: the current name to edit, and a button
// that gives back the organ's own.
void askText(const juce::String& title, const juce::String& prompt, const juce::String& current,
             const juce::String& original, std::function<void(juce::String)> done) {
  auto* w = new juce::AlertWindow(title, prompt, juce::MessageBoxIconType::NoIcon);
  w->addTextEditor("name", current);
  w->addButton(TRANS("OK"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  if (original.isNotEmpty()) w->addButton(TRANS("Organ's name"), 2);
  w->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  if (auto* ed = w->getTextEditor("name")) ed->selectAll();
  w->enterModalState(true, juce::ModalCallbackFunction::create([w, done](int r) {
                       if (r == 1) done(w->getTextEditorContents("name").trim());
                       else if (r == 2) done({});
                     }),
                     true);
}
}  // namespace

void PanelView::askForName(const PanelElement& e) {
  const auto now = shown(e);
  const juce::String current = juce::String::fromUTF8(now.name.c_str()) +
                               (now.footage.empty() ? "" : " " + juce::String::fromUTF8(now.footage.c_str()));
  const juce::String original = juce::String::fromUTF8(e.name.c_str()) +
                                (e.footage.empty() ? "" : " " + juce::String::fromUTF8(e.footage.c_str()));
  juce::Component::SafePointer<PanelView> self(this);
  const std::string key = e.key();
  askText(TRANS("Name"), TRANS("The name this panel shows for %1.").replace("%1", original), current, original,
          [self, key, original](juce::String name) {
            if (self == nullptr) return;
            if (name.isEmpty() || name == original) self->layout_.names.erase(key);
            else self->layout_.names[key] = name.toStdString();
            self->changed();
            self->layoutSections();
          });
}

juce::String PanelView::title() const {
  return layout_.title.empty() ? TRANS("Panel %1").replace("%1", juce::String(index_ + 1))
                               : juce::String::fromUTF8(layout_.title.c_str());
}

void PanelView::askForTitle() {
  juce::Component::SafePointer<PanelView> self(this);
  askText(TRANS("Panel name"), TRANS("The name on this panel's tab and window."), title(), {},
          [self](juce::String name) {
            if (self == nullptr) return;
            self->layout_.title = name.toStdString();
            self->changed();
            if (self->onRenamed) self->onRenamed();
          });
}

void PanelView::setEditing(bool editing) {
  editing_ = editing;
  layoutSections();
}

void PanelView::changed() {
  save_.setButtonText(hasUnsavedChanges() ? TRANS("Save *") : TRANS("Save"));
}

void PanelView::resized() {
  auto r = getLocalBounds();
  auto bar = r.removeFromTop(kMobile ? 44 : 34).reduced(2);
  save_.setBounds(bar.removeFromRight(70).reduced(2));
  edit_.setBounds(bar.removeFromRight(60).reduced(2));
  rename_.setBounds(bar.removeFromRight(76).reduced(2));
  if (detach_.isVisible()) detach_.setBounds(bar.removeFromRight(76).reduced(2));
  scheme_.setBounds(bar.removeFromRight(130).reduced(2));
  // The size, before the section toggles: the smaller and the bigger, the
  // size between them, and the fit.
  fit_.setBounds(bar.removeFromRight(60).reduced(2));
  size_.setBounds(bar.removeFromRight(52).reduced(2));
  bigger_.setBounds(bar.removeFromRight(30).reduced(2));
  smaller_.setBounds(bar.removeFromRight(30).reduced(2));
  // The section toggles share what is left.
  if (!sectionButtons_.isEmpty()) {
    const int w = std::max(60, bar.getWidth() / sectionButtons_.size());
    for (auto* b : sectionButtons_) b->setBounds(bar.removeFromLeft(w).reduced(2));
  }
  viewport_.setBounds(r);
  refreshSizeControls();
  layoutSections();
}

int PanelView::scale() const {
  return layout_.fitToWindow ? fittedScale() : layout_.scale;
}

void PanelView::extentAt(int at, int& width, int& height) const {
  const Geometry g = geometryAt(at);
  int tallest = 0;
  for (size_t si : shownSections()) {
    const auto& s = sections_[si];
    const bool faders = s.kind == PanelSection::Kind::Controls;
    const int cols = g.columnsFor(faders);
    int k = 0;
    for (const auto& e : s.elements)
      if (editing_ || layout_.hidden.count(e.key()) == 0) ++k;
    if (k == 0) continue;
    const int rows = (k + cols - 1) / cols;
    tallest = std::max(tallest, g.top + rows * (faders ? g.faderH : g.cellH) + 6);
  }
  width = g.columnW * static_cast<int>(std::max<size_t>(1, shownSections().size()));
  height = tallest;
}

std::vector<size_t> PanelView::shownSections() const {
  std::vector<size_t> shown;
  for (size_t i = 0; i < sections_.size(); ++i)
    if (layout_.sectionsOff.count(sections_[i].key) == 0) shown.push_back(i);
  return shown;
}

PanelView::Geometry PanelView::geometryAt(int at) const {
  // Every size of the panel at `at` per cent; at 100 these are the sizes the
  // panel always had, to the pixel.
  Geometry g;
  const int viewW = std::max(1, viewport_.getMaximumVisibleWidth());
  const int n = std::max<int>(1, static_cast<int>(shownSections().size()));
  g.columnW = std::max((kMobile ? 150 : 170) * at / 100, viewW / n);
  g.cellH = (kMobile ? 64 : 58) * at / 100;
  g.faderH = 220 * at / 100;
  g.cellMinW = 104 * at / 100;
  g.faderMinW = 90 * at / 100;
  g.top = 30 * at / 100;
  g.titleH = 22 * at / 100;
  return g;
}

int PanelView::fittedScale() const {
  const int viewW = std::max(1, viewport_.getMaximumVisibleWidth());
  const int viewH = std::max(1, viewport_.getMaximumVisibleHeight());
  int best = 50;
  for (int candidate = 50; candidate <= 250; candidate += 5) {
    int w = 0, h = 0;
    extentAt(candidate, w, h);
    if (w > viewW || h > viewH) break;
    best = candidate;
  }
  return best;
}

void PanelView::setScale(int at) {
  // A size chosen by hand ends the fit: the keys step from the size on show.
  layout_.fitToWindow = false;
  layout_.scale = std::clamp(at, 50, 250);
  changed();
  refreshSizeControls();
  layoutSections();
}

void PanelView::refreshSizeControls() {
  const int now = scale();
  size_.setText(juce::String(now) + "%", juce::dontSendNotification);
  smaller_.setEnabled(!layout_.fitToWindow && layout_.scale > 50);
  bigger_.setEnabled(!layout_.fitToWindow && layout_.scale < 250);
  fit_.setToggleState(layout_.fitToWindow, juce::dontSendNotification);
}

void PanelView::layoutSections() {
  titles_.clear();
  cells_.clear();
  std::vector<size_t> shown;
  for (size_t i = 0; i < sections_.size(); ++i)
    if (layout_.sectionsOff.count(sections_[i].key) == 0) shown.push_back(i);

  const int at = scale();
  const Geometry g = geometryAt(at);
  const int viewW = std::max(1, viewport_.getMaximumVisibleWidth());
  const int columnW = g.columnW;
  const int cellH = g.cellH;
  const int faderH = g.faderH;
  int tallest = 0;
  int x = 0;
  for (size_t si : shown) {
    const auto& s = sections_[si];
    auto* t = titles_.add(new juce::Label({}, juce::String::fromUTF8(s.title.c_str())));
    t->setFont(juce::Font(juce::FontOptions(15.0f * at / 100.0f, juce::Font::bold)));
    t->setColour(juce::Label::textColourId, juce::Colour(0xffe0a050));
    t->setJustificationType(juce::Justification::centredLeft);
    t->setBounds(x + 6, 4 * at / 100, columnW - 12, g.titleH);
    body_.addAndMakeVisible(t);

    const bool faders = s.kind == PanelSection::Kind::Controls;
    const int cols = g.columnsFor(faders);
    const int cellW = (columnW - 8) / cols;
    int k = 0;
    for (const auto& e : s.elements) {
      if (!editing_ && layout_.hidden.count(e.key()) != 0) continue;
      const int col = k % cols, row = k / cols;
      const int h = faders ? faderH : cellH;
      juce::Component* c = faders ? static_cast<juce::Component*>(new Fader(*this, e, si))
                                  : static_cast<juce::Component*>(new Cell(*this, e, si));
      c->setBounds(x + 4 + col * cellW, g.top + row * h, cellW, h);
      body_.addAndMakeVisible(c);
      cells_.add(c);
      tallest = std::max(tallest, g.top + (row + 1) * h + 6);
      ++k;
    }
    x += columnW;
  }
  body_.setSize(std::max(x, viewW), std::max(tallest, viewport_.getMaximumVisibleHeight()));
  repaint();
}

void PanelView::paint(juce::Graphics& g) {
  g.fillAll(schemeColours(layout_.scheme, 0).background);
}

void PanelView::timerCallback() {
  // While nobody can see it, four looks a second, each of which only finds
  // out whether that is still so; its own rate from the first look after
  // it is shown. Every look of a timer wakes two threads.
  if (!isShowing()) {
    if (getTimerInterval() < 250) startTimer(250);
    return;
  }
  if (getTimerInterval() >= 250) startTimerHz(15);
  for (auto* c : cells_) {
    if (auto* cell = dynamic_cast<Cell*>(c)) cell->follow();
    else if (auto* fader = dynamic_cast<Fader*>(c)) fader->follow();
  }
}

// ---- a panel in its own window -------------------------------------------------

PanelWindow::PanelWindow(const juce::String& title, PanelView& view)
    : juce::DocumentWindow(title, juce::Colour(0xff15171c), juce::DocumentWindow::allButtons) {
  setUsingNativeTitleBar(true);
  setResizable(true, true);
  setContentNonOwned(&view, false);
  const auto& l = view.layout();
  if (l.w > 100 && l.h > 100) setBounds(l.x, l.y, l.w, l.h);
  else centreWithSize(1024, 600);
}

void PanelWindow::closeButtonPressed() {
  if (onClose) onClose();
}

}  // namespace mp::ui
