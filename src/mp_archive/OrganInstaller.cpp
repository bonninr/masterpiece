#include "OrganInstaller.h"
#include "../mp_core/Utf8Path.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mp {

namespace fs = std::filesystem;

namespace {

constexpr const char* kDefinitions = "OrganDefinitions";
constexpr const char* kPackages = "OrganInstallationPackages";
constexpr const char* kGrandOrgue = "GrandOrgue";
// Where an install is assembled before it is moved into place, and where
// the files of the components that are neither organs nor packages are
// listed, so they can be removed again.
constexpr const char* kStaging = ".masterpiece-installing";
constexpr const char* kRecords = ".masterpiece-installed";

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::vector<std::string> split(const std::string& rel) {
  std::vector<std::string> out;
  std::string part;
  for (char c : rel) {
    if (c == '/' || c == '\\') {
      if (!part.empty()) out.push_back(part);
      part.clear();
    } else {
      part += c;
    }
  }
  if (!part.empty()) out.push_back(part);
  return out;
}

std::string join(const std::vector<std::string>& parts, size_t from = 0) {
  std::string out;
  for (size_t i = from; i < parts.size(); ++i) out += (out.empty() ? "" : "/") + parts[i];
  return out;
}

bool isOrganDefinitionName(const std::string& name) {
  return endsWith(lower(name), ".organ_hauptwerk_xml");
}

std::string readText(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

// The text of the first <tag>...</tag>, entities left as written.
std::string tagValue(const std::string& text, const std::string& tag) {
  const std::string open = "<" + tag + ">";
  const auto at = text.find(open);
  if (at == std::string::npos) return {};
  const auto from = at + open.size();
  const auto end = text.find('<', from);
  if (end == std::string::npos) return {};
  std::string v = text.substr(from, end - from);
  const auto first = v.find_first_not_of(" \t\r\n");
  const auto last = v.find_last_not_of(" \t\r\n");
  return first == std::string::npos ? std::string() : v.substr(first, last - first + 1);
}

std::string unescape(std::string s) {
  static const std::pair<const char*, const char*> entities[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
  for (const auto& [from, to] : entities)
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + 1))
      s.replace(at, std::strlen(from), to);
  return s;
}

std::string sixDigits(const std::string& digits) {
  return digits.size() >= 6 ? digits : std::string(6 - digits.size(), '0') + digits;
}

// The installation packages a definition names. The long form names the
// field, the compact one writes it as <a>.
std::vector<std::string> neededPackages(const std::string& text) {
  std::vector<std::string> out;
  const auto at = text.find("ObjectType=\"RequiredInstallationPackage\"");
  if (at == std::string::npos) return out;
  const auto end = text.find("</ObjectList>", at);
  const std::string list = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
  static const std::regex id(R"(<(?:InstallationPackageID|a)>\s*(\d{1,6})\s*<)");
  for (std::sregex_iterator it(list.begin(), list.end(), id), stop; it != stop; ++it) {
    const std::string p = sixDigits((*it)[1].str());
    // The standard components every Hauptwerk installation has.
    if (p != "000001" && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
  }
  return out;
}

void describeOrgan(InstallComponent& c, const fs::path& definition) {
  const std::string name = mp::utf8Of(definition.filename());
  c.name = name.substr(0, name.size() - std::string(".Organ_Hauptwerk_xml").size());
  const std::string text = readText(definition);
  c.version = unescape(tagValue(text, "Control_OrganVersion"));
  c.needs = neededPackages(text);
}

// A package's own definition, at the top of its folder.
void describePackage(InstallComponent& c, const fs::path& folder) {
  c.name.clear();
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), stop; !ec && it != stop; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    if (!endsWith(lower(mp::utf8Of(it->path().filename())), ".installationpackagedefinition_hauptwerk_xml"))
      continue;
    const std::string text = readText(it->path());
    c.name = unescape(tagValue(text, "Name"));
    c.supplier = unescape(tagValue(text, "SupplierName"));
    c.version = unescape(tagValue(text, "PackageVersion"));
    break;
  }
  if (c.name.empty()) c.name = c.packageId;
}

