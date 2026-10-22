// The player's own folder: global and organ settings, MIDI maps,
// combinations, packages, the sample cache. Every path into it starts here.
//
// A test run or a render points it at a folder of its own, so what it
// changes never reaches the player's settings.
#pragma once
#include <juce_core/juce_core.h>

namespace mp {

// The application data folder's "Masterpiece", or the folder set below.
juce::File dataFolder();

// For this process from now on. Before anything reads or writes a setting;
// an empty File goes back to the player's own folder.
void setDataFolder(const juce::File& folder);

}  // namespace mp
