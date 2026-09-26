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

// True for a name the importer treats as an organ package: a .rar, or a
// GrandOrgue .orgue (any case).
bool isOrganArchive(const std::string& path);

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

  // The index, kept beside the unpacked files so a solid archive is walked
  // once, not on every load. loadIndex() refuses an index whose archives
  // have changed size or gone.
  bool saveIndex(const std::string& file) const;
  bool loadIndex(const std::string& file);

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