// Where an archive's file goes in the organs folder, and the component it is
// part of. The two Hauptwerk folders are written with their own names
// whatever case the archive used: a case-sensitive file system would
// otherwise hold two of each.
struct Placement {
  std::string target;     // relative to the organs folder
  std::string component;  // its InstallComponent::id
  InstallComponent::Kind kind = InstallComponent::Kind::Other;
};

Placement placeHauptwerk(const std::string& stored) {
  Placement p;
  auto parts = split(OrganArchive::placedPath(stored));
  if (parts.empty()) return p;
  const std::string top = lower(parts[0]);
  if (top == lower(kDefinitions)) parts[0] = kDefinitions;
  if (top == lower(kPackages)) parts[0] = kPackages;
  p.target = join(parts);
  if (parts[0] == kDefinitions && parts.size() == 2 && isOrganDefinitionName(parts[1])) {
    p.kind = InstallComponent::Kind::Organ;
    p.component = p.target;
  } else if (parts[0] == kPackages && parts.size() >= 3) {
    p.kind = InstallComponent::Kind::Data;
    p.component = std::string(kPackages) + "/" + parts[1];
  } else {
    p.component = parts.size() > 1 ? parts[0] : std::string();
  }
  return p;
}

// A GrandOrgue package has no Hauptwerk folders and an organ file: it is one
// organ, installed in a folder of its own named for the package.
bool isGrandOrguePackage(const OrganArchive& archive) {
  bool organ = false;
  for (const auto& e : archive.entries()) {
    const auto parts = split(OrganArchive::placedPath(e.path));
    if (parts.empty()) continue;
    const std::string top = lower(parts[0]);
    if (parts.size() > 1 && (top == lower(kDefinitions) || top == lower(kPackages))) return false;
    if (endsWith(lower(parts.back()), ".organ")) organ = true;
  }
  return organ;
}

std::string grandOrgueFolder(const OrganArchive& archive) {
  std::string name = archive.archives().empty() ? std::string("Organ") : archive.archiveName(0);
  for (const char* ext : {".orgue", ".zip", ".rar", ".7z"})
    if (endsWith(lower(name), ext)) name.resize(name.size() - std::strlen(ext));
  // "Name.part1" of a set in volumes.
  static const std::regex volume(R"((.*)\.part0*1$)", std::regex::icase);
  std::smatch m;
  if (std::regex_match(name, m, volume)) name = m[1].str();
  for (auto& c : name)
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  return std::string(kGrandOrgue) + "/" + (name.empty() ? std::string("Organ") : name);
}

std::function<Placement(const std::string&)> placerFor(const OrganArchive& archive) {
  if (isGrandOrguePackage(archive)) {
    const std::string folder = grandOrgueFolder(archive);
    return [folder](const std::string& stored) {
      Placement p;
      p.target = folder + "/" + join(split(stored));
      p.component = folder;
      p.kind = InstallComponent::Kind::Organ;
      return p;
    };
  }
  return placeHauptwerk;
}

// A GrandOrgue organ's name: its organ file, at the top or one folder down.
std::string grandOrgueName(const fs::path& folder) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(folder, ec), stop; !ec && it != stop; it.increment(ec)) {
    if (it.depth() > 1) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec) && endsWith(lower(mp::utf8Of(it->path().filename())), ".organ"))
      return mp::utf8Of(it->path().stem());
  }
  return {};
}

void measure(InstallComponent& c, const fs::path& path) {
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) {
    c.bytes = static_cast<int64_t>(fs::file_size(path, ec));
    c.files = 1;
    return;
  }
  for (fs::recursive_directory_iterator it(path, ec), stop; !ec && it != stop; it.increment(ec))
    if (it->is_regular_file(ec)) {
      c.bytes += static_cast<int64_t>(it->file_size(ec));
      ++c.files;
    }
}

