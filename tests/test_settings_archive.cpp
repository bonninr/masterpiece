// Back up and restore (SettingsArchive.h) on a settings folder of the test's
// own: an organ's settings and all of them exported and restored byte for
// byte, one organ restored without touching another, archives that are not
// ours refused without a change, and parts copied between organs.
#include "MpTest.h"
#include "../src/mp_audio/SettingsArchive.h"

#include <map>
#include <string>

namespace {

using mp::test::Category;
namespace sa = mp::settings;

using Snapshot = std::map<juce::String, juce::MemoryBlock>;

// Every file under `root` but the backups, by relative path.
Snapshot snapshot(const juce::File& root) {
  Snapshot s;
  for (const auto& f : root.findChildFiles(juce::File::findFiles, true)) {
    const juce::String rel = f.getRelativePathFrom(root).replaceCharacter('\\', '/');
    if (rel.startsWith("backups/")) continue;
    juce::MemoryBlock data;
    f.loadFileAsData(data);
    s[rel] = data;
  }
  return s;
}

void put(const juce::File& root, const juce::String& rel, const juce::String& text) {
  const juce::File f = root.getChildFile(rel);
  f.getParentDirectory().createDirectory();
  f.replaceWithData(text.toRawUTF8(), text.getNumBytesAsUTF8());
}

// A set name with an accent, as a player types one.
const juce::String kSet = juce::String::fromUTF8("R\xc3\xa9" "cital");

// Organ B's own settings, with the mixer and voicing lines the copy below
// takes from it.
const juce::String kOrganB =
    "# Masterpiece per-organ settings\r\nstorage float\r\nroute 201 bus 1\r\n"
    "voicingadj a rank 201 2.000 0.000 0.000 0.000\r\nvoicingslot b\r\n";

juce::File freshRoot(const char* name) {
  const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
  root.deleteRecursively();
  put(root, "settings.mpglobal", "reopenlastorgan 1\r\nmemlimit 4096\r\n");
  put(root, "midi/console.mpmidi", "# console\r\nbind cc 1 7 stepper-next\r\n");
  put(root, "split-keyboard.txt", "on 1\n");
  put(root, "audio.xml", "<DEVICESETUP/>");
  put(root, "organs/Organ-A-12.mporgan",
      "# Masterpiece per-organ settings\r\nstorage int16\r\nroute 101 bus 2\r\nvoicingadj a rank 101 -3.000 0.000 0.000 0.000\r\n"
      "voicingadj a pipe 5001 1.000 2.000 0.000 0.000\r\ntranspose 2\r\n");
  put(root, "organs/Organ-A-12.mppanels", "# Masterpiece panels\npanel\nx 1\n");
  put(root, "midi/Organ-A-12.mpmidi", "# organ\r\nbind note 1 36 switch 9\r\n");
  put(root, "combinations/Organ-A-12.mpcomb", "general 1 9\n");
  put(root, "combinations/Organ-A-12." + kSet + ".mpcomb", "general 1 10\n");
  put(root, "split/Organ-A-12.mpsplit", "layout 2\n");
  put(root, "organs/Organ-B-7.mporgan", kOrganB);
  put(root, "midi/Organ-B-7.mpmidi", "# organ B\r\nbind note 2 40 switch 12\r\n");
  put(root, "combinations/Organ-B-7.mpcomb", "general 2 12\n");
  return root;
}

bool same(const Snapshot& a, const Snapshot& b) {
  if (a.size() != b.size()) return false;
  for (const auto& [k, v] : a) {
    const auto it = b.find(k);
    if (it == b.end() || it->second != v) return false;
  }
  return true;
}

class SettingsArchiveTest final : public mp::test::Test {
public:
  SettingsArchiveTest() : Test("functional.settings.archive", Category::Functional) {}
  void run() override {
    const juce::File root = freshRoot("mp-settings-archive");
    const juce::File out = root.getSiblingFile("mp-settings-archive-out");
    out.deleteRecursively();
    out.createDirectory();

    // What belongs to whom.
    const auto aFiles = sa::organFiles(root, "Organ-A-12");
    MP_CHECK(aFiles.size() == 6, "organ A's six files: " + aFiles.joinIntoString(", ").toStdString());
    MP_CHECK(sa::organKeys(root) == juce::StringArray({"Organ-A-12", "Organ-B-7"}), "the two organs");
    MP_CHECK(sa::globalFiles(root).size() == 3 && !sa::globalFiles(root).contains("audio.xml"),
             "the program's three files; the audio device stays with the computer");

    // One organ, there and back.
    const Snapshot original = snapshot(root);
    const juce::File zipA = out.getChildFile("a.zip");
    juce::String error;
    MP_CHECK(sa::exportOrgan(root, "Organ-A-12", "Organ A", "9.9.9", zipA, error), "exported: " + error.toStdString());
    sa::Contents c;
    MP_CHECK(sa::inspect(zipA, c, error) && c.scope == sa::Scope::Organ && c.organKey == "Organ-A-12" &&
                 c.organName == "Organ A" && c.program == "9.9.9" && c.paths.size() == 6,
             "and it says what it holds");

    // The player changes organ A afterwards: edits, a set removed, one added;
    // and organ B is changed too.
    put(root, "organs/Organ-A-12.mporgan", "# changed\r\n");
    root.getChildFile("combinations/Organ-A-12." + kSet + ".mpcomb").deleteFile();
    put(root, "combinations/Organ-A-12.Wedding.mpcomb", "general 3 1\n");
    put(root, "organs/Organ-B-7.mporgan", "# B changed\r\n");
    const Snapshot bChanged = snapshot(root);
    juce::File backup;
    MP_CHECK(sa::restore(zipA, root, "9.9.9", backup, error), "restored: " + error.toStdString());
    Snapshot now = snapshot(root);
    for (const auto& p : aFiles)
      MP_CHECK(now.count(p) == 1 && now[p] == original.at(p), "organ A's file back byte for byte: " + p.toStdString());
    MP_CHECK(now.count("combinations/Organ-A-12.Wedding.mpcomb") == 0, "a set the archive does not hold is gone");
    MP_CHECK(now["organs/Organ-B-7.mporgan"] == bChanged.at("organs/Organ-B-7.mporgan") &&
                 now["midi/Organ-B-7.mpmidi"] == original.at("midi/Organ-B-7.mpmidi") &&
                 now["settings.mpglobal"] == original.at("settings.mpglobal"),
             "organ B and the program's settings untouched");
    sa::Contents b;
    MP_CHECK(backup.existsAsFile() && backup.getParentDirectory() == root.getChildFile("backups") &&
                 sa::inspect(backup, b, error) && b.paths.contains("combinations/Organ-A-12.Wedding.mpcomb"),
             "what was there is in the backup, the added set too");

    // Everything, there and back.
    const Snapshot before = snapshot(root);
    const juce::File zipAll = out.getChildFile("all.zip");
    MP_CHECK(sa::exportAll(root, "9.9.9", zipAll, error), "all exported: " + error.toStdString());
    MP_CHECK(sa::inspect(zipAll, c, error) && c.scope == sa::Scope::All, "an archive of everything");
    put(root, "settings.mpglobal", "# changed\r\n");
    root.getChildFile("midi/Organ-B-7.mpmidi").deleteFile();
    put(root, "organs/Organ-C-3.mporgan", "# a new organ\r\n");
    MP_CHECK(sa::restore(zipAll, root, "9.9.9", backup, error), "all restored: " + error.toStdString());
    now = snapshot(root);
    Snapshot expected = before;
    MP_CHECK(same(now, expected), "every setting back as exported, the new organ's gone");
    MP_CHECK(now["audio.xml"] == original.at("audio.xml"), "the audio device left as it was");

    // Archives that are not ours: refused, nothing changed.
    const Snapshot steady = snapshot(root);
    const auto refused = [&](const juce::File& zip, const char* what) {
      juce::File bk;
      juce::String err;
      MP_CHECK(!sa::restore(zip, root, "9.9.9", bk, err) && err.isNotEmpty(), std::string("refused: ") + what);
      MP_CHECK(same(snapshot(root), steady) && bk == juce::File(), std::string("and nothing changed: ") + what);
    };
    const juce::File junk = out.getChildFile("junk.zip");
    junk.replaceWithText("this is no zip at all");
    refused(junk, "not a zip");
    const auto makeZip = [&](const char* name, const juce::String& manifest,
                             std::vector<std::pair<juce::String, juce::String>> entries) {
      juce::ZipFile::Builder z;
      if (manifest.isNotEmpty())
        z.addEntry(new juce::MemoryInputStream(manifest.toRawUTF8(), manifest.getNumBytesAsUTF8(), true), 9,
                   "masterpiece-settings.txt", juce::Time::getCurrentTime());
      for (auto& [entry, text] : entries)
        z.addEntry(new juce::MemoryInputStream(text.toRawUTF8(), text.getNumBytesAsUTF8(), true), 9, entry,
                   juce::Time::getCurrentTime());
      const juce::File f = out.getChildFile(name);
      f.deleteFile();
      juce::FileOutputStream os(f);
      z.writeToStream(os, nullptr);
      return f;
    };
    refused(makeZip("nomanifest.zip", {}, {{"f1", "x"}}), "a zip without a manifest");
    refused(makeZip("escape.zip", "Masterpiece settings 1\nscope all\nfile f1 ../escape.txt\n", {{"f1", "x"}}),
            "a path out of the folder");
    refused(makeZip("other.zip", "Masterpiece settings 1\nscope organ\norgan Organ-A-12\nfile f1 organs/Organ-B-7.mporgan\n",
                    {{"f1", "x"}}),
            "another organ's file in an organ's archive");
    refused(makeZip("stray.zip", "Masterpiece settings 1\nscope all\nfile f1 audio.xml\n", {{"f1", "x"}}),
            "a file that is no setting");
    refused(makeZip("missing.zip", "Masterpiece settings 1\nscope all\nfile f1 settings.mpglobal\n", {}),
            "an entry missing");
    refused(makeZip("newer.zip", "Masterpiece settings 2\nscope all\n", {}), "a newer format");
    MP_CHECK(!root.getChildFile("../escape.txt").existsAsFile(), "nothing written outside the folder");

    // Parts of organ B copied to organ A.
    // B's own settings were changed above, to prove that restoring A's
    // archive leaves B alone. The copy takes B's mixer and voicing lines, so
    // they are given back first: there is nothing to copy from "# B changed".
    put(root, "organs/Organ-B-7.mporgan", kOrganB);
    MP_CHECK(sa::copy(root, "Organ-B-7", "Organ-A-12", sa::MidiMap | sa::MixerAndVoicing, "9.9.9", backup, error),
             "copied: " + error.toStdString());
    now = snapshot(root);
    MP_CHECK(now["midi/Organ-A-12.mpmidi"] == now["midi/Organ-B-7.mpmidi"], "the MIDI map copied");
    const juce::String aSettings = root.getChildFile("organs/Organ-A-12.mporgan").loadFileAsString();
    MP_CHECK(aSettings.contains("storage int16") && aSettings.contains("transpose 2") &&
                 aSettings.contains("route 201 bus 1") && aSettings.contains("voicingslot b") &&
                 !aSettings.contains("route 101") && !aSettings.contains("pipe 5001"),
             "the mixer and voicing lines replaced, A's own loading kept");
    MP_CHECK(now["combinations/Organ-A-12.mpcomb"] == steady.at("combinations/Organ-A-12.mpcomb"),
             "combinations left alone when not asked for");
    MP_CHECK(backup.existsAsFile(), "A's settings saved before the copy");
    MP_CHECK(sa::copy(root, "Organ-B-7", "Organ-A-12", sa::Combinations, "9.9.9", backup, error) &&
                 !root.getChildFile("combinations/Organ-A-12." + kSet + ".mpcomb").existsAsFile() &&
                 root.getChildFile("combinations/Organ-A-12.mpcomb").loadFileAsString() == "general 2 12\n",
             "combinations copied, A's own sets replaced");
    const auto ids = sa::voicingIds(root, "Organ-B-7");
    MP_CHECK(ids.ranks.size() == 1 && ids.ranks[0] == 201 && ids.pipes.isEmpty(), "the ids B's voicing names");
    MP_CHECK(!sa::copy(root, "Organ-A-12", "Organ-A-12", sa::MidiMap, "9.9.9", backup, error), "not onto itself");

    // The path rules on their own.
    MP_CHECK(sa::allowedPath("combinations/Organ-A-12." + kSet + ".mpcomb", sa::Scope::Organ, "Organ-A-12"),
             "a set with an accent");
    MP_CHECK(!sa::allowedPath("combinations/Organ-A-12.mpcomb.exe", sa::Scope::All, {}), "a stray extension");
    MP_CHECK(!sa::allowedPath("midi/console.mpmidi", sa::Scope::Organ, "console"), "the console map is no organ's");
    MP_CHECK(sa::allowedPath("midi/console.mpmidi", sa::Scope::All, {}), "but is the program's");
    MP_CHECK(!sa::allowedPath("organs\\Organ-A-12.mporgan", sa::Scope::All, {}), "backslashes refused");

    root.deleteRecursively();
    out.deleteRecursively();
  }
};
static SettingsArchiveTest g_settingsArchive;

}  // namespace
