#include "DataFolder.h"

namespace mp {

namespace {

juce::File& chosen() {
  static juce::File folder;
  return folder;
}

}  // namespace

juce::File dataFolder() {
  if (chosen().getFullPathName().isNotEmpty()) return chosen();
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Masterpiece");
}

void setDataFolder(const juce::File& folder) { chosen() = folder; }

}  // namespace mp
