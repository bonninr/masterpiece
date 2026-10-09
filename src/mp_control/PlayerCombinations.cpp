#include "PlayerCombinations.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mp {

namespace {

// The division a keyboard plays: the file's hint, or failing that the division
// that lists the keyboard as its own.
Id divisionOfKeyboard(const OrganModel& model, int keyboardId) {
  const auto it = model.keyboards.find(keyboardId);
  if (it != model.keyboards.end() && it->second.primaryDivisionHint != 0)
    return it->second.primaryDivisionHint;
  for (const auto& [id, div] : model.divisions)
    if (std::find(div.keyboardIds.begin(), div.keyboardIds.end(), keyboardId) !=
        div.keyboardIds.end())
      return id;
  return 0;
}

// One division when every vote agrees, 0 when they disagree or there are none.
struct Vote {
  Id division = 0;
  bool mixed = false;
  bool any = false;
  void add(Id d) {
    if (!any) {
      division = d;
      any = true;
    } else if (d != division) {
      mixed = true;
    }
  }
  Id result() const { return any && !mixed ? division : 0; }
};

bool mentions(std::string text, const char* word) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text.find(word) != std::string::npos;
}

// A stop that is really a sound effect: the blower, a motor, a tracker
// noise. Sets model these as ordinary stops more often than through the
// Noise table -- Vasvar's blower is "Stop: Noises: Blower", in a division
// called NOISES, with no Noise row at all -- and a general cancel that
// switched the blower off would be a very bad piston.
bool isEffect(const OrganModel& model, const Stop& stop) {
  if (mentions(stop.name, "noise") || mentions(stop.name, "blower")) return true;
  const auto div = model.divisions.find(stop.divisionId);
  if (div != model.divisions.end() && mentions(div->second.name, "noise")) return true;
  bool anyRank = false;
  for (const auto& entry : stop.ranks) {
    const auto rit = model.ranks.find(entry.rankId);
    if (rit == model.ranks.end()) continue;
    if (!rit->second.isNoise) return false;
    anyRank = true;
  }
  return anyRank;  // every rank it sounds is a noise
}

} // namespace

std::string PlayerCombinations::keyOf(const Element& e) {
  return (e.kind == ElementKind::Stop ? "s" : "w") + std::to_string(e.id);
}

