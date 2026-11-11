// Settings export and import open where the last one was (#1059): the folder
// is the program's, written when it is chosen, and read at the next start.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

using mp::test::Category;

class BackupFolderTest final : public mp::test::Test {
public:
  BackupFolderTest() : Test("functional.settings.backup-folder", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-backup-folder");
    dir.deleteRecursively();
    dir.createDirectory();
    // The program's own settings file is not this test's to change.
    const auto data = dir.getChildFile("data");
    data.createDirectory();
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);
    const auto chosen = dir.getChildFile("My backups");
    chosen.createDirectory();

    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.settingsBackupDirectory() == juce::File(), "no folder before the first export");
      proc.setSettingsBackupDirectory(chosen);
      MP_CHECK(proc.globalSettingsFile().loadFileAsString().contains("backupdir " + chosen.getFullPathName()),
               "the folder is written the moment it is chosen");
    }
    {
      mp::MasterpieceProcessor again;
      again.loadGlobalDefaults();
      MP_CHECK(again.settingsBackupDirectory() == chosen, "and read back at the next start");
    }

    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static BackupFolderTest g_backupFolder;

#endif  // MP_TEST_HAS_AUDIO
