#include "SettingsArchive.h"
#include "DataFolder.h"

namespace mp::settings {

namespace {

constexpr const char* kManifest = "masterpiece-settings.txt";
constexpr const char* kHeader = "Masterpiece settings 1";
// Far above any real settings file; a guard against an archive that is
// something else entirely.
constexpr juce::int64 kMaxFile = 16 * 1024 * 1024;
constexpr juce::int64 kMaxTotal = 128 * 1024 * 1024;

const char* const kGlobal[] = {"settings.mpglobal", "midi/console.mpmidi", "split-keyboard.txt"};

// An organ key as the program makes one: letters, digits and dashes.
bool validKey(const juce::String& k) {
  return k.isNotEmpty() && k.length() <= 80 &&
         k.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-");
}

juce::File fileAt(const juce::File& root, const juce::String& path) {
  juce::File f = root;
  for (const auto& part : juce::StringArray::fromTokens(path, "/", "")) f = f.getChildFile(part);
  return f;
}

// The key a file in one of the organ folders belongs to, or empty.
juce::String keyOf(const juce::String& folder, const juce::String& name) {
  const juce::String k = name.upToFirstOccurrenceOf(".", false, false);
  if (!validKey(k)) return {};
  const juce::String rest = name.fromFirstOccurrenceOf(".", true, false);
  if (folder == "organs" && (rest == ".mporgan" || rest == ".mppanels")) return k;
  if (folder == "midi" && rest == ".mpmidi" && k != "console") return k;
  if (folder == "split" && rest == ".mpsplit") return k;
  if (folder == "combinations" && rest.endsWith(".mpcomb")) {
    // <key>.mpcomb or <key>.<set>.mpcomb
    const juce::String set = rest.dropLastCharacters(7);
    if (set.isEmpty() || (set.startsWithChar('.') && set.length() > 1)) return k;
  }
  return {};
}

juce::String backupName(const juce::String& what) {
  return juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S") + " " + what + ".zip";
}

struct Entry {
  juce::String path;
  juce::MemoryBlock data;
};

// Writes a zip of these files (read from `root`) with its manifest.
bool writeArchive(const juce::File& root, const juce::StringArray& paths, Scope scope, const juce::String& key,
                  const juce::String& name, const juce::String& program, const juce::File& zip,
                  juce::String& error) {
  juce::String manifest;
  manifest << kHeader << "\n"
           << "scope " << (scope == Scope::Organ ? "organ" : "all") << "\n";
  if (scope == Scope::Organ) manifest << "organ " << key << "\n";
  if (name.isNotEmpty()) manifest << "name " << name.replaceCharacters("\r\n", "  ") << "\n";
  manifest << "program " << program << "\n";
  juce::ZipFile::Builder builder;
  int n = 0;
  for (const auto& p : paths) {
    const juce::File f = fileAt(root, p);
    juce::MemoryBlock data;
    if (!f.loadFileAsData(data)) {
      error = "could not read " + f.getFullPathName();
      return false;
    }
    const juce::String entry = "f" + juce::String(++n);
    manifest << "file " << entry << " " << p << "\n";
    builder.addEntry(new juce::MemoryInputStream(data, true), 9, entry, f.getLastModificationTime());
  }
  builder.addEntry(new juce::MemoryInputStream(manifest.toRawUTF8(), manifest.getNumBytesAsUTF8(), true), 9,
                   kManifest, juce::Time::getCurrentTime());
  if (!zip.getParentDirectory().createDirectory()) {
    error = "could not create " + zip.getParentDirectory().getFullPathName();
    return false;
  }
  juce::TemporaryFile tmp(zip);
  {
    juce::FileOutputStream out(tmp.getFile());
    if (!out.openedOk() || !builder.writeToStream(out, nullptr)) {
      error = "could not write " + zip.getFullPathName();
      return false;
    }
  }
  if (!tmp.overwriteTargetFileWithTemporary()) {
    error = "could not write " + zip.getFullPathName();
    return false;
  }
  return true;
}

// Reads an archive whole, checking everything first.
bool readArchive(const juce::File& zip, Contents& contents, std::vector<Entry>& entries, juce::String& error) {
  contents = {};
  entries.clear();
  if (!zip.existsAsFile()) {
    error = "no such file";
    return false;
  }
  juce::ZipFile zf(zip);
  const int manifestIndex = zf.getIndexOfFileName(kManifest);
  if (manifestIndex < 0) {
    error = "not a Masterpiece settings archive";
    return false;
  }
  const auto readEntry = [&zf](int index, juce::MemoryBlock& out) {
    const auto* e = zf.getEntry(index);
    if (e == nullptr || e->uncompressedSize > kMaxFile) return false;
    std::unique_ptr<juce::InputStream> in(zf.createStreamForEntry(index));
    if (in == nullptr) return false;
    out.reset();
    in->readIntoMemoryBlock(out);
    return static_cast<juce::int64>(out.getSize()) == e->uncompressedSize;
  };
  juce::MemoryBlock manifestData;
  if (!readEntry(manifestIndex, manifestData)) {
    error = "the archive is damaged";
    return false;
  }
  const auto lines = juce::StringArray::fromLines(manifestData.toString());
  if (lines.isEmpty() || lines[0].trim() != kHeader) {
    error = lines.isEmpty() || !lines[0].startsWith("Masterpiece settings")
                ? "not a Masterpiece settings archive"
                : "made by a newer version of Masterpiece";
    return false;
  }
  bool scoped = false;
  std::vector<std::pair<juce::String, juce::String>> files;  // entry, path
  for (int i = 1; i < lines.size(); ++i) {
    const juce::String line = lines[i].trimEnd();
    const juce::String word = line.upToFirstOccurrenceOf(" ", false, false);
    const juce::String rest = line.fromFirstOccurrenceOf(" ", false, false);
    if (word == "scope") {
      if (rest != "organ" && rest != "all") {
        error = "unknown scope " + rest;
        return false;
      }
      contents.scope = rest == "organ" ? Scope::Organ : Scope::All;
      scoped = true;
    } else if (word == "organ") {
      contents.organKey = rest;
    } else if (word == "name") {
      contents.organName = rest;
    } else if (word == "program") {
      contents.program = rest;
    } else if (word == "file") {
      files.emplace_back(rest.upToFirstOccurrenceOf(" ", false, false), rest.fromFirstOccurrenceOf(" ", false, false));
    }
  }
  if (!scoped || (contents.scope == Scope::Organ && !validKey(contents.organKey))) {
    error = "the archive does not say whose settings it holds";
    return false;
  }
  juce::int64 total = 0;
  for (const auto& [entry, path] : files) {
    if (!allowedPath(path, contents.scope, contents.organKey) || contents.paths.contains(path)) {
      error = "the archive holds a file that is not a setting: " + path;
      return false;
    }
    const int index = zf.getIndexOfFileName(entry);
    Entry e;
    e.path = path;
    if (!entry.startsWithChar('f') || index < 0 || !readEntry(index, e.data)) {
      error = "the archive is damaged: " + path;
      return false;
    }
    total += static_cast<juce::int64>(e.data.getSize());
    if (total > kMaxTotal) {
      error = "the archive is too large to be settings";
      return false;
    }
    contents.paths.add(path);
    entries.push_back(std::move(e));
  }
  return true;
}

// Saves these files, as they are now, to backups/ and says where.
bool backUp(const juce::File& root, const juce::StringArray& paths, Scope scope, const juce::String& key,
            const juce::String& program, const juce::String& what, juce::File& backup, juce::String& error) {
  backup = juce::File();
  juce::StringArray present;
  for (const auto& p : paths)
    if (fileAt(root, p).existsAsFile()) present.add(p);
  if (present.isEmpty()) return true;  // nothing there to lose
  const juce::File dir = root.getChildFile("backups");
  const juce::File target = dir.getChildFile(backupName(what)).getNonexistentSibling(false);
  if (!writeArchive(root, present, scope, key, {}, program, target, error)) return false;
  backup = target;
  return true;
}

juce::StringArray linesOf(const juce::String& text) { return juce::StringArray::fromLines(text); }

bool isMixerOrVoicing(const juce::String& line) {
  return line.startsWith("route ") || line.startsWith("voicingadj ") || line.startsWith("voicingslot ");
}

}  // namespace

juce::String programVersion() {
#ifdef MP_VERSION
  return MP_VERSION;
#else
  return "dev";
#endif
}

bool allowedPath(const juce::String& path, Scope scope, const juce::String& key) {
  if (path.isEmpty() || path.contains("..") || path.containsAnyOf("\\:*?\"<>|") || path.startsWithChar('/'))
    return false;
  for (int i = 0; i < path.length(); ++i)
    if (path[i] < 0x20) return false;
  if (scope == Scope::All)
    for (const char* g : kGlobal)
      if (path == g) return true;
  const juce::StringArray parts = juce::StringArray::fromTokens(path, "/", "");
  if (parts.size() != 2) return false;
  const juce::String k = keyOf(parts[0], parts[1]);
  if (k.isEmpty()) return false;
  return scope == Scope::All || k == key;
}

juce::StringArray organFiles(const juce::File& root, const juce::String& key) {
  juce::StringArray out;
  if (!validKey(key)) return out;
  for (const char* p : {"organs/%.mporgan", "organs/%.mppanels", "midi/%.mpmidi", "combinations/%.mpcomb"}) {
    const juce::String path = juce::String(p).replace("%", key);
    if (fileAt(root, path).existsAsFile()) out.add(path);
  }
  juce::StringArray sets;
  for (const auto& f : root.getChildFile("combinations")
                           .findChildFiles(juce::File::findFiles, false, key + ".*.mpcomb"))
    if (allowedPath("combinations/" + f.getFileName(), Scope::Organ, key)) sets.add("combinations/" + f.getFileName());
  sets.sort(false);
  out.addArray(sets);
  if (fileAt(root, "split/" + key + ".mpsplit").existsAsFile()) out.add("split/" + key + ".mpsplit");
  return out;
}

juce::StringArray globalFiles(const juce::File& root) {
  juce::StringArray out;
  for (const char* g : kGlobal)
    if (fileAt(root, g).existsAsFile()) out.add(g);
  return out;
}

juce::StringArray organKeys(const juce::File& root) {
  juce::StringArray keys;
  for (const char* folder : {"organs", "midi", "combinations", "split"})
    for (const auto& f : root.getChildFile(folder).findChildFiles(juce::File::findFiles, false))
      if (const juce::String k = keyOf(folder, f.getFileName()); k.isNotEmpty()) keys.addIfNotAlreadyThere(k);
  keys.sort(true);
  return keys;
}

bool exportOrgan(const juce::File& root, const juce::String& key, const juce::String& organName,
                 const juce::String& program, const juce::File& zip, juce::String& error) {
  const juce::StringArray paths = organFiles(root, key);
  if (paths.isEmpty()) {
    error = "this organ has no settings saved yet";
    return false;
  }
  return writeArchive(root, paths, Scope::Organ, key, organName, program, zip, error);
}

bool exportAll(const juce::File& root, const juce::String& program, const juce::File& zip, juce::String& error) {
  juce::StringArray paths = globalFiles(root);
  for (const auto& k : organKeys(root)) paths.addArray(organFiles(root, k));
  if (paths.isEmpty()) {
    error = "there are no settings saved yet";
    return false;
  }
  return writeArchive(root, paths, Scope::All, {}, {}, program, zip, error);
}

bool inspect(const juce::File& zip, Contents& out, juce::String& error) {
  std::vector<Entry> entries;
  return readArchive(zip, out, entries, error);
}

bool restore(const juce::File& zip, const juce::File& root, const juce::String& program, juce::File& backup,
             juce::String& error) {
  backup = juce::File();
  Contents c;
  std::vector<Entry> entries;
  if (!readArchive(zip, c, entries, error)) return false;

  // Everything the archive's scope covers, as it is now.
  juce::StringArray affected;
  if (c.scope == Scope::Organ) {
    affected = organFiles(root, c.organKey);
  } else {
    affected = globalFiles(root);
    for (const auto& k : organKeys(root)) affected.addArray(organFiles(root, k));
  }
  for (const auto& e : entries) affected.addIfNotAlreadyThere(e.path);
  if (!backUp(root, affected, c.scope, c.organKey, program,
              c.scope == Scope::Organ ? "before import " + c.organKey : juce::String("before import"), backup,
              error))
    return false;

  // The archive is the whole of what it covers: a file it does not hold goes.
  bool ok = true;
  for (const auto& p : affected) {
    if (c.paths.contains(p)) continue;
    const juce::File f = fileAt(root, p);
    if (f.existsAsFile() && !f.deleteFile()) ok = false;
  }
  for (const auto& e : entries) {
    const juce::File f = fileAt(root, e.path);
    if (!f.getParentDirectory().createDirectory() || !mp::writeDataFile(f, e.data.getData(), e.data.getSize()))
      ok = false;
  }
  if (!ok)
    error = "some files could not be written" +
            (backup != juce::File() ? "; what was there is saved in " + backup.getFullPathName() : juce::String());
  return ok;
}

bool copy(const juce::File& root, const juce::String& from, const juce::String& to, int parts,
          const juce::String& program, juce::File& backup, juce::String& error) {
  backup = juce::File();
  if (!validKey(from) || !validKey(to) || from == to) {
    error = "choose another organ to copy from";
    return false;
  }
  if (!backUp(root, organFiles(root, to), Scope::Organ, to, program, "before copy " + to, backup, error))
    return false;
  bool ok = true;
  if ((parts & MidiMap) != 0) {
    const juce::File src = fileAt(root, "midi/" + from + ".mpmidi");
    if (src.existsAsFile()) {
      const juce::File dst = fileAt(root, "midi/" + to + ".mpmidi");
      ok = dst.getParentDirectory().createDirectory() && src.copyFileTo(dst) && ok;
    }
  }
  if ((parts & Combinations) != 0) {
    juce::StringArray sources;
    for (const auto& p : organFiles(root, from))
      if (p.startsWith("combinations/")) sources.add(p);
    if (!sources.isEmpty()) {
      for (const auto& p : organFiles(root, to))
        if (p.startsWith("combinations/")) ok = fileAt(root, p).deleteFile() && ok;
      for (const auto& p : sources) {
        const juce::String name = p.fromFirstOccurrenceOf("/", false, false);
        const juce::File dst = root.getChildFile("combinations").getChildFile(to + name.fromFirstOccurrenceOf(".", true, false));
        ok = dst.getParentDirectory().createDirectory() && fileAt(root, p).copyFileTo(dst) && ok;
      }
    }
  }
  if ((parts & MixerAndVoicing) != 0) {
    const juce::File src = fileAt(root, "organs/" + from + ".mporgan");
    if (src.existsAsFile()) {
      const juce::File dst = fileAt(root, "organs/" + to + ".mporgan");
      const juce::String before = dst.existsAsFile() ? dst.loadFileAsString() : juce::String();
      const juce::String nl = before.contains("\r\n") || before.isEmpty() ? "\r\n" : "\n";
      juce::StringArray out;
      for (const auto& l : linesOf(before))
        if (!isMixerOrVoicing(l)) out.add(l);
      while (!out.isEmpty() && out[out.size() - 1].isEmpty()) out.remove(out.size() - 1);
      if (out.isEmpty()) out.add("# Masterpiece per-organ settings");
      for (const auto& l : linesOf(src.loadFileAsString()))
        if (isMixerOrVoicing(l)) out.add(l);
      const juce::String text = out.joinIntoString(nl) + nl;
      ok = dst.getParentDirectory().createDirectory() &&
           mp::writeDataFile(dst, text.toRawUTF8(), text.getNumBytesAsUTF8()) && ok;
    }
  }
  if (!ok)
    error = "some files could not be written" +
            (backup != juce::File() ? "; what was there is saved in " + backup.getFullPathName() : juce::String());
  return ok;
}

VoicingIds voicingIds(const juce::File& root, const juce::String& key) {
  VoicingIds ids;
  if (!validKey(key)) return ids;
  for (const auto& l : linesOf(fileAt(root, "organs/" + key + ".mporgan").loadFileAsString())) {
    const auto t = juce::StringArray::fromTokens(l, " ", "");
    if (t.size() >= 2 && t[0] == "route") ids.ranks.addIfNotAlreadyThere(t[1].getLargeIntValue());
    if (t.size() >= 4 && t[0] == "voicingadj") {
      if (t[2] == "rank") ids.ranks.addIfNotAlreadyThere(t[3].getLargeIntValue());
      if (t[2] == "pipe") ids.pipes.addIfNotAlreadyThere(t[3].getLargeIntValue());
    }
  }
  return ids;
}

}  // namespace mp::settings
