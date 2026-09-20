#include "OrganDialog.h"
#include "Ui.h"
#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_core/OdfLoader.h"
#include <pugixml.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <regex>

namespace mp::ui {

juce::String humaniseEta(double secs) {
  if (secs < 45.0) return "less than a minute";
  const int mins = static_cast<int>(secs / 60.0 + 0.5);
  if (mins <= 1) return "about a minute";
  return "about " + juce::String(mins) + " minutes";
}

juce::String formatByteSize(juce::int64 bytes) {
  if (bytes <= 0) return "0 B";
  if (bytes < 1024) return juce::String(bytes) + " B";
  if (bytes < 1024 * 1024)
    return juce::String(bytes / 1024.0, 1) + " KB";
  if (bytes < 1024 * 1024 * 1024)
    return juce::String(bytes / (1024.0 * 1024.0), 1) + " MB";
  return juce::String(bytes / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
}

juce::int64 computeDirectorySize(const juce::File& dir) {
  if (!dir.isDirectory()) return 0;
  static std::unordered_map<std::string, std::pair<juce::int64, juce::int64>> cache;
  const auto path = dir.getFullPathName().toStdString();
  const auto mtime = dir.getLastModificationTime().toMilliseconds();
  auto it = cache.find(path);
  if (it != cache.end() && it->second.first == mtime) {
    return it->second.second;
  }
  juce::int64 total = 0;
  for (const auto& iter : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles)) {
    total += iter.getFile().getSize();
  }
  cache[path] = {mtime, total};
  return total;
}

static juce::String packageDirName(uint32_t packageId) {
  juce::String s = juce::String(packageId);
  while (s.length() < 6) s = "0" + s;
  return s;
}

OrganEntry getOrganDetails(const juce::File& odfFile, const MasterpieceProcessor& proc) {
  OrganEntry entry;
  entry.file = odfFile;
  entry.exists = odfFile.existsAsFile();
  entry.isCurrent = (proc.loadedOdf() == odfFile);
  entry.odfSizeBytes = entry.exists ? odfFile.getSize() : 0;
  entry.diskSpaceBytes = entry.odfSizeBytes;

  if (!entry.exists) {
    entry.name = odfFile.getFileNameWithoutExtension();
    return entry;
  }

  pugi::xml_document doc;
  const auto fileSize = odfFile.getSize();
  bool parsedFullDoc = false;
  if (fileSize > 0) {
    std::unique_ptr<juce::FileInputStream> stream(odfFile.createInputStream());
    if (stream != nullptr) {
      juce::MemoryBlock block;
      if (fileSize <= 64 * 1024 * 1024) {
        block.setSize(static_cast<size_t>(fileSize));
        stream->read(block.getData(), static_cast<int>(fileSize));
      } else {
        const int bytesToRead = 16 * 1024 * 1024;
        block.setSize(static_cast<size_t>(bytesToRead));
        stream->read(block.getData(), bytesToRead);
      }

      pugi::xml_parse_result res = doc.load_buffer(block.getData(), block.getSize(),
                                                   pugi::parse_default, pugi::encoding_utf8);
      if (!res) {
        pugi::xml_document docLatin;
        res = docLatin.load_buffer(block.getData(), block.getSize(),
                                   pugi::parse_default, pugi::encoding_latin1);
        if (res) {
          doc.reset(docLatin);
          parsedFullDoc = (fileSize <= 64 * 1024 * 1024);
        }
      } else {
        parsedFullDoc = (fileSize <= 64 * 1024 * 1024);
      }
    }
  }

  pugi::xml_node hwNode = doc.child("Hauptwerk");
  for (pugi::xml_node list : hwNode.children("ObjectList")) {
    const std::string objType = list.attribute("ObjectType").value();
    if (objType == "_General" || objType == "_general") {
      for (pugi::xml_node g : list.children()) {
        if (entry.name.isEmpty()) {
          pugi::xml_node n = g.child("Identification_Name");
          if (!n) n = g.child("Identification_OrganName");
          if (!n) n = g.child("Name");
          if (n && n.text().as_string()[0] != '\0')
            entry.name = juce::String::fromUTF8(n.text().as_string()).trim();
        }
        if (entry.uniqueOrganId.isEmpty()) {
          pugi::xml_node uid = g.child("Identification_UniqueOrganID");
          if (!uid) uid = g.child("UniqueOrganID");
          if (uid && uid.text().as_string()[0] != '\0')
            entry.uniqueOrganId = juce::String::fromUTF8(uid.text().as_string()).trim();
        }
      }
    }
  }

  if (entry.name.isEmpty()) {
    entry.name = odfFile.getFileNameWithoutExtension();
  }

  std::string rootStr = mp::deriveOrganRoot(odfFile.getFullPathName().toStdString());
  juce::File organRoot(rootStr);
  entry.organRootDir = organRoot;

  std::unordered_map<uint32_t, OrganPackageInfo> pkgMap;
  for (pugi::xml_node list : hwNode.children("ObjectList")) {
    const std::string objType = list.attribute("ObjectType").value();
    if (objType == "RequiredInstallationPackage" || objType == "_RequiredInstallationPackage") {
      for (pugi::xml_node row : list.children()) {
        pugi::xml_node idNode = row.child("InstallationPackageID");
        if (!idNode) idNode = row.child("PackageID");
        if (!idNode) idNode = row.child("Package_PackageID");
        if (idNode) {
          uint32_t pkgId = static_cast<uint32_t>(idNode.text().as_uint(0));
          if (pkgId > 0) {
            OrganPackageInfo pkg;
            pkg.packageId = pkgId;
            pugi::xml_node nameNode = row.child("Name");
            if (!nameNode) nameNode = row.child("PackageName");
            if (!nameNode) nameNode = row.child("Identification_Name");
            if (nameNode) pkg.name = juce::String::fromUTF8(nameNode.text().as_string()).trim();

            pugi::xml_node supNode = row.child("SupplierName");
            if (!supNode) supNode = row.child("Supplier");
            if (supNode) pkg.supplierName = juce::String::fromUTF8(supNode.text().as_string()).trim();

            pkgMap[pkgId] = std::move(pkg);
          }
        }
      }
    }
  }

  if (pkgMap.empty()) {
    for (pugi::xml_node list : hwNode.children("ObjectList")) {
      const std::string objType = list.attribute("ObjectType").value();
      if (objType == "Sample" || objType == "sample") {
        for (pugi::xml_node s : list.children()) {
          pugi::xml_node idNode = s.child("InstallationPackageID");
          if (idNode) {
            uint32_t pkgId = static_cast<uint32_t>(idNode.text().as_uint(0));
            if (pkgId > 0 && pkgMap.find(pkgId) == pkgMap.end()) {
              OrganPackageInfo pkg;
              pkg.packageId = pkgId;
              pkg.name = "Sample Package " + juce::String(pkgId);
              pkgMap[pkgId] = std::move(pkg);
            }
          }
        }
      }
    }
  }

  // Fast text regex fallback for ODF files where RequiredInstallationPackage was not in DOM
  if (pkgMap.empty() && entry.exists) {
    const auto fullText = odfFile.loadFileAsString();
    const std::string textStd = fullText.toStdString();

    static const std::regex reqPkgRegex(
        "<RequiredInstallationPackage\\b[^>]*>([\\s\\S]*?)</RequiredInstallationPackage>",
        std::regex_constants::icase);
    auto words_begin = std::sregex_iterator(textStd.begin(), textStd.end(), reqPkgRegex);
    auto words_end = std::sregex_iterator();

    static const std::regex idRegex("<(?:InstallationPackageID|PackageID)>(\\d+)</", std::regex_constants::icase);
    static const std::regex nameRegex("<(?:Name|PackageName|Identification_Name)>([^<]+)</", std::regex_constants::icase);
    static const std::regex supRegex("<(?:SupplierName|Supplier)>([^<]+)</", std::regex_constants::icase);

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
      std::smatch match = *i;
      std::string chunk = match.str(1);

      std::smatch idMatch;
      if (std::regex_search(chunk, idMatch, idRegex)) {
        uint32_t pkgId = static_cast<uint32_t>(std::stoul(idMatch.str(1)));
        if (pkgId > 0 && pkgMap.find(pkgId) == pkgMap.end()) {
          OrganPackageInfo pkg;
          pkg.packageId = pkgId;
          std::smatch nameMatch;
          if (std::regex_search(chunk, nameMatch, nameRegex)) {
            pkg.name = juce::String::fromUTF8(nameMatch.str(1).c_str()).trim();
          }
          std::smatch supMatch;
          if (std::regex_search(chunk, supMatch, supRegex)) {
            pkg.supplierName = juce::String::fromUTF8(supMatch.str(1).c_str()).trim();
          }
          pkgMap[pkgId] = std::move(pkg);
        }
      }
    }

    if (pkgMap.empty()) {
      auto s_begin = std::sregex_iterator(textStd.begin(), textStd.end(), idRegex);
      for (std::sregex_iterator i = s_begin; i != words_end; ++i) {
        std::smatch match = *i;
        uint32_t pkgId = static_cast<uint32_t>(std::stoul(match.str(1)));
        if (pkgId > 0 && pkgMap.find(pkgId) == pkgMap.end()) {
          OrganPackageInfo pkg;
          pkg.packageId = pkgId;
          pkg.name = "Sample Package " + juce::String(pkgId);
          pkgMap[pkgId] = std::move(pkg);
        }
      }
    }
  }

  const auto dataDir = MasterpieceProcessor::dataDirectory();
  for (auto& [id, pkg] : pkgMap) {
    juce::String digits = packageDirName(id);
    juce::File pkgDir = organRoot.getChildFile("OrganInstallationPackages").getChildFile(digits);
    if (!pkgDir.isDirectory()) {
      pkgDir = dataDir.getChildFile("OrganInstallationPackages").getChildFile(digits);
    }
    pkg.directory = pkgDir;
    pkg.isInstalled = pkgDir.isDirectory();
    if (pkg.isInstalled) {
      pkg.diskSizeBytes = computeDirectorySize(pkgDir);
      entry.diskSpaceBytes += pkg.diskSizeBytes;
    }
    entry.packages.push_back(pkg);
  }

  // Also check for direct PipeSamples folder if no installation packages were found
  if (pkgMap.empty() && entry.exists) {
    juce::File pipeSamples = organRoot.getChildFile("PipeSamples");
    if (!pipeSamples.isDirectory()) {
      pipeSamples = entry.file.getParentDirectory().getChildFile("PipeSamples");
    }
    if (pipeSamples.isDirectory()) {
      const auto psSize = computeDirectorySize(pipeSamples);
      entry.diskSpaceBytes += psSize;
    }
  }

  std::sort(entry.packages.begin(), entry.packages.end(), [](const OrganPackageInfo& a, const OrganPackageInfo& b) {
    return a.packageId < b.packageId;
  });

  return entry;
}

juce::File getOrganAudioStatCacheFile(const OrganEntry& entry) {
  const auto cacheDir = MasterpieceProcessor::dataDirectory().getChildFile("cache");
  const juce::String statKey = entry.uniqueOrganId.isNotEmpty()
                                   ? entry.uniqueOrganId
                                   : juce::String::toHexString(entry.file.getFullPathName().hashCode64());
  return cacheDir.getChildFile(statKey + ".mpstats");
}

bool hasCachedOrganAudioStat(const OrganEntry& entry) {
  const auto cacheFile = getOrganAudioStatCacheFile(entry);
  if (!cacheFile.existsAsFile()) return false;
  const auto lines = juce::StringArray::fromLines(cacheFile.loadFileAsString());
  return lines.size() >= 7 && lines[0].getLargeIntValue() > 0;
}