std::string recordName(const std::string& id) {
  std::string out = "other-";
  for (char ch : id.empty() ? std::string("top") : id)
    out += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
  return out + ".txt";
}

std::vector<std::string> readRecord(const fs::path& file) {
  std::vector<std::string> out;
  std::istringstream in(readText(file));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

// True if `path` is strictly inside `root`.
bool inside(const fs::path& root, const fs::path& path) {
  std::error_code ec;
  const fs::path r = fs::weakly_canonical(root, ec);
  const fs::path p = fs::weakly_canonical(path, ec);
  auto ri = r.begin(), pi = p.begin();
  for (; ri != r.end(); ++ri, ++pi) {
    if (pi == p.end()) return false;
    if (ri->empty() && std::next(ri) == r.end()) break;  // a trailing separator
    if (*ri != *pi) return false;
  }
  return pi != p.end();
}

// Empty folders left behind, up to the organs folder.
void pruneEmpty(const fs::path& root, fs::path dir) {
  std::error_code ec;
  while (inside(root, dir) && fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
    fs::remove(dir, ec);
    dir = dir.parent_path();
  }
}

std::string sizeText(int64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f GB", static_cast<double>(bytes) / 1e9);
  return buf;
}

}  // namespace

std::vector<InstallComponent> packageComponents(const OrganArchive& archive, const std::string& smallFilesDir) {
  const auto place = placerFor(archive);
  std::map<std::string, InstallComponent> byId;
  for (const auto& e : archive.entries()) {
    const Placement p = place(e.path);
    if (p.target.empty()) continue;
    auto [it, fresh] = byId.try_emplace(p.component);
    InstallComponent& c = it->second;
    if (fresh) {
      c.kind = p.kind;
      c.id = p.component;
      c.name = p.component;
    }
    c.bytes += e.size;
    ++c.files;
  }
  const fs::path small = mp::pathOf(smallFilesDir);
  for (auto& [id, c] : byId) {
    if (c.kind == InstallComponent::Kind::Data) {
      c.packageId = split(id).back();
      describePackage(c, small / mp::pathOf(id));
    } else if (c.kind == InstallComponent::Kind::Organ && id.rfind(kGrandOrgue, 0) == 0) {
      c.name = split(id).back();
      for (const auto& e : archive.entries())
        if (endsWith(lower(e.path), ".organ")) {
          c.name = mp::utf8Of(mp::pathOf(split(e.path).back()).stem());
          break;
        }
    } else if (c.kind == InstallComponent::Kind::Organ) {
      describeOrgan(c, small / mp::pathOf(id));
    }
  }
  // Organs first, then their packages, then the rest: the order a player
  // reads them in.
  std::vector<InstallComponent> out;
  for (auto kind : {InstallComponent::Kind::Organ, InstallComponent::Kind::Data, InstallComponent::Kind::Other})
    for (auto& [id, c] : byId)
      if (c.kind == kind) out.push_back(c);
  return out;
}

