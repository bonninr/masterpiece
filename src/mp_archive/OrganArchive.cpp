#include "OrganArchive.h"

#include <archive.h>
#include <archive_entry.h>
#include <unarr.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <map>
#include <memory>
#include <regex>
#include <set>

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

// The name a second download was saved beside: a browser's "Name (1).rar",
// a download manager's "Name.1.rar" -- and "Name.1.r06" for a volume of a
// split set, which is how Bückeburg's arrived. Empty for a name that is not
// one. Only a guess from the name: whether the original is really there is
// for the caller to check.
std::string originalOf(const std::string& fileName) {
  static const std::regex browser(R"(^(.*) \(\d+\)(\.[^. ]+)$)");
  static const std::regex numbered(R"(^(.*)\.\d+(\.rar|\.r\d\d)$)", std::regex::icase);
  std::smatch m;
  if (std::regex_match(fileName, m, browser)) return m[1].str() + m[2].str();
  if (std::regex_match(fileName, m, numbered)) return m[1].str() + m[2].str();
  return "";
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

uint32_t crc32Of(const unsigned char* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// Where a RAR 4 volume's archive really ends: just past its end-of-archive
// block. A volume can be padded to its full size after that block -- St.
// Maximin's first volume has 18 zero bytes there -- and libarchive, taking
// the volumes as one stream, then meets the padding where the next volume's
// marker should be and stops with "Bad RAR file". The block is found from the
// end and trusted only if its header's CRC is right; otherwise the whole file
// is used, as before.
int64_t rarVolumeEnd(const std::string& path) {
  std::error_code ec;
  const auto size = static_cast<int64_t>(fs::file_size(fs::u8path(path), ec));
  if (ec || size < 64) return size;
  const int64_t tail = std::min<int64_t>(size, 64 * 1024);
  std::ifstream in(fs::u8path(path), std::ios::binary);
  in.seekg(size - tail);
  std::vector<unsigned char> b(static_cast<size_t>(tail));
  in.read(reinterpret_cast<char*>(b.data()), tail);
  if (in.gcount() != tail) return size;
  size_t zeros = b.size();
  while (zeros > 0 && b[zeros - 1] == 0) --zeros;
  if (zeros == b.size()) return size;  // no padding: nothing to cut
  // The block may end in zeros of its own (a volume number of 0, reserved
  // space), so its end lies at or past where the padding appears to begin.
  const size_t lowest = zeros > 256 ? zeros - 256 : 0;
  for (size_t i = zeros; i-- > lowest;) {
    if (i + 7 > b.size() || b[i + 2] != 0x7b) continue;
    const size_t hsize = b[i + 5] | (b[i + 6] << 8);
    const size_t end = i + hsize;
    if (hsize < 7 || end < zeros || end > b.size()) continue;
    const uint32_t crc = crc32Of(&b[i + 2], hsize - 2) & 0xffff;
    if (crc != static_cast<uint32_t>(b[i] | (b[i + 1] << 8))) continue;
    return size - tail + static_cast<int64_t>(end);
  }
  return size;
}

// The volumes of a set, one after another, each only as far as its
// end-of-archive block: what libarchive expects when it reads a set as one
// stream.
struct VolumeStream {
  std::vector<std::string> paths;
  std::vector<int64_t> ends;
  size_t index = 0;
  FILE* file = nullptr;
  int64_t pos = 0;
  std::vector<char> buffer = std::vector<char>(1 << 20);

  bool openCurrent() {
    if (file != nullptr) std::fclose(file);
    file = nullptr;
    pos = 0;
    if (index >= paths.size()) return false;
#ifdef _WIN32
    file = _wfopen(fs::u8path(paths[index]).wstring().c_str(), L"rb");
#else
    file = std::fopen(paths[index].c_str(), "rb");
#endif
    return file != nullptr;
  }
  static la_ssize_t read(archive*, void* data, const void** out) {
    auto* s = static_cast<VolumeStream*>(data);
    for (;;) {
      if (s->file == nullptr && !s->openCurrent()) return 0;
      const int64_t left = s->ends[s->index] - s->pos;
      if (left <= 0) {
        std::fclose(s->file);
        s->file = nullptr;
        ++s->index;
        continue;
      }
      const size_t want = static_cast<size_t>(std::min<int64_t>(left, static_cast<int64_t>(s->buffer.size())));
      const size_t got = std::fread(s->buffer.data(), 1, want, s->file);
      if (got == 0) return -1;
      s->pos += static_cast<int64_t>(got);
      *out = s->buffer.data();
      return static_cast<la_ssize_t>(got);
    }
  }
  // Forward only, within the current volume; libarchive reads the rest.
  static la_int64_t skip(archive*, void* data, la_int64_t request) {
    auto* s = static_cast<VolumeStream*>(data);
    if (s->file == nullptr && !s->openCurrent()) return 0;
    const int64_t n = std::min<int64_t>(request, s->ends[s->index] - s->pos);
    if (n <= 0) return 0;
#ifdef _WIN32
    if (_fseeki64(s->file, n, SEEK_CUR) != 0) return 0;
#else
    if (fseeko(s->file, static_cast<off_t>(n), SEEK_CUR) != 0) return 0;
#endif
    s->pos += n;
    return n;
  }
  static int close(archive*, void* data) {
    auto* s = static_cast<VolumeStream*>(data);
    if (s->file != nullptr) std::fclose(s->file);
    s->file = nullptr;
    return ARCHIVE_OK;
  }
};

struct Reader {
  archive* a = archive_read_new();
  std::unique_ptr<VolumeStream> volumes_;
  Reader() {
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    // GrandOrgue's .orgue packages: ZIP, every file stored uncompressed, so
    // no codec is needed to read them.
    archive_read_support_format_zip(a);
  }
  ~Reader() { archive_read_free(a); }
  bool open(const std::vector<std::string>& volumes) {
    if (volumes.size() > 1) {
      volumes_ = std::make_unique<VolumeStream>();
      volumes_->paths = volumes;
      for (const auto& v : volumes) volumes_->ends.push_back(rarVolumeEnd(v));
      archive_read_set_read_callback(a, &VolumeStream::read);
      archive_read_set_skip_callback(a, &VolumeStream::skip);
      archive_read_set_close_callback(a, &VolumeStream::close);
      archive_read_set_callback_data(a, volumes_.get());
      return archive_read_open1(a) == ARCHIVE_OK;
    }
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

// One walk over an archive's files, front to back -- the only way RAR can be
// read -- whichever library does the reading. Two do: libarchive for RAR 5,
// ZIP and RAR 4 that is not solid; unarr for solid RAR 4, which libarchive
// refuses, and which is most of what Hauptwerk sets ship as.
class Pass {
public:
  virtual ~Pass() = default;
  // The next file (folders are passed over); false at the end, or where
  // reading stopped -- failed() says which.
  virtual bool next(std::string& name, int64_t& size, bool& encrypted) = 0;
  virtual bool readData(std::vector<char>& out) = 0;
  // Pass the file by in a walk that decompresses nothing: the index.
  virtual void skip() = 0;
  // Pass it by in a walk that goes on to read later files. A solid archive
  // is one stream: its files have to be decompressed anyway, in order, or
  // the next one has no state to start from.
  virtual bool discard() = 0;
  virtual bool failed() const = 0;
  virtual std::string error() const = 0;
};

class LibarchivePass final : public Pass {
public:
  explicit LibarchivePass(const std::vector<std::string>& volumes) : opened_(r_.open(volumes)) {}
  bool next(std::string& name, int64_t& size, bool& encrypted) override {
    if (!opened_) return false;
    archive_entry* entry = nullptr;
    while ((rc_ = archive_read_next_header(r_.a, &entry)) == ARCHIVE_OK) {
      if (archive_entry_filetype(entry) != AE_IFREG) continue;
      const char* n = archive_entry_pathname_utf8(entry);
      if (n == nullptr) n = archive_entry_pathname(entry);
      if (n == nullptr) continue;
      name = n;
      size = archive_entry_size(entry);
      encrypted = archive_entry_is_encrypted(entry) != 0;
      return true;
    }
    return false;
  }
  bool readData(std::vector<char>& out) override { return readAll(r_.a, -1, out); }
  void skip() override { archive_read_data_skip(r_.a); }
  bool discard() override { return archive_read_data_skip(r_.a) == ARCHIVE_OK; }
  bool failed() const override { return !opened_ || rc_ != ARCHIVE_EOF; }
  std::string error() const override { return r_.error(); }

private:
  Reader r_;
  bool opened_ = false;
  int rc_ = ARCHIVE_OK;
};

class UnarrPass final : public Pass {
public:
  explicit UnarrPass(const std::string& path) {
#ifdef _WIN32
    stream_ = ar_open_file_w(fs::u8path(path).wstring().c_str());
#else
    stream_ = ar_open_file(path.c_str());
#endif
    if (stream_ != nullptr) ar_ = ar_open_rar_archive(stream_);
    if (ar_ == nullptr) error_ = "the solid RAR 4 reader cannot open it";
  }
  ~UnarrPass() override {
    if (ar_ != nullptr) ar_close_archive(ar_);
    if (stream_ != nullptr) ar_close(stream_);
  }
  bool next(std::string& name, int64_t& size, bool& encrypted) override {
    if (ar_ == nullptr) return false;
    if (!ar_parse_entry(ar_)) {
      end_ = ar_at_eof(ar_);
      if (!end_) error_ = "its list of files stops short (damaged, or not all downloaded)";
      return false;
    }
    const char* n = ar_entry_get_name(ar_);
    name = n != nullptr ? n : "";
    size_ = ar_entry_get_size(ar_);
    size = static_cast<int64_t>(size_);
    encrypted = false;  // unarr opens no encrypted RAR; inspect() refuses them first
    return true;
  }
  bool readData(std::vector<char>& out) override {
    out.resize(size_);
    if (size_ > 0 && !ar_entry_uncompress(ar_, out.data(), size_)) {
      error_ = "its data does not decompress (damaged, or not all downloaded)";
      return false;
    }
    return true;
  }
  void skip() override {}
  bool discard() override {
    scratch_.resize(1 << 20);
    for (size_t left = size_; left > 0;) {
      const size_t n = (std::min)(left, scratch_.size());
      if (!ar_entry_uncompress(ar_, scratch_.data(), n)) {
        error_ = "its data does not decompress (damaged, or not all downloaded)";
        return false;
      }
      left -= n;
    }
    return true;
  }
  bool failed() const override { return ar_ == nullptr || !end_; }
  std::string error() const override { return error_; }

private:
  ar_stream* stream_ = nullptr;
  ar_archive* ar_ = nullptr;
  size_t size_ = 0;
  bool end_ = false;
  std::string error_;
  std::vector<char> scratch_;
};

}  // namespace

// Which reader an archive needs, from its first header.
static bool needsUnarr(const std::vector<std::string>& volumes) {
  if (volumes.size() != 1) return false;
  const ArchiveKind k = inspectArchive(volumes.front());
  return k.format == ArchiveKind::Format::Rar4 && k.solid;
}

static std::unique_ptr<Pass> passFor(const std::vector<std::string>& volumes) {
  if (needsUnarr(volumes)) return std::make_unique<UnarrPass>(volumes.front());
  return std::make_unique<LibarchivePass>(volumes);
}

namespace {

}  // namespace

bool isOrganArchive(const std::string& path) {
  const std::string p = lower(path);
  return endsWith(p, ".rar") || endsWith(p, ".orgue");
}

namespace {

uint64_t readVint(const std::vector<unsigned char>& b, size_t& i) {
  uint64_t v = 0;
  for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
    const unsigned char c = b[i++];
    v |= static_cast<uint64_t>(c & 0x7f) << shift;
    if ((c & 0x80) == 0) break;
  }
  return v;
}

std::string gigabytes(int64_t bytes) {
  char buf[32];
  if (bytes >= (int64_t{1} << 30))
    std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / (1 << 30));
  else
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1 << 20));
  return buf;
}

}  // namespace