OrganAudioStat computeOrganAudioStat(
    const OrganEntry& entry,
    const MasterpieceProcessor& proc,
    std::function<void(double progress, int current, int total)> progressCallback,
    std::atomic<bool>* cancelFlag) {
  OrganAudioStat stat;
  if (!entry.exists) return stat;

  const auto cacheFile = getOrganAudioStatCacheFile(entry);
  if (cacheFile.existsAsFile()) {
    const auto lines = juce::StringArray::fromLines(cacheFile.loadFileAsString());
    if (lines.size() >= 7) {
      stat.totalAudioFrames = lines[0].getLargeIntValue();
      stat.attackFrames = lines[1].getLargeIntValue();
      stat.releaseFrames = lines[2].getLargeIntValue();
      stat.attackLoopFrames = lines[3].getLargeIntValue();
      stat.attackCount = lines[4].getIntValue();
      stat.releaseCount = lines[5].getIntValue();
      stat.rawPcmBytes = lines[6].getLargeIntValue();
      stat.hasStats = (stat.totalAudioFrames > 0);
      if (stat.hasStats) {
        if (progressCallback) progressCallback(1.0, 1, 1);
        return stat;
      }
    }
  }

  // Scan installed packages or sample directories for WAV files
  std::vector<juce::File> sampleDirs;
  for (const auto& pkg : entry.packages) {
    if (pkg.isInstalled && pkg.directory.isDirectory()) {
      sampleDirs.push_back(pkg.directory);
    }
  }
  if (sampleDirs.empty()) {
    juce::File pipeSamples = entry.organRootDir.getChildFile("PipeSamples");
    if (!pipeSamples.isDirectory()) {
      pipeSamples = entry.file.getParentDirectory().getChildFile("PipeSamples");
    }
    if (pipeSamples.isDirectory()) {
      sampleDirs.push_back(pipeSamples);
    }
  }

  if (sampleDirs.empty()) {
    return stat;
  }

  juce::Array<juce::File> wavFiles;
  for (const auto& sDir : sampleDirs) {
    if (cancelFlag && cancelFlag->load()) return stat;
    for (const auto& iter : juce::RangedDirectoryIterator(sDir, true, "*.wav", juce::File::findFiles)) {
      if (cancelFlag && cancelFlag->load()) return stat;
      wavFiles.add(iter.getFile());
    }
  }

  const int totalFiles = wavFiles.size();
  if (totalFiles == 0) return stat;

  for (int fIdx = 0; fIdx < totalFiles; ++fIdx) {
    if (cancelFlag && cancelFlag->load()) return stat;

    if (progressCallback && (fIdx % 25 == 0 || fIdx == totalFiles - 1)) {
      const double p = static_cast<double>(fIdx + 1) / static_cast<double>(totalFiles);
      progressCallback(p, fIdx + 1, totalFiles);
    }

    const auto& f = wavFiles[fIdx];
      const auto pathStr = f.getFullPathName().toLowerCase();
      const bool isRelease = pathStr.contains("/r") || pathStr.contains("\\r");

      std::unique_ptr<juce::FileInputStream> stream(f.createInputStream());
      if (stream == nullptr || stream->getTotalLength() < 44) continue;

      char riffHdr[12];
      if (stream->read(riffHdr, 12) < 12) continue;
      if (std::memcmp(riffHdr, "RIFF", 4) != 0 || std::memcmp(riffHdr + 8, "WAVE", 4) != 0) continue;

      uint16_t numChannels = 2;
      uint16_t bitsPerSample = 24;
      juce::int64 dataBytes = 0;
      bool hasLoop = false;
      uint32_t maxLoopEnd = 0;

      while (!stream->isExhausted()) {
        char chunkHdr[8];
        if (stream->read(chunkHdr, 8) < 8) break;
        const uint32_t chunkSize = juce::ByteOrder::littleEndianInt(chunkHdr + 4);

        if (std::memcmp(chunkHdr, "fmt ", 4) == 0) {
          const int toRead = std::min(static_cast<int>(chunkSize), 16);
          char fmtBuf[16] = {0};
          if (stream->read(fmtBuf, toRead) < toRead) break;
          numChannels = juce::ByteOrder::littleEndianShort(fmtBuf + 2);
          bitsPerSample = juce::ByteOrder::littleEndianShort(fmtBuf + 14);
          if (chunkSize > 16) stream->skipNextBytes(chunkSize - 16);
        } else if (std::memcmp(chunkHdr, "data", 4) == 0) {
          dataBytes = chunkSize;
          stream->skipNextBytes(chunkSize);
        } else if (std::memcmp(chunkHdr, "smpl", 4) == 0) {
          if (chunkSize >= 36) {
            juce::MemoryBlock smplData(chunkSize);
            if (stream->read(smplData.getData(), static_cast<int>(chunkSize)) == static_cast<int>(chunkSize)) {
              const char* p = static_cast<const char*>(smplData.getData());
              const uint32_t numLoops = juce::ByteOrder::littleEndianInt(p + 28);
              if (numLoops > 0 && chunkSize >= 36 + numLoops * 24) {
                hasLoop = true;
                for (uint32_t l = 0; l < numLoops; ++l) {
                  const uint32_t loopEnd = juce::ByteOrder::littleEndianInt(p + 36 + l * 24 + 12);
                  if (loopEnd > maxLoopEnd) maxLoopEnd = loopEnd;
                }
              }
            }
          } else {
            stream->skipNextBytes(chunkSize);
          }
        } else {
          stream->skipNextBytes(chunkSize);
        }

        if (chunkSize % 2 != 0) {
          stream->skipNextBytes(1);
        }
      }

      const int bytesPerFrame = (numChannels > 0 ? numChannels : 2) * (bitsPerSample > 0 ? ((bitsPerSample + 7) / 8) : 3);
      const juce::int64 frames = bytesPerFrame > 0 ? (dataBytes / bytesPerFrame) : 0;

      stat.rawPcmBytes += dataBytes;
      stat.totalAudioFrames += frames;

      if (isRelease) {
        stat.releaseCount++;
        stat.releaseFrames += frames;
      } else {
        stat.attackCount++;
        stat.attackFrames += frames;
        if (hasLoop && maxLoopEnd > 0) {
          stat.attackLoopFrames += std::min(static_cast<juce::int64>(maxLoopEnd), frames);
        } else {
          stat.attackLoopFrames += frames;
        }
      }
    }

  stat.hasStats = (stat.totalAudioFrames > 0);

  if (stat.hasStats && (!cancelFlag || !cancelFlag->load())) {
    cacheFile.getParentDirectory().createDirectory();
    juce::String out;
    out << juce::String(stat.totalAudioFrames) << "\n"
        << juce::String(stat.attackFrames) << "\n"
        << juce::String(stat.releaseFrames) << "\n"
        << juce::String(stat.attackLoopFrames) << "\n"
        << juce::String(stat.attackCount) << "\n"
        << juce::String(stat.releaseCount) << "\n"
        << juce::String(stat.rawPcmBytes) << "\n";
    cacheFile.replaceWithText(out);
  }

  if (progressCallback && (!cancelFlag || !cancelFlag->load())) {
    progressCallback(1.0, totalFiles, totalFiles);
  }

  return stat;
}

void triggerBackgroundAudioStatPrecomputation(const juce::File& odfFile) {
  if (!odfFile.existsAsFile()) return;
  juce::Thread::launch([odfFile] {
    MasterpieceProcessor dummyProc;
    const auto entry = getOrganDetails(odfFile, dummyProc);
    if (entry.exists && !hasCachedOrganAudioStat(entry)) {
      computeOrganAudioStat(entry, dummyProc);
    }
  });
}

void triggerDirectoryAudioStatPrecomputation(const juce::File& dir) {
  if (!dir.isDirectory()) return;
  juce::Thread::launch([dir] {
    MasterpieceProcessor dummyProc;
    juce::Array<juce::File> odfs;
    dir.findChildFiles(odfs, juce::File::findFiles, true, "*.Organ_Hauptwerk_xml");
    dir.findChildFiles(odfs, juce::File::findFiles, true, "*.CustomOrgan_Hauptwerk_xml");
    for (const auto& odf : odfs) {
      const auto entry = getOrganDetails(odf, dummyProc);
      if (entry.exists && !hasCachedOrganAudioStat(entry)) {
        computeOrganAudioStat(entry, dummyProc);
      }
    }
  });
}

juce::int64 estimateRamFootprintBytes(const OrganAudioStat& stat, const OrganAudioConfig& cfg) {
  if (!stat.hasStats || stat.totalAudioFrames == 0) {
    return 0;
  }

  const int bytesPerSample = (cfg.storage == SampleStorage::Int16 ? 2 : (cfg.storage == SampleStorage::Float32 ? 4 : 3));
  const int channels = cfg.mono ? 1 : 2;
  const int bytesPerFrame = bytesPerSample * channels;

  juce::int64 residentAttackFrames = stat.attackFrames;
  if (cfg.preloadHeadFrames > 0) {
    if (stat.attackCount > 0) {
      residentAttackFrames = stat.attackLoopFrames + static_cast<juce::int64>(stat.attackCount) * cfg.preloadHeadFrames;
      residentAttackFrames = std::min(residentAttackFrames, stat.attackFrames);
    }
  }

  juce::int64 residentReleaseFrames = stat.releaseFrames;
  if (cfg.streamReleases) {
    const juce::int64 streamHead = (cfg.streamHeadFrames > 0 ? cfg.streamHeadFrames : 44100);
    residentReleaseFrames = static_cast<juce::int64>(stat.releaseCount) * streamHead;
    residentReleaseFrames = std::min(residentReleaseFrames, stat.releaseFrames);
  }

  const juce::int64 sampleRam = (residentAttackFrames + residentReleaseFrames) * bytesPerFrame;
  constexpr juce::int64 engineOverheadBytes = 128 * 1024 * 1024;
  return sampleRam + engineOverheadBytes;
}

OrganAudioConfig loadOrganAudioConfig(const MasterpieceProcessor& proc, const juce::File& odf) {
  OrganAudioConfig cfg;
  auto readFromLines = [&](const juce::String& content) {
    for (const auto& line : juce::StringArray::fromLines(content)) {
      if (line.trim().isEmpty() || line.trimStart().startsWith("#")) continue;
      const auto key = line.upToFirstOccurrenceOf(" ", false, false).trim();
      const auto val = line.fromFirstOccurrenceOf(" ", false, false).trim();
      const bool on = val.getIntValue() != 0;
      if (key == "storage") {
        const int s = val.getIntValue();
        cfg.storage = (s == 16 ? SampleStorage::Int16 : (s == 32 ? SampleStorage::Float32 : SampleStorage::Int24));
      } else if (key == "mono") {
        cfg.mono = on;
      } else if (key == "rate") {
        cfg.sampleRate = val.getDoubleValue();
      } else if (key == "cache") {
        const int c = val.getIntValue();
        cfg.cacheMode = (c == 0 ? SampleLibrary::CacheMode::Off : (c == 2 ? SampleLibrary::CacheMode::PerOrgan : SampleLibrary::CacheMode::Single));
      } else if (key == "stream") {
        cfg.streamReleases = on;
      } else if (key == "streamhead") {
        cfg.streamHeadFrames = val.getLargeIntValue();
      } else if (key == "preload") {
        cfg.preloadHeadFrames = val.getLargeIntValue();
      } else if (key == "root") {
        cfg.organRootOverride = val.isEmpty() ? juce::File() : juce::File(val);
      } else if (key == "simple") {
        cfg.engineSwitch.simpleWavOnly = on;
      } else if (key == "wind") {
        cfg.engineSwitch.enableWindModel = on;
      } else if (key == "tremulant") {
        cfg.engineSwitch.enableTremulant = on;
      } else if (key == "enclosure") {
        cfg.engineSwitch.enableEnclosure = on;
      } else if (key == "voicing") {
        cfg.engineSwitch.enableVoicing = on;
      } else if (key == "originalpitch") {
        cfg.engineSwitch.playAtOriginalOrganPitch = on;
      }
    }
  };

  const auto gf = proc.globalSettingsFile();
  if (gf.existsAsFile()) {
    readFromLines(gf.loadFileAsString());
  }

  if (odf.existsAsFile()) {
    const auto of = proc.settingsFileFor(odf);
    if (of.existsAsFile()) {
      readFromLines(of.loadFileAsString());
    }
  }
  return cfg;
}