std::vector<PlayerCombinations::Element> PlayerCombinations::collect(
    const OrganModel& model, const std::function<Id(Id)>& playerSwitch,
    const std::function<bool(const Element&)>& onConsole) {
  auto player = [&](Id sw) { return playerSwitch ? playerSwitch(sw) : sw; };
  std::vector<Element> out;

  // Switches that are something other than registration, and must never be
  // captured or cancelled as though they were: the stops' own switches (the
  // stop is registered as a stop), pistons and the setter.
  std::unordered_set<Id> notRegistration;
  for (const auto& [id, stop] : model.stops)
    if (stop.controllingSwitchId != 0) {
      notRegistration.insert(stop.controllingSwitchId);
      notRegistration.insert(player(stop.controllingSwitchId));
    }
  std::unordered_set<int> pistonCodes{12};  // 12 is the setter
  for (const auto& [id, combo] : model.combinations) {
    if (combo.activatingSwitchId != 0) notRegistration.insert(combo.activatingSwitchId);
    if (combo.type > 0) pistonCodes.insert(combo.type);
  }
  for (const auto& [sid, sw] : model.switches)
    if (sw.asgnCode > 0 && pistonCodes.count(sw.asgnCode) != 0) notRegistration.insert(sid);

  // A stop worked from a blower switch is that division's wind (#256):
  // Clarendon's Antiphonal Trumpet stands on "Blower: Antiphonal Wind",
  // which the organ's own combinations never store, and cancelling it left
  // every drawstop that couples the trumpet silent with no knob to restore it.
  auto onBlower = [&](const Stop& stop) {
    if (stop.controllingSwitchId == 0) return false;
    for (Id sw : {stop.controllingSwitchId, player(stop.controllingSwitchId)}) {
      const auto it = model.switches.find(sw);
      if (it != model.switches.end() && mentions(it->second.name, "blower")) return true;
    }
    return false;
  };

  // Stops, in the stop list's order.
  std::vector<std::pair<Id, Id>> stops;  // (division, stop)
  for (const auto& [id, stop] : model.stops)
    if (!isEffect(model, stop) && !onBlower(stop)) stops.emplace_back(stop.divisionId, id);
  std::sort(stops.begin(), stops.end());
  for (const auto& [div, id] : stops) {
    Element e;
    e.kind = ElementKind::Stop;
    e.id = id;
    e.divisionId = div;
    e.name = model.stops.at(id).name;
    out.push_back(std::move(e));
  }

  // Couplers: whatever switch a key action waits on. A coupler belongs to the
  // manual it is played from -- Swell to Great is part of the Great's
  // registration -- so the vote is the source keyboard's division.
  //
  // A coupler can be worked through a delay (Friesach): the knob conditions a
  // ramp, and the ramp's top stage engages the switch the key action waits
  // on. No switch linkage leads from there back to the knob, so the coupler
  // had no knob to capture and general combinations left it out (#90). Its
  // owner is the switch that conditions the ramp.
  std::unordered_map<Id, Id> delayedBy;
  {
    std::unordered_map<Id, Id> conditionOfControl;
    for (const auto& l : model.controlLinkages)
      if (l.conditionSwitchId != 0) conditionOfControl.emplace(l.destControlId, l.conditionSwitchId);
    for (const auto& st : model.controlStageSwitches) {
      const auto it = conditionOfControl.find(st.controlId);
      if (it != conditionOfControl.end()) delayedBy.emplace(st.controlledSwitchId, it->second);
    }
  }
  std::map<Id, Vote> couplers;
  auto addActions = [&](const std::vector<KeyAction>& actions, Id fallbackDivision) {
    for (const KeyAction& ka : actions) {
      if (ka.conditionSwitchId == 0) continue;
      Id condition = ka.conditionSwitchId;
      if (const auto d = delayedBy.find(condition); d != delayedBy.end()) condition = d->second;
      const Id sw = player(condition);
      if (notRegistration.count(sw) != 0 || notRegistration.count(ka.conditionSwitchId) != 0)
        continue;
      Id div = ka.sourceDivision != 0 ? ka.sourceDivision
                                      : divisionOfKeyboard(model, ka.sourceKeyboard);
      if (div == 0) div = fallbackDivision;
      couplers[sw].add(div);
    }
  };
  addActions(model.keyActions, 0);
  for (const auto& [id, div] : model.divisions) addActions(div.keyActions, id);

  // Tremulants: the division of the stops whose pipes they shake.
  std::map<Id, Vote> tremulants;
  {
    std::unordered_map<Id, Vote> byTrem;
    if (!model.tremulantPipes.empty())
      for (const auto& [sid, stop] : model.stops)
        for (const auto& entry : stop.ranks) {
          const auto rit = model.ranks.find(entry.rankId);
          if (rit == model.ranks.end()) continue;
          for (const Pipe& p : rit->second.pipes) {
            const auto tp = model.tremulantPipes.find(p.pipeId);
            if (tp != model.tremulantPipes.end()) byTrem[tp->second.tremulantId].add(stop.divisionId);
          }
        }
    for (const auto& [id, t] : model.tremulants) {
      if (t.controllingSwitchId == 0) continue;
      const Id sw = player(t.controllingSwitchId);
      if (notRegistration.count(sw) != 0) continue;
      const auto v = byTrem.find(id);
      if (v != byTrem.end()) tremulants[sw].add(v->second.result());
      else tremulants[sw].add(0);
    }
    // A file with no tremulant rows but switches flagged as tremulants.
    for (const auto& [sid, sw] : model.switches)
      if (sw.isTremulant && notRegistration.count(sid) == 0 && tremulants.count(sid) == 0)
        tremulants[sid].add(0);
  }

  std::unordered_set<Id> taken;
  auto addSwitch = [&](Id sw, Id div) {
    if (!taken.insert(sw).second) return;
    Element e;
    e.kind = ElementKind::Switch;
    e.id = sw;
    e.divisionId = div;
    const auto it = model.switches.find(sw);
    e.name = it != model.switches.end() ? it->second.name : std::string();
    out.push_back(std::move(e));
  };
  for (const auto& [sw, vote] : couplers) addSwitch(sw, vote.result());
  for (const auto& [sid, sw] : model.switches)
    if (sw.isCoupler && notRegistration.count(sid) == 0) addSwitch(sid, 0);
  for (const auto& [sw, vote] : tremulants) addSwitch(sw, vote.result());
  if (onConsole)
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const Element& e) { return !onConsole(e); }),
              out.end());
  return out;
}

