// Console profiles on disk and on the loaded organ (#866). The matching
// itself is JUCE-free, in mp_control/ConsoleProfile.h; this is where the
// profiles are kept, how the loaded organ's controls are read for it, and how
// the matches become the organ's own MIDI mappings.
//
// The profiles live in the player's data folder, in "consoles", one file
// each. The one in use is named in "consoles/active.txt". Each organ keeps
// the stamp of the profile it was last mapped from, so a profile is offered
// to an organ once, and again after it changes.
#pragma once
#include <juce_core/juce_core.h>

#include <string>
#include <vector>

#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_control/ConsoleProfile.h"

namespace mp::ui {

juce::File consoleProfilesFolder();
// Every profile on disk, by name.
juce::Array<juce::File> consoleProfileFiles();
// The profile in use; none when the file is empty or gone.
juce::File activeConsoleProfile();
void setActiveConsoleProfile(const juce::File& file);
bool loadConsoleProfile(const juce::File& file, ConsoleProfile& out);
bool saveConsoleProfile(const juce::File& file, const ConsoleProfile& profile);
// A file for a new profile of this name, not yet on disk.
juce::File newConsoleProfileFile(const juce::String& name);

// One control of the loaded organ, as a profile's tab is matched to it.
struct OrganControl {
  ConsoleTab tab;       // kind, name, keyboards; tab.switchId is what MIDI works
  std::string section;  // the panel section it shows in: "Great"
  bool mapped = false;  // the organ's own map already has a binding for it
};
// The organ's stops, couplers, tremulants and pistons, in the order of the
// stop list, each once.
std::vector<OrganControl> organControls(const MasterpieceProcessor& p);

// The matches worth offering: a tab that has a message, onto a control the
// player has not mapped, with a message the organ's map does not use yet.
std::vector<ProfileMatch> profileMatches(const MasterpieceProcessor& p, const ConsoleProfile& profile,
                                         const std::vector<OrganControl>& controls);
// The chosen matches made the organ's own mappings, and saved. Returns how
// many controls were mapped.
int applyProfileMatches(MasterpieceProcessor& p, const ConsoleProfile& profile,
                        const std::vector<OrganControl>& controls, const std::vector<ProfileMatch>& chosen);

// The mapped controls of the loaded organ as a profile's tabs, each with
// the messages it has: a console mapped by hand once becomes the profile.
// Controls already in `into` by name and keyboard are left out.
int addOrganToProfile(const MasterpieceProcessor& p, ConsoleProfile& into);

// The stamp of the profile this organ was last mapped from, kept with the
// organ's other files.
juce::String organProfileStamp(const MasterpieceProcessor& p);
void setOrganProfileStamp(const MasterpieceProcessor& p, const juce::String& stamp);
// The stamp an organ keeps for a profile: its file and its contents.
juce::String profileStampFor(const juce::File& file, const ConsoleProfile& profile);

}  // namespace mp::ui