bool saveOrganAudioConfig(MasterpieceProcessor& proc, const juce::File& odf, const OrganAudioConfig& cfg) {
  if (odf.getFullPathName().isEmpty()) return false;
  juce::File targetFile = proc.settingsFileFor(odf);
  if (!targetFile.existsAsFile()) {
    const auto dir = MasterpieceProcessor::dataDirectory().getChildFile("organs");
    dir.createDirectory();
    targetFile = dir.getChildFile(juce::String(MasterpieceProcessor::organKeyFor(odf)) + ".mporgan");
  }

  juce::StringArray preservedLines;
  if (targetFile.existsAsFile()) {
    for (const auto& line : juce::StringArray::fromLines(targetFile.loadFileAsString())) {
      if (line.trim().isEmpty() || line.trimStart().startsWith("#")) continue;
      const auto key = line.upToFirstOccurrenceOf(" ", false, false).trim();
      if (key == "storage" || key == "mono" || key == "rate" || key == "cache" ||
          key == "stream" || key == "streamhead" || key == "preload" || key == "root" ||
          key == "simple" || key == "wind" || key == "tremulant" || key == "enclosure" ||
          key == "voicing" || key == "originalpitch")
        continue;
      preservedLines.add(line);
    }
  }

  targetFile.getParentDirectory().createDirectory();

  juce::String text = "# Masterpiece per-organ settings\n";
  text << "storage " << (cfg.storage == SampleStorage::Int16 ? 16 : (cfg.storage == SampleStorage::Int24 ? 24 : 32)) << "\n";
  text << "mono " << (cfg.mono ? 1 : 0) << "\n";
  text << "rate " << juce::String(cfg.sampleRate, 0) << "\n";
  text << "cache " << static_cast<int>(cfg.cacheMode) << "\n";
  text << "stream " << (cfg.streamReleases ? 1 : 0) << "\n";
  text << "streamhead " << juce::String(cfg.streamHeadFrames) << "\n";
  text << "preload " << juce::String(cfg.preloadHeadFrames) << "\n";
  if (cfg.organRootOverride.getFullPathName().isNotEmpty())
    text << "root " << cfg.organRootOverride.getFullPathName() << "\n";
  text << "simple " << (cfg.engineSwitch.simpleWavOnly ? 1 : 0) << "\n";
  text << "wind " << (cfg.engineSwitch.enableWindModel ? 1 : 0) << "\n";
  text << "tremulant " << (cfg.engineSwitch.enableTremulant ? 1 : 0) << "\n";
  text << "enclosure " << (cfg.engineSwitch.enableEnclosure ? 1 : 0) << "\n";
  text << "voicing " << (cfg.engineSwitch.enableVoicing ? 1 : 0) << "\n";
  text << "originalpitch " << (cfg.engineSwitch.playAtOriginalOrganPitch ? 1 : 0) << "\n";

  for (const auto& line : preservedLines) {
    text << line << "\n";
  }

  bool ok = targetFile.replaceWithText(text);

  if (proc.loadedOdf() == odf) {
    proc.setSampleStorage(cfg.storage);
    proc.setLoadMono(cfg.mono);
    proc.setLoadSampleRate(cfg.sampleRate);
    proc.setCacheMode(cfg.cacheMode);
    proc.setStreamReleases(cfg.streamReleases);
    proc.setStreamHeadFrames(cfg.streamHeadFrames);
    proc.setPreloadHeadFrames(cfg.preloadHeadFrames);
    proc.setOrganRootOverride(cfg.organRootOverride);
    proc.setEngineSwitch(cfg.engineSwitch);
    proc.markSettingsDirty();
  }

  return ok;
}

juce::File findUnrarBinary() {
  const juce::String binName =
#if JUCE_WINDOWS
      "unrar.exe";
#else
      "unrar";
#endif

  const auto appExe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
  const auto sibling = appExe.getSiblingFile(binName);
  if (sibling.existsAsFile()) return sibling;

#if JUCE_MAC
  const auto macOsDir = appExe.getParentDirectory();
  const auto bundleMacOS = macOsDir.getChildFile(binName);
  if (bundleMacOS.existsAsFile()) return bundleMacOS;
  const auto bundleResources = macOsDir.getSiblingFile("Resources").getChildFile(binName);
  if (bundleResources.existsAsFile()) return bundleResources;
#endif

  const auto dataDirBin = MasterpieceProcessor::dataDirectory().getChildFile(binName);
  if (dataDirBin.existsAsFile()) return dataDirBin;

#if !JUCE_WINDOWS
  for (const char* path : {"/opt/homebrew/bin/unrar",
                           "/usr/local/bin/unrar",
                           "/usr/bin/unrar",
                           "/bin/unrar"}) {
    juce::File f(path);
    if (f.existsAsFile()) return f;
  }
#endif

  const auto envPath = juce::SystemStats::getEnvironmentVariable("PATH", {});
#if JUCE_WINDOWS
  const auto sep = ";";
#else
  const auto sep = ":";
#endif
  auto tokens = juce::StringArray::fromTokens(envPath, sep, "\"");
  for (const auto& dirStr : tokens) {
    const auto dir = juce::File(dirStr.trim());
    const auto candidate = dir.getChildFile(binName);
    if (candidate.existsAsFile()) return candidate;
  }

  return {};
}

static bool isSecondaryArchivePart(const juce::String& filename) {
  const auto lower = filename.toLowerCase();

  int partIdx = lower.lastIndexOf(".part");
  if (partIdx >= 0) {
    int rarIdx = lower.lastIndexOf(".rar");
    if (rarIdx > partIdx + 5) {
      juce::String numStr = lower.substring(partIdx + 5, rarIdx);
      int partNum = numStr.getIntValue();
      if (partNum > 1) return true;
    }
  }

  int extDot = lower.lastIndexOfChar('.');
  if (extDot >= 0 && extDot + 3 < lower.length()) {
    if (lower[extDot + 1] == 'r' &&
        juce::CharacterFunctions::isDigit(lower[extDot + 2]) &&
        juce::CharacterFunctions::isDigit(lower[extDot + 3])) {
      return true;
    }
  }

  int compPartIdx = lower.lastIndexOf("part");
  if (compPartIdx >= 0 && lower.contains("comppkg_hauptwerk_rar")) {
    juce::String tail = lower.substring(compPartIdx + 4);
    int dotAfter = tail.indexOfChar('.');
    juce::String numStr = (dotAfter >= 0 ? tail.substring(0, dotAfter) : tail);
    int partNum = numStr.getIntValue();
    if (partNum > 1) return true;
  }

  return false;
}

static juce::File findPrimaryVolume(const juce::File& file) {
  const auto dir = file.getParentDirectory();
  const auto name = file.getFileName();
  const auto lower = name.toLowerCase();

  int partIdx = lower.lastIndexOf(".part");
  if (partIdx >= 0) {
    juce::String prefix = name.substring(0, partIdx);
    for (const char* p1 : {".part1.rar", ".part01.rar", ".part001.rar", ".part1.RAR", ".part01.RAR"}) {
      auto f = dir.getChildFile(prefix + p1);
      if (f.existsAsFile()) return f;
    }
  }

  int extDot = lower.lastIndexOfChar('.');
  if (extDot >= 0 && extDot + 3 < lower.length() && lower[extDot + 1] == 'r' &&
      juce::CharacterFunctions::isDigit(lower[extDot + 2])) {
    juce::String base = name.substring(0, extDot);
    for (const char* ext : {".rar", ".RAR"}) {
      auto f = dir.getChildFile(base + ext);
      if (f.existsAsFile()) return f;
    }
  }

  return file;
}

juce::Array<juce::File> filterArchivesForExtraction(const juce::Array<juce::File>& files) {
  juce::Array<juce::File> result;
  for (const auto& file : files) {
    if (!file.existsAsFile()) continue;

    if (isSecondaryArchivePart(file.getFileName())) {
      const auto primary = findPrimaryVolume(file);
      if (primary.existsAsFile() && !result.contains(primary)) {
        result.add(primary);
      }
    } else {
      if (!result.contains(file)) {
        result.add(file);
      }
    }
  }
  return result;
}

juce::String readOrganNameFromOdf(const juce::File& file) {
  if (!file.existsAsFile()) return file.getFileNameWithoutExtension();

  std::unique_ptr<juce::FileInputStream> stream(file.createInputStream());
  if (stream == nullptr) return file.getFileNameWithoutExtension();

  const int bytesToRead = std::min(static_cast<int>(stream->getTotalLength()), 65536);
  juce::MemoryBlock block(static_cast<size_t>(bytesToRead));
  stream->read(block.getData(), bytesToRead);

  pugi::xml_document doc;
  pugi::xml_parse_result res = doc.load_buffer(block.getData(), block.getSize(),
                                               pugi::parse_default, pugi::encoding_utf8);

  if (!res) {
    pugi::xml_document docLatin;
    res = docLatin.load_buffer(block.getData(), block.getSize(),
                               pugi::parse_default, pugi::encoding_latin1);
    if (res) {
      doc.reset(docLatin);
    }
  }

  if (res) {
    for (pugi::xml_node general : doc.child("Hauptwerk").children("ObjectList")) {
      if (std::string(general.attribute("ObjectType").value()) == "_General") {
        for (pugi::xml_node g : general.children("_General")) {
          pugi::xml_node nameNode = g.child("Identification_Name");
          if (nameNode && nameNode.text().as_string()[0] != '\0') {
            return juce::String::fromUTF8(nameNode.text().as_string()).trim();
          }
          pugi::xml_node organNameNode = g.child("Identification_OrganName");
          if (organNameNode && organNameNode.text().as_string()[0] != '\0') {
            return juce::String::fromUTF8(organNameNode.text().as_string()).trim();
          }
        }
      }
    }
  }

  return file.getFileNameWithoutExtension();
}

std::vector<OrganEntry> discoverOrgans(const MasterpieceProcessor& proc) {
  std::vector<OrganEntry> result;
  std::unordered_set<std::string> seenPaths;

  auto addFile = [&](const juce::File& f, bool isFromDataDir) {
    const auto p = f.getFullPathName().toStdString();
    if (seenPaths.count(p) > 0) return;
    if (proc.isOrganHidden(f)) return;

    // Filter out missing phantom/placeholder "Organ1" entries
    if (!f.existsAsFile()) {
      const auto fname = f.getFileName();
      const auto fstem = f.getFileNameWithoutExtension();
      if (fname == "Organ1" || fstem == "Organ1" || fname.startsWithIgnoreCase("Organ1.") ||
          f.getFullPathName() == "Organ1") {
        return;
      }
    }

    seenPaths.insert(p);

    OrganEntry entry = getOrganDetails(f, proc);
    entry.isInstalled = isFromDataDir;
    result.push_back(std::move(entry));
  };

  const auto dataDir = MasterpieceProcessor::dataDirectory();
  const auto defsDir = dataDir.getChildFile("OrganDefinitions");

  auto scanDir = [&](const juce::File& d) {
    if (!d.isDirectory()) return;
    juce::Array<juce::File> found;
    d.findChildFiles(found, juce::File::findFiles, true, "*.Organ_Hauptwerk_xml");
    for (const auto& f : found) addFile(f, true);
    found.clear();
    d.findChildFiles(found, juce::File::findFiles, true, "*.CustomOrgan_Hauptwerk_xml");
    for (const auto& f : found) addFile(f, true);
  };

  scanDir(defsDir);
  if (defsDir != dataDir) {
    scanDir(dataDir);
  }

  for (const auto& f : proc.recentOrgans()) {
    addFile(f, f.isAChildOf(dataDir));
  }

  if (proc.loadedOdf().existsAsFile()) {
    addFile(proc.loadedOdf(), proc.loadedOdf().isAChildOf(dataDir));
  }

  std::sort(result.begin(), result.end(), [](const OrganEntry& a, const OrganEntry& b) {
    return a.name.compareIgnoreCase(b.name) < 0;
  });

  return result;
}

// ==============================================================================
// OrganDetailsDialog implementation
// ==============================================================================
OrganDetailsDialog::OrganDetailsDialog(const OrganEntry& entry, MasterpieceProcessor& proc,
                                       std::function<void()> onOpenAudioSettings)
    : entry_(entry), proc_(proc), onOpenAudioSettings_(std::move(onOpenAudioSettings)) {
  setSize(660, 520);

  titleLabel_.setText(entry_.name, juce::dontSendNotification);
  titleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
  titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
  addAndMakeVisible(titleLabel_);

  pathLabel_.setText("File: " + entry_.file.getFullPathName() +
                     (entry_.odfSizeBytes > 0 ? " (" + formatByteSize(entry_.odfSizeBytes) + ")" : ""),
                     juce::dontSendNotification);
  pathLabel_.setFont(juce::FontOptions(11.0f));
  pathLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8a95a5));
  addAndMakeVisible(pathLabel_);

  juce::String rootStr = entry_.organRootDir.getFullPathName();
  if (rootStr.isEmpty()) rootStr = "(Default)";
  rootLabel_.setText("Root: " + rootStr, juce::dontSendNotification);
  rootLabel_.setFont(juce::FontOptions(11.0f));
  rootLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8a95a5));
  addAndMakeVisible(rootLabel_);

  juce::String sizeStr = "Total Disk Space: " + formatByteSize(entry_.diskSpaceBytes);
  if (!entry_.packages.empty()) {
    juce::int64 pkgBytes = entry_.diskSpaceBytes - entry_.odfSizeBytes;
    sizeStr += " (" + formatByteSize(pkgBytes) + " across " +
               juce::String(entry_.packages.size()) + " package" +
               (entry_.packages.size() == 1 ? "" : "s") + " + " +
               formatByteSize(entry_.odfSizeBytes) + " definition)";
  }
  sizeLabel_.setText(sizeStr, juce::dontSendNotification);
  sizeLabel_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
  sizeLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff98e2b0));
  addAndMakeVisible(sizeLabel_);

  OrganAudioConfig cfg = loadOrganAudioConfig(proc_, entry_.file);
  juce::String audioSummary = "Configured: ";
  audioSummary += (cfg.storage == SampleStorage::Int16 ? "16-bit" : (cfg.storage == SampleStorage::Float32 ? "32-bit" : "24-bit"));
  audioSummary += cfg.mono ? ", Mono" : ", Stereo";
  audioSummary += cfg.streamReleases ? ", Stream Releases" : ", Hold in RAM";
  settingsLabel_.setText(audioSummary, juce::dontSendNotification);
  settingsLabel_.setFont(juce::FontOptions(11.0f));
  settingsLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc2d4ea));
  addAndMakeVisible(settingsLabel_);

  packagesHeaderLabel_.setText("Associated Packages (" + juce::String(entry_.packages.size()) + "):",
                               juce::dontSendNotification);
  packagesHeaderLabel_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
  packagesHeaderLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
  addAndMakeVisible(packagesHeaderLabel_);

  packageList_.setModel(this);
  packageList_.setRowHeight(40);
  packageList_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff121418));
  packageList_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff232833));
  addAndMakeVisible(packageList_);

  adjustAudioBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a3442));
  adjustAudioBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2d4ea));
  adjustAudioBtn_.onClick = [this] {
    if (onOpenAudioSettings_) {
      onOpenAudioSettings_();
    }
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
      dw->exitModalState(0);
  };
  addAndMakeVisible(adjustAudioBtn_);

  revealBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  revealBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2c8d2));
  revealBtn_.onClick = [this] {
    if (entry_.file.existsAsFile()) {
      entry_.file.revealToUser();
    } else if (entry_.organRootDir.isDirectory()) {
      entry_.organRootDir.revealToUser();
    }
  };
  addAndMakeVisible(revealBtn_);

  closeBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  closeBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2c8d2));
  closeBtn_.onClick = [this] {
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
      dw->exitModalState(0);
  };
  addAndMakeVisible(closeBtn_);
}

