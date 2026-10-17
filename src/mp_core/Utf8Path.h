#pragma once

// Paths held as UTF-8 strings, made into filesystem paths and back. On
// Windows a std::filesystem::path made from a std::string, and its string(),
// use the system's code page: a name with a letter outside it was not found
// ("Vörösbereny"), and one inside it came back in that code page and broke
// the next step that took it for UTF-8. Elsewhere both are plain copies.

#include <filesystem>
#include <string>

namespace mp {

inline std::filesystem::path pathOf(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string utf8Of(const std::filesystem::path& path) {
  const std::u8string u = path.u8string();
  return std::string(u.begin(), u.end());
}

} // namespace mp