// The marker, then the archive's own header. RAR 4 keeps its flags in the
// main header's 16-bit field; RAR 5 in variable-length integers, and puts an
// encryption header first when even the file list is locked.
ArchiveKind inspectArchive(const std::string& path) {
  ArchiveKind k;
  std::error_code ec;
  k.bytes = static_cast<int64_t>(fs::file_size(path, ec));
  std::ifstream in(path, std::ios::binary);
  std::vector<unsigned char> b(1 << 16);
  in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
  b.resize(static_cast<size_t>(in.gcount()));
  if (b.size() >= 4 && b[0] == 'P' && b[1] == 'K') {
    k.format = ArchiveKind::Format::Zip;
    return k;
  }
  // A self-extracting archive has a program in front; look a little way in.
  static const unsigned char mark[] = {'R', 'a', 'r', '!', 0x1a, 0x07};
  const auto at = std::search(b.begin(), b.end(), std::begin(mark), std::end(mark));
  if (at == b.end()) return k;
  size_t i = static_cast<size_t>(at - b.begin()) + 6;
  if (i >= b.size()) return k;
  if (b[i] == 0x00) {
    k.format = ArchiveKind::Format::Rar4;
    i += 1;  // end of the 7-byte marker
    if (i + 7 > b.size() || b[i + 2] != 0x73) return k;
    const unsigned flags = b[i + 3] | (b[i + 4] << 8);
    k.multiVolume = (flags & 0x0001) != 0;
    k.solid = (flags & 0x0008) != 0;
    k.encryptedHeaders = (flags & 0x0080) != 0;
    k.firstVolume = (flags & 0x0100) != 0;
    const size_t size = b[i + 5] | (b[i + 6] << 8);
    const size_t f = i + size;
    if (f + 7 <= b.size() && b[f + 2] == 0x74) {
      const unsigned fileFlags = b[f + 3] | (b[f + 4] << 8);
      k.encryptedFiles = (fileFlags & 0x0004) != 0;
    }
    return k;
  }
  if (b[i] == 0x01 && i + 1 < b.size() && b[i + 1] == 0x00) {
    k.format = ArchiveKind::Format::Rar5;
    i += 2;
    for (int header = 0; header < 3 && i + 4 < b.size(); ++header) {
      i += 4;  // CRC
      const uint64_t size = readVint(b, i);
      const size_t start = i;
      const uint64_t type = readVint(b, i);
      const uint64_t hflags = readVint(b, i);
      if (hflags & 1) readVint(b, i);
      if (hflags & 2) readVint(b, i);
      if (type == 4) {
        k.encryptedHeaders = true;
        break;
      }
      if (type == 1) {
        const uint64_t aflags = readVint(b, i);
        k.multiVolume = (aflags & 0x1) != 0;
        k.solid = (aflags & 0x4) != 0;
        // RAR 5 numbers its volumes; the first carries no number.
        k.firstVolume = k.multiVolume && (aflags & 0x2) == 0;
      }
      i = start + static_cast<size_t>(size);
    }
  }
  return k;
}