void OrganDetailsDialog::resized() {
  const int pad = 16;
  const int w = getWidth() - pad * 2;
  int y = pad;

  titleLabel_.setBounds(pad, y, w, 24);
  y += 26;
  pathLabel_.setBounds(pad, y, w, 18);
  y += 20;
  rootLabel_.setBounds(pad, y, w, 18);
  y += 22;
  sizeLabel_.setBounds(pad, y, w, 20);
  y += 22;
  settingsLabel_.setBounds(pad, y, w, 18);
  y += 24;

  packagesHeaderLabel_.setBounds(pad, y, w, 20);
  y += 24;

  const int bottomH = 32;
  const int bottomY = getHeight() - pad - bottomH;

  packageList_.setBounds(pad, y, w, bottomY - y - 12);

  adjustAudioBtn_.setBounds(pad, bottomY, 180, bottomH);
  revealBtn_.setBounds(pad + 190, bottomY, 140, bottomH);
  closeBtn_.setBounds(getWidth() - pad - 80, bottomY, 80, bottomH);
}

void OrganDetailsDialog::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff1b1e24));
}

int OrganDetailsDialog::getNumRows() {
  return static_cast<int>(entry_.packages.size());
}

void OrganDetailsDialog::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                                         bool rowIsSelected) {
  if (rowNumber < 0 || rowNumber >= static_cast<int>(entry_.packages.size())) return;

  const auto& pkg = entry_.packages[rowNumber];

  if (rowIsSelected) {
    g.setColour(juce::Colour(0xff23354d));
    g.fillRect(0, 0, width, height);
  } else if (rowNumber % 2 == 1) {
    g.setColour(juce::Colour(0xff16181f));
    g.fillRect(0, 0, width, height);
  }

  const int leftMargin = 12;
  const int rightMargin = 90;
  const int contentWidth = width - leftMargin - rightMargin;

  g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
  g.setColour(pkg.isInstalled ? juce::Colour(0xffe8edf5) : juce::Colour(0xffa0abbd));
  juce::String pkgTitle = juce::String::formatted("Package %06d", pkg.packageId);
  if (pkg.name.isNotEmpty()) {
    pkgTitle += " - " + pkg.name;
  }
  g.drawText(pkgTitle, leftMargin, 3, contentWidth, 18, juce::Justification::left, true);

  g.setFont(juce::FontOptions(11.0f));
  g.setColour(juce::Colour(0xff758092));
  juce::String sub;
  if (pkg.supplierName.isNotEmpty()) {
    sub += "Supplier: " + pkg.supplierName + "   \u2022   ";
  }
  if (pkg.isInstalled) {
    sub += formatByteSize(pkg.diskSizeBytes);
  } else {
    sub += "Not found on disk";
  }
  g.drawText(sub, leftMargin, 21, contentWidth, 16, juce::Justification::left, true);

  auto badgeRect = juce::Rectangle<int>(width - 86, (height - 20) / 2, 76, 20);
  if (pkg.isInstalled) {
    g.setColour(juce::Colour(0xff1b3d2b));
    g.fillRoundedRectangle(badgeRect.toFloat(), 4.0f);
    g.setColour(juce::Colour(0xff68d391));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("Installed", badgeRect, juce::Justification::centred, false);
  } else {
    g.setColour(juce::Colour(0xff3d1f1f));
    g.fillRoundedRectangle(badgeRect.toFloat(), 4.0f);
    g.setColour(juce::Colour(0xffe28080));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("Missing", badgeRect, juce::Justification::centred, false);
  }

  g.setColour(juce::Colour(0xff20242c));
  g.fillRect(0, height - 1, width, 1);
}

// ==============================================================================
// RamGraphMeterComponent implementation
// ==============================================================================
RamGraphMeterComponent::RamGraphMeterComponent() {}

void RamGraphMeterComponent::setValues(juce::int64 totalSystemRamBytes,
                                       juce::int64 osUsedRamBytes,
                                       juce::int64 organFootprintBytes) {
  totalSystemRam_ = totalSystemRamBytes;
  osUsedRam_ = osUsedRamBytes;
  organFootprint_ = organFootprintBytes;
  repaint();
}

void RamGraphMeterComponent::paint(juce::Graphics& g) {
  const auto bounds = getLocalBounds().toFloat();
  const float w = bounds.getWidth();
  const float h = bounds.getHeight();

  // Background bar track
  g.setColour(juce::Colour(0xff12151b));
  g.fillRoundedRectangle(0, 0, w, h, 6.0f);

  if (totalSystemRam_ <= 0) return;

  const double total = static_cast<double>(totalSystemRam_);
  const double osFraction = juce::jlimit(0.0, 1.0, static_cast<double>(osUsedRam_) / total);
  const double organFraction = juce::jlimit(0.0, 1.0 - osFraction, static_cast<double>(organFootprint_) / total);
  const double totalUsedFraction = osFraction + organFraction;

  const float osW = static_cast<float>(w * osFraction);
  const float organW = static_cast<float>(w * organFraction);

  // 1. Draw OS reservation / baseline
  if (osW > 1.0f) {
    g.setColour(juce::Colour(0xff333e50)); // Slate blue-gray
    g.fillRoundedRectangle(0, 0, osW + 4.0f, h, 6.0f);
    g.fillRect(osW - 4.0f, 0.0f, 4.0f, h);
  }

  // 2. Draw Organ Footprint segment with color thresholding
  if (organW > 1.0f) {
    juce::Colour organCol;
    if (totalUsedFraction < 0.60) {
      organCol = juce::Colour(0xff38a169); // Green / Safe
    } else if (totalUsedFraction < 0.75) {
      organCol = juce::Colour(0xffd69e2e); // Yellow / Moderate
    } else if (totalUsedFraction < 0.88) {
      organCol = juce::Colour(0xffdd6b20); // Orange / High
    } else {
      organCol = juce::Colour(0xffe53e3e); // Red / Danger
    }

    g.setColour(organCol);
    if (totalUsedFraction >= 0.98f) {
      g.fillRoundedRectangle(osW, 0, organW, h, 6.0f);
    } else {
      g.fillRect(osW, 0.0f, organW, h);
    }
  }

  // Border outline
  g.setColour(juce::Colour(0xff2f3a4d));
  g.drawRoundedRectangle(0.5f, 0.5f, w - 1.0f, h - 1.0f, 6.0f, 1.0f);

  // Threshold tick marks at 75% and 88%
  const float x75 = w * 0.75f;
  const float x88 = w * 0.88f;
  g.setColour(juce::Colour(0x60ffffff));
  g.drawVerticalLine(static_cast<int>(x75), 0.0f, h);
  g.drawVerticalLine(static_cast<int>(x88), 0.0f, h);
}

// ==============================================================================
// StatScanThread implementation
// ==============================================================================
class OrganAudioSettingsDialog::StatScanThread : public juce::Thread {
public:
  StatScanThread(OrganAudioSettingsDialog& owner, const OrganEntry& entry, const MasterpieceProcessor& proc)
      : juce::Thread("OrganStatScanner"), owner_(&owner), entry_(entry), proc_(proc) {}

  ~StatScanThread() override {
    cancel();
    stopThread(3000);
  }

  void cancel() {
    cancelFlag_.store(true);
    signalThreadShouldExit();
  }

  void run() override {
    OrganAudioStat stat = computeOrganAudioStat(
        entry_, proc_,
        [this](double progress, int current, int total) {
          if (cancelFlag_.load() || threadShouldExit()) return;
          juce::MessageManager::callAsync([owner = owner_, progress, current, total] {
            if (owner != nullptr) {
              owner->onScanProgress(progress, current, total);
            }
          });
        },
        &cancelFlag_);

    if (!cancelFlag_.load() && !threadShouldExit()) {
      juce::MessageManager::callAsync([owner = owner_, stat] {
        if (owner != nullptr) {
          owner->onScanCompleted(stat);
        }
      });
    }
  }

private:
  juce::Component::SafePointer<OrganAudioSettingsDialog> owner_;
  OrganEntry entry_;
  const MasterpieceProcessor& proc_;
  std::atomic<bool> cancelFlag_{false};
};