std::vector<InstallComponent> installedComponents(const std::string& rootUtf8) {
  std::vector<InstallComponent> out;
  const fs::path root = mp::pathOf(rootUtf8);
  std::error_code ec;
  for (fs::directory_iterator it(root / kDefinitions, ec), stop; !ec && it != stop; it.increment(ec)) {
    const std::string name = mp::utf8Of(it->path().filename());
    if (!it->is_regular_file(ec) || !isOrganDefinitionName(name)) continue;
    InstallComponent c;
    c.kind = InstallComponent::Kind::Organ;
    c.id = std::string(kDefinitions) + "/" + name;
    describeOrgan(c, it->path());
    measure(c, it->path());
    out.push_back(std::move(c));
  }
  ec.clear();
  for (fs::directory_iterator it(root / kGrandOrgue, ec), stop; !ec && it != stop; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const std::string organ = grandOrgueName(it->path());
    if (organ.empty()) continue;
    InstallComponent c;
    c.kind = InstallComponent::Kind::Organ;
    c.id = std::string(kGrandOrgue) + "/" + mp::utf8Of(it->path().filename());
    c.name = organ;
    measure(c, it->path());
    out.push_back(std::move(c));
  }
  ec.clear();
  for (fs::directory_iterator it(root / kPackages, ec), stop; !ec && it != stop; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    InstallComponent c;
    c.kind = InstallComponent::Kind::Data;
    c.packageId = mp::utf8Of(it->path().filename());
    c.id = std::string(kPackages) + "/" + c.packageId;
    describePackage(c, it->path());
    measure(c, it->path());
    out.push_back(std::move(c));
  }
  ec.clear();
  for (fs::directory_iterator it(root / kRecords, ec), stop; !ec && it != stop; it.increment(ec)) {
    const auto lines = readRecord(it->path());
    if (lines.empty() || lines.front().rfind("id ", 0) != 0) continue;
    InstallComponent c;
    c.kind = InstallComponent::Kind::Other;
    c.id = lines.front().substr(3);
    c.name = c.id;
    for (size_t i = 1; i < lines.size(); ++i) {
      std::error_code fe;
      const fs::path f = root / mp::pathOf(lines[i]);
      if (fs::is_regular_file(f, fe)) {
        c.bytes += static_cast<int64_t>(fs::file_size(f, fe));
        ++c.files;
      }
    }
    if (c.files > 0) out.push_back(std::move(c));
  }
  return out;
}

int compareVersions(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty() || a == b) return 0;
  // One dot and digits: a decimal number, as packages write them ("1.02" is
  // older than "1.1").
  auto decimal = [](const std::string& s) {
    return !s.empty() && std::count(s.begin(), s.end(), '.') <= 1 &&
           std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; });
  };
  if (decimal(a) && decimal(b)) {
    const double x = std::strtod(a.c_str(), nullptr), y = std::strtod(b.c_str(), nullptr);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  auto parts = [](const std::string& s) {
    std::vector<long> out;
    std::string cur;
    for (char c : s + ".") {
      if (c == '.') {
        out.push_back(std::strtol(cur.c_str(), nullptr, 10));
        cur.clear();
      } else {
        cur += c;
      }
    }
    return out;
  };
  const auto x = parts(a), y = parts(b);
  for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
    const long u = i < x.size() ? x[i] : 0, v = i < y.size() ? y[i] : 0;
    if (u != v) return u < v ? -1 : 1;
  }
  return 0;
}

InstallState installStateOf(const InstallComponent& offered, const InstallComponent* installed) {
  if (installed == nullptr) return InstallState::New;
  const int c = compareVersions(offered.version, installed->version);
  return c > 0 ? InstallState::Newer : (c < 0 ? InstallState::Older : InstallState::Same);
}

bool installsByDefault(InstallState state) {
  return state == InstallState::New || state == InstallState::Newer;
}

int64_t installBytes(const std::vector<InstallComponent>& chosen) {
  int64_t total = 0;
  for (const auto& c : chosen) total += c.bytes;
  return total;
}

int64_t freeBytesAt(const std::string& rootUtf8) {
  fs::path p = mp::pathOf(rootUtf8);
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) {
    if (p == p.parent_path()) break;
    p = p.parent_path();
  }
  const auto space = fs::space(p, ec);
  return ec ? -1 : static_cast<int64_t>(space.available);
}

