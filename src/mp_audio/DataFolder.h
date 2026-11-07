// The player's own folder: global and organ settings, MIDI maps,
// combinations, packages, the sample cache. Every path into it starts here.
//
// A test run or a render points it at a folder of its own, so what it
// changes never reaches the player's settings.
#pragma once
#include <juce_core/juce_core.h>

namespace mp {

// The application data folder's "Masterpiece", or the folder set below. In a
// Flatpak, the sandbox's own configuration folder's "Masterpiece".
juce::File dataFolder();

// A Flatpak's own folder, "Masterpiece" in its configuration folder
// (~/.var/app/<id>/config), apart from a native install's (#891). The first
// time, the settings in the shared folder come along: everything but the
// sample cache, the unpacked packages, the portable copies and the logs, which
// the player makes again. Copied beside it and renamed at the end, so a copy
// cut short is made again at the next start; until then the shared folder
// serves.
juce::File sandboxFolder(const juce::File& configHome, const juce::File& shared);

// For this process from now on. Before anything reads or writes a setting;
// an empty File goes back to the player's own folder.
void setDataFolder(const juce::File& folder);

// A whole file written afresh, the bytes File::replaceWithText and
// replaceWithData write, through a temporary file beside it. The file is
// replaced only once every byte is on the disk: a full disk or a failed write
// leaves the old file as it was, removes the temporary one, and returns false
// (#519).
bool writeTextFile(const juce::File& target, const juce::String& text);
bool writeDataFile(const juce::File& target, const void* data, size_t size);

}  // namespace mp