// ==============================================================================
// OrganAudioSettingsDialog implementation
// ==============================================================================
OrganAudioSettingsDialog::OrganAudioSettingsDialog(const OrganEntry& entry, MasterpieceProcessor& proc)
    : entry_(entry), proc_(proc) {
  setSize(640, 660);
  config_ = loadOrganAudioConfig(proc_, entry_.file);

  titleLabel_.setText(entry_.name + " - Audio Settings", juce::dontSendNotification);
  titleLabel_.setFont(juce::FontOptions(16.0f, juce::Font::bold));
  titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
  addAndMakeVisible(titleLabel_);

  subtitleLabel_.setText("Configure sample loading, bit depth, streaming, and DSP switches without loading into RAM.",
                         juce::dontSendNotification);
  subtitleLabel_.setFont(juce::FontOptions(11.0f));
  subtitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8a95a5));
  addAndMakeVisible(subtitleLabel_);

  // RAM Usage Graph & progress
  ramHeading_.setText("RAM Consumption & System Impact", juce::dontSendNotification);
  ramHeading_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
  ramHeading_.setColour(juce::Label::textColourId, juce::Colour(0xffc2c8d2));
  addAndMakeVisible(ramHeading_);

  scanProgressBar_.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xff4a90e2));
  scanProgressBar_.setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xff12151b));
  addChildComponent(scanProgressBar_);

  scanStatusLabel_.setFont(juce::FontOptions(11.0f));
  scanStatusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff68b2ff));
  addChildComponent(scanStatusLabel_);

  addAndMakeVisible(ramMeter_);

  ramDetailsLabel_.setFont(juce::FontOptions(11.0f));
  ramDetailsLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff98a9c2));
  addAndMakeVisible(ramDetailsLabel_);

  profileLabel_.setText("Memory profile", juce::dontSendNotification);
  profileLabel_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
  profileLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc2c8d2));
  addAndMakeVisible(profileLabel_);

  profileCombo_.onChange = [this] {
    const int id = profileCombo_.getSelectedId();
    if (id == 1) {
      config_.storage = SampleStorage::Int24;
      config_.streamReleases = false;
      config_.mono = false;
      config_.preloadHeadFrames = 0;
      updateControlsFromConfig();
    } else if (id == 2) {
      config_.storage = SampleStorage::Int16;
      config_.streamReleases = true;
      config_.mono = false;
      config_.preloadHeadFrames = 0;
      updateControlsFromConfig();
    } else if (id == 3) {
      config_.storage = SampleStorage::Int16;
      config_.streamReleases = true;
      config_.mono = true;
      config_.preloadHeadFrames = 0;
      updateControlsFromConfig();
    }
    updateRamFootprintDisplay();
  };
  addAndMakeVisible(profileCombo_);

  storageLabel_.setText("Resident sample format", juce::dontSendNotification);
  storageLabel_.setFont(juce::FontOptions(12.0f));
  storageLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa0abbd));
  addAndMakeVisible(storageLabel_);

  storageCombo_.onChange = [this] {
    const int id = storageCombo_.getSelectedId();
    config_.storage = (id == 2 ? SampleStorage::Int16 : (id == 3 ? SampleStorage::Float32 : SampleStorage::Int24));
    syncProfile();
    updateRamFootprintDisplay();
  };
  addAndMakeVisible(storageCombo_);

  rebuildDropdownItemTexts();

  monoToggle_.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffe8edf5));
  monoToggle_.onClick = [this] {
    config_.mono = monoToggle_.getToggleState();
    syncProfile();
    updateRamFootprintDisplay();
  };
  addAndMakeVisible(monoToggle_);

  rateLabel_.setText("Sample rate", juce::dontSendNotification);
  rateLabel_.setFont(juce::FontOptions(12.0f));
  rateLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa0abbd));
  addAndMakeVisible(rateLabel_);

  rateCombo_.addItem("As recorded", 1);
  rateCombo_.addItem("48 kHz", 2);
  rateCombo_.addItem("44.1 kHz", 3);
  rateCombo_.onChange = [this] {
    const int id = rateCombo_.getSelectedId();
    config_.sampleRate = (id == 2 ? 48000.0 : (id == 3 ? 44100.0 : 0.0));
  };
  addAndMakeVisible(rateCombo_);

  streamToggle_.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffe8edf5));
  streamToggle_.onClick = [this] {
    config_.streamReleases = streamToggle_.getToggleState();
    syncProfile();
    updateRamFootprintDisplay();
  };
  addAndMakeVisible(streamToggle_);

  preloadLabel_.setText("Preloaded per sample", juce::dontSendNotification);
  preloadLabel_.setFont(juce::FontOptions(12.0f));
  preloadLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa0abbd));
  addAndMakeVisible(preloadLabel_);

  preloadCombo_.addItem("Whole samples (hold all in RAM)", 1);
  preloadCombo_.addItem("Loop + 2 s", 2);
  preloadCombo_.addItem("Loop + 1 s", 3);
  preloadCombo_.addItem("Loop only (minimal RAM, stream remainder)", 4);
  preloadCombo_.onChange = [this] {
    const int id = preloadCombo_.getSelectedId();
    config_.preloadHeadFrames = (id == 1 ? 0 : (id == 2 ? 88200 : (id == 3 ? 44100 : 1)));
    syncProfile();
    updateRamFootprintDisplay();
  };
  addAndMakeVisible(preloadCombo_);

  cacheLabel_.setText("Sample cache", juce::dontSendNotification);
  cacheLabel_.setFont(juce::FontOptions(12.0f));
  cacheLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa0abbd));
  addAndMakeVisible(cacheLabel_);

  cacheCombo_.addItem("One cache, replaced as organs change", 1);
  cacheCombo_.addItem("One cache per organ (uses more disk)", 2);
  cacheCombo_.addItem("Off", 3);
  cacheCombo_.onChange = [this] {
    const int id = cacheCombo_.getSelectedId();
    config_.cacheMode = (id == 3 ? SampleLibrary::CacheMode::Off : (id == 2 ? SampleLibrary::CacheMode::PerOrgan : SampleLibrary::CacheMode::Single));
  };
  addAndMakeVisible(cacheCombo_);

  rootLabel_.setText("Organ folder (OrganInstallationPackages)", juce::dontSendNotification);
  rootLabel_.setFont(juce::FontOptions(12.0f));
  rootLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffa0abbd));
  addAndMakeVisible(rootLabel_);

  rootValue_.setFont(juce::FontOptions(11.0f));
  rootValue_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(rootValue_);

  rootChooseBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  rootChooseBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2c8d2));
  rootChooseBtn_.onClick = [this] {
    rootChooser_ = std::make_unique<juce::FileChooser>(
        "Which folder holds OrganInstallationPackages?",
        config_.organRootOverride.exists() ? config_.organRootOverride : juce::File(proc_.organRootDir()));
    rootChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc) {
          const auto dir = fc.getResult();
          if (dir.getFullPathName().isEmpty()) return;
          config_.organRootOverride = dir;
          showOrganRoot();
        });
  };
  addAndMakeVisible(rootChooseBtn_);

  rootDefaultBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  rootDefaultBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff8a95a5));
  rootDefaultBtn_.onClick = [this] {
    config_.organRootOverride = juce::File();
    showOrganRoot();
  };
  addAndMakeVisible(rootDefaultBtn_);

  dspHeading_.setText("Engine & DSP switches", juce::dontSendNotification);
  dspHeading_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
  dspHeading_.setColour(juce::Label::textColourId, juce::Colour(0xffc2c8d2));
  addAndMakeVisible(dspHeading_);

  for (auto* tb : {&simpleWav_, &wind_, &tremulant_, &enclosure_, &voicing_, &originalPitch_}) {
    tb->setColour(juce::ToggleButton::textColourId, juce::Colour(0xffe8edf5));
    addAndMakeVisible(*tb);
  }

  simpleWav_.onClick = [this] { config_.engineSwitch.simpleWavOnly = simpleWav_.getToggleState(); };
  wind_.onClick = [this] { config_.engineSwitch.enableWindModel = wind_.getToggleState(); };
  tremulant_.onClick = [this] { config_.engineSwitch.enableTremulant = tremulant_.getToggleState(); };
  enclosure_.onClick = [this] { config_.engineSwitch.enableEnclosure = enclosure_.getToggleState(); };
  voicing_.onClick = [this] { config_.engineSwitch.enableVoicing = voicing_.getToggleState(); };
  originalPitch_.onClick = [this] { config_.engineSwitch.playAtOriginalOrganPitch = originalPitch_.getToggleState(); };

  saveBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2d4a6e));
  saveBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe8edf5));
  saveBtn_.onClick = [this] { save(); };
  addAndMakeVisible(saveBtn_);

  defaultsBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  defaultsBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff8a95a5));
  defaultsBtn_.onClick = [this] { resetToDefaults(); };
  addAndMakeVisible(defaultsBtn_);

  cancelBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  cancelBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2c8d2));
  cancelBtn_.onClick = [this] { closeDialog(); };
  addAndMakeVisible(cancelBtn_);

  updateControlsFromConfig();

  if (hasCachedOrganAudioStat(entry_)) {
    audioStat_ = computeOrganAudioStat(entry_, proc_);
    isScanning_ = false;
    rebuildDropdownItemTexts();

    const juce::File sf = proc_.settingsFileFor(entry_.file);
    if (!sf.existsAsFile()) {
      const juce::int64 sysRamBytes = static_cast<juce::int64>(juce::SystemStats::getMemorySizeInMegabytes()) * 1024 * 1024;
      const juce::int64 osReservation = std::max<juce::int64>(static_cast<juce::int64>(4LL * 1024 * 1024 * 1024),
                                                              static_cast<juce::int64>(sysRamBytes * 0.20));
      const juce::int64 availableForOrgan = std::max<juce::int64>(0, sysRamBytes - osReservation);

      OrganAudioConfig q24;
      q24.storage = SampleStorage::Int24;
      q24.streamReleases = false;
      q24.mono = false;

      OrganAudioConfig r16;
      r16.storage = SampleStorage::Int16;
      r16.streamReleases = true;
      r16.mono = false;

      const juce::int64 ram24 = estimateRamFootprintBytes(audioStat_, q24);
      const juce::int64 ram16 = estimateRamFootprintBytes(audioStat_, r16);

      if (ram24 > 0 && ram24 <= availableForOrgan) {
        config_.storage = SampleStorage::Int24;
        config_.streamReleases = false;
        config_.mono = false;
      } else if (ram16 > 0 && ram16 <= availableForOrgan) {
        config_.storage = SampleStorage::Int16;
        config_.streamReleases = true;
        config_.mono = false;
      } else if (ram16 > 0) {
        config_.storage = SampleStorage::Int16;
        config_.streamReleases = true;
        config_.mono = true;
      }
      updateControlsFromConfig();
    }
    updateRamFootprintDisplay();
  } else {
    isScanning_ = true;
    ramMeter_.setVisible(false);
    ramDetailsLabel_.setVisible(false);
    scanProgressBar_.setVisible(true);
    scanStatusLabel_.setVisible(true);
    scanStatusLabel_.setText("Calculating RAM footprint: scanning samples...", juce::dontSendNotification);

    statThread_ = std::make_unique<StatScanThread>(*this, entry_, proc_);
    statThread_->startThread(juce::Thread::Priority::normal);
  }
}

OrganAudioSettingsDialog::~OrganAudioSettingsDialog() {
  if (statThread_ != nullptr) {
    statThread_->cancel();
    statThread_->stopThread(3000);
    statThread_.reset();
  }
}

void OrganAudioSettingsDialog::onScanProgress(double progress, int current, int total) {
  scanProgress_ = progress;
  scanStatusLabel_.setText(
      juce::String::formatted("Calculating RAM footprint: %d%% (%d / %d files)...",
                              static_cast<int>(progress * 100.0 + 0.5), current, total),
      juce::dontSendNotification);
}

void OrganAudioSettingsDialog::onScanCompleted(const OrganAudioStat& stat) {
  audioStat_ = stat;
  isScanning_ = false;

  scanProgressBar_.setVisible(false);
  scanStatusLabel_.setVisible(false);
  ramMeter_.setVisible(true);
  ramDetailsLabel_.setVisible(true);

  rebuildDropdownItemTexts();

  const juce::File sf = proc_.settingsFileFor(entry_.file);
  if (!sf.existsAsFile()) {
    const juce::int64 sysRamBytes = static_cast<juce::int64>(juce::SystemStats::getMemorySizeInMegabytes()) * 1024 * 1024;
    const juce::int64 osReservation = std::max<juce::int64>(static_cast<juce::int64>(4LL * 1024 * 1024 * 1024),
                                                            static_cast<juce::int64>(sysRamBytes * 0.20));
    const juce::int64 availableForOrgan = std::max<juce::int64>(0, sysRamBytes - osReservation);

    OrganAudioConfig q24;
    q24.storage = SampleStorage::Int24;
    q24.streamReleases = false;
    q24.mono = false;

    OrganAudioConfig r16;
    r16.storage = SampleStorage::Int16;
    r16.streamReleases = true;
    r16.mono = false;

    const juce::int64 ram24 = estimateRamFootprintBytes(audioStat_, q24);
    const juce::int64 ram16 = estimateRamFootprintBytes(audioStat_, r16);

    if (ram24 > 0 && ram24 <= availableForOrgan) {
      config_.storage = SampleStorage::Int24;
      config_.streamReleases = false;
      config_.mono = false;
    } else if (ram16 > 0 && ram16 <= availableForOrgan) {
      config_.storage = SampleStorage::Int16;
      config_.streamReleases = true;
      config_.mono = false;
    } else if (ram16 > 0) {
      config_.storage = SampleStorage::Int16;
      config_.streamReleases = true;
      config_.mono = true;
    }
    updateControlsFromConfig();
  }

  updateRamFootprintDisplay();
  repaint();
}

void OrganAudioSettingsDialog::rebuildDropdownItemTexts() {
  const int currentProfileId = profileCombo_.getSelectedId();
  const int currentStorageId = storageCombo_.getSelectedId();

  profileCombo_.clear(juce::dontSendNotification);
  storageCombo_.clear(juce::dontSendNotification);

  OrganAudioConfig optBest;
  optBest.storage = SampleStorage::Int24;
  optBest.streamReleases = false;
  optBest.mono = false;

  OrganAudioConfig optRec;
  optRec.storage = SampleStorage::Int16;
  optRec.streamReleases = true;
  optRec.mono = false;

  OrganAudioConfig optSmall;
  optSmall.storage = SampleStorage::Int16;
  optSmall.streamReleases = true;
  optSmall.mono = true;

  juce::String bestDesc = "Best quality - 24-bit, hold everything";
  juce::String recDesc = "Recommended - 16-bit, stream releases";
  juce::String smallDesc = "Smallest - 16-bit mono, stream releases";

  if (audioStat_.hasStats) {
    bestDesc += " (~" + formatByteSize(estimateRamFootprintBytes(audioStat_, optBest)) + " RAM)";
    recDesc += " (~" + formatByteSize(estimateRamFootprintBytes(audioStat_, optRec)) + " RAM)";
    smallDesc += " (~" + formatByteSize(estimateRamFootprintBytes(audioStat_, optSmall)) + " RAM)";
  }

  profileCombo_.addItem(bestDesc, 1);
  profileCombo_.addItem(recDesc, 2);
  profileCombo_.addItem(smallDesc, 3);
  profileCombo_.addItem("Custom", 4);

  OrganAudioConfig s24 = config_; s24.storage = SampleStorage::Int24;
  OrganAudioConfig s16 = config_; s16.storage = SampleStorage::Int16;
  OrganAudioConfig s32 = config_; s32.storage = SampleStorage::Float32;

  juce::String s24Text = "24-bit - original sample format";
  juce::String s16Text = "16-bit - reduces memory by ~33%";
  juce::String s32Text = "32-bit float - higher memory usage";
  if (audioStat_.hasStats) {
    s24Text += " (~" + formatByteSize(estimateRamFootprintBytes(audioStat_, s24)) + ")";
    s16Text += " (~" + formatByteSize(estimateRamFootprintBytes(audioStat_, s16)) + ")";
    s32Text += " (~" + formatByteSize(estimateRamFootprintBytes(audioStat_, s32)) + ")";
  }

  storageCombo_.addItem(s24Text, 1);
  storageCombo_.addItem(s16Text, 2);
  storageCombo_.addItem(s32Text, 3);

  if (currentProfileId > 0) profileCombo_.setSelectedId(currentProfileId, juce::dontSendNotification);
  if (currentStorageId > 0) storageCombo_.setSelectedId(currentStorageId, juce::dontSendNotification);
}