bool installComponents(const OrganArchive& archive, const std::vector<InstallComponent>& chosen,
                       const std::string& rootUtf8, std::string& error,
                       const std::function<bool(double)>& progress) {
  const fs::path root = mp::pathOf(rootUtf8);
  std::unordered_set<std::string> ids;
  for (const auto& c : chosen) ids.insert(c.id);
  const auto place = placerFor(archive);

  // What each archive is asked for, and where each file goes.
  std::vector<std::unordered_set<std::string>> wanted(archive.archives().size());
  std::vector<std::unordered_map<std::string, std::string>> targets(archive.archives().size());
  std::vector<std::string> empty;
  int64_t total = 0;
  for (const auto& e : archive.entries()) {
    const Placement p = place(e.path);
    if (p.target.empty() || !ids.count(p.component)) continue;
    if (e.encrypted) {
      error = "\"" + e.path + "\" is locked: this package needs the licence of the program it was made for.";
      return false;
    }
    if (e.size == 0) {
      empty.push_back(p.target);
      continue;
    }
    const std::string k = OrganArchive::key(e.path);
    wanted[e.archive].insert(k);
    targets[e.archive][k] = p.target;
    total += e.size;
  }

  std::error_code ec;
  fs::create_directories(root, ec);
  if (!fs::is_directory(root, ec)) {
    error = "cannot create the organs folder " + rootUtf8;
    return false;
  }
  if (const int64_t free = freeBytesAt(rootUtf8); free >= 0 && free < total) {
    error = "needs " + sizeText(total) + ", and " + sizeText(free) + " is free on that drive";
    return false;
  }
  const fs::path staging = root / kStaging;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  auto fail = [&](const std::string& why) {
    std::error_code e2;
    fs::remove_all(staging, e2);
    error = why;
    return false;
  };

  int64_t done = 0;
  bool cancelled = false;
  if (progress && !progress(0.0)) return fail("cancelled");
  for (size_t i = 0; i < wanted.size(); ++i) {
    if (wanted[i].empty()) continue;
    size_t arrived = 0;
    std::string writeError;
    std::string readError;
    const bool read = archive.read(
        i, wanted[i],
        [&](const std::string& k, std::vector<char>&& bytes) {
          const auto t = targets[i].find(k);
          if (t == targets[i].end()) return true;
          const fs::path file = staging / mp::pathOf(t->second);
          std::error_code de;
          fs::create_directories(file.parent_path(), de);
          std::ofstream out(file, std::ios::binary | std::ios::trunc);
          out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
          out.close();
          if (!out) {
            writeError = "cannot write " + mp::utf8Of(file);
            return false;
          }
          ++arrived;
          done += static_cast<int64_t>(bytes.size());
          if (progress && !progress(total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0)) {
            cancelled = true;
            return false;
          }
          return true;
        },
        readError);
    if (cancelled) return fail("cancelled");
    if (!writeError.empty()) return fail(writeError);
    if (!read || !readError.empty()) {
      while (!readError.empty() && readError.back() == '\n') readError.pop_back();
      return fail(readError.empty() ? archive.archiveName(i) + " could not be read" : readError);
    }
    if (arrived < wanted[i].size())
      return fail(archive.archiveName(i) + ": " + std::to_string(wanted[i].size() - arrived) +
                  " file(s) did not arrive; the package may be cut short. Download it again.");
  }
  for (const auto& t : empty) {
    const fs::path file = staging / mp::pathOf(t);
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary | std::ios::trunc);
  }

  // Every file read whole: into place. A folder not yet installed moves at
  // once; one already there takes the package's files over its own.
  for (const auto& c : chosen) {
    const fs::path from = staging / mp::pathOf(c.id);
    const fs::path to = root / mp::pathOf(c.id);
    if (c.id.empty() || !fs::exists(from, ec)) continue;
    if (!fs::exists(to, ec)) {
      fs::create_directories(to.parent_path(), ec);
      fs::rename(from, to, ec);
      if (!ec) continue;
      ec.clear();
    }
    if (fs::is_regular_file(from, ec)) {
      fs::remove(to, ec);
      fs::rename(from, to, ec);
      if (ec) return fail("cannot replace " + mp::utf8Of(to) + ": " + ec.message());
    }
  }
  // What is left: files over installed folders, and the files at the top.
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(staging, ec), stop; !ec && it != stop; it.increment(ec))
    if (it->is_regular_file(ec)) files.push_back(it->path());
  for (const auto& f : files) {
    const fs::path to = root / fs::relative(f, staging, ec);
    fs::create_directories(to.parent_path(), ec);
    fs::remove(to, ec);
    fs::rename(f, to, ec);
    if (ec) return fail("cannot replace " + mp::utf8Of(to) + ": " + ec.message());
  }
  fs::remove_all(staging, ec);

  // The other files are listed, so they can be removed with their component.
  for (const auto& c : chosen) {
    if (c.kind != InstallComponent::Kind::Other) continue;
    const fs::path record = root / kRecords / recordName(c.id);
    std::vector<std::string> lines = readRecord(record);
    if (lines.empty()) lines.push_back("id " + c.id);
    for (const auto& e : archive.entries()) {
      const Placement p = place(e.path);
      if (p.component == c.id && std::find(lines.begin() + 1, lines.end(), p.target) == lines.end())
        lines.push_back(p.target);
    }
    fs::create_directories(record.parent_path(), ec);
    std::ofstream out(record, std::ios::binary | std::ios::trunc);
    for (const auto& l : lines) out << l << "\n";
  }
  if (progress) progress(1.0);
  return true;
}

