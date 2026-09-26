#include "OrganArchive.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>

namespace mp {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool isAudio(const std::string& path) {
  const std::string p = lower(path);
  for (const char* ext : {".wav", ".wv", ".flac", ".aif", ".aiff", ".ogg"})
    if (endsWith(p, ext)) return true;
  return false;
}

bool isDefinition(const std::string& path) {
  const std::string p = lower(path);
  return endsWith(p, ".organ_hauptwerk_xml") || endsWith(p, ".customorgan_hauptwerk_xml") ||
         endsWith(p, ".organ");
}

// Which organ a package file belongs to, from its name. Distributors number
// their packages in front ("01_", "02_"), split them into parts at the end
// ("_part01", "-002", ".2"), and add the format's own suffix; what is left is
// the organ.
std::string stemOf(const std::string& fileName) {
  std::string s = lower(fileName);
  static const std::regex volume(R"(\.part\d+\.rar$|\.r\d\d$)");
  s = std::regex_replace(s, volume, "");
  if (endsWith(s, ".rar")) s.resize(s.size() - 4);
  static const std::regex suffix(R"(\.(custom)?organ\.comppkg\.hauptwerk.*$|\.comppkg\.hauptwerk.*$)");
  s = std::regex_replace(s, suffix, "");
  static const std::regex lead(R"(^\d{1,3}[_ -])");
  s = std::regex_replace(s, lead, "");
  static const std::regex part(R"([_ -]?part\d+$|[_ -]\d{1,3}$)");
  s = std::regex_replace(s, part, "");
  return s;
}

// The volume number of a multi-volume member, or -1. "Name.part3.rar" is 3;
// the old scheme's "Name.rar" is 0 and "Name.r00" is 1.
int volumeNumber(const std::string& fileName, std::string& setName) {
  static const std::regex partN(R"(^(.*)\.part(\d+)\.rar$)", std::regex::icase);
  static const std::regex rNN(R"(^(.*)\.r(\d\d)$)", std::regex::icase);
  std::smatch m;
  if (std::regex_match(fileName, m, partN)) {
    setName = lower(m[1].str());
    return std::stoi(m[2].str());
  }
  if (std::regex_match(fileName, m, rNN)) {
    setName = lower(m[1].str());
    return std::stoi(m[2].str()) + 1;
  }
  return -1;
}

struct Reader {
  archive* a = archive_read_new();
  Reader() {
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
  }
  ~Reader() { archive_read_free(a); }
  bool open(const std::vector<std::string>& volumes) {
    std::vector<const char*> names;
    for (const auto& v : volumes) names.push_back(v.c_str());
    names.push_back(nullptr);
    return archive_read_open_filenames(a, names.data(), 1 << 20) == ARCHIVE_OK;
  }
  std::string error() const {
    const char* e = archive_error_string(a);
    return e ? e : "unreadable archive";
  }
};

bool readAll(archive* a, int64_t size, std::vector<char>& out) {
  out.clear();
  if (size > 0) out.reserve(static_cast<size_t>(size));
  char buf[1 << 16];
  for (;;) {
    const la_ssize_t n = archive_read_data(a, buf, sizeof buf);
    if (n < 0) return false;
    if (n == 0) return true;
    out.insert(out.end(), buf, buf + n);
  }
}

}  // namespace

bool isOrganArchive(const std::string& path) {
  return endsWith(lower(path), ".rar");
}

std::string OrganArchive::key(const std::string& path) {
  std::string k = lower(path);
  std::replace(k.begin(), k.end(), '\\', '/');
  while (!k.empty() && k.front() == '/') k.erase(k.begin());
  return k;
}

bool OrganArchive::open(const std::string& path, std::string& error) {
  return discover(path, error) && index(error);
}

