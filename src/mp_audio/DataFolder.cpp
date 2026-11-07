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

// JUCE finds the configuration folder in user-dirs.dirs alone, so a Flatpak
// and a native install shared ~/.config/Masterpiece. Flatpak names the
// sandbox's own in XDG_CONFIG_HOME.
juce::File playerFolder() {
  const auto shared =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Masterpiece");
 #if JUCE_LINUX
  if (juce::File("/.flatpak-info").existsAsFile()) {
    const auto configHome = juce::SystemStats::getEnvironmentVariable("XDG_CONFIG_HOME", {});
    if (juce::File::isAbsolutePath(configHome)) return sandboxFolder(juce::File(configHome), shared);
  }
 #endif
  return shared;
}

}  // namespace

juce::File sandboxFolder(const juce::File& configHome, const juce::File& shared) {
  const auto own = configHome.getChildFile("Masterpiece");
  if (own.isDirectory() || !shared.isDirectory()) return own;
  const auto partial = configHome.getChildFile("Masterpiece.part");
  partial.deleteRecursively();
  static const juce::StringArray remade{"cache", "Packaged", "Portable", "masterpiece.log",
                                        "masterpiece.previous.log"};
  bool copied = partial.createDirectory().wasOk();
  for (const auto& item : shared.findChildFiles(juce::File::findFilesAndDirectories, false)) {
    if (!copied) break;
    if (remade.contains(item.getFileName())) continue;
    const auto into = partial.getChildFile(item.getFileName());
    copied = item.isDirectory() ? item.copyDirectoryTo(into) : item.copyFileTo(into);
  }
  if (copied && partial.moveFileTo(own)) return own;
  // A full disk or a folder it cannot write: this start keeps to the shared
  // folder, and the next one tries again.
  partial.deleteRecursively();
  return shared;
}

juce::File dataFolder() {
  if (chosen().getFullPathName().isNotEmpty()) return chosen();
  static const juce::File player = playerFolder();
  return player;
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
