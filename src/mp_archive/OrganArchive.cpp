#include "OrganArchive.h"

#include <archive.h>
#include <archive_entry.h>
#include <unarr.h>

#include <mutex>
#include <thread>
#include <cstring>
#include <chrono>
#include <atomic>
#include <set>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstdio>
#ifndef _WIN32
 #include <unistd.h>
#endif
#include <map>
#include <memory>
#include <regex>
#include <set>

namespace mp {
namespace fs = std::filesystem;

namespace {
ArchiveOpenHook openHook = nullptr;

// A descriptor from the hook, when it has one for this path.
int hookedDescriptor(const std::string& path) {
  return openHook != nullptr ? openHook(path) : -1;
}

// Every package is opened through here, so a hooked path is read through its
// descriptor and any other the usual way.
std::FILE* openPackage(const std::string& path) {
#ifndef _WIN32
  const int fd = hookedDescriptor(path);
  if (fd >= 0) {
    if (std::FILE* f = fdopen(fd, "rb")) return f;
    ::close(fd);
    return nullptr;
  }
  return std::fopen(path.c_str(), "rb");
#else
  return _wfopen(fs::u8path(path).wstring().c_str(), L"rb");
#endif
}

bool seekTo(std::FILE* f, int64_t offset, int whence) {
#ifdef _WIN32
  return _fseeki64(f, offset, whence) == 0;
#else
  return fseeko(f, static_cast<off_t>(offset), whence) == 0;
#endif
}

int64_t tellOf(std::FILE* f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return static_cast<int64_t>(ftello(f));
#endif
}
}  // namespace

void setArchiveOpenHook(ArchiveOpenHook hook) { openHook = hook; }

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

// The image editors' own files a set ships beside its pictures: the organ
// never reads them. Ashton keeps its stops' Photoshop sources in
// psd_zaloha folders, and three of them that did not unpack were reported
// as a damaged organ (#321).
bool isEditorSource(const std::string& path) {
  const std::string p = lower(path);
  for (const char* ext : {".psd", ".psb", ".xcf"})
    if (endsWith(p, ext)) return true;
  return false;
}

bool isDefinition(const std::string& path) {
  const std::string p = lower(path);
  // Version 3 of the format named its files with dots where later ones use
  // underscores: "Wildervank.Organ.Hauptwerk.xml". The contents are the same.
  return endsWith(p, ".organ_hauptwerk_xml") || endsWith(p, ".customorgan_hauptwerk_xml") ||
         endsWith(p, ".organ.hauptwerk.xml") || endsWith(p, ".customorgan.hauptwerk.xml") ||
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
  std::FILE* in = openPackage(path);
  if (in == nullptr) return size;
  std::vector<unsigned char> b(static_cast<size_t>(tail));
  const bool whole = seekTo(in, size - tail, SEEK_SET) &&
                     std::fread(b.data(), 1, b.size(), in) == b.size();
  std::fclose(in);
  if (!whole) return size;
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

// One file of a RAR 4 volume, from its header alone.
struct Rar4File {
  std::string name;  // UTF-8, as stored ('\' separated)
  int64_t size = 0;  // unpacked
  bool encrypted = false;
};

// RAR 3's Unicode names: an ASCII name, a zero, then the Unicode name coded
// against it (unrar's EncodeFileName::Decode). Out as UTF-8.
static bool decodeRarUnicode(const unsigned char* name, size_t nameSize, const unsigned char* enc,
                             size_t encSize, std::string& out) {
  std::u16string w;
  if (encSize == 0) return false;
  const unsigned high = enc[0];
  size_t pos = 1;
  unsigned flags = 0, bits = 0;
  while (pos < encSize) {
    if (bits == 0) {
      flags = enc[pos++];
      bits = 8;
    }
    switch (flags >> 6) {
      case 0:
        if (pos >= encSize) return false;
        w += static_cast<char16_t>(enc[pos++]);
        break;
      case 1:
        if (pos >= encSize) return false;
        w += static_cast<char16_t>(enc[pos++] + (high << 8));
        break;
      case 2:
        if (pos + 1 >= encSize) return false;
        w += static_cast<char16_t>(enc[pos] + (enc[pos + 1] << 8));
        pos += 2;
        break;
      default: {
        if (pos >= encSize) return false;
        unsigned length = enc[pos++];
        if (length & 0x80) {
          if (pos >= encSize) return false;
          const unsigned correction = enc[pos++];
          for (length = (length & 0x7f) + 2; length > 0 && w.size() < nameSize; --length)
            w += static_cast<char16_t>(((name[w.size()] + correction) & 0xff) + (high << 8));
        } else {
          for (length += 2; length > 0 && w.size() < nameSize; --length)
            w += static_cast<char16_t>(name[w.size()]);
        }
        break;
      }
    }
    flags = (flags << 2) & 0xff;
    bits -= 2;
  }
  out.clear();
  for (size_t i = 0; i < w.size(); ++i) {
    uint32_t c = w[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
      c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return true;
}

// One file header of a RAR 4 volume, as stored, and where its data lies.
struct Rar4Header {
  std::vector<unsigned char> raw;  // the whole header, from its CRC to its end
  unsigned flags = 0;
  int64_t pack = 0;      // this part's data
  int64_t unpacked = 0;  // the whole file's
  size_t nameAt = 0, nameSize = 0;
  int64_t dataAt = 0;  // in the volume
};

// Walks one volume of a RAR 4 set by its headers, seeking past each file's
// data, and hands `file` each file header; `file` returns false to stop. The
// libraries walk a set only from its first volume; this reads any volume on
// its own. False for anything it does not follow: a header whose CRC is
// wrong, a password on the headers, a cut file. `brokenAt`, if given, is set
// to where a header could not be read or its CRC was wrong.
static bool walkRar4(const std::string& path, const std::atomic<bool>* stop,
                     const std::function<bool(const Rar4Header&)>& file,
                     int64_t* brokenAt = nullptr) {
  std::FILE* in = openPackage(path);
  if (in == nullptr) return false;
  struct Closer {
    std::FILE* f;
    ~Closer() { std::fclose(f); }
  } closer{in};
  unsigned char marker[7];
  if (std::fread(marker, 1, 7, in) != 7 || std::memcmp(marker, "Rar!\x1a\x07\x00", 7) != 0)
    return false;
  const int64_t end = rarVolumeEnd(path);
  int64_t pos = 7;
  Rar4Header f;
  auto& h = f.raw;
  const auto u32 = [&h](size_t o) {
    return static_cast<uint32_t>(h[o]) | static_cast<uint32_t>(h[o + 1]) << 8 |
           static_cast<uint32_t>(h[o + 2]) << 16 | static_cast<uint32_t>(h[o + 3]) << 24;
  };
  const auto broken = [&]() {
    if (brokenAt != nullptr) *brokenAt = pos;
    return false;
  };
  while (pos < end) {
    if (stop != nullptr && stop->load(std::memory_order_relaxed)) return false;
    if (!seekTo(in, pos, SEEK_SET)) return broken();
    h.resize(7);
    if (std::fread(h.data(), 1, 7, in) != 7) return broken();
    const unsigned type = h[2];
    const unsigned flags = h[3] | (h[4] << 8);
    const size_t hsize = h[5] | (h[6] << 8);
    if (hsize < 7) return broken();
    h.resize(hsize);
    if (hsize > 7 && std::fread(h.data() + 7, 1, hsize - 7, in) != hsize - 7) return broken();
    if ((crc32Of(&h[2], hsize - 2) & 0xffff) != static_cast<uint32_t>(h[0] | (h[1] << 8))) return broken();
    int64_t data = 0;
    if (type == 0x73) {
      if (flags & 0x0080) return false;  // a password on the headers
    } else if (type == 0x74) {
      if (hsize < 32) return false;
      f.flags = flags;
      f.pack = u32(7);
      f.unpacked = u32(11);
      f.nameSize = h[26] | (h[27] << 8);
      f.nameAt = 32;
      if (flags & 0x0100) {
        if (hsize < 40) return false;
        f.pack |= static_cast<int64_t>(u32(32)) << 32;
        f.unpacked |= static_cast<int64_t>(u32(36)) << 32;
        f.nameAt = 40;
      }
      if (hsize < f.nameAt + f.nameSize) return false;
      f.dataAt = pos + static_cast<int64_t>(hsize);
      data = f.pack;
      if (!file(f)) return true;
    } else if (type == 0x7b) {
      return true;
    } else if (flags & 0x8000) {
      if (hsize < 11) return false;
      data = u32(7);
    }
    pos += static_cast<int64_t>(hsize) + data;
  }
  return true;
}

// A file header's name as UTF-8. False for a name in a code page, which only
// ASCII is safe to read without knowing.
static bool rar4Name(const Rar4Header& f, std::string& name) {
  const unsigned char* n = &f.raw[f.nameAt];
  const auto zero = std::find(n, n + f.nameSize, static_cast<unsigned char>(0));
  if ((f.flags & 0x0200) && zero != n + f.nameSize) {
    const size_t ascii = static_cast<size_t>(zero - n);
    return decodeRarUnicode(n, ascii, zero + 1, f.nameSize - ascii - 1, name);
  }
  // UTF-8 when flagged Unicode without a coded part.
  name.assign(reinterpret_cast<const char*>(n), f.nameSize);
  if (!(f.flags & 0x0200))
    for (unsigned char c : name)
      if (c >= 0x80) return false;
  return true;
}

// What is wrong with volume `v` of a split RAR 4 set, for the player, or
// empty if its headers show nothing. To the libraries either fault is only a
// damaged file.
// - Its headers break partway through: a download with a bad stretch.
//   St Mary Portsea's demo: its .r00 was whole in size and unreadable from
//   57.9 MB on.
// - It does not follow the volume before: the file the earlier one ends
//   partway through must be the file it starts by continuing. Two volumes
//   from different copies of a set.
static std::string rar4VolumeTrouble(const std::vector<std::string>& volumes, size_t v) {
  if (v >= volumes.size()) return {};
  const std::string b = fs::u8path(volumes[v]).filename().string();
  int64_t brokenAt = -1;
  if (!walkRar4(volumes[v], nullptr, [](const Rar4Header&) { return true; }, &brokenAt) && brokenAt >= 0) {
    std::error_code ec;
    const auto size = static_cast<double>(fs::file_size(fs::u8path(volumes[v]), ec));
    char at[64];
    std::snprintf(at, sizeof at, "%.1f MB", static_cast<double>(brokenAt) / 1e6);
    std::string share;
    if (!ec && size > 0) {
      char pct[32];
      std::snprintf(pct, sizeof pct, " (%d%% of the file)",
                    static_cast<int>(100.0 * static_cast<double>(brokenAt) / size));
      share = pct;
    }
    return b + " is damaged from " + at + " on" + share +
           ": the files after that point cannot be read. Download that volume again.";
  }
  if (v == 0) return {};
  Rar4Header last, first;
  bool any = false, started = false;
  if (!walkRar4(volumes[v - 1], nullptr, [&](const Rar4Header& f) {
        last = f;
        any = true;
        return true;
      }) ||
      !walkRar4(volumes[v], nullptr, [&](const Rar4Header& f) {
        first = f;
        started = true;
        return false;
      }) ||
      !any || !started)
    return {};
  const bool split = (last.flags & 0x0002) != 0, continues = (first.flags & 0x0001) != 0;
  std::string ends, goesOn;
  if (!rar4Name(last, ends) || !rar4Name(first, goesOn)) return {};
  if (split == continues && (!split || ends == goesOn)) return {};
  const std::string a = fs::u8path(volumes[v - 1]).filename().string();
  std::string why;
  if (split && continues)
    why = a + " ends partway through " + ends + ", and " + b + " continues " + goesOn;
  else if (split)
    why = a + " ends partway through " + ends + ", and " + b + " starts with a new file";
  else
    why = b + " continues " + goesOn + ", which " + a + " does not hold";
  return b + " does not follow " + a + ": " + why +
         ". Download all of the set's volumes again, from one source.";
}

// The files that begin in one volume of a RAR 4 set, for listing the volumes
// of a set side by side (#321: Ashton, 11 volumes, 68,578 files, 451 s one
// after another).
static bool listRar4Volume(const std::string& path, const std::atomic<bool>& stop,
                           std::vector<Rar4File>& out) {
  bool named = true;
  const bool walked = walkRar4(path, &stop, [&](const Rar4Header& f) {
    const bool directory = (f.flags & 0x00E0) == 0x00E0;
    const bool continued = (f.flags & 0x0001) != 0;  // the rest of a file begun before
    if (directory || continued) return true;
    Rar4File r;
    if (!rar4Name(f, r.name)) {
      named = false;
      return false;
    }
    r.size = f.unpacked;
    r.encrypted = (f.flags & 0x0004) != 0;
    out.push_back(std::move(r));
    return true;
  });
  return walked && named;
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
  // How much the next read takes. Small after a skip: a header is a few
  // hundred bytes, and libarchive skips what it has already read by using it
  // up. A 1 MB read for every file of a set whose files average less than
  // that read the whole set to list it -- Ashton, 51 GB, a quarter of an
  // hour (#321). Reads in a row grow back to the full buffer.
  size_t nextRead = 1 << 20;

  bool openCurrent() {
    if (file != nullptr) std::fclose(file);
    file = nullptr;
    pos = 0;
    if (index >= paths.size()) return false;
    file = openPackage(paths[index]);
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
      const size_t want = static_cast<size_t>(
          (std::min<int64_t>)(left, static_cast<int64_t>((std::min)(s->nextRead, s->buffer.size()))));
      const size_t got = std::fread(s->buffer.data(), 1, want, s->file);
      if (got == 0) return -1;
      s->pos += static_cast<int64_t>(got);
      s->nextRead = (std::min)(s->nextRead * 2, s->buffer.size());
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
    s->nextRead = 8192;
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
    // A package read through the hook goes the same way as a split set:
    // libarchive's own file opening would ask for it by name.
    bool hooked = false;
    for (const auto& v : volumes) {
      const int fd = hookedDescriptor(v);
      if (fd >= 0) {
#ifndef _WIN32
        ::close(fd);
#endif
        hooked = true;
      }
    }
    if (volumes.size() > 1 || hooked) {
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

// A file of a split RAR 4 set that is not solid, read by its parts: the
// compressed data of each volume's part joined into one single-volume archive
// in memory, which libarchive decompresses whole. libarchive reads a
// compressed file that runs on into the next volume wrongly: four of Ashton's
// samples failed with "File CRC error" or "Bad RAR file data", while the RAR
// tools read them (#321, and #90's Buckeburg). `reached` is the volume the
// reading had reached; the file begins there or in the one before.
static bool readRar4Parts(const std::vector<std::string>& volumes, size_t reached,
                          const std::string& wanted, std::vector<char>& out) {
  std::vector<size_t> order;
  for (size_t v : {reached, reached > 0 ? reached - 1 : reached})
    if (v < volumes.size() && std::find(order.begin(), order.end(), v) == order.end()) order.push_back(v);
  struct Part {
    size_t volume;
    Rar4Header header;
  };
  std::vector<Part> parts;
  for (size_t v : order) {
    walkRar4(volumes[v], nullptr, [&](const Rar4Header& f) {
      std::string name;
      if ((f.flags & 0x0001) || !rar4Name(f, name)) return true;
      std::replace(name.begin(), name.end(), '\\', '/');
      if (OrganArchive::key(name) != wanted) return true;
      parts.push_back({v, f});
      return false;
    });
    if (!parts.empty()) break;
  }
  if (parts.empty() || (parts.front().header.flags & 0x0010)) return false;  // solid: needs what came before
  const auto sameName = [](const Rar4Header& a, const Rar4Header& b) {
    return a.nameSize == b.nameSize &&
           std::equal(a.raw.begin() + static_cast<std::ptrdiff_t>(a.nameAt),
                      a.raw.begin() + static_cast<std::ptrdiff_t>(a.nameAt + a.nameSize),
                      b.raw.begin() + static_cast<std::ptrdiff_t>(b.nameAt));
  };
  while (parts.back().header.flags & 0x0002) {
    const size_t v = parts.back().volume + 1;
    if (v >= volumes.size()) return false;
    bool found = false;
    walkRar4(volumes[v], nullptr, [&](const Rar4Header& f) {
      if ((f.flags & 0x0001) && sameName(f, parts.front().header)) {
        parts.push_back({v, f});
        found = true;
      }
      return false;  // the rest of a file is the first in its volume
    });
    if (!found) return false;
  }
  if (parts.size() < 2) return false;

  // The last part's header carries the whole file's CRC; its split flags go,
  // and its size becomes that of all the parts.
  Rar4Header h = parts.back().header;
  int64_t total = 0;
  for (const auto& part : parts) total += part.header.pack;
  if (total > 0xffffffffLL && !(h.flags & 0x0100)) return false;
  const unsigned flags = h.flags & ~0x0003u;
  h.raw[3] = static_cast<unsigned char>(flags & 0xff);
  h.raw[4] = static_cast<unsigned char>(flags >> 8);
  for (int i = 0; i < 4; ++i) h.raw[7 + static_cast<size_t>(i)] = static_cast<unsigned char>(total >> (8 * i));
  if (flags & 0x0100)
    for (int i = 0; i < 4; ++i)
      h.raw[32 + static_cast<size_t>(i)] = static_cast<unsigned char>(total >> (32 + 8 * i));
  const auto sealed = [](std::vector<unsigned char>& b) {
    const uint32_t crc = crc32Of(&b[2], b.size() - 2) & 0xffff;
    b[0] = static_cast<unsigned char>(crc & 0xff);
    b[1] = static_cast<unsigned char>(crc >> 8);
  };
  sealed(h.raw);
  std::vector<unsigned char> mainHeader = {0, 0, 0x73, 0, 0, 13, 0, 0, 0, 0, 0, 0, 0};
  std::vector<unsigned char> endHeader = {0, 0, 0x7b, 0x00, 0x40, 7, 0};
  sealed(mainHeader);
  sealed(endHeader);

  std::vector<unsigned char> whole;
  whole.reserve(static_cast<size_t>(7 + mainHeader.size() + h.raw.size() + total + endHeader.size()));
  whole.insert(whole.end(), {'R', 'a', 'r', '!', 0x1a, 0x07, 0x00});
  whole.insert(whole.end(), mainHeader.begin(), mainHeader.end());
  whole.insert(whole.end(), h.raw.begin(), h.raw.end());
  for (const auto& part : parts) {
    std::FILE* in = openPackage(volumes[part.volume]);
    if (in == nullptr) return false;
    const size_t at = whole.size();
    whole.resize(at + static_cast<size_t>(part.header.pack));
    const bool ok = seekTo(in, part.header.dataAt, SEEK_SET) &&
                    std::fread(whole.data() + at, 1, static_cast<size_t>(part.header.pack), in) ==
                        static_cast<size_t>(part.header.pack);
    std::fclose(in);
    if (!ok) return false;
  }
  whole.insert(whole.end(), endHeader.begin(), endHeader.end());

  archive* a = archive_read_new();
  archive_read_support_format_rar(a);
  bool ok = archive_read_open_memory(a, whole.data(), whole.size()) == ARCHIVE_OK;
  archive_entry* entry = nullptr;
  ok = ok && archive_read_next_header(a, &entry) == ARCHIVE_OK && readAll(a, h.unpacked, out);
  archive_read_free(a);
  return ok;
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
  // Which of the pass's volumes it is reading now, for a split set.
  virtual size_t volume() const { return 0; }
  virtual int64_t position() const = 0;
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
  size_t volume() const override { return r_.volumes_ ? r_.volumes_->index : 0; }
  int64_t position() const override { return archive_filter_bytes(r_.a, -1); }
  std::string error() const override { return r_.error(); }

private:
  Reader r_;
  bool opened_ = false;
  int rc_ = ARCHIVE_OK;
};

#ifndef _WIN32
// unarr's stream over a FILE we opened. ar_open_stream is part of unarr's own
// sources, which are built here, but not of its public header.
extern "C" {
typedef void (*ar_stream_close_fn)(void* data);
typedef size_t (*ar_stream_read_fn)(void* data, void* buffer, size_t count);
typedef bool (*ar_stream_seek_fn)(void* data, off64_t offset, int origin);
typedef off64_t (*ar_stream_tell_fn)(void* data);
ar_stream* ar_open_stream(void* data, ar_stream_close_fn close, ar_stream_read_fn read,
                          ar_stream_seek_fn seek, ar_stream_tell_fn tell);
}
void unarrClose(void* f) { std::fclose(static_cast<std::FILE*>(f)); }
size_t unarrRead(void* f, void* buffer, size_t count) {
  return std::fread(buffer, 1, count, static_cast<std::FILE*>(f));
}
bool unarrSeek(void* f, off64_t offset, int origin) {
  return seekTo(static_cast<std::FILE*>(f), offset, origin);
}
off64_t unarrTell(void* f) { return tellOf(static_cast<std::FILE*>(f)); }
#endif

class UnarrPass final : public Pass {
public:
  explicit UnarrPass(const std::string& path, bool solid = true) : solid_(solid) {
#ifdef _WIN32
    stream_ = ar_open_file_w(fs::u8path(path).wstring().c_str());
#else
    // Through the hook when it knows the path, with unarr reading our FILE as
    // its stream; otherwise unarr opens the file itself.
    if (const int fd = hookedDescriptor(path); fd >= 0) {
      ::close(fd);
      if (std::FILE* f = openPackage(path))
        stream_ = ar_open_stream(f, &unarrClose, &unarrRead, &unarrSeek, &unarrTell);
    } else {
      stream_ = ar_open_file(path.c_str());
    }
#endif
    if (stream_ != nullptr) ar_ = ar_open_rar_archive(stream_);
    if (ar_ == nullptr) error_ = "the RAR 4 reader cannot open it";
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
    // Each file of a non-solid archive starts afresh: the next header is
    // found without decompressing this one.
    if (!solid_) return true;
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
  int64_t position() const override { return stream_ != nullptr ? ar_tell(stream_) : 0; }
  std::string error() const override { return error_; }

private:
  ar_stream* stream_ = nullptr;
  ar_archive* ar_ = nullptr;
  bool solid_ = true;
  size_t size_ = 0;
  bool end_ = false;
  std::string error_;
  std::vector<char> scratch_;
};

}  // namespace

// RAR 4 archives libarchive turned away because they use RAR's filters
// (the x86, delta and audio transforms), which it does not decode and unarr
// does. Learned while reading; there is no telling from the headers.
static std::mutex gFilteredMutex;
static std::set<std::string> gFiltered;

static bool knownFiltered(const std::string& path) {
  const std::lock_guard<std::mutex> lock(gFilteredMutex);
  return gFiltered.count(path) != 0;
}

// Which reader an archive needs, from its first header.
static bool needsUnarr(const std::vector<std::string>& volumes) {
  if (volumes.size() != 1) return false;
  const ArchiveKind k = inspectArchive(volumes.front());
  return k.format == ArchiveKind::Format::Rar4 && (k.solid || knownFiltered(volumes.front()));
}

static std::unique_ptr<Pass> passFor(const std::vector<std::string>& volumes) {
  if (needsUnarr(volumes)) {
    const bool solid = inspectArchive(volumes.front()).solid;
    return std::make_unique<UnarrPass>(volumes.front(), solid);
  }
  return std::make_unique<LibarchivePass>(volumes);
}

// libarchive stopped on a RAR filter in a RAR 4 archive unarr can read: said
// once, and the archive is read with unarr from then on. A split set stays
// as it is: unarr reads no RAR 4 volumes. Its "Internal error extracting RAR
// file" is the same case under another name: Pekela's archive tests sound
// and 29 of its samples stopped there.
static bool switchedForFilters(const std::vector<std::string>& volumes, const Pass& pass) {
  const bool unarrMayRead = pass.error().find("filters") != std::string::npos ||
                            pass.error().find("Internal error extracting RAR file") != std::string::npos;
  if (volumes.size() != 1 || !unarrMayRead) return false;
  if (inspectArchive(volumes.front()).format != ArchiveKind::Format::Rar4) return false;
  const std::lock_guard<std::mutex> lock(gFilteredMutex);
  return gFiltered.insert(volumes.front()).second;
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
  std::vector<unsigned char> b(1 << 16);
  if (std::FILE* in = openPackage(path)) {
    b.resize(std::fread(b.data(), 1, b.size(), in));
    std::fclose(in);
  } else {
    b.clear();
  }
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

// The installation package an archive belongs to: the first
// OrganInstallationPackages/<six digits>/ folder in its headers. Read until
// it is found, which is seldom past the first few files; empty when there is
// none (a definitions-only archive, or another kind of file).
static std::string packageInPath(const std::string& name) {
  std::string k = lower(name);
  std::replace(k.begin(), k.end(), '\\', '/');
  const std::string folder = "organinstallationpackages/";
  const auto at = k.find(folder);
  if (at != std::string::npos && k.size() >= at + folder.size() + 6) {
    const std::string id = k.substr(at + folder.size(), 6);
    if (std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); })) return id;
  }
  return {};
}

static std::string firstPackageId(const std::vector<std::string>& volumes) {
  if (volumes.empty()) return {};
  // A RAR 4 volume by its headers alone, which are never compressed: a solid
  // archive need not be unpacked file by file to reach its first package
  // folder. Asking every archive in a download folder that way took minutes
  // when one began with a large definition.
  std::string id;
  int seen = 0;
  const bool walked = walkRar4(volumes.front(), nullptr, [&](const Rar4Header& f) {
    std::string name(reinterpret_cast<const char*>(f.raw.data() + f.nameAt), f.nameSize);
    if (const auto zero = name.find('\0'); zero != std::string::npos) name.resize(zero);
    id = packageInPath(name);
    // A package's files lie under its folder from the first entries; an
    // archive that has not reached one in 60 headers carries none. Each
    // header is a seek, which on a hard disk added up to minutes.
    return id.empty() && ++seen < 60;
  });
  if (!id.empty() || walked) return id;
  // Another kind of archive: its first entries, as the libraries list them.
  // Few, and little skipped: passing an entry of a solid archive means
  // decompressing it, and a large definition first would cost its whole size.
  auto pass = passFor(volumes);
  std::string name;
  int64_t size = 0, skipped = 0;
  bool encrypted = false;
  for (int n = 0; n < 20 && pass->next(name, size, encrypted); ++n) {
    if (const auto found = packageInPath(name); !found.empty()) return found;
    skipped += size;
    if (skipped > (int64_t{64} << 20)) break;
    pass->skip();
  }
  return {};
}

// The installation packages the definitions in an archive say they need
// (RequiredInstallationPackage), as six-digit folder names: "002261". Read
// only from an archive that is definitions alone, which is small; empty for
// anything else.
static std::set<std::string> requiredPackages(const std::vector<std::string>& volumes) {
  std::set<std::string> out;
  auto pass = passFor(volumes);
  std::string name;
  int64_t size = 0;
  bool encrypted = false;
  for (int n = 0; n < 200 && pass->next(name, size, encrypted); ++n) {
    const std::string k = lower(name);
    if (encrypted || size <= 0 || size > (int64_t{256} << 20) || !endsWith(k, ".organ_hauptwerk_xml")) {
      pass->skip();
      continue;
    }
    std::vector<char> data;
    if (!pass->readData(data)) break;
    const std::string text(data.begin(), data.end());
    const auto at = text.find("ObjectType=\"RequiredInstallationPackage\"");
    if (at == std::string::npos) continue;
    const auto end = text.find("</ObjectList>", at);
    const std::string list = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    // The long form names the field, the compact one writes it as <a>.
    static const std::regex id(R"(<(?:InstallationPackageID|a)>\s*(\d{1,6})\s*<)");
    for (std::sregex_iterator it(list.begin(), list.end(), id), stop; it != stop; ++it) {
      std::string digits = (*it)[1].str();
      out.insert(std::string(6 - digits.size(), '0') + digits);
    }
  }
  return out;
}

// How many letters two organ words share at the start: "burtonberlindemo"
// and "burtonberlinsurround1" share twelve.
static size_t sharedStart(const std::string& a, const std::string& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  return i;
}

// A part's number in front of its name: 2 for "02_Ada(dry)demo...", 0 for none.
static int partNumber(const std::string& fileName) {
  size_t i = 0;
  while (i < fileName.size() && i < 3 && std::isdigit(static_cast<unsigned char>(fileName[i]))) ++i;
  if (i == 0 || i >= fileName.size() || fileName[i] != '_') return 0;
  return std::stoi(fileName.substr(0, i));
}

// The organ's own word in a part's file name: "01_Ada(wet)demo..." and
// "03_Ada(sur)demo..." are both "ada", "02_Rieger_szhely_dry_demo" is
// "rieger". A part's number in front is not part of it.
static std::string organWord(const std::string& fileName) {
  std::string n = lower(fileName);
  size_t i = 0;
  while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) ++i;
  if (i > 0 && i < n.size() && (n[i] == '_' || n[i] == '-' || n[i] == ' ')) n.erase(0, i + 1);
  size_t e = 0;
  while (e < n.size() && std::isalnum(static_cast<unsigned char>(n[e]))) ++e;
  return n.substr(0, e);
}

void OrganArchive::join(const OrganArchive& other) {
  const size_t base = archives_.size();
  for (const auto& group : other.archives_) archives_.push_back(group);
  for (const Entry& e : other.entries_) {
    Entry n = e;
    n.archive += base;
    entries_.push_back(n);
    byKey_.emplace(key(n.path), entries_.size() - 1);
  }
  for (const auto& line : other.report_) report_.push_back(line);
}

std::vector<std::string> OrganArchive::carryingIn(const std::vector<std::string>& folders,
                                                  const std::set<std::string>& packages) {
  std::vector<std::string> out;
  if (packages.empty()) return out;
  std::error_code ec;
  size_t looked = 0;
  for (const auto& dir : folders)
    for (const auto& e : fs::directory_iterator(fs::path(dir), ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string name = e.path().filename().string();
      if (!isOrganArchive(name) || !originalOf(name).empty()) continue;
      // A later volume of a set: "Name.part2.rar", "Name.r00". "Name.part1.rar"
      // and the old scheme's "Name.rar" are first volumes.
      static const std::regex partOne(R"(\.part0*1\.rar$)", std::regex::icase);
      std::string setName;
      const int volume = volumeNumber(name, setName);
      if (volume > 1 || (volume == 1 && !std::regex_search(name, partOne))) continue;
      if (++looked > 2000) break;  // a folder of everything
      if (packages.count(firstPackageId({e.path().string()})) != 0) out.push_back(e.path().string());
    }
  std::sort(out.begin(), out.end());
  return out;
}

bool OrganArchive::discover(const std::string& path, std::string& error) {
  archives_.clear();
  entries_.clear();
  byKey_.clear();
  chosen_ = path;

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
  // A folder that may not be listed: on an iPhone or iPad a picked document
  // is lent on its own, and its folder stays closed (#197). The package then
  // stands alone; one that turns out to be a volume of a set says so below.
  if (archives_.empty() && isOrganArchive(chosen.filename().string()) &&
      fs::is_regular_file(chosen, ec))
    archives_.push_back({path});
  if (archives_.empty()) {
    error = "no archive found at " + path;
    return false;
  }

  // Parts of one set published as archives of their own names: Augustine's
  // demos come as 01_Ada(wet), 02_Ada(dry) and 03_Ada(sur), each holding part
  // of package 002374, and the surround definition plays samples from all
  // three. An archive in the same folder belongs with the chosen one when its
  // name starts with the same organ word and it carries the same package;
  // two organs that share a word do not share a package.
  if (!dir.empty() && archives_.size() == 1 && isOrganArchive(chosen.filename().string())) {
    const std::string word = organWord(chosen.filename().string());
    std::string packageId;
    bool looked = false;
    std::vector<std::string> parts;
    std::vector<std::string> candidates;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string name = e.path().filename().string();
      if (!isOrganArchive(name) || word.size() < 3 || organWord(name) != word) continue;
      if (!originalOf(name).empty()) continue;  // a second download
      std::string setName;
      if (volumeNumber(name, setName) >= 0) continue;  // a volume of another set
      bool already = false;
      for (const auto& group : archives_)
        for (const auto& file : group)
          if (fs::equivalent(fs::path(file), e.path(), ec)) already = true;
      if (!already) candidates.push_back(e.path().string());
    }
    // Numbered parts, "01_", "02_", "03_", each number once among them and
    // the chosen archive: one set, whatever packages its parts carry (Eule
    // Szombathely gives each perspective a package of its own, and the
    // surround definition plays all three). Two organs with the same word
    // number their parts from 1 each, and are told apart by package below.
    std::set<int> numbers;
    bool numbered = partNumber(chosen.filename().string()) > 0;
    if (numbered) numbers.insert(partNumber(chosen.filename().string()));
    for (const auto& c : candidates) {
      const int n = partNumber(fs::path(c).filename().string());
      if (n <= 0 || !numbers.insert(n).second) numbered = false;
    }
    for (const auto& c : candidates) {
      if (numbered) {
        parts.push_back(c);
        continue;
      }
      if (!looked) {
        packageId = firstPackageId(archives_.front());
        looked = true;
      }
      if (packageId.empty()) break;
      if (firstPackageId({c}) == packageId) parts.push_back(c);
    }
    std::sort(parts.begin(), parts.end());
    for (const auto& part : parts) {
      archives_.push_back({part});
      report_.push_back("with " + fs::path(part).filename().string() + ": another part of the set");
    }
  }

  // A definition published as a package of its own, beside the packages that
  // hold its samples and artwork, under other names: Hill, Burton-Berlin
  // comes as BurtonBerlinDemo_ODF beside BurtonBerlinSurround1 and 2 (#345).
  // The definition names the packages it needs. The archives beside it whose
  // names start as the organ's does (five letters or more) and that carry one
  // of them belong with it, whichever of them was opened.
  if (!dir.empty() && archives_.size() == 1 && isOrganArchive(chosen.filename().string())) {
    const std::string word = organWord(chosen.filename().string());
    std::vector<std::pair<std::string, std::string>> beside;  // (path, its package)
    if (word.size() >= 5)
      for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path() == chosen) continue;
        const std::string name = e.path().filename().string();
        if (!isOrganArchive(name) || !originalOf(name).empty()) continue;
        std::string setName;
        if (volumeNumber(name, setName) >= 0) continue;
        if (sharedStart(organWord(name), word) < 5) continue;
        beside.emplace_back(e.path().string(), firstPackageId({e.path().string()}));
      }
    std::vector<std::string> found;
    std::set<std::string> needs;
    std::string definition;  // the definitions-only archive, when it was not the one opened
    if (!beside.empty()) {
      const std::string ownPackage = firstPackageId(archives_.front());
      if (ownPackage.empty()) {
        needs = requiredPackages(archives_.front());
      } else {
        for (const auto& [path, package] : beside) {
          if (!package.empty()) continue;
          auto wanted = requiredPackages({path});
          if (wanted.count(ownPackage) == 0) continue;
          definition = path;
          needs = std::move(wanted);
          break;
        }
      }
      if (!definition.empty()) found.push_back(definition);
      for (const auto& [path, package] : beside)
        if (!package.empty() && needs.count(package) != 0) found.push_back(path);
    }
    std::sort(found.begin(), found.end());
    for (const auto& f : found) {
      archives_.push_back({f});
      report_.push_back("with " + fs::path(f).filename().string() +
                        (f == definition ? ": the definition that needs this package"
                                         : ": holds a package the definition needs"));
    }
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

// A file unpacked with the definitions: everything that is neither a sample,
// an image editor's source nor locked.
static bool isSmallFile(const std::string& rel, bool encrypted) {
  return !isAudio(rel) && !isEditorSource(rel) && !encrypted;
}

// Where a small file goes. A Hauptwerk definition belongs in
// OrganDefinitions/, beside the packages, whatever the archive did with it. A
// GrandOrgue one stays put: its paths are relative to its own folder.
static fs::path smallFileTarget(const std::string& dir, std::string rel) {
  if ((lower(rel).find("_hauptwerk_xml") != std::string::npos ||
       lower(rel).find(".hauptwerk.xml") != std::string::npos) &&
      rel.find('/') == std::string::npos)
    rel = "OrganDefinitions/" + rel;
  return fs::u8path(dir) / fs::u8path(rel);
}

static bool writeWhole(const fs::path& target, const std::vector<char>& bytes) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

bool OrganArchive::indexVolumesAtOnce(size_t archive, const Progress& scanned,
                                      const std::string& unpackDir, std::string& error) {
  const auto& volumes = archives_[archive];
  if (volumes.size() < 2) return false;
  const ArchiveKind kind = inspectArchive(volumes.front());
  if (kind.format != ArchiveKind::Format::Rar4 || kind.solid || kind.encryptedHeaders) return false;

  std::vector<int64_t> sizes(volumes.size(), 0);
  for (size_t v = 0; v < volumes.size(); ++v) {
    std::error_code ec;
    sizes[v] = static_cast<int64_t>(fs::file_size(fs::u8path(volumes[v]), ec));
  }
  const size_t threads =
      std::min<size_t>(volumes.size(), (std::max)(2u, std::thread::hardware_concurrency()));
  // Runs `job` for 0..count-1 on the threads, and calls in on this one until
  // they are done: false from it stops them.
  std::atomic<bool> stop{false};
  std::atomic<int64_t> doneBytes{0};
  const auto runAll = [&](size_t count, const std::function<void(size_t)>& job) {
    std::atomic<size_t> next{0}, finished{0};
    std::vector<std::thread> pool;
    for (size_t t = 0; t < (std::min)(threads, count); ++t)
      pool.emplace_back([&] {
        for (size_t k; (k = next.fetch_add(1)) < count;) {
          if (!stop.load()) job(k);
          ++finished;
        }
      });
    bool going = true;
    while (finished.load() < count) {
      if (going && scanned && !scanned(static_cast<double>(doneBytes.load()))) {
        going = false;
        stop = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto& t : pool) t.join();
    return going;
  };

  std::vector<std::vector<Rar4File>> listed(volumes.size());
  std::atomic<bool> failed{false};
  if (!runAll(volumes.size(), [&](size_t v) {
        if (!listRar4Volume(volumes[v], stop, listed[v])) {
          failed = true;
          stop = true;
        }
        doneBytes += sizes[v];
      })) {
    error = "cancelled";
    return false;
  }
  if (failed) return false;

  // In the order the old listing gives, the first copy of a file winning.
  std::vector<std::vector<std::string>> smallIn(volumes.size());
  size_t count = 0, locked = 0;
  for (size_t v = 0; v < volumes.size(); ++v)
    for (auto& f : listed[v]) {
      ++count;
      if (f.encrypted) ++locked;
      Entry e;
      e.archive = archive;
      e.path = std::move(f.name);
      std::replace(e.path.begin(), e.path.end(), '\\', '/');
      e.size = f.size;
      e.encrypted = f.encrypted;
      const std::string k = key(e.path);
      if (byKey_.count(k) != 0) continue;
      if (isSmallFile(e.path, e.encrypted)) smallIn[v].push_back(k);
      byKey_[k] = entries_.size();
      entries_.push_back(std::move(e));
    }
  report_.push_back(fs::path(volumes.front()).filename().string() + " -- " + std::to_string(count) +
                    " files" + (locked ? ", " + std::to_string(locked) + " password-protected" : "") +
                    ", listed " + std::to_string(threads) + " volumes at a time");
  if (unpackDir.empty()) return true;

  // The small files, from each volume they begin in, side by side: a walk
  // from the first volume to Ashton's last image read the whole set. What a
  // walk cannot give, unpackSmallFiles() takes the old way.
  std::vector<size_t> withSmall;
  for (size_t v = 0; v < volumes.size(); ++v)
    if (!smallIn[v].empty()) withSmall.push_back(v);
  std::mutex merge;
  doneBytes = 0;
  if (!runAll(withSmall.size(), [&](size_t n) {
        const size_t v = withSmall[n];
        std::unordered_set<std::string> want(smallIn[v].begin(), smallIn[v].end());
        auto pass = passFor(std::vector<std::string>(volumes.begin() + static_cast<long>(v), volumes.end()));
        std::string name;
        int64_t size = 0;
        bool encrypted = false;
        std::vector<char> data;
        std::vector<std::string> got;
        while (!want.empty() && !stop.load() && pass->next(name, size, encrypted)) {
          std::string rel = name;
          std::replace(rel.begin(), rel.end(), '\\', '/');
          const std::string k = key(rel);
          if (want.count(k) == 0) {
            pass->skip();
            continue;
          }
          if (!pass->readData(data) || !writeWhole(smallFileTarget(unpackDir, rel), data)) break;
          want.erase(k);
          got.push_back(k);
        }
        doneBytes += sizes[v];
        const std::lock_guard<std::mutex> lock(merge);
        unpacked_.insert(got.begin(), got.end());
      })) {
    error = "cancelled";
    return false;
  }
  return true;
}

bool OrganArchive::index(std::string& error, const Progress& progress, const std::string& unpackDir) {
  unpacked_.clear();
  entries_.clear();
  byKey_.clear();

  std::vector<double> bytes(archives_.size(), 0.0);
  double totalBytes = 0.0, completedBytes = 0.0;
  if (progress)
    for (size_t i = 0; i < archives_.size(); ++i)
      for (const auto& volume : archives_[i]) {
        std::error_code ec;
        const auto size = fs::file_size(fs::u8path(volume), ec);
        if (!ec) bytes[i] += static_cast<double>(size);
        else { error = "cannot read the size of " + volume; return false; }
      }
  for (const auto size : bytes) totalBytes += size;
  auto notify = [&](double scanned) {
    if (!progress || progress(totalBytes > 0.0 ? std::clamp(scanned / totalBytes, 0.0, 1.0) : -1.0))
      return true;
    error = "cancelled";
    return false;
  };
  if (!notify(0.0)) return false;

  // Headers only. libarchive skips a non-solid file by seeking, and has to
  // decompress what it skips in a solid RAR 5; unarr lists a solid RAR 4
  // without decompressing anything -- Nancy's 15,577 files in about a second.
  for (size_t i = 0; i < archives_.size(); ++i) {
    const auto scanned = [&](double s) { return notify(completedBytes + std::clamp(s, 0.0, bytes[i])); };
    if (indexVolumesAtOnce(i, scanned, unpackDir, error)) {
      completedBytes += bytes[i];
      if (!notify(completedBytes)) return false;
      continue;
    }
    if (error == "cancelled") return false;
    // With an unpackDir, small files come out as they are passed. A file
    // that will not come out may have left the reader unable to go on, so
    // the archive is listed again without unpacking, and unpackSmallFiles()
    // takes its small files the old way.
    bool unpacking = !unpackDir.empty();
    const size_t entriesBefore = entries_.size();
    std::vector<std::string> added, unpackedHere;
    std::vector<char> data;
    std::unique_ptr<Pass> pass;
    std::string name, last;
    size_t count = 0, locked = 0;
    for (bool again = true; again;) {
      again = false;
      pass = passFor(archives_[i]);
      last.clear();
      count = locked = 0;
      int64_t size = 0;
      bool encrypted = false;
      while (pass->next(name, size, encrypted)) {
        if (!notify(completedBytes + std::clamp(static_cast<double>(pass->position()), 0.0, bytes[i])))
          return false;
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
        if (byKey_.count(k) != 0) {
          pass->skip();
          continue;
        }
        const std::string rel = e.path;
        byKey_[k] = entries_.size();
        entries_.push_back(std::move(e));
        added.push_back(k);
        if (!unpacking || !isSmallFile(rel, encrypted)) {
          pass->skip();
          continue;
        }
        if (!pass->readData(data)) {
          for (const auto& a : added) byKey_.erase(a);
          entries_.resize(entriesBefore);
          added.clear();
          unpackedHere.clear();
          unpacking = false;
          again = true;
          break;
        }
        const fs::path target = smallFileTarget(unpackDir, rel);
        if (!writeWhole(target, data)) {
          error = "cannot write " + target.string();
          return false;
        }
        unpackedHere.push_back(k);
      }
    }
    unpacked_.insert(unpackedHere.begin(), unpackedHere.end());
    const std::string file = fs::path(archives_[i].front()).filename().string();
    if (pass->failed()) {
      const std::string what =
          "\"" + file + "\" could not be read: " + pass->error() + " (" +
          (last.empty() ? std::string("before its first file")
                        : "after " + std::to_string(count) + " files, the last \"" + last + "\"") +
          ").";
      // A damaged part found beside the archive that was opened gives the
      // files it lists up to the damage; the opened archive still loads.
      bool opened = chosen_.empty();
      for (const auto& volume : archives_[i]) {
        std::error_code ec;
        if (volume == chosen_ || fs::equivalent(fs::u8path(volume), fs::u8path(chosen_), ec))
          opened = true;
      }
      if (opened) {
        error = what;
        return false;
      }
      report_.push_back(what + " The files after that point are left out.");
      completedBytes += bytes[i];
      if (!notify(completedBytes)) return false;
      continue;
    }
    report_.push_back(file + " -- " + std::to_string(count) + " files" +
                      (locked ? ", " + std::to_string(locked) + " password-protected" : ""));
    completedBytes += bytes[i];
    if (!notify(completedBytes)) return false;
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

bool OrganArchive::loadIndex(const std::string& file) { return readIndex(file, true); }

bool OrganArchive::readIndex(const std::string& file, bool checkVolumes) {
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
        if (checkVolumes && (fs::file_size(fs::u8path(v), ec) != size || ec)) return false;
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

bool OrganArchive::loadIndexFor(const std::string& file) {
  const auto found = archives_;
  if (found.empty()) return loadIndex(file);
  // Read the index against the volumes found now: a saved volume whose file
  // name and size match one of them is that volume, wherever it was.
  std::ifstream in(fs::u8path(file), std::ios::binary);
  if (!in) return false;
  std::vector<std::vector<std::string>> archives;
  std::vector<Entry> entries;
  std::string line;
  auto baseName = [](const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
  };
  auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  try {
    while (std::getline(in, line)) {
      if (line == "A") {
        archives.emplace_back();
      } else if (line.rfind("V	", 0) == 0 && !archives.empty()) {
        const size_t tab = line.find('	', 2);
        if (tab == std::string::npos) return false;
        const auto size = std::stoull(line.substr(2, tab - 2));
        const std::string name = lower(baseName(line.substr(tab + 1)));
        const size_t a = archives.size() - 1, v = archives.back().size();
        if (a >= found.size() || v >= found[a].size()) return false;
        const std::string& now = found[a][v];
        std::error_code ec;
        if (lower(baseName(now)) != name ||
            fs::file_size(fs::u8path(now), ec) != size || ec)
          return false;
        archives.back().push_back(now);
      } else if (line.rfind("E	", 0) == 0) {
        break;  // the entries are read below, as ever
      }
    }
  } catch (...) {
    return false;
  }
  if (archives.size() != found.size()) return false;
  for (size_t a = 0; a < found.size(); ++a)
    if (archives[a].size() != found[a].size()) return false;
  // The entries do not name volumes, so the rest is the plain read, with the
  // paths put back to the ones found now.
  if (!readIndex(file, false)) return false;
  archives_ = std::move(archives);
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

std::string OrganArchive::archiveName(size_t index) const {
  return index < archives_.size() && !archives_[index].empty()
             ? fs::path(archives_[index].front()).filename().string()
             : std::string("the archive");
}

bool OrganArchive::unpackSmallFiles(const std::string& dir, std::string& error,
                                    std::vector<std::string>& damaged, const Progress& progress) const {
  // Count only the files we must walk to reach the last definition/artwork
  // in each archive; samples after it need not be decompressed here.
  size_t total = 0, done = 0;
  if (progress)
    for (size_t i = 0; i < archives_.size(); ++i) {
      size_t count = 0, last = 0;
      for (const auto& e : entries_)
        if (e.archive == i) {
          ++count;
          if (isSmallFile(e.path, e.encrypted) && !unpacked_.count(key(e.path))) last = count;
        }
      total += last;
    }
  auto notify = [&] {
    if (!progress || progress(total > 0 ? (std::min)(1.0, static_cast<double>(done) / total) : 1.0))
      return true;
    error = "cancelled";
    return false;
  };
  if (!notify()) return false;
  for (size_t i = 0; i < archives_.size(); ++i) {
    // Nothing to take from an archive that is all audio.
    bool any = false;
    for (const auto& e : entries_)
      if (e.archive == i && isSmallFile(e.path, e.encrypted) && !unpacked_.count(key(e.path))) any = true;
    if (!any) continue;

    // The last small file tells the walk where to stop: a set whose artwork
    // comes first need not decompress all of its samples to get at it.
    size_t remaining = 0;
    for (const auto& e : entries_)
      if (e.archive == i && isSmallFile(e.path, e.encrypted) && !unpacked_.count(key(e.path))) ++remaining;
    auto pass = passFor(archives_[i]);
    std::string name;
    int64_t size = 0;
    bool encrypted = false;
    std::vector<char> bytes;
    while (remaining > 0 && pass->next(name, size, encrypted)) {
      if (!notify()) return false;
      ++done;
      std::string rel = name;
      std::replace(rel.begin(), rel.end(), '\\', '/');
      if (!isSmallFile(rel, encrypted) || unpacked_.count(key(rel))) {
        if (!pass->discard()) {
          // Past here the archive cannot be followed, so what is left of its
          // small files is lost; the other archives are still read.
          damaged.push_back(archiveName(i) + ": damaged at " + rel + " (" + pass->error() +
                            "); nothing after it could be read");
          break;
        }
        continue;
      }
      --remaining;
      const fs::path target = smallFileTarget(dir, rel);
      if (!pass->readData(bytes)) {
        if (switchedForFilters(archives_[i], *pass)) {
          // From the top with unarr: what was written is written again.
          pass = passFor(archives_[i]);
          remaining = 0;
          for (const auto& e : entries_)
            if (e.archive == i && isSmallFile(e.path, e.encrypted) && !unpacked_.count(key(e.path))) ++remaining;
          bytes = {};
          continue;
        }
        damaged.push_back(archiveName(i) + ": " + rel + " is damaged (" + pass->error() + ")");
        bytes = {};
        continue;
      }
      if (!writeWhole(target, bytes)) {
        error = "cannot write " + target.string();
        return false;
      }
    }
  }
  return notify();
}

bool OrganArchive::read(size_t archiveIndex, const std::unordered_set<std::string>& wanted,
                        const Deliver& deliver, std::string& error) const {
  if (archiveIndex >= archives_.size()) return false;
  size_t remaining = 0;
  for (const auto& e : entries_)
    if (e.archive == archiveIndex && wanted.count(key(e.path))) ++remaining;
  if (remaining == 0) return true;

  // A file that does not read is that file lost, not the rest of the archive
  // (#90's Buckeburg report: a CRC error on a file continued from one volume
  // into the next stopped the load two divisions in, reported as a single
  // unreadable sample). After a failure a split set is read on from the
  // volume the pass had reached: each volume starts with headers of its own,
  // so only the files across that boundary are lost. Each failure is a line
  // of `error`.
  const auto& volumes = archives_[archiveIndex];
  std::unordered_set<std::string> finished;  // delivered, or failed for good
  // A damaged volume, or one that does not follow the one before, said once
  // each.
  std::unordered_set<size_t> broken;
  const auto checkBreak = [&](size_t reached) {
    if (volumes.size() < 2 || needsUnarr(volumes)) return;
    for (size_t v : {reached > 0 ? reached - 1 : reached, reached, reached + 1}) {
      if (v >= volumes.size() || !broken.insert(v).second) continue;
      const std::string why = rar4VolumeTrouble(volumes, v);
      if (!why.empty()) error += archiveName(archiveIndex) + ": " + why + "\n";
    }
  };
  size_t from = 0;
  auto pass = passFor(volumes);
  auto restart = [&]() -> bool {
    if (volumes.size() < 2 || needsUnarr(volumes)) return false;
    // At least the next volume: libarchive gives up on a compressed file
    // continued into the next volume while still at the end of the first.
    const size_t at = from + std::max<size_t>(1, pass->volume());
    if (at >= volumes.size()) return false;
    from = at;
    pass = passFor(std::vector<std::string>(volumes.begin() + static_cast<std::ptrdiff_t>(at), volumes.end()));
    return true;
  };
  std::string name;
  int64_t size = 0;
  bool encrypted = false;
  std::vector<char> bytes;
  for (;;) {
    while (remaining > 0 && pass->next(name, size, encrypted)) {
      const std::string k = key(name);
      if (!wanted.count(k) || finished.count(k)) {
        if (!pass->discard()) break;
        continue;
      }
      if (!pass->readData(bytes)) {
        if (switchedForFilters(volumes, *pass)) {
          // The same file again, with unarr: what was delivered is skipped.
          pass = passFor(volumes);
          bytes = {};
          continue;
        }
        // A compressed file that runs on into the next volume, read by its
        // parts (#321).
        if (volumes.size() > 1 && !needsUnarr(volumes) &&
            readRar4Parts(volumes, from + pass->volume(), k, bytes)) {
          finished.insert(k);
          --remaining;
          if (!deliver(k, std::move(bytes))) return error.empty();
          bytes = {};
          continue;
        }
        error += archiveName(archiveIndex) + ": " + name + " is damaged (" + pass->error() + ")\n";
        checkBreak(from + pass->volume());
        finished.insert(k);
        --remaining;
        bytes = {};
        continue;
      }
      finished.insert(k);
      --remaining;
      if (!deliver(k, std::move(bytes))) return error.empty();
      bytes = {};
    }
    if (remaining == 0 || !pass->failed()) break;
    if (!restart()) {
      error += archiveName(archiveIndex) + ": damaged (" + pass->error() +
               "); the samples after this point could not be read\n";
      checkBreak(from + pass->volume());
      break;
    }
  }
  if (!error.empty()) error.pop_back();  // the last newline
  return error.empty();
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