bool OrganArchive::discover(const std::string& path, std::string& error) {
  archives_.clear();
  entries_.clear();
  byKey_.clear();

  const fs::path chosen(path);
  const fs::path dir = chosen.parent_path();
  const std::string stem = stemOf(chosen.filename().string());

  // The siblings: every archive in the folder with the same organ stem.
  // Members of one multi-volume set become a single archive.
  std::map<std::string, std::map<int, std::string>> volumeSets;
  std::vector<std::string> singles;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir.empty() ? fs::path(".") : dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = e.path().filename().string();
    std::string setName;
    const int vol = volumeNumber(name, setName);
    if (vol < 0 && !isOrganArchive(name)) continue;
    if (stemOf(name) != stem) continue;
    if (vol >= 0) volumeSets[setName][vol] = e.path().string();
    else singles.push_back(e.path().string());
  }
  // The old scheme names its first volume plainly ("Name.rar"), which is
  // also how a single archive is named: it belongs to the set if one exists.
  for (auto it = singles.begin(); it != singles.end();) {
    const std::string base = lower(fs::path(*it).stem().string());
    const auto set = volumeSets.find(base);
    if (set != volumeSets.end() && set->second.count(0) == 0) {
      set->second[0] = *it;
      it = singles.erase(it);
    } else {
      ++it;
    }
  }
  std::sort(singles.begin(), singles.end());
  for (const auto& s : singles) archives_.push_back({s});
  for (const auto& [name, vols] : volumeSets) {
    std::vector<std::string> v;
    for (const auto& [n, file] : vols) v.push_back(file);
    archives_.push_back(v);
  }
  if (archives_.empty()) {
    error = "no archive found at " + path;
    return false;
  }
  return true;
}

bool OrganArchive::index(std::string& error) {
  entries_.clear();
  byKey_.clear();

  // Headers only. Skipping a non-solid entry is a seek; a solid archive has
  // to decompress what it skips, which is the price of indexing one.
  for (size_t i = 0; i < archives_.size(); ++i) {
    Reader r;
    if (!r.open(archives_[i])) {
      error = archives_[i].front() + ": " + r.error();
      return false;
    }
    archive_entry* entry = nullptr;
    int rc;
    while ((rc = archive_read_next_header(r.a, &entry)) == ARCHIVE_OK) {
      if (archive_entry_filetype(entry) != AE_IFREG) continue;
      const char* name = archive_entry_pathname_utf8(entry);
      if (name == nullptr) name = archive_entry_pathname(entry);
      if (name == nullptr) continue;
      Entry e;
      e.archive = i;
      e.path = name;
      std::replace(e.path.begin(), e.path.end(), '\\', '/');
      e.size = archive_entry_size(entry);
      e.encrypted = archive_entry_is_encrypted(entry) != 0;
      // The first copy wins: a package re-downloaded under another name
      // lists the same files again.
      const std::string k = key(e.path);
      if (byKey_.count(k) == 0) {
        byKey_[k] = entries_.size();
        entries_.push_back(std::move(e));
      }
      archive_read_data_skip(r.a);
    }
    if (rc != ARCHIVE_EOF) {
      error = archives_[i].front() + ": " + r.error();
      return false;
    }
  }
  return true;
}

bool OrganArchive::saveIndex(const std::string& file) const {
  std::ofstream out(fs::u8path(file), std::ios::binary | std::ios::trunc);
  for (const auto& vols : archives_) {
    out << "A\n";
    for (const auto& v : vols) {
      std::error_code ec;
      out << "V\t" << fs::file_size(fs::u8path(v), ec) << "\t" << v << "\n";
    }
  }
  for (const auto& e : entries_)
    out << "E\t" << e.archive << "\t" << e.size << "\t" << (e.encrypted ? 1 : 0) << "\t"
        << e.path << "\n";
  return static_cast<bool>(out);
}

bool OrganArchive::loadIndex(const std::string& file) {
  std::ifstream in(fs::u8path(file), std::ios::binary);
  if (!in) return false;
  std::vector<std::vector<std::string>> archives;
  std::vector<Entry> entries;
  std::string line;
  // Fields are tab-separated; the path is last, so it may hold anything else.
  auto field = [&line](size_t& p) {
    const size_t q = line.find('\t', p);
    const std::string f = line.substr(p, q == std::string::npos ? std::string::npos : q - p);
    p = q == std::string::npos ? line.size() : q + 1;
    return f;
  };
  try {
    while (std::getline(in, line)) {
      if (line == "A") {
        archives.emplace_back();
      } else if (line.rfind("V\t", 0) == 0 && !archives.empty()) {
        size_t p = 2;
        const auto size = std::stoull(field(p));
        const std::string v = line.substr(p);
        std::error_code ec;
        if (fs::file_size(fs::u8path(v), ec) != size || ec) return false;
        archives.back().push_back(v);
      } else if (line.rfind("E\t", 0) == 0) {
        size_t p = 2;
        Entry e;
        e.archive = std::stoul(field(p));
        e.size = std::stoll(field(p));
        e.encrypted = field(p) == "1";
        e.path = line.substr(p);
        if (e.archive >= archives.size()) return false;
        entries.push_back(std::move(e));
      }
    }
  } catch (...) {
    return false;
  }
  if (archives.empty() || entries.empty()) return false;
  archives_ = std::move(archives);
  entries_ = std::move(entries);
  byKey_.clear();
  for (size_t i = 0; i < entries_.size(); ++i) byKey_.emplace(key(entries_[i].path), i);
  return true;
}