bool uninstallComponents(const std::string& rootUtf8, const std::vector<InstallComponent>& chosen,
                         std::string& error) {
  const fs::path root = mp::pathOf(rootUtf8);
  std::vector<std::string> failed;
  for (const auto& c : chosen) {
    std::error_code ec;
    if (c.kind == InstallComponent::Kind::Other) {
      const fs::path record = root / kRecords / recordName(c.id);
      const auto lines = readRecord(record);
      for (size_t i = 1; i < lines.size(); ++i) {
        const fs::path f = root / mp::pathOf(lines[i]);
        if (!inside(root, f)) continue;
        fs::remove(f, ec);
        if (ec) failed.push_back(mp::utf8Of(f));
        pruneEmpty(root, f.parent_path());
      }
      fs::remove(record, ec);
      pruneEmpty(root, record.parent_path());
      continue;
    }
    const fs::path target = root / mp::pathOf(c.id);
    if (c.id.empty() || !inside(root, target)) {
      failed.push_back(c.id);
      continue;
    }
    fs::remove_all(target, ec);
    if (ec || fs::exists(target)) failed.push_back(mp::utf8Of(target));
    pruneEmpty(root, target.parent_path());
  }
  if (failed.empty()) return true;
  error = "could not remove " + failed.front();
  if (failed.size() > 1) error += " and " + std::to_string(failed.size() - 1) + " more";
  return false;
}

std::vector<std::string> organsNeeding(const std::vector<InstallComponent>& installed, const std::string& packageId) {
  std::vector<std::string> out;
  for (const auto& c : installed)
    if (c.kind == InstallComponent::Kind::Organ &&
        std::find(c.needs.begin(), c.needs.end(), packageId) != c.needs.end())
      out.push_back(c.name);
  return out;
}

std::vector<std::string> installedOrganFiles(const std::string& rootUtf8) {
  std::vector<std::string> out;
  const fs::path root = mp::pathOf(rootUtf8);
  std::error_code ec;
  for (fs::directory_iterator it(root / kDefinitions, ec), stop; !ec && it != stop; it.increment(ec))
    if (it->is_regular_file(ec) && isOrganDefinitionName(mp::utf8Of(it->path().filename())))
      out.push_back(mp::utf8Of(it->path()));
  ec.clear();
  for (fs::directory_iterator it(root / kGrandOrgue, ec), stop; !ec && it != stop; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    std::error_code e2;
    for (fs::recursive_directory_iterator f(it->path(), e2), end; !e2 && f != end; f.increment(e2)) {
      if (f.depth() > 1) {
        f.disable_recursion_pending();
        continue;
      }
      if (f->is_regular_file(e2) && endsWith(lower(mp::utf8Of(f->path().filename())), ".organ")) {
        out.push_back(mp::utf8Of(f->path()));
        break;
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace mp
