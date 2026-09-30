#include "PathCase.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {
namespace {

std::string lowered(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// A folder's entries by lower-cased name, read once.
using Listing = std::unordered_map<std::string, std::filesystem::path>;

const Listing& listingOf(const std::filesystem::path& dir) {
  static std::mutex lock;
  static std::unordered_map<std::string, Listing> cache;
  std::lock_guard<std::mutex> hold(lock);
  auto [it, fresh] = cache.try_emplace(dir.string());
  if (fresh) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
      it->second.emplace(lowered(entry.path().filename().string()), entry.path());
  }
  return it->second;
}

}  // namespace

std::filesystem::path resolvePathIgnoringCase(const std::filesystem::path& wanted) {
  std::error_code ec;
  if (std::filesystem::exists(wanted, ec)) return wanted;

  // The deepest part of the path that exists as spelled...
  std::filesystem::path base = wanted;
  std::vector<std::filesystem::path> rest;
  while (!base.empty() && !std::filesystem::exists(base, ec)) {
    if (base == base.parent_path()) return wanted;
    rest.push_back(base.filename());
    base = base.parent_path();
  }
  if (base.empty()) return wanted;
  // ...then each missing part found in its folder, whatever its case.
  for (auto it = rest.rbegin(); it != rest.rend(); ++it) {
    if (!std::filesystem::is_directory(base, ec)) return wanted;
    const Listing& listing = listingOf(base);
    const auto found = listing.find(lowered(it->string()));
    if (found == listing.end()) return wanted;
    base = found->second;
  }
  return base;
}

}  // namespace mp
