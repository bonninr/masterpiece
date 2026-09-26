// GrandOrgue organ definitions, read by converting them.
//
// A GrandOrgue sample set describes its organ in an INI-style .organ file:
// manuals, stops, ranks of pipes, couplers, tremulants, enclosures, and the
// panels they are drawn on. Masterpiece does not load that format itself.
// This translates it into the Hauptwerk definition the loader already reads
// -- as an XML document in memory, never written to disk -- and the organ is
// then loaded exactly like any other. Nothing downstream knows it came from
// a .organ file.
//
// The translation takes GrandOrgue's own semantics wherever the two formats
// differ, and says so where Hauptwerk has no equivalent:
//
//   * a manual is both a keyboard and a division, so it becomes one of each,
//     joined by a key action;
//   * a pipe's release is, by default, the tail of its own attack file,
//     found at the file's cue point: it becomes a release sample on the same
//     file that starts at that marker;
//   * a stop that carries its pipes itself, as older sets do, gets a rank
//     made from them;
//   * a synthesised tremulant modulates amplitude only, as GrandOrgue's does;
//   * an enclosure lowers the level of the pipes on its windchest groups,
//     with no filter, as GrandOrgue's does.
//
// GrandOrgue has no organ id, so one is made from a hash of the file: the
// same file is always the same organ, with its own settings.
#pragma once

#include <string>
#include <vector>

namespace mp {

// True for a GrandOrgue definition (*.organ), by name.
bool isGrandOrgueDefinition(const std::string& path);

struct GrandOrgueImportReport {
  std::string xml;                 // the Hauptwerk definition, when ok
  std::string error;               // why not, when not ok
  std::vector<std::string> notes;  // what was approximated or left out
  bool ok = false;
};

// Convert the .organ file at `path`. Sample and image paths stay relative to
// the file's own folder, which the loader is given as the organ's root.
GrandOrgueImportReport convertGrandOrgue(const std::string& path);

// The same, from the file's text. `organRoot` is the folder its paths are
// relative to; the console layout needs it to measure the set's bitmaps, and
// without it only what the text states is drawn.
GrandOrgueImportReport convertGrandOrgueText(const std::string& text,
                                             const std::string& organRoot = "");

}  // namespace mp