std::string ArchiveKind::describe() const {
  std::string s = format == Format::Rar4   ? "RAR 4"
                  : format == Format::Rar5 ? "RAR 5"
                  : format == Format::Zip  ? "ZIP"
                                           : "not an archive Masterpiece knows";
  if (solid) s += ", solid";
  if (multiVolume) s += firstVolume ? ", first volume of a set" : ", a volume of a set";
  if (encryptedHeaders || encryptedFiles) s += ", password-protected";
  return s + ", " + gigabytes(bytes);
}

namespace {

// One file's bytes, from an archive, by name. For the small index a package
// carries about itself.
bool readOne(const std::vector<std::string>& volumes, const std::string& wantedKey,
             std::vector<char>& out) {
  auto pass = passFor(volumes);
  std::string name;
  int64_t size = 0;
  bool encrypted = false;
  while (pass->next(name, size, encrypted)) {
    if (OrganArchive::key(name) == wantedKey) return pass->readData(out);
    if (!pass->discard()) return false;
  }
  return false;
}

std::vector<std::string> orgueDependencies(const std::string& path) {
  std::vector<char> bytes;
  std::vector<std::string> ids;
  if (!readOne({path}, "organindex.ini", bytes)) return ids;
  std::string section;
  size_t i = 0;
  const std::string text(bytes.begin(), bytes.end());
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    i = e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty() && line.front() == '[') {
      section = lower(line);
    } else if (section.rfind("[dependency", 0) == 0 && lower(line).rfind("packageid=", 0) == 0) {
      ids.push_back(lower(line.substr(10)));
    }
  }
  return ids;
}

}  // namespace

