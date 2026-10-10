#include "Perspectives.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace mp {
namespace {

std::string lowerTrim(std::string s) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Words that name where the microphones stood and nothing else. "Front" or
// "Rear" could never be a stop's own name; "Echo" or "Solo" could, and are
// not here.
const std::set<std::string>& positions() {
  static const std::set<std::string> words = {
      "close",  "direct", "near",     "dry",     "front",   "middle", "mid",
      "diffuse", "rear",  "far",      "back",    "surround", "ambient", "ambience",
      "distant", "wet",   "gallery",  "nave",    "center",  "centre"};
  return words;
}

}  // namespace

std::string perspectiveOf(const std::string& rankName) {
  const std::string name = lowerTrim(rankName);
  // A prefix before a colon, when it names a position: "Rear: Ped Octave 4",
  // "Front (Diffuse): HW Fagott 16 (tremmed)" (Buckeburg). It comes first,
  // because the bracket at the end of such a name is the stop's own. A
  // prefix naming no position ("HW: Principal 8") is a division, not this.
  const auto colon = name.find(':');
  if (colon != std::string::npos) {
    const std::string prefix = lowerTrim(name.substr(0, colon));
    std::string word;
    for (const char c : prefix + " ") {
      if (std::isalpha(static_cast<unsigned char>(c))) { word += c; continue; }
      if (positions().count(word) != 0) return prefix;
      word.clear();
    }
  }
  if (!name.empty() && name.back() == ')') {
    const auto open = name.rfind('(');
    if (open != std::string::npos) return lowerTrim(name.substr(open + 1, name.size() - open - 2));
  }
  // The last word, after a space, a dash or an underscore.
  const auto cut = name.find_last_of(" -_");
  const std::string last = cut == std::string::npos ? name : name.substr(cut + 1);
  return positions().count(last) != 0 ? last : std::string();
}

namespace {

// The level control every layer of a rank answers to, or 0 when they differ
// or name none.
Id levelControlOf(const Rank& rank) {
  Id found = 0;
  for (const auto& pipe : rank.pipes)
    for (const auto& layer : pipe.layers) {
      if (layer.ampScalingControlId == 0 || (found != 0 && layer.ampScalingControlId != found)) return 0;
      found = layer.ampScalingControlId;
    }
  return found;
}

// A surround set gives each microphone position a level slider of its own
// (Buckeburg: "Volume Direct chan.", "Volume Diffuse chan.", "Volume Distant
// chan."), and every layer of a rank answers to one. That is the organ's own
// account of its perspectives, whatever the ranks are called. A set can give
// each division a slider too; perspectives record the SAME stops, divisions
// different ones, so most stops must draw ranks from two groups or more.
std::map<std::string, std::vector<Id>> byLevelControl(const OrganModel& model, size_t floor) {
  std::map<Id, std::vector<Id>> groups;
  for (const auto& [rankId, rank] : model.ranks)
    if (const Id c = levelControlOf(rank); c != 0) groups[c].push_back(rankId);
  // Groups too small to establish that the organ has perspectives, kept
  // aside: once the large ones have, a small one recorded for part of the
  // organ is one too (#260).
  std::map<Id, std::vector<Id>> small;
  for (auto it = groups.begin(); it != groups.end();) {
    if (it->second.size() >= floor) { ++it; continue; }
    if (it->second.size() >= 3) small.insert(*it);
    it = groups.erase(it);
  }
  if (groups.size() < 2) return {};

  std::map<Id, Id> groupOfRank;
  for (const auto& [c, ranks] : groups)
    for (const Id r : ranks) groupOfRank[r] = c;
  size_t stops = 0, spanning = 0;
  for (const auto& [stopId, stop] : model.stops) {
    std::set<Id> touched;
    for (const auto& e : stop.ranks)
      if (const auto it = groupOfRank.find(e.rankId); it != groupOfRank.end()) touched.insert(it->second);
    if (touched.empty()) continue;
    ++stops;
    if (touched.size() >= 2) ++spanning;
  }
  if (spanning * 2 <= stops) return {};

  // A small group joins when every rank in it names the same position:
  // Ashton's Rear has a slider of its own and covers the Great alone, 11
  // ranks of 211, under the tenth the large ones need. A group that names
  // no position (a noise, a division) stays out.
  for (auto& [c, ranks] : small) {
    const std::string name = perspectiveOf(model.ranks.at(ranks.front()).name);
    if (name.empty()) continue;
    bool agree = true;
    for (const Id r : ranks) agree = agree && perspectiveOf(model.ranks.at(r).name) == name;
    if (agree) groups[c] = std::move(ranks);
  }

  // Named as the rank names put it when they all agree ("rear"), so a choice
  // saved by name still applies; else by the slider.
  std::map<std::string, std::vector<Id>> named;
  for (auto& [c, ranks] : groups) {
    std::string name = perspectiveOf(model.ranks.at(ranks.front()).name);
    for (const Id r : ranks)
      if (perspectiveOf(model.ranks.at(r).name) != name) name.clear();
    if (name.empty()) {
      const auto it = model.continuousControls.find(c);
      name = it != model.continuousControls.end() && !lowerTrim(it->second.name).empty()
                 ? lowerTrim(it->second.name)
                 : "level " + std::to_string(c);
    }
    if (named.count(name) != 0) name += " (" + std::to_string(c) + ")";
    std::sort(ranks.begin(), ranks.end());
    named[name] = std::move(ranks);
  }
  return named;
}

}  // namespace

std::map<std::string, std::vector<Id>> perspectivesOf(const OrganModel& model) {
  // A perspective is a large share of the organ: a tenth of the ranks at the
  // least, and never fewer than three.
  const size_t floor = std::max<size_t>(3, model.ranks.size() / 10);
  if (auto byLevel = byLevelControl(model, floor); !byLevel.empty()) return byLevel;

  std::map<std::string, std::vector<Id>> groups;
  for (const auto& [rankId, rank] : model.ranks) {
    const std::string p = perspectiveOf(rank.name);
    if (!p.empty()) groups[p].push_back(rankId);
  }
  for (auto it = groups.begin(); it != groups.end();)
    it = it->second.size() < floor ? groups.erase(it) : std::next(it);
  if (groups.size() < 2) groups.clear();
  for (auto& [name, ids] : groups) std::sort(ids.begin(), ids.end());
  if (!groups.empty()) return groups;
  return windchestsOf(model);
}

std::map<std::string, std::vector<Id>> windchestsOf(const OrganModel& model) {
  std::map<std::string, std::vector<Id>> groups;
  for (const auto& [rankId, name] : model.rankWindchests)
    if (model.ranks.count(rankId) != 0) groups[name].push_back(rankId);
  if (groups.size() < 2) groups.clear();
  for (auto& [name, ids] : groups) std::sort(ids.begin(), ids.end());
  return groups;
}

bool groupedByWindchest(const OrganModel& model) {
  if (model.rankWindchests.empty()) return false;
  const auto groups = perspectivesOf(model);
  return !groups.empty() && groups == windchestsOf(model);
}

}  // namespace mp
