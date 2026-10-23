#include "DataFolder.h"

namespace mp {

namespace {

juce::File& chosen() {
  static juce::File folder;
  return folder;
}

// JUCE renames its temporary file over the target whether or not the write
// into it worked, so a full disk swaps in an empty file. Here the write is
// checked: the stream's status, then the size on the disk.
bool writeThrough(const juce::File& target, const void* data, size_t size) {
  juce::TemporaryFile temp(target, juce::TemporaryFile::useHiddenFile);
  {
    juce::FileOutputStream out(temp.getFile());
    if (!out.openedOk()) return false;
    if (size > 0 && !out.write(data, size)) return false;
    out.flush();
    if (out.getStatus().failed()) return false;
  }
  if (temp.getFile().getSize() != static_cast<juce::int64>(size)) return false;
  return temp.overwriteTargetFileWithTemporary();
}

}  // namespace

juce::File dataFolder() {
  if (chosen().getFullPathName().isNotEmpty()) return chosen();
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Masterpiece");
}

void setDataFolder(const juce::File& folder) { chosen() = folder; }

bool writeTextFile(const juce::File& target, const juce::String& text) {
  juce::MemoryOutputStream bytes;
  bytes.writeText(text, false, false, "\r\n");
  return writeThrough(target, bytes.getData(), bytes.getDataSize());
}

bool writeDataFile(const juce::File& target, const void* data, size_t size) {
  if (size == 0) return target.deleteFile();
  return writeThrough(target, data, size);
}

}  // namespace mp