std::vector<PlayerCombinations::Division> PlayerCombinations::divisionsOf(
    const OrganModel& model, const std::vector<Element>& elements) {
  std::set<Id> used;
  for (const auto& e : elements)
    if (e.divisionId != 0) used.insert(e.divisionId);
  std::vector<std::pair<std::pair<int, Id>, Division>> ordered;
  for (Id id : used) {
    Division d;
    d.divisionId = id;
    int manual = 1000;
    const auto it = model.divisions.find(id);
    if (it != model.divisions.end()) {
      d.name = it->second.name;
      manual = it->second.manualNumber;
    }
    if (d.name.empty()) d.name = "Division " + std::to_string(id);
    ordered.push_back({{manual, id}, std::move(d)});
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<Division> out;
  for (auto& [k, d] : ordered) out.push_back(std::move(d));
  return out;
}

void PlayerCombinations::reset(std::vector<Element> elements,
                               std::vector<Division> divisions) {
  elements_ = std::move(elements);
  divisions_ = std::move(divisions);
  const Registration blank{false, std::vector<char>(elements_.size(), 0)};
  generals_.assign(kMaxGenerals + 1, blank);
  frames_.assign(kMaxFrames + 1, blank);
  divisionals_.assign(divisions_.size() * (kMaxDivisionals + 1), blank);
  litDivisional_.assign(divisions_.size(), 0);
  clearAll();
}

void PlayerCombinations::clearAll() {
  generalCount_ = kDefaultGenerals;
  divisionalCount_ = kDefaultDivisionals;
  for (auto* bank : {&generals_, &frames_, &divisionals_})
    for (auto& r : *bank) {
      r.set = false;
      std::fill(r.drawn.begin(), r.drawn.end(), 0);
    }
  frame_ = 0;
  registrationMoved();
}

void PlayerCombinations::setGeneralCount(int n) {
  generalCount_ = std::clamp(n, 1, kMaxGenerals);
}

void PlayerCombinations::setDivisionalCount(int n) {
  divisionalCount_ = std::clamp(n, 1, kMaxDivisionals);
}

int PlayerCombinations::divisionIndex(Id divisionId) const {
  for (size_t i = 0; i < divisions_.size(); ++i)
    if (divisions_[i].divisionId == divisionId) return static_cast<int>(i);
  return -1;
}

PlayerCombinations::Registration* PlayerCombinations::divisional(Id divisionId, int n) {
  const int d = divisionIndex(divisionId);
  if (d < 0 || n < 1 || n > divisionalCount_) return nullptr;
  return &divisionals_[static_cast<size_t>(d) * (kMaxDivisionals + 1) +
                       static_cast<size_t>(n)];
}

const PlayerCombinations::Registration* PlayerCombinations::divisional(Id divisionId,
                                                                       int n) const {
  return const_cast<PlayerCombinations*>(this)->divisional(divisionId, n);
}

void PlayerCombinations::captureInto(Registration& r, Id divisionScope, bool all,
                                     const Reader& isEngaged) const {
  r.set = true;
  for (size_t i = 0; i < elements_.size(); ++i) {
    const Element& e = elements_[i];
    const bool inScope = all || e.divisionId == divisionScope;
    r.drawn[i] = inScope && isEngaged && isEngaged(e) ? 1 : 0;
  }
}

void PlayerCombinations::recallInto(const Registration& r, Id divisionScope,
                                    bool all, std::vector<Change>& out) const {
  if (!r.set) return;
  for (size_t i = 0; i < elements_.size(); ++i) {
    const Element& e = elements_[i];
    if (!all && e.divisionId != divisionScope) continue;
    out.push_back({e.kind, e.id, r.drawn[i] != 0});
  }
}

bool PlayerCombinations::captureGeneral(int n, const Reader& isEngaged) {
  if (n < 1 || n > generalCount_) return false;
  captureInto(generals_[static_cast<size_t>(n)], 0, true, isEngaged);
  lightGeneral(n);
  return true;
}

bool PlayerCombinations::captureDivisional(Id divisionId, int n,
                                           const Reader& isEngaged) {
  Registration* r = divisional(divisionId, n);
  if (r == nullptr) return false;
  captureInto(*r, divisionId, false, isEngaged);
  lightDivisional(divisionId, n);
  return true;
}

void PlayerCombinations::recallGeneral(int n, std::vector<Change>& out) const {
  if (n < 1 || n > generalCount_) return;
  recallInto(generals_[static_cast<size_t>(n)], 0, true, out);
}

void PlayerCombinations::recallDivisional(Id divisionId, int n,
                                          std::vector<Change>& out) const {
  if (const Registration* r = divisional(divisionId, n))
    recallInto(*r, divisionId, false, out);
}

void PlayerCombinations::generalCancel(std::vector<Change>& out) const {
  for (const auto& e : elements_) out.push_back({e.kind, e.id, false});
}

void PlayerCombinations::divisionalCancel(Id divisionId,
                                          std::vector<Change>& out) const {
  for (const auto& e : elements_)
    if (e.divisionId == divisionId) out.push_back({e.kind, e.id, false});
}

bool PlayerCombinations::generalSet(int n) const {
  return n >= 1 && n <= kMaxGenerals && generals_[static_cast<size_t>(n)].set;
}

bool PlayerCombinations::divisionalSet(Id divisionId, int n) const {
  const Registration* r = divisional(divisionId, n);
  return r != nullptr && r->set;
}

int PlayerCombinations::litDivisional(Id divisionId) const {
  const int d = divisionIndex(divisionId);
  return d < 0 ? 0 : litDivisional_[static_cast<size_t>(d)];
}

void PlayerCombinations::registrationMoved() {
  litGeneral_ = 0;
  std::fill(litDivisional_.begin(), litDivisional_.end(), 0);
}

// A general describes the whole console, so it replaces every lit piston; a
// divisional describes one division, so it only unlights the general.
void PlayerCombinations::lightGeneral(int n) {
  registrationMoved();
  litGeneral_ = n;
}

void PlayerCombinations::lightDivisional(Id divisionId, int n) {
  litGeneral_ = 0;
  const int d = divisionIndex(divisionId);
  if (d >= 0) litDivisional_[static_cast<size_t>(d)] = n;
}

// --------------------------------------------------------------- stepper

int PlayerCombinations::lastUsedFrame() const {
  for (int n = kMaxFrames; n >= 1; --n)
    if (frames_[static_cast<size_t>(n)].set) return n;
  return 0;
}

bool PlayerCombinations::frameSet(int oneBased) const {
  return oneBased >= 1 && oneBased <= kMaxFrames &&
         frames_[static_cast<size_t>(oneBased)].set;
}

bool PlayerCombinations::fireFrame(int oneBased, bool capturing,
                                   const Reader& isEngaged,
                                   std::vector<Change>& out) {
  frame_ = oneBased;
  Registration& r = frames_[static_cast<size_t>(oneBased)];
  if (capturing) captureInto(r, 0, true, isEngaged);
  else recallInto(r, 0, true, out);
  registrationMoved();
  return true;
}

bool PlayerCombinations::stepNext(bool capturing, const Reader& isEngaged,
                                  std::vector<Change>& out) {
  const int limit = capturing ? kMaxFrames : lastUsedFrame();
  if (frame_ >= limit) return false;  // held at the end, never wrapped
  return fireFrame(frame_ + 1, capturing, isEngaged, out);
}

bool PlayerCombinations::stepPrev(bool capturing, const Reader& isEngaged,
                                  std::vector<Change>& out) {
  if (frame_ <= 1) return false;
  return fireFrame(frame_ - 1, capturing, isEngaged, out);
}

bool PlayerCombinations::gotoFrame(int oneBased, bool capturing,
                                   const Reader& isEngaged,
                                   std::vector<Change>& out) {
  if (oneBased < 1 || oneBased > kMaxFrames) return false;
  return fireFrame(oneBased, capturing, isEngaged, out);
}

bool PlayerCombinations::insertFrame() {
  const int at = std::max(frame_, 1);
  if (frames_[kMaxFrames].set) return false;  // the last frame would fall off
  // Rotating within the fixed bank moves every later frame up one and brings
  // the empty last frame round to `at`, without reallocating anything.
  std::rotate(frames_.begin() + at, frames_.begin() + kMaxFrames, frames_.end());
  frame_ = at;
  return true;
}

bool PlayerCombinations::deleteFrame() {
  if (frame_ < 1) return false;
  std::rotate(frames_.begin() + frame_, frames_.begin() + frame_ + 1, frames_.end());
  Registration& last = frames_[kMaxFrames];
  last.set = false;
  std::fill(last.drawn.begin(), last.drawn.end(), 0);
  const int used = std::max(lastUsedFrame(), 1);
  if (frame_ > used) frame_ = used;
  return true;
}

// ----------------------------------------------------------- persistence
//
//   counts <generals> <divisionals>
//   general <n> [key...]
//   divisional <division> <n> [key...]
//   frame <n> [key...]
//
// Keys are s<stop> and w<switch>, so a file survives an organ update that
// adds or reorders stops. A piston captured empty is written with no keys,
// which is different from not being written at all.

std::string PlayerCombinations::toText() const {
  std::ostringstream out;
  out << "counts " << generalCount_ << ' ' << divisionalCount_ << '\n';
  auto keys = [&](const Registration& r) {
    for (size_t i = 0; i < elements_.size(); ++i)
      if (r.drawn[i]) out << ' ' << keyOf(elements_[i]);
    out << '\n';
  };
  for (int n = 1; n <= kMaxGenerals; ++n)
    if (generals_[static_cast<size_t>(n)].set) {
      out << "general " << n;
      keys(generals_[static_cast<size_t>(n)]);
    }
  for (size_t d = 0; d < divisions_.size(); ++d)
    for (int n = 1; n <= kMaxDivisionals; ++n) {
      const auto& r = divisionals_[d * (kMaxDivisionals + 1) + static_cast<size_t>(n)];
      if (!r.set) continue;
      out << "divisional " << divisions_[d].divisionId << ' ' << n;
      keys(r);
    }
  for (int n = 1; n <= kMaxFrames; ++n)
    if (frames_[static_cast<size_t>(n)].set) {
      out << "frame " << n;
      keys(frames_[static_cast<size_t>(n)]);
    }
  return out.str();
}

bool PlayerCombinations::fromText(const std::string& text) {
  clearAll();
  std::unordered_map<std::string, size_t> index;
  for (size_t i = 0; i < elements_.size(); ++i) index.emplace(keyOf(elements_[i]), i);

  bool ok = true;
  auto readKeys = [&](std::istringstream& ls, Registration& r) {
    r.set = true;
    std::fill(r.drawn.begin(), r.drawn.end(), 0);
    // A key this organ no longer has is dropped: it can never be recalled.
    std::string k;
    while (ls >> k) {
      const auto it = index.find(k);
      if (it != index.end()) r.drawn[it->second] = 1;
    }
  };
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string what;
    ls >> what;
    if (what == "counts") {
      int g = 0, d = 0;
      if (ls >> g >> d) {
        setGeneralCount(g);
        setDivisionalCount(d);
      } else {
        ok = false;
      }
    } else if (what == "general") {
      int n = 0;
      if (!(ls >> n) || n < 1 || n > kMaxGenerals) { ok = false; continue; }
      readKeys(ls, generals_[static_cast<size_t>(n)]);
    } else if (what == "divisional") {
      long long div = 0;
      int n = 0;
      if (!(ls >> div >> n) || n < 1 || n > kMaxDivisionals) { ok = false; continue; }
      const int d = divisionIndex(static_cast<Id>(div));
      if (d < 0) { ok = false; continue; }  // a division this organ lacks
      readKeys(ls, divisionals_[static_cast<size_t>(d) * (kMaxDivisionals + 1) +
                                static_cast<size_t>(n)]);
    } else if (what == "frame") {
      int n = 0;
      if (!(ls >> n) || n < 1 || n > kMaxFrames) { ok = false; continue; }
      readKeys(ls, frames_[static_cast<size_t>(n)]);
    } else {
      ok = false;
    }
  }
  return ok;
}

} // namespace mp