std::string OrganArchive::key(const std::string& path) {
  std::string k = lower(path);
  std::replace(k.begin(), k.end(), '\\', '/');
  while (!k.empty() && k.front() == '/') k.erase(k.begin());
  return k;
}

bool OrganArchive::open(const std::string& path, std::string& error) {
  return discover(path, error) && inspect(error) && index(error);
}

bool OrganArchive::discover(const std::string& path, std::string& error) {
  archives_.clear();
  entries_.clear();
  byKey_.clear();

  const fs::path chosen(path);
  const fs::path dir = chosen.parent_path();
  const std::string stem = stemOf(chosen.filename().string());

  // A GrandOrgue package names what else it needs by package id, and a
  // package is published with that id in its file name ("demo-4232D4....
  // orgue"). Its dependencies are the packages beside it that carry them.
  if (endsWith(lower(path), ".orgue")) {
    archives_.push_back({path});
    std::error_code ec;
    for (const auto& id : orgueDependencies(path)) {
      bool found = false;
      for (const auto& e : fs::directory_iterator(dir.empty() ? fs::path(".") : dir, ec)) {
        const std::string name = lower(e.path().filename().string());
        if (endsWith(name, ".orgue") && name.find(id) != std::string::npos &&
            e.path() != chosen) {
          archives_.push_back({e.path().string()});
          found = true;
          break;
        }
      }
      if (!found) {
        error = "the package needs another package, id " + id +
                ", which is not beside it";
        return false;
      }
    }
    return true;
  }

  // The siblings: every archive in the folder with the same organ stem.
  // Members of one multi-volume set become a single archive.
  std::map<std::string, std::map<int, std::string>> volumeSets;
  std::vector<std::string> singles;
  std::vector<std::string> renamed;  // "(1)" copies with no original beside them
  report_.clear();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir.empty() ? fs::path(".") : dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string name = e.path().filename().string();
    // A second download of a file that is here already. Counted as another
    // package it would be read twice at best; as part of a volume set it
    // breaks the set -- the commonest reason a multi-part package will not
    // open (Sonus Paradisi's notes on it say the same).
    if (const std::string original = originalOf(name); !original.empty()) {
      if (fs::exists(e.path().parent_path() / original, ec)) {
        if (stemOf(original) == stem)
          report_.push_back("ignored " + name + ": a second download of " + original);
        continue;
      }
      if (stemOf(original) == stem) renamed.push_back(name);
    }
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
    // Every volume from the first to the last, or the set cannot be read:
    // name what is missing rather than letting the reader stop in the middle
    // with "truncated data". The part scheme counts from 1, the old one from
    // the plain .rar (0).
    const bool partScheme =
        lower(fs::path(vols.rbegin()->second).filename().string()).find(".part") != std::string::npos;
    const int first = partScheme ? 1 : 0;
    std::vector<int> missing;
    for (int n = first; n <= vols.rbegin()->first; ++n)
      if (vols.count(n) == 0) missing.push_back(n);
    if (!missing.empty()) {
      std::string list;
      for (int n : missing) {
        if (!list.empty()) list += ", ";
        if (partScheme) list += "part " + std::to_string(n);
        else if (n == 0) list += "the first volume (" + name + ".rar)";
        else {
          char suffix[8];
          std::snprintf(suffix, sizeof suffix, ".r%02d", n - 1);
          list += suffix;
        }
      }
      error = "\"" + fs::path(vols.begin()->second).filename().string() +
              "\" is one volume of a multi-volume set, and " + list +
              (missing.size() == 1 ? " is" : " are") +
              " not in the same folder. Put every volume of the set beside it and open it again.";
      if (!renamed.empty())
        error += " \"" + renamed.front() + "\"" +
                 (renamed.size() > 1 ? " and " + std::to_string(renamed.size() - 1) + " more look" : " looks") +
                 " like a download the browser renamed, adding a number: give it the name "
                 "without the number (if the original is not there) or delete it.";
      return false;
    }
    std::vector<std::string> v;
    for (const auto& [n, file] : vols) v.push_back(file);
    archives_.push_back(v);
  }
  if (archives_.empty()) {
    error = "no archive found at " + path;
    return false;
  }

  // A download still in progress has the downloader's own file beside it.
  // The archive is there, the right size on disk, and cut off at the end.
  for (const auto& group : archives_)
    for (const auto& file : group)
      for (const char* partial : {".crdownload", ".part", ".download", ".opdownload"})
        if (fs::exists(file + partial, ec)) {
          error = "\"" + fs::path(file).filename().string() +
                  "\" is still downloading (" + fs::path(file + partial).filename().string() +
                  " is beside it). Open it again once the download has finished.";
          return false;
        }
  return true;
}