std::vector<std::string> OrganArchive::definitions() const {
  std::vector<std::string> out;
  for (const auto& e : entries_)
    if (isDefinition(e.path)) out.push_back(e.path);
  std::sort(out.begin(), out.end());
  return out;
}

const OrganArchive::Entry* OrganArchive::find(const std::string& relativePath) const {
  const auto it = byKey_.find(key(relativePath));
  return it == byKey_.end() ? nullptr : &entries_[it->second];
}

bool OrganArchive::unpackSmallFiles(const std::string& dir, std::string& error) const {
  for (size_t i = 0; i < archives_.size(); ++i) {
    // Nothing to take from an archive that is all audio.
    bool any = false;
    for (const auto& e : entries_)
      if (e.archive == i && !isAudio(e.path)) any = true;
    if (!any) continue;

    Reader r;
    if (!r.open(archives_[i])) {
      error = archives_[i].front() + ": " + r.error();
      return false;
    }
    archive_entry* entry = nullptr;
    std::vector<char> bytes;
    while (archive_read_next_header(r.a, &entry) == ARCHIVE_OK) {
      const char* name = archive_entry_pathname_utf8(entry);
      if (name == nullptr) name = archive_entry_pathname(entry);
      if (name == nullptr || archive_entry_filetype(entry) != AE_IFREG) continue;
      std::string rel = name;
      std::replace(rel.begin(), rel.end(), '\\', '/');
      if (isAudio(rel) || archive_entry_is_encrypted(entry)) {
        archive_read_data_skip(r.a);
        continue;
      }
      // A Hauptwerk definition belongs in OrganDefinitions/, beside the
      // packages, whatever the archive did with it. A GrandOrgue one stays
      // put: its paths are relative to its own folder.
      if (lower(rel).find("_hauptwerk_xml") != std::string::npos && rel.find('/') == std::string::npos)
        rel = "OrganDefinitions/" + rel;
      const fs::path target = fs::u8path(dir) / fs::u8path(rel);
      std::error_code ec;
      fs::create_directories(target.parent_path(), ec);
      if (!readAll(r.a, archive_entry_size(entry), bytes)) {
        error = rel + ": " + r.error();
        return false;
      }
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      if (!out) {
        error = "cannot write " + target.string();
        return false;
      }
    }
  }
  return true;
}

bool OrganArchive::read(size_t archiveIndex, const std::unordered_set<std::string>& wanted,
                        const Deliver& deliver, std::string& error) const {
  if (archiveIndex >= archives_.size()) return false;
  size_t remaining = 0;
  for (const auto& e : entries_)
    if (e.archive == archiveIndex && wanted.count(key(e.path))) ++remaining;
  if (remaining == 0) return true;

  Reader r;
  if (!r.open(archives_[archiveIndex])) {
    error = archives_[archiveIndex].front() + ": " + r.error();
    return false;
  }
  archive_entry* entry = nullptr;
  std::vector<char> bytes;
  while (remaining > 0 && archive_read_next_header(r.a, &entry) == ARCHIVE_OK) {
    const char* name = archive_entry_pathname_utf8(entry);
    if (name == nullptr) name = archive_entry_pathname(entry);
    if (name == nullptr || archive_entry_filetype(entry) != AE_IFREG) continue;
    const std::string k = key(name);
    if (!wanted.count(k)) {
      archive_read_data_skip(r.a);
      continue;
    }
    --remaining;
    if (!readAll(r.a, archive_entry_size(entry), bytes)) {
      error = std::string(name) + ": " + r.error();
      return false;
    }
    if (!deliver(k, std::move(bytes))) return true;
    bytes = {};
  }
  return true;
}

std::string OrganArchive::identity() const {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const std::string& s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 1099511628211ull;
    }
  };
  for (const auto& vols : archives_)
    for (const auto& v : vols) {
      mix(lower(fs::path(v).filename().string()));
      std::error_code ec;
      mix(std::to_string(fs::file_size(v, ec)));
    }
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

bool writeArchiveMarker(const std::string& dir, const std::string& archivePath) {
  std::ofstream out(fs::u8path(dir) / "masterpiece-archive.txt", std::ios::trunc);
  out << archivePath << "\n";
  return static_cast<bool>(out);
}

std::string readArchiveMarker(const std::string& organRoot) {
  std::ifstream in(fs::u8path(organRoot) / "masterpiece-archive.txt");
  std::string line;
  std::getline(in, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  return line;
}

}  // namespace mp
