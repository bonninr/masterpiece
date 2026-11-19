// A GrandOrgue settings file (.cmb), read as GrandOrgue writes it: sections in
// order, each with its keys. Shared by the import of its MIDI events
// (GrandOrgueMidi) and of everything else it holds (GrandOrgueSettings).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp::gocmb {

inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

inline std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// The file's sections in order, each with its keys.
struct Section {
  std::string name;
  std::unordered_map<std::string, std::string> keys;

  bool has(const std::string& key) const { return keys.count(key) != 0; }
  std::string str(const std::string& key, const std::string& fallback = {}) const {
    const auto it = keys.find(key);
    return it == keys.end() ? fallback : it->second;
  }
  int num(const std::string& key, int fallback) const {
    const auto it = keys.find(key);
    if (it == keys.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    return end == it->second.c_str() ? fallback : static_cast<int>(v);
  }
  double real(const std::string& key, double fallback) const {
    const auto it = keys.find(key);
    if (it == keys.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : v;
  }
  // GrandOrgue writes Y and N; a hand-edited file may say true or 1.
  bool yes(const std::string& key, bool fallback) const {
    const auto it = keys.find(key);
    if (it == keys.end() || it->second.empty()) return fallback;
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(it->second[0])));
    return c == 'y' || c == 't' || c == '1';
  }
};

inline std::vector<Section> readSections(const std::string& text) {
  std::vector<Section> out;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first && line.rfind("\xef\xbb\xbf", 0) == 0) line.erase(0, 3);
    first = false;
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      out.push_back({line.substr(1, line.size() - 2), {}});
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos || out.empty()) continue;
    out.back().keys[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return out;
}

}  // namespace mp::gocmb