// What each one is, before a byte of it is decompressed: the reasons an
// archive cannot be read are mostly in its first header.
bool OrganArchive::inspect(std::string& error) {
  for (const auto& group : archives_) {
    const ArchiveKind kind = inspectArchive(group.front());
    const std::string name = fs::path(group.front()).filename().string();
    std::string line = name + " -- " + kind.describe();
    if (group.size() > 1) line += ", " + std::to_string(group.size()) + " volumes";
    report_.push_back(line);
    if (kind.format == ArchiveKind::Format::Unknown) {
      error = "\"" + name + "\" is not a RAR or ZIP archive that Masterpiece can read.";
      return false;
    }
    if (kind.encryptedHeaders || kind.encryptedFiles) {
      error = "\"" + name + "\" is password-protected. Masterpiece cannot open "
              "password-protected archives yet: extract it with the password (7-Zip or "
              "WinRAR) and open the organ definition file inside instead.";
      return false;
    }
    if (kind.format == ArchiveKind::Format::Rar4 && kind.solid && group.size() > 1) {
      error = "\"" + name + "\" is a solid RAR 4 archive split into volumes, which "
              "Masterpiece cannot read directly yet. Extract it with 7-Zip or WinRAR and open "
              "the organ definition file inside instead (OrganDefinitions/*.Organ_Hauptwerk_xml).";
      return false;
    }
    if (kind.format == ArchiveKind::Format::Rar4 && kind.solid)
      report_.back() += "; read with the solid RAR 4 reader";
    if (kind.multiVolume && group.size() == 1) {
      error = "\"" + name + "\" is one volume of a multi-volume set (its header says so), "
              "but the other volumes are not beside it. Put every volume of the set in the "
              "same folder and open it again.";
      return false;
    }
    if (kind.multiVolume && !kind.firstVolume && kind.format == ArchiveKind::Format::Rar4) {
      error = "\"" + name + "\" is not the first volume of its set, and the first "
              "volume is not beside it.";
      return false;
    }
  }
  return true;
}

