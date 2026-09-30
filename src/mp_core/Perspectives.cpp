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
  if (!name.empty() && name.back() == ')') {
    const auto open = name.rfind('(');
    if (open != std::string::npos) return lowerTrim(name.substr(open + 1, name.size() - open - 2));
  }
  // The last word, after a space, a dash or an underscore.
  const auto cut = name.find_last_of(" -_");
  const std::string last = cut == std::string::npos ? name : name.substr(cut + 1);
  return positions().count(last) != 0 ? last : std::string();
}

std::map<std::string, std::vector<Id>> perspectivesOf(const OrganModel& model) {
  std::map<std::string, std::vector<Id>> groups;
  for (const auto& [rankId, rank] : model.ranks) {
    const std::string p = perspectiveOf(rank.name);
    if (!p.empty()) groups[p].push_back(rankId);
  }
  // A perspective is a large share of the organ: a tenth of the ranks at the
  // least, and never fewer than three.
  const size_t floor = std::max<size_t>(3, model.ranks.size() / 10);
  for (auto it = groups.begin(); it != groups.end();)
    it = it->second.size() < floor ? groups.erase(it) : std::next(it);
  if (groups.size() < 2) groups.clear();
  for (auto& [name, ids] : groups) std::sort(ids.begin(), ids.end());
  return groups;
}

}  // namespace mp
