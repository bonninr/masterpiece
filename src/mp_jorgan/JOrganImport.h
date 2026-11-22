// jOrgan organs, read by converting them, as GrandOrgue organs are.
//
// A jOrgan organ is a disposition (*.disposition, see JOrganDisposition.h)
// and the SoundFonts its sounds name (see SoundFont2.h). jOrgan plays the
// SoundFonts through FluidSynth: a rank selects a preset with bank and
// program change messages, swell filters send volume controllers, and
// tremulant filters change the program or send controllers.
//
// The loader reads none of that. The disposition is translated into the
// Hauptwerk definition the loader already reads -- in memory, never written
// as a file the program opens -- through a plain description of the organ
// (JOrganOrgan.h, written out by JOrganHauptwerk.h):
//
//   * keyboards are keyboards and divisions; stops, couplers, swells and
//     tremulants keep their own switches;
//   * a rank is 128 pipes, each the preset's region for its key, retuned
//     from the region's root;
//   * the SoundFont samples the ranks use are written once as WAV files,
//     each with its loop and, after a release marker, the tail of the
//     SoundFont's release envelope, in a folder of the organ's own: the
//     organ's root, where its samples are found.
//
// The folder and the translated definition stay until the disposition or
// one of its SoundFonts changes.
#pragma once

#include <string>
#include <vector>

namespace mp {

// True for a jOrgan disposition (*.disposition), by name.
bool isJOrganDisposition(const std::string& path);

// The folder for a disposition's samples: a folder of its own in
// `baseFolder` (empty: the jOrgan folder of Masterpiece's settings folder),
// named after the disposition and its place on disk.
std::string jorganWorkFolder(const std::string& dispositionPath, const std::string& baseFolder = "");

struct JOrganImportReport {
  std::string xml;                 // the Hauptwerk definition, when ok
  std::string name;                // the organ's name
  long long id = 0;                // its id, from the disposition's bytes
  std::string workFolder;          // where its samples are: the organ root
  std::string error;               // why not, when not ok
  std::vector<std::string> notes;  // what was approximated or left out
  bool ok = false;
  bool reused = false;             // the folder was current; nothing was written
};

// Convert the disposition at `path`. `workFolder` empty: jorganWorkFolder(path).
// `headerOnly`: the name and id alone, no conversion.
JOrganImportReport convertJOrgan(const std::string& path, const std::string& workFolder = "",
                                 bool headerOnly = false);

// The same from the disposition's text: `dispositionDir` is where its
// SoundFont paths are relative to; `fallbackName` names an organ whose
// console has no name. Always writes the samples.
JOrganImportReport convertJOrganText(const std::string& text, const std::string& dispositionDir,
                                     const std::string& workFolder, const std::string& fallbackName = "");

}  // namespace mp