void OrganAudioSettingsDialog::syncProfile() {
  const bool is24 = (config_.storage == SampleStorage::Int24);
  const bool is16 = (config_.storage == SampleStorage::Int16);
  const bool streaming = config_.streamReleases;
  const bool mono = config_.mono;
  const bool fullPreload = (config_.preloadHeadFrames == 0);

  if (is24 && !streaming && !mono && fullPreload) {
    profileCombo_.setSelectedId(1, juce::dontSendNotification);
  } else if (is16 && streaming && !mono && fullPreload) {
    profileCombo_.setSelectedId(2, juce::dontSendNotification);
  } else if (is16 && streaming && mono && fullPreload) {
    profileCombo_.setSelectedId(3, juce::dontSendNotification);
  } else {
    profileCombo_.setSelectedId(4, juce::dontSendNotification);
  }
}

void OrganAudioSettingsDialog::updateRamFootprintDisplay() {
  const juce::int64 sysRamBytes = static_cast<juce::int64>(juce::SystemStats::getMemorySizeInMegabytes()) * 1024 * 1024;
  const juce::int64 osReservation = std::max<juce::int64>(static_cast<juce::int64>(4LL * 1024 * 1024 * 1024),
                                                          static_cast<juce::int64>(sysRamBytes * 0.20));
  const juce::int64 organBytes = estimateRamFootprintBytes(audioStat_, config_);

  ramMeter_.setValues(sysRamBytes, osReservation, organBytes);

  juce::String details;
  if (organBytes > 0) {
    const double organPct = (static_cast<double>(organBytes) / static_cast<double>(sysRamBytes)) * 100.0;
    const double totalUsedPct = (static_cast<double>(osReservation + organBytes) / static_cast<double>(sysRamBytes)) * 100.0;
    juce::String status = "Safe";
    if (totalUsedPct >= 88.0) status = "Danger: OS paging / out-of-memory risk";
    else if (totalUsedPct >= 75.0) status = "High memory load";
    else if (totalUsedPct >= 60.0) status = "Moderate";

    details = "Organ RAM: ~" + formatByteSize(organBytes) + " (" + juce::String(organPct, 0) + "%), OS/System: ~" +
              formatByteSize(osReservation) + "  •  Total: " + formatByteSize(sysRamBytes) + "  •  " + status;
  } else {
    details = "System RAM: " + formatByteSize(sysRamBytes) + " (OS buffer: " + formatByteSize(osReservation) + ")";
  }
  ramDetailsLabel_.setText(details, juce::dontSendNotification);
}

void OrganAudioSettingsDialog::updateControlsFromConfig() {
  storageCombo_.setSelectedId(config_.storage == SampleStorage::Int16 ? 2 : (config_.storage == SampleStorage::Float32 ? 3 : 1), juce::dontSendNotification);
  monoToggle_.setToggleState(config_.mono, juce::dontSendNotification);
  rateCombo_.setSelectedId(config_.sampleRate == 48000.0 ? 2 : (config_.sampleRate == 44100.0 ? 3 : 1), juce::dontSendNotification);
  streamToggle_.setToggleState(config_.streamReleases, juce::dontSendNotification);
  preloadCombo_.setSelectedId(config_.preloadHeadFrames == 0 ? 1 : (config_.preloadHeadFrames >= 88200 ? 2 : (config_.preloadHeadFrames >= 44100 ? 3 : 4)), juce::dontSendNotification);
  cacheCombo_.setSelectedId(config_.cacheMode == SampleLibrary::CacheMode::Off ? 3 : (config_.cacheMode == SampleLibrary::CacheMode::PerOrgan ? 2 : 1), juce::dontSendNotification);

  simpleWav_.setToggleState(config_.engineSwitch.simpleWavOnly, juce::dontSendNotification);
  wind_.setToggleState(config_.engineSwitch.enableWindModel, juce::dontSendNotification);
  tremulant_.setToggleState(config_.engineSwitch.enableTremulant, juce::dontSendNotification);
  enclosure_.setToggleState(config_.engineSwitch.enableEnclosure, juce::dontSendNotification);
  voicing_.setToggleState(config_.engineSwitch.enableVoicing, juce::dontSendNotification);
  originalPitch_.setToggleState(config_.engineSwitch.playAtOriginalOrganPitch, juce::dontSendNotification);

  showOrganRoot();
  syncProfile();
}

void OrganAudioSettingsDialog::showOrganRoot() {
  if (config_.organRootOverride.exists()) {
    rootValue_.setText(config_.organRootOverride.getFullPathName(), juce::dontSendNotification);
  } else {
    rootValue_.setText("(Default: auto-detect from definition)", juce::dontSendNotification);
  }
}

void OrganAudioSettingsDialog::save() {
  saveOrganAudioConfig(proc_, entry_.file, config_);
  closeDialog();
}

void OrganAudioSettingsDialog::resetToDefaults() {
  config_ = loadOrganAudioConfig(proc_, juce::File());
  updateControlsFromConfig();
  updateRamFootprintDisplay();
}

void OrganAudioSettingsDialog::closeDialog() {
  if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
    dw->exitModalState(0);
}

void OrganAudioSettingsDialog::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff1b1e24));

  g.setColour(juce::Colour(0xff272c36));
  g.fillRect(16, 360, getWidth() - 32, 1);
  g.fillRect(16, 420, getWidth() - 32, 1);
  g.fillRect(16, 574, getWidth() - 32, 1);
}

void OrganAudioSettingsDialog::resized() {
  const int pad = 16;
  const int w = getWidth() - pad * 2;
  int y = pad;

  titleLabel_.setBounds(pad, y, w, 22);
  y += 24;
  subtitleLabel_.setBounds(pad, y, w, 16);
  y += 24;

  ramHeading_.setBounds(pad, y, w, 18);
  y += 20;
  ramMeter_.setBounds(pad, y, w, 20);
  scanProgressBar_.setBounds(pad, y, w, 20);
  y += 24;
  ramDetailsLabel_.setBounds(pad, y, w, 16);
  scanStatusLabel_.setBounds(pad, y, w, 16);
  y += 26;

  profileLabel_.setBounds(pad, y, 120, 26);
  profileCombo_.setBounds(pad + 124, y, w - 124, 26);
  y += 34;

  const int halfW = (w - 10) / 2;
  storageLabel_.setBounds(pad, y, 140, 24);
  storageCombo_.setBounds(pad + 140, y, halfW - 140, 24);

  rateLabel_.setBounds(pad + halfW + 10, y, 90, 24);
  rateCombo_.setBounds(pad + halfW + 100, y, halfW - 100, 24);
  y += 30;

  monoToggle_.setBounds(pad, y, halfW, 24);
  streamToggle_.setBounds(pad + halfW + 10, y, halfW, 24);
  y += 30;

  preloadLabel_.setBounds(pad, y, 140, 24);
  preloadCombo_.setBounds(pad + 140, y, halfW - 140, 24);

  cacheLabel_.setBounds(pad + halfW + 10, y, 90, 24);
  cacheCombo_.setBounds(pad + halfW + 100, y, halfW - 100, 24);
  y += 38;

  rootLabel_.setBounds(pad, y, w, 18);
  y += 20;
  rootValue_.setBounds(pad, y, w - 170, 24);
  rootChooseBtn_.setBounds(pad + w - 165, y, 80, 24);
  rootDefaultBtn_.setBounds(pad + w - 80, y, 80, 24);
  y += 34;

  dspHeading_.setBounds(pad, y, w, 20);
  y += 24;

  const int colW = halfW;
  simpleWav_.setBounds(pad, y, colW, 22);
  wind_.setBounds(pad + halfW + 10, y, colW, 22);
  y += 24;

  tremulant_.setBounds(pad, y, colW, 22);
  enclosure_.setBounds(pad + halfW + 10, y, colW, 22);
  y += 24;

  voicing_.setBounds(pad, y, colW, 22);
  originalPitch_.setBounds(pad + halfW + 10, y, colW, 22);
  y += 30;

  const int bottomY = getHeight() - pad - 32;
  saveBtn_.setBounds(pad, bottomY, 130, 32);
  defaultsBtn_.setBounds(pad + 140, bottomY, 140, 32);
  cancelBtn_.setBounds(getWidth() - pad - 80, bottomY, 80, 32);
}

// ==============================================================================
// OverlayPanel implementation
// ==============================================================================
OrganDialog::OverlayPanel::OverlayPanel(double& progressRef)
    : progressBar(progressRef) {
  titleLabel.setText("Installing Organ Packages", juce::dontSendNotification);
  titleLabel.setFont(juce::FontOptions(16.0f, juce::Font::bold));
  titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
  titleLabel.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(titleLabel);

  archiveLabel.setFont(juce::FontOptions(13.0f, juce::Font::bold));
  archiveLabel.setColour(juce::Label::textColourId, juce::Colour(0xff68b2ff));
  archiveLabel.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(archiveLabel);

  etaLabel.setFont(juce::FontOptions(12.0f));
  etaLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0abbd));
  etaLabel.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(etaLabel);

  progressBar.setColour(juce::ProgressBar::foregroundColourId, juce::Colour(0xff4a90e2));
  progressBar.setColour(juce::ProgressBar::backgroundColourId, juce::Colour(0xff121418));
  addAndMakeVisible(progressBar);

  fileLabel.setFont(juce::FontOptions(11.0f));
  fileLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8a95a5));
  fileLabel.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(fileLabel);

  cancelBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3b2424));
  cancelBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe58787));
  cancelBtn.onClick = [this] {
    cancelBtn.setEnabled(false);
    cancelBtn.setButtonText("Cancelling...");
    fileLabel.setText("Cancelling extraction...", juce::dontSendNotification);
    if (onCancel) onCancel();
  };
  addAndMakeVisible(cancelBtn);
}

void OrganDialog::OverlayPanel::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xd00e1014));

  auto card = getLocalBounds().withSizeKeepingCentre(
      std::min(520, getWidth() - 32), std::min(250, getHeight() - 32));

  g.setColour(juce::Colour(0xff1c2027));
  g.fillRoundedRectangle(card.toFloat(), 8.0f);

  g.setColour(juce::Colour(0xff2f3a4d));
  g.drawRoundedRectangle(card.toFloat(), 8.0f, 1.0f);
}

void OrganDialog::OverlayPanel::resized() {
  auto card = getLocalBounds().withSizeKeepingCentre(
      std::min(520, getWidth() - 32), std::min(250, getHeight() - 32));

  int y = card.getY() + 18;
  const int w = card.getWidth() - 32;
  const int x = card.getX() + 16;

  titleLabel.setBounds(x, y, w, 24);
  y += 28;
  archiveLabel.setBounds(x, y, w, 20);
  y += 22;
  etaLabel.setBounds(x, y, w, 18);
  y += 24;
  progressBar.setBounds(x, y, w, 22);
  y += 26;
  fileLabel.setBounds(x, y, w, 18);
  y += 26;
  cancelBtn.setBounds(card.getCentreX() - 50, y, 100, 28);
}

// ==============================================================================
// InstallThread implementation
// ==============================================================================
class OrganDialog::InstallThread : public juce::Thread {
public:
  InstallThread(OrganDialog& owner, const juce::File& unrar,
                const juce::Array<juce::File>& archives,
                const juce::File& destDir)
      : juce::Thread("OrganPackageInstaller"),
        owner_(&owner),
        unrar_(unrar),
        archives_(archives),
        destDir_(destDir) {}

  ~InstallThread() override {
    cancel();
    stopThread(3000);
  }

  void cancel() {
    signalThreadShouldExit();
    juce::ScopedLock sl(lock_);
    if (process_.isRunning()) {
      process_.kill();
    }
  }