bool OrganArchive::index(std::string& error) {
  entries_.clear();
  byKey_.clear();

  // Headers only. libarchive skips a non-solid file by seeking, and has to
  // decompress what it skips in a solid RAR 5; unarr lists a solid RAR 4
  // without decompressing anything -- Nancy's 15,577 files in about a second.
  for (size_t i = 0; i < archives_.size(); ++i) {
    auto pass = passFor(archives_[i]);
    std::string name, last;
    int64_t size = 0;
    bool encrypted = false;
    size_t count = 0, locked = 0;
    while (pass->next(name, size, encrypted)) {
      last = name;
      ++count;
      if (encrypted) ++locked;
      Entry e;
      e.archive = i;
      e.path = name;
      std::replace(e.path.begin(), e.path.end(), '\\', '/');
      e.size = size;
      e.encrypted = encrypted;
      // The first copy wins: a package re-downloaded under another name
      // lists the same files again.
      const std::string k = key(e.path);
      if (byKey_.count(k) == 0) {
        byKey_[k] = entries_.size();
        entries_.push_back(std::move(e));
      }
      pass->skip();
    }
    const std::string file = fs::path(archives_[i].front()).filename().string();
    if (pass->failed()) {
      error = "\"" + file + "\" could not be read: " + pass->error() + " (" +
              (last.empty() ? std::string("before its first file")
                            : "after " + std::to_string(count) + " files, the last \"" + last + "\"") +
              ").";
      return false;
    }
    report_.push_back(file + " -- " + std::to_string(count) + " files" +
                      (locked ? ", " + std::to_string(locked) + " password-protected" : ""));
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

    // The last small file tells the walk where to stop: a set whose artwork
    // comes first need not decompress all of its samples to get at it.
    size_t remaining = 0;
    for (const auto& e : entries_)
      if (e.archive == i && !isAudio(e.path) && !e.encrypted) ++remaining;
    auto pass = passFor(archives_[i]);
    std::string name;
    int64_t size = 0;
    bool encrypted = false;
    std::vector<char> bytes;
    while (remaining > 0 && pass->next(name, size, encrypted)) {
      std::string rel = name;
      std::replace(rel.begin(), rel.end(), '\\', '/');
      if (isAudio(rel) || encrypted) {
        if (!pass->discard()) {
          error = "\"" + fs::path(archives_[i].front()).filename().string() +
                  "\" could not be read at \"" + rel + "\": " + pass->error();
          return false;
        }
        continue;
      }
      --remaining;
      // A Hauptwerk definition belongs in OrganDefinitions/, beside the
      // packages, whatever the archive did with it. A GrandOrgue one stays
      // put: its paths are relative to its own folder.
      if (lower(rel).find("_hauptwerk_xml") != std::string::npos && rel.find('/') == std::string::npos)
        rel = "OrganDefinitions/" + rel;
      const fs::path target = fs::u8path(dir) / fs::u8path(rel);
      std::error_code ec;
      fs::create_directories(target.parent_path(), ec);
      if (!pass->readData(bytes)) {
        error = "\"" + fs::path(archives_[i].front()).filename().string() +
                "\" could not be read at \"" + rel + "\": " + pass->error();
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

  auto pass = passFor(archives_[archiveIndex]);
  std::string name;
  int64_t size = 0;
  bool encrypted = false;
  std::vector<char> bytes;
  while (remaining > 0 && pass->next(name, size, encrypted)) {
    const std::string k = key(name);
    if (!wanted.count(k)) {
      if (!pass->discard()) {
        error = std::string(name) + ": " + pass->error();
        return false;
      }
      continue;
    }
    --remaining;
    if (!pass->readData(bytes)) {
      error = std::string(name) + ": " + pass->error();
      return false;
    }
    if (!deliver(k, std::move(bytes))) return true;
    bytes = {};
  }
  if (remaining > 0 && pass->failed()) {
    error = fs::path(archives_[archiveIndex].front()).filename().string() + ": " + pass->error();
    return false;
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
