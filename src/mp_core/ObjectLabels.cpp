#include "ObjectLabels.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace mp {

std::string readableName(const std::string& raw) {
  std::string s = raw;
  if (s.rfind("__", 0) == 0) {
    // An internal name: "__Midm_Pedal to Great" is the console's "Pedal to
    // Great". Only a short leading word counts as the prefix, so a name that
    // starts with underscores for another reason keeps its own text.
    const size_t start = s.find_first_not_of('_');
    s.erase(0, start == std::string::npos ? s.size() : start);
    const size_t cut = s.find('_');
    if (cut != std::string::npos && cut > 0 && cut < 8 &&
        s.substr(0, cut).find(' ') == std::string::npos)
      s.erase(0, cut + 1);
  }
  std::string out;
  out.reserve(s.size());
  for (const char c : s)
    if (!(c == ' ' && !out.empty() && out.back() == ' ')) out.push_back(c);
  const size_t first = out.find_first_not_of(' ');
  const size_t last = out.find_last_not_of(' ');
  if (first == std::string::npos) return {};
  return out.substr(first, last - first + 1);
}

namespace {

// A name with words in it is a label. A name that is only a number is the
// set's internal tag for a relay.
bool hasWords(const std::string& s) {
  return std::any_of(s.begin(), s.end(), [](unsigned char c) {
    // Anything outside ASCII counts as a letter too: a set in Cyrillic or
    // Greek names its controls in words just as one in English does.
    return std::isalpha(c) != 0 || c >= 0x80;
  });
}

// The stops and tremulants whose own switch this is, lowest id first, each
// name once.
std::vector<std::string> namesHere(const OrganModel& m, Id switchId) {
  std::vector<std::pair<Id, std::string>> found;
  for (const auto& [stopId, stop] : m.stops)
    if (stop.controllingSwitchId == switchId && !stop.name.empty()) found.emplace_back(stopId, stop.name);
  for (const auto& [tremId, trem] : m.tremulants)
    if (trem.controllingSwitchId == switchId && !trem.name.empty()) found.emplace_back(tremId, trem.name);
  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<std::string> out;
  for (const auto& entry : found) {
    auto readable = readableName(entry.second);
    if (readable.empty()) continue;
    if (std::find(out.begin(), out.end(), readable) == out.end()) out.push_back(std::move(readable));
  }
  return out;
}

}  // namespace

std::vector<std::string> switchWorkedNames(const OrganModel& m, Id switchId) {
  // Forwards first: what a knob drives is nearer than what drives it. Four
  // hops reaches a knob through a state machine three switches deep, which is
  // as far as these sets go. The nearest ring that names anything answers,
  // so a knob is never named after a stop its state machine reaches before
  // the one it works.
  for (const bool forward : {true, false}) {
    std::vector<Id> ring{switchId};
    std::unordered_set<Id> seen{switchId};
    for (int depth = 0; depth < (forward ? 4 : 2) && !ring.empty(); ++depth) {
      std::vector<std::string> names;
      for (const Id sw : ring)
        for (auto& n : namesHere(m, sw))
          if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(std::move(n));
      if (!names.empty()) return names;
      std::vector<Id> next;
      for (const auto& l : m.switchLinkages) {
        const Id from = forward ? l.sourceSwitchId : l.destSwitchId;
        const Id to = forward ? l.destSwitchId : l.sourceSwitchId;
        for (const Id sw : ring)
          if (from == sw && seen.insert(to).second) next.push_back(to);
      }
      ring = std::move(next);
    }
  }
  return {};
}

std::string switchLabel(const OrganModel& m, Id switchId) {
  const auto it = m.switches.find(switchId);
  const bool named = it != m.switches.end() && hasWords(it->second.name);
  // A coupler is its own control, named by the set: the stops its wiring may
  // reach are not what it is.
  if (named && it->second.isCoupler) return readableName(it->second.name);
  const auto worked = switchWorkedNames(m, switchId);

  // One stop or tremulant along the wiring is the answer, and is what these
  // sets name the control behind a knob by: "CustPg1_Stop 2224" is the
  // console's "Open Diapason 8".
  if (worked.size() == 1) return worked.front();

  // Several is a control of its own: Azzio's two Sesquialtera knobs both reach
  // the one switch that turns the stop on, and both stops hang off it. Named
  // from the wiring, the two knobs carried the same label, and which of the
  // two stop names appeared was not fixed between runs (#591). The set's own
  // words tell them apart, so where the switch has words they win.
  if (named) return readableName(it->second.name);

  // A switch named by a number or not at all, over several stops: the lowest
  // id names it, so every control over it reads the same way from run to run.
  if (!worked.empty()) return worked.front();
  if (it != m.switches.end()) return readableName(it->second.name);
  return {};
}

}  // namespace mp