  void run() override {
    destDir_.createDirectory();
    const int total = archives_.size();
    int succeeded = 0;
    juce::String lastError;

    juce::Array<juce::int64> archSizes;
    juce::int64 totalBytes = 0;
    for (const auto& a : archives_) {
      const juce::int64 sz = a.getSize();
      archSizes.add(sz);
      totalBytes += sz;
    }
    juce::int64 completedBytes = 0;

    for (int i = 0; i < total; ++i) {
      if (threadShouldExit()) break;

      const auto& arch = archives_[i];
      const juce::String archName = arch.getFileName();
      const juce::int64 thisSize = archSizes[i];

      juce::StringArray args;
      args.add(unrar_.getFullPathName());
      args.add("x");
      args.add("-o+");
      args.add("-inul");
      args.add(arch.getFullPathName());
      args.add(destDir_.getFullPathName() + juce::File::getSeparatorString());

      juce::String commandLine;
      for (const auto& a : args) {
        if (a.containsChar(' ')) {
          commandLine += "\"" + a + "\" ";
        } else {
          commandLine += a + " ";
        }
      }

      {
        juce::ScopedLock sl(lock_);
        if (threadShouldExit()) break;
        if (!process_.start(commandLine.trim(), juce::ChildProcess::wantStdErr | juce::ChildProcess::wantStdOut)) {
          lastError = "Failed to launch unrar for " + archName;
          continue;
        }
      }

      juce::String currentFile;
      double subProgress = 0.0;
      char buffer[512];

      while (process_.isRunning() && !threadShouldExit()) {
        const int bytesRead = process_.readProcessOutput(buffer, sizeof(buffer) - 1);
        if (bytesRead > 0) {
          buffer[bytesRead] = '\0';
          juce::String outputChunk(buffer);
          for (int cIdx = 0; cIdx < outputChunk.length(); ++cIdx) {
            if (outputChunk[cIdx] == '%' && cIdx >= 2) {
              int startNum = cIdx - 1;
              while (startNum >= 0 && juce::CharacterFunctions::isDigit(outputChunk[startNum])) {
                startNum--;
              }
              startNum++;
              int pct = outputChunk.substring(startNum, cIdx).getIntValue();
              if (pct >= 0 && pct <= 100) {
                subProgress = pct / 100.0;
              }
            }
          }

          int extrIdx = outputChunk.lastIndexOf("Extracting ");
          if (extrIdx >= 0) {
            juce::String tail = outputChunk.substring(extrIdx + 11).trim();
            int endLine = tail.indexOfAnyOf("\r\n");
            if (endLine > 0) tail = tail.substring(0, endLine).trim();
            if (tail.isNotEmpty()) currentFile = tail;
          }
        }

        double overallProgress = 0.0;
        if (totalBytes > 0) {
          overallProgress = (static_cast<double>(completedBytes) + static_cast<double>(thisSize) * subProgress) /
                            static_cast<double>(totalBytes);
        } else {
          overallProgress = (static_cast<double>(i) + subProgress) / static_cast<double>(total);
        }
        overallProgress = juce::jlimit(0.0, 0.999, overallProgress);

        juce::MessageManager::callAsync([owner = owner_, i, total, archName, overallProgress, subProgress, currentFile] {
          if (owner != nullptr) {
            owner->updateInstallProgress(i + 1, total, archName, overallProgress, subProgress, currentFile);
          }
        });

        juce::Thread::sleep(40);
      }

      const int exitCode = process_.getExitCode();
      if (threadShouldExit()) {
        break;
      }

      if (exitCode == 0) {
        succeeded++;
      } else {
        lastError = "unrar returned exit code " + juce::String(exitCode) + " on " + archName;
      }

      completedBytes += thisSize;
    }

    const bool aborted = threadShouldExit();
    juce::MessageManager::callAsync([owner = owner_, succeeded, total, aborted, lastError] {
      if (owner != nullptr) {
        owner->installFinished(succeeded, total, aborted, lastError);
      }
    });

    if (!aborted && succeeded > 0) {
      triggerDirectoryAudioStatPrecomputation(destDir_);
    }
  }

private:
  juce::Component::SafePointer<OrganDialog> owner_;
  const juce::File unrar_;
  const juce::Array<juce::File> archives_;
  const juce::File destDir_;
  juce::CriticalSection lock_;
  juce::ChildProcess process_;
};

// ==============================================================================
// OrganDialog implementation
// ==============================================================================
OrganDialog::OrganDialog(MasterpieceEditor& editor, MasterpieceProcessor& proc)
    : editor_(editor), proc_(proc) {
  titleLabel_.setText("Organs", juce::dontSendNotification);
  titleLabel_.setFont(juce::FontOptions(20.0f, juce::Font::bold));
  titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe8edf5));
  addAndMakeVisible(titleLabel_);

  subtitleLabel_.setText(
      "Select an installed organ, configure audio, inspect package dependencies, or unpack Hauptwerk RAR packages.",
      juce::dontSendNotification);
  subtitleLabel_.setFont(juce::FontOptions(12.0f));
  subtitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8a95a5));
  addAndMakeVisible(subtitleLabel_);

  filterBox_.setTextToShowWhenEmpty("Filter organs...", juce::Colour(0xff6a7585));
  filterBox_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff121418));
  filterBox_.setColour(juce::TextEditor::textColourId, juce::Colour(0xffe8edf5));
  filterBox_.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2f3a4d));
  filterBox_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff4a90e2));
  filterBox_.onTextChange = [this] { updateFilter(); };
  addAndMakeVisible(filterBox_);

  listBox_.setModel(this);
  listBox_.setRowHeight(48);
  listBox_.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff121418));
  listBox_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff232833));
  addAndMakeVisible(listBox_);

  // Row 1 buttons: Selected organ actions
  loadBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2d4a6e));
  loadBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe8edf5));
  loadBtn_.onClick = [this] { loadSelected(); };
  addAndMakeVisible(loadBtn_);

  adjustAudioBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2a3442));
  adjustAudioBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2d4ea));
  adjustAudioBtn_.onClick = [this] { adjustAudioSettings(); };
  addAndMakeVisible(adjustAudioBtn_);

  detailsBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  detailsBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2c8d2));
  detailsBtn_.onClick = [this] { showDetails(); };
  addAndMakeVisible(detailsBtn_);

  removeBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3b2424));
  removeBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff09898));
  removeBtn_.onClick = [this] { removeSelected(); };
  addAndMakeVisible(removeBtn_);

  // Row 2 buttons: General actions
  openOdfBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff232833));
  openOdfBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffc2c8d2));
  openOdfBtn_.onClick = [this] { openOdf(); };
  addAndMakeVisible(openOdfBtn_);

  installBtn_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff243b30));
  installBtn_.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff98e2b0));
  installBtn_.onClick = [this] { installPackages(); };
  addAndMakeVisible(installBtn_);

  loadBtn_.setEnabled(false);
  adjustAudioBtn_.setEnabled(false);
  detailsBtn_.setEnabled(false);
  removeBtn_.setEnabled(false);

  installPanel_.onCancel = [this] { cancelInstallation(); };
  addChildComponent(installPanel_);

  refreshList();
}

OrganDialog::~OrganDialog() {
  stopTimer();
  if (installThread_ != nullptr) {
    installThread_->cancel();
    installThread_->stopThread(3000);
    installThread_.reset();
  }
}

void OrganDialog::show(MasterpieceEditor& editor, MasterpieceProcessor& proc) {
  auto content = std::make_unique<OrganDialog>(editor, proc);
  content->setSize(760, 560);
  juce::DialogWindow::LaunchOptions o;
  o.content.setOwned(content.release());
  o.dialogTitle = "Organs";
  o.dialogBackgroundColour = juce::Colour(0xff1b1e24);
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = true;
  o.launchAsync();
}

void OrganDialog::closeDialog() {
  if (installThread_ != nullptr) {
    installThread_->cancel();
    installThread_->stopThread(3000);
    installThread_.reset();
  }
  if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
    dw->exitModalState(0);
  }
}

void OrganDialog::refreshList() {
  allOrgans_ = discoverOrgans(proc_);
  updateFilter();
}

void OrganDialog::updateFilter() {
  const auto filter = filterBox_.getText().trim();
  filteredOrgans_.clear();
  for (const auto& entry : allOrgans_) {
    if (filter.isEmpty() ||
        entry.name.containsIgnoreCase(filter) ||
        entry.file.getFileName().containsIgnoreCase(filter)) {
      filteredOrgans_.push_back(entry);
    }
  }
  listBox_.updateContent();

  for (int i = 0; i < static_cast<int>(filteredOrgans_.size()); ++i) {
    if (filteredOrgans_[i].isCurrent) {
      listBox_.selectRow(i);
      break;
    }
  }
  selectedRowsChanged(listBox_.getSelectedRow());
}

void OrganDialog::resized() {
  const int pad = 16;
  const int w = getWidth() - pad * 2;

  titleLabel_.setBounds(pad, 14, w, 26);
  subtitleLabel_.setBounds(pad, 40, w, 18);
  filterBox_.setBounds(pad, 64, w, 28);

  const int bottomH = 32;
  const int gap = 8;
  const int row2Y = getHeight() - pad - bottomH;
  const int row1Y = row2Y - gap - bottomH;

  const int listY = 100;
  listBox_.setBounds(pad, listY, w, row1Y - gap - listY);

  loadBtn_.setBounds(pad, row1Y, 80, bottomH);
  adjustAudioBtn_.setBounds(pad + 88, row1Y, 175, bottomH);
  detailsBtn_.setBounds(pad + 88 + 175 + gap, row1Y, 90, bottomH);
  removeBtn_.setBounds(pad + 88 + 175 + gap + 90 + gap, row1Y, 90, bottomH);

  openOdfBtn_.setBounds(pad, row2Y, 110, bottomH);
  installBtn_.setBounds(pad + 118, row2Y, 195, bottomH);

  installPanel_.setBounds(getLocalBounds());
}

void OrganDialog::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff1b1e24));
}

int OrganDialog::getNumRows() {
  return static_cast<int>(filteredOrgans_.size());
}

void OrganDialog::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                                   bool rowIsSelected) {
  if (rowNumber < 0 || rowNumber >= static_cast<int>(filteredOrgans_.size()))
    return;

  const auto& entry = filteredOrgans_[rowNumber];

  if (rowIsSelected) {
    g.setColour(juce::Colour(0xff23354d));
    g.fillRect(0, 0, width, height);
  } else if (rowNumber % 2 == 1) {
    g.setColour(juce::Colour(0xff16181f));
    g.fillRect(0, 0, width, height);
  }

  if (entry.isCurrent) {
    g.setColour(juce::Colour(0xff4a90e2));
    g.fillRect(0, 0, 4, height);
  }

  const int leftMargin = 16;
  const int rightMargin = 120;
  const int contentWidth = width - leftMargin - rightMargin;

  g.setFont(juce::FontOptions(14.0f, entry.isCurrent ? juce::Font::bold : juce::Font::plain));
  g.setColour(entry.exists ? (entry.isCurrent ? juce::Colour(0xff68b2ff) : juce::Colour(0xffe8edf5))
                           : juce::Colour(0xff6e7888));
  g.drawText(entry.name, leftMargin, 4, contentWidth, 20, juce::Justification::left, true);

  g.setFont(juce::FontOptions(11.0f));
  g.setColour(juce::Colour(0xff758092));
  juce::String pathDisplay = entry.file.getFullPathName();
  const auto dataDir = MasterpieceProcessor::dataDirectory();
  if (entry.file.isAChildOf(dataDir)) {
    pathDisplay = "OrganDefinitions/" + entry.file.getRelativePathFrom(dataDir.getChildFile("OrganDefinitions"));
  }

  juce::String sub = pathDisplay;
  if (entry.diskSpaceBytes > 0) {
    sub += "   \u2022   " + formatByteSize(entry.diskSpaceBytes);
  }
  if (!entry.packages.empty()) {
    int inst = 0;
    for (const auto& p : entry.packages) {
      if (p.isInstalled) inst++;
    }
    if (inst < static_cast<int>(entry.packages.size())) {
      sub += "   \u2022   " + juce::String(inst) + "/" + juce::String(entry.packages.size()) + " pkgs";
    }
  }
  g.drawText(sub, leftMargin, 26, contentWidth, 16, juce::Justification::left, true);

  auto badgeRect = juce::Rectangle<int>(width - 110, (height - 20) / 2, 96, 20);
  if (entry.isCurrent) {
    g.setColour(juce::Colour(0xff1b3d2b));
    g.fillRoundedRectangle(badgeRect.toFloat(), 4.0f);
    g.setColour(juce::Colour(0xff68d391));
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText("Loaded", badgeRect, juce::Justification::centred, false);
  } else if (!entry.exists) {
    g.setColour(juce::Colour(0xff3d1f1f));
    g.fillRoundedRectangle(badgeRect.toFloat(), 4.0f);
    g.setColour(juce::Colour(0xffe28080));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("Missing", badgeRect, juce::Justification::centred, false);
  } else {
    bool missingAnyPackage = false;
    for (const auto& p : entry.packages) {
      if (!p.isInstalled) {
        missingAnyPackage = true;
        break;
      }
    }

    if (missingAnyPackage) {
      g.setColour(juce::Colour(0xff382e1d));
      g.fillRoundedRectangle(badgeRect.toFloat(), 4.0f);
      g.setColour(juce::Colour(0xffe2be80));
      g.setFont(juce::FontOptions(11.0f));
      g.drawText("Missing Pkg", badgeRect, juce::Justification::centred, false);
    } else if (entry.isInstalled) {
      g.setColour(juce::Colour(0xff222a36));
      g.fillRoundedRectangle(badgeRect.toFloat(), 4.0f);
      g.setColour(juce::Colour(0xff98a9c2));
      g.setFont(juce::FontOptions(11.0f));
      g.drawText("Installed", badgeRect, juce::Justification::centred, false);
    }
  }

  g.setColour(juce::Colour(0xff20242c));
  g.fillRect(0, height - 1, width, 1);
}

