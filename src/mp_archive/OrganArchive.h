// Organs played straight from the RAR packages they are distributed in.
//
// A sample set arrives as one or more archives -- a single .rar, numbered
// packages ("01_Name...rar", "02_Name...rar"), parts ("Name_part01...rar"),
// or a multi-volume set ("Name.part1.rar", "Name.part2.rar") -- holding the
// same tree an installed organ has: OrganDefinitions/ and
// OrganInstallationPackages/<id>/. Unpacking one takes as much disk again as
// the set itself, often tens of gigabytes.
//
// This reads them in place. The small files (the organ definition, the
// console artwork, the package definitions) are unpacked into a folder of
// their own, so the loader and the console find an ordinary organ there. The
// samples -- almost all of the bytes -- never touch the disk: the sample
// library asks for them here and gets them, decompressed, in memory.
//
// RAR can only be read front to back: a solid archive has to be decompressed
// from the start to reach any file, and even a non-solid one is found by
// walking its headers. So samples are delivered in ARCHIVE order, one pass per
// archive, never looked up one by one.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mp {

// A package the system will not let us open by its name: on Android, a file
// chosen in the system's picker is reached through a descriptor the system
// opens for us, and its path cannot be opened again. A platform that has such
// files installs this hook; it returns a fresh descriptor (with its own
// position) for a path it knows, or -1, and the archive reader opens every
// package through it. No hook, as on the desktop: every path is opened as is.
using ArchiveOpenHook = int (*)(const std::string& path);
void setArchiveOpenHook(ArchiveOpenHook hook);

// True for a name the importer treats as an organ package: a .rar, or a
// GrandOrgue .orgue (any case).
bool isOrganArchive(const std::string& path);

// What an archive is, from its first headers alone: enough to say why it
// cannot be read before trying, and to write down what was found.
struct ArchiveKind {
  enum class Format { Unknown, Rar4, Rar5, Zip };
  Format format = Format::Unknown;
  bool solid = false;
  bool multiVolume = false;      // one volume of a set
  bool firstVolume = false;      // ...and the first one (RAR 4 says so)
  bool encryptedHeaders = false; // even the file names are locked
  bool encryptedFiles = false;   // the first file is password-protected
  int64_t bytes = 0;
  // "RAR 4, solid, 6.70 GB"
  std::string describe() const;
};
ArchiveKind inspectArchive(const std::string& path);

class OrganArchive {
public:
  struct Entry {
    size_t archive = 0;   // which of archives()
    std::string path;     // as stored, '/'-separated
    int64_t size = 0;
    bool encrypted = false;
  };

  // Find the archives that belong with `path` -- itself, its volumes, and
  // its sibling packages in the same folder -- and read their headers.
  bool open(const std::string& path, std::string& error);

  // The two halves of open(). discover() only lists the folder, which is
  // enough for identity(); index() reads every header, which for a solid
  // archive means decompressing all of it.
  bool discover(const std::string& path, std::string& error);
  bool index(std::string& error);
  // Between the two: each archive's first header, which is where most
  // reasons it cannot be read are -- a solid RAR 4, a password, a volume
  // without its set. Cheap: a few kilobytes of each.
  bool inspect(std::string& error);

  // What discover() and index() found, a line each, for the log: the
  // archives, their kind and size, how many files. A player's report is only
  // as good as this.
  const std::vector<std::string>& report() const { return report_; }

  // The index, kept beside the unpacked files so a solid archive is walked
  // once, not on every load. loadIndex() refuses an index whose archives
  // have changed size or gone.
  bool saveIndex(const std::string& file) const;
  bool loadIndex(const std::string& file);
  // The same index for the volumes discover() found now, which may be a copy
  // of the set somewhere else: each volume is matched by file name and size,
  // not by where it was, and takes its new place. The set is the same one --
  // the folder is named by the archives' identity -- so its index still holds.
  bool loadIndexFor(const std::string& file);

  // Each archive is the list of its volume files, in order.
  const std::vector<std::vector<std::string>>& archives() const { return archives_; }
  const std::vector<Entry>& entries() const { return entries_; }

  // The organ definitions inside, as stored paths.
  std::vector<std::string> definitions() const;

  // The entry at a path relative to the organ's root, matched without regard
  // to case or separator, as the loader's own file lookups are. Null if none.
  const Entry* find(const std::string& relativePath) const;

  // Unpack everything that is not audio into `dir`, keeping the archive's
  // own layout. A Hauptwerk definition stored at the top level is placed in
  // OrganDefinitions/, where the loader looks for its root.
  bool unpackSmallFiles(const std::string& dir, std::string& error) const;

  // Read the entries at `wanted` (lower-case, '/'-separated paths) of one
  // archive, in its own order, calling `deliver` with each file's bytes.
  // `deliver` returns false to stop. Safe to run for several archives at
  // once, one thread each.
  using Deliver = std::function<bool(const std::string& path, std::vector<char>&& bytes)>;
  bool read(size_t archive, const std::unordered_set<std::string>& wanted,
            const Deliver& deliver, std::string& error) const;

  // The key find() and read() use for a path.
  static std::string key(const std::string& path);

  // A stable name for this set of archives: their names and sizes. Used for
  // the folder its small files are unpacked into.
  std::string identity() const;

private:
  bool readIndex(const std::string& file, bool checkVolumes);
  std::vector<std::string> report_;
  std::vector<std::vector<std::string>> archives_;
  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t> byKey_;
};

// The folder an organ was unpacked into records the archives it came from,
// so that reopening it -- as the last organ, from favourites -- finds its
// samples again. These write and read that record.
bool writeArchiveMarker(const std::string& dir, const std::string& archivePath);
std::string readArchiveMarker(const std::string& organRoot);

}  // namespace mp
