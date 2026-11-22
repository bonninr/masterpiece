// jOrgan organs, converted to GrandOrgue organs.
//
// A jOrgan organ is a disposition (*.disposition, see JOrganDisposition.h)
// and the SoundFonts its sounds name (see SoundFont2.h). jOrgan plays the
// SoundFonts through FluidSynth: a rank selects a preset with bank and
// program change messages and sends its notes there, swell filters send
// volume controllers, and tremulant filters change the program or send
// controllers. This module translates that into an organ Masterpiece already
// plays, and touches nothing else in the program:
//
//   * each SoundFont sample a rank uses becomes a WAV file, with the
//     sample's loop and, after a release marker, the tail FluidSynth's
//     release envelope would play;
//   * the disposition becomes a GrandOrgue definition (organ.organ) beside
//     those files: keyboards are manuals, ranks are ranks of 128 pipes --
//     each pipe the preset's region for its key, retuned from the region's
//     root -- stops are stops on the keyboard that holds them, couplers
//     couple to the keyboard that holds the stops they reach, swell filters
//     are enclosures, and a tremulant filter that switches its ranks to
//     another preset is a wave tremulant.
//
// The program then opens organ.organ as any GrandOrgue organ. The files go
// to a folder of the organ's own (jorganWorkFolder), and are made again only
// when the disposition or one of its SoundFonts changes.
#pragma once

#include <string>
#include <vector>

namespace mp {

// True for a jOrgan disposition (*.disposition), by name.
bool isJOrganDisposition(const std::string& path);

// Where a disposition's converted organ goes: a folder of its own in
// `baseFolder` (empty: a jOrgan folder in the user's data folder), named
// after the disposition and its place on disk.
std::string jorganWorkFolder(const std::string& dispositionPath, const std::string& baseFolder = "");

struct JOrganImportReport {
  std::string organText;           // the GrandOrgue definition made
  std::string organFile;           // where it is: <workFolder>/organ.organ
  std::string workFolder;
  std::string error;               // why not, when not ok
  std::vector<std::string> notes;  // what was approximated or left out
  bool ok = false;
  bool reused = false;             // the work folder was current; nothing was written
};

// Convert the disposition at `path`. `workFolder` empty: jorganWorkFolder(path).
JOrganImportReport convertJOrgan(const std::string& path, const std::string& workFolder = "");

// The same from the disposition's text: `dispositionDir` is where its
// SoundFont paths are relative to. Always writes the work folder.
JOrganImportReport convertJOrganText(const std::string& text, const std::string& dispositionDir,
                                     const std::string& workFolder);

}  // namespace mp