void OrganDialog::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
  if (row >= 0 && row < static_cast<int>(filteredOrgans_.size())) {
    const auto& entry = filteredOrgans_[row];
    if (entry.exists) {
      loadSelected();
    }
  }
}

void OrganDialog::selectedRowsChanged(int lastRowSelected) {
  const bool valid = (lastRowSelected >= 0 &&
                      lastRowSelected < static_cast<int>(filteredOrgans_.size()));
  const bool exists = valid && filteredOrgans_[lastRowSelected].exists;
  loadBtn_.setEnabled(exists);
  adjustAudioBtn_.setEnabled(valid);
  detailsBtn_.setEnabled(valid);
  removeBtn_.setEnabled(valid);
}

void OrganDialog::deleteKeyPressed(int lastRowSelected) {
  juce::ignoreUnused(lastRowSelected);
  removeSelected();
}

void OrganDialog::returnKeyPressed(int lastRowSelected) {
  if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(filteredOrgans_.size())) {
    loadSelected();
  }
}

void OrganDialog::loadSelected() {
  const int row = listBox_.getSelectedRow();
  if (row < 0 || row >= static_cast<int>(filteredOrgans_.size())) return;
  const auto& entry = filteredOrgans_[row];
  if (!entry.exists) return;

  proc_.addRecentOrgan(entry.file);
  editor_.loadOrgan(entry.file);
  closeDialog();
}

void OrganDialog::adjustAudioSettings() {
  const int row = listBox_.getSelectedRow();
  if (row < 0 || row >= static_cast<int>(filteredOrgans_.size())) return;
  const auto entry = filteredOrgans_[row];

  auto panel = std::make_unique<OrganAudioSettingsDialog>(entry, proc_);
  juce::DialogWindow::LaunchOptions opts;
  opts.content.setOwned(panel.release());
  opts.dialogTitle = "Audio Settings - " + entry.name;
  opts.dialogBackgroundColour = juce::Colour(0xff1b1e24);
  opts.escapeKeyTriggersCloseButton = true;
  opts.useNativeTitleBar = true;
  opts.resizable = true;
  opts.launchAsync();
}

void OrganDialog::showDetails() {
  const int row = listBox_.getSelectedRow();
  if (row < 0 || row >= static_cast<int>(filteredOrgans_.size())) return;
  const auto entry = filteredOrgans_[row];

  auto panel = std::make_unique<OrganDetailsDialog>(entry, proc_, [this] {
    adjustAudioSettings();
  });
  juce::DialogWindow::LaunchOptions opts;
  opts.content.setOwned(panel.release());
  opts.dialogTitle = "Organ Details - " + entry.name;
  opts.dialogBackgroundColour = juce::Colour(0xff1b1e24);
  opts.escapeKeyTriggersCloseButton = true;
  opts.useNativeTitleBar = true;
  opts.resizable = true;
  opts.launchAsync();
}

void OrganDialog::removeSelected() {
  const int row = listBox_.getSelectedRow();
  if (row < 0 || row >= static_cast<int>(filteredOrgans_.size())) return;
  const auto entry = filteredOrgans_[row];

  juce::StringArray options;
  options.add("Remove from list (keep files on disk)");
  options.add("Delete organ and files permanently");
  options.add("Cancel");

  juce::String extraMsg;
  if (entry.diskSpaceBytes > 0) {
    extraMsg = "\nTotal disk space used: " + formatByteSize(entry.diskSpaceBytes);
  }

  auto* aw = new juce::AlertWindow(
      "Remove Organ - " + entry.name,
      "What would you like to do with \"" + entry.name + "\"?" + extraMsg,
      juce::AlertWindow::QuestionIcon);
  aw->addButton("Remove from List", 1);
  aw->addButton("Delete from Disk", 2);
  aw->addButton("Cancel", 0);
  aw->enterModalState(true, juce::ModalCallbackFunction::create([this, entry](int result) {
    if (result == 1) {
      proc_.hideOrgan(entry.file);
      proc_.removeRecentOrgan(entry.file);
      refreshList();
    } else if (result == 2) {
      juce::AlertWindow::showOkCancelBox(
          juce::AlertWindow::WarningIcon, "Confirm Permanent Deletion",
          "Are you sure you want to permanently delete \"" + entry.name + "\"?\n\n"
          "This will delete the organ definition and its associated installation package files from disk.\n"
          "This action cannot be undone.",
          "Delete Permanently", "Cancel", nullptr,
          juce::ModalCallbackFunction::create([this, entry](int confirmResult) {
            if (confirmResult == 1) {
              performDeleteOrgan(entry);
            }
          }));
    }
  }), true);
}

void OrganDialog::performDeleteOrgan(const OrganEntry& entry) {
  juce::int64 freedBytes = 0;

  if (entry.file.existsAsFile()) {
    freedBytes += entry.file.getSize();
    entry.file.deleteFile();
  }

  const auto dataDir = MasterpieceProcessor::dataDirectory();
  for (const auto& pkg : entry.packages) {
    if (pkg.isInstalled && pkg.directory.isDirectory()) {
      if (pkg.directory.isAChildOf(dataDir)) {
        freedBytes += computeDirectorySize(pkg.directory);
        pkg.directory.deleteRecursively();
      }
    }
  }

  proc_.hideOrgan(entry.file);
  proc_.removeRecentOrgan(entry.file);
  refreshList();

  juce::String msg = "\"" + entry.name + "\" has been deleted from disk";
  if (freedBytes > 0) {
    msg += ".\nFreed " + formatByteSize(freedBytes) + " of disk space.";
  }
  juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Organ Deleted", msg);
}

void OrganDialog::openOdf() {
  chooser_ = std::make_unique<juce::FileChooser>(
      "Choose an organ definition file", juce::File(),
      "*.Organ_Hauptwerk_xml;*.CustomOrgan_Hauptwerk_xml;*.organ_hauptwerk_xml;*.customorgan_hauptwerk_xml");
  chooser_->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [this](const juce::FileChooser& fc) {
        const auto file = fc.getResult();
        if (file.existsAsFile()) {
          proc_.unhideOrgan(file);
          proc_.addRecentOrgan(file);
          editor_.loadOrgan(file);
          closeDialog();
        }
      });
}

void OrganDialog::installPackages() {
  const auto unrar = findUnrarBinary();
  if (!unrar.existsAsFile()) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::WarningIcon, "unrar not found",
        "The unrar binary could not be found.\n\nPlease download the nonfree unrar binary and place it into Masterpiece's data folder:\n" +
            MasterpieceProcessor::dataDirectory().getFullPathName() +
            "\nor install it onto your system via Homebrew ('brew install unrar') or package manager.");
    return;
  }

  chooser_ = std::make_unique<juce::FileChooser>(
      "Select Hauptwerk Organ RAR Archive(s) to Install", juce::File(),
      "*.rar;*.part1.rar;*.r00;*.CompPkg_Hauptwerk_rar;*.RAR;*.PART1.RAR");
  chooser_->launchAsync(
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
          juce::FileBrowserComponent::canSelectMultipleItems,
      [this](const juce::FileChooser& fc) {
        const auto results = fc.getResults();
        if (results.isEmpty()) return;

        const auto filtered = filterArchivesForExtraction(results);
        if (filtered.isEmpty()) {
          juce::AlertWindow::showMessageBoxAsync(
              juce::AlertWindow::InfoIcon, "Installation",
              "No valid RAR archive volumes found in selection.");
          return;
        }

        startExtraction(filtered);
      });
}

void OrganDialog::startExtraction(const juce::Array<juce::File>& archives) {
  const auto unrar = findUnrarBinary();
  const auto destDir = MasterpieceProcessor::dataDirectory();

  installProgress_ = 0.0;
  installStartTimeMs_ = juce::Time::getMillisecondCounterHiRes();
  installEtaSeconds_ = -1.0;
  installPanel_.titleLabel.setText("Installing " + juce::String(archives.size()) + " Organ Package(s)", juce::dontSendNotification);
  installPanel_.archiveLabel.setText("Preparing...", juce::dontSendNotification);
  installPanel_.etaLabel.setText("Calculating time remaining...", juce::dontSendNotification);
  installPanel_.fileLabel.setText("", juce::dontSendNotification);
  installPanel_.cancelBtn.setEnabled(true);
  installPanel_.cancelBtn.setButtonText("Cancel");
  installPanel_.setVisible(true);

  listBox_.setEnabled(false);
  loadBtn_.setEnabled(false);
  adjustAudioBtn_.setEnabled(false);
  detailsBtn_.setEnabled(false);
  removeBtn_.setEnabled(false);
  openOdfBtn_.setEnabled(false);
  installBtn_.setEnabled(false);

  installThread_ = std::make_unique<InstallThread>(*this, unrar, archives, destDir);
  installThread_->startThread();
  startTimer(200);
}

void OrganDialog::cancelInstallation() {
  if (installThread_ != nullptr) {
    installThread_->cancel();
  }
}

void OrganDialog::timerCallback() {
  if (installPanel_.isVisible()) {
    installPanel_.repaint();
  }
}

void OrganDialog::updateInstallProgress(int currentArchIdx, int totalArchs, const juce::String& archName,
                                       double overallProgress, double subProgress, const juce::String& currentFile) {
  juce::ignoreUnused(subProgress);
  installProgress_ = overallProgress;
  installPanel_.archiveLabel.setText(
      juce::String::formatted("Archive %d of %d: %s", currentArchIdx, totalArchs, archName.toRawUTF8()),
      juce::dontSendNotification);

  if (currentFile.isNotEmpty()) {
    installPanel_.fileLabel.setText(currentFile, juce::dontSendNotification);
  }

  const double nowMs = juce::Time::getMillisecondCounterHiRes();
  const double elapsedSecs = std::max(0.1, (nowMs - installStartTimeMs_) / 1000.0);

  if (overallProgress > 0.02 && elapsedSecs >= 1.0) {
    const double rawEta = (elapsedSecs / overallProgress) * (1.0 - overallProgress);
    if (installEtaSeconds_ < 0.0) {
      installEtaSeconds_ = rawEta;
    } else {
      installEtaSeconds_ = 0.85 * installEtaSeconds_ + 0.15 * rawEta;
    }
    installPanel_.etaLabel.setText(humaniseEta(installEtaSeconds_) + " remaining", juce::dontSendNotification);
  } else {
    installPanel_.etaLabel.setText("Calculating time remaining...", juce::dontSendNotification);
  }
}

void OrganDialog::installFinished(int succeeded, int total, bool aborted, const juce::String& error) {
  stopTimer();
  installPanel_.setVisible(false);
  listBox_.setEnabled(true);
  openOdfBtn_.setEnabled(true);
  installBtn_.setEnabled(true);

  if (installThread_ != nullptr) {
    installThread_->stopThread(1000);
    installThread_.reset();
  }

  refreshList();

  if (aborted) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Installation Cancelled",
        "Package extraction was cancelled by user.\n" + juce::String(succeeded) + " of " + juce::String(total) + " package(s) extracted.");
  } else if (succeeded == total) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Installation Complete",
        "Successfully extracted and installed " + juce::String(succeeded) + " package(s) into Masterpiece.");
  } else {
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::WarningIcon, "Installation Finished With Errors",
        juce::String(succeeded) + " of " + juce::String(total) + " package(s) extracted successfully.\n\n" +
            (error.isNotEmpty() ? error : "Some packages failed to extract."));
  }
}

} // namespace mp::ui
