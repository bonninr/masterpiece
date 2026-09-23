// SPDX-License-Identifier: GPL-3.0-only
// ODF front-end (M1 unified ObjectList engine, ADR-014).
//
// FORMAT TRUTH (verified against real sets: Nancy demo + Lemmer, and
// OdfEdit's /Hauptwerk/ObjectList XPath): BOTH FileFormat="Organ" (full)
// and FileFormat="CustomOrgan" (CODM) use
//   <ObjectList ObjectType="T"><T><Field>value</Field>...</T></ObjectList>
// Expanded files use full field names; compacted files use the OdfEdit-dict
// single-letter codes — field() tries the full name first, then the code.
// (An earlier revision parsed a flat attribute syntax that no real file
// uses; it was replaced wholesale after the Nancy finding.)
//
// - Unknown tables -> warnings + unknownTables (ADR-002), never hard errors.
// - Dangling IDREFs -> OdfDiagnostics::danglingIds; missing _General (full
//   ODF) -> error; missing WAVs/bitmaps on disk -> missing lists (M1.1-M1.4
//   validator queries).
// - No JUCE anywhere: plain std::ifstream file I/O, so the validator,
//   tests and core-only builds never need an audio/UI stack.
#include "OdfLoader.h"

#include "CodmCompiler.h"
#include "Temperament.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace mp {
namespace {

// Table elements recognised as ObjectList ObjectType values (full + CODM
// families, exact case as written by Hauptwerk). Everything else warns
// (forward-compat) — the validator surfaces the list against real sets.
const std::unordered_set<std::string>& knownTables() {
  static const std::unordered_set<std::string> tables = {
    // _General + packages + samples
    "_General", "RequiredInstallationPackage", "Sample",
    // pipework core
    "Rank", "Pipe_SoundEngine01", "Pipe_SoundEngine01_Layer",
    "Pipe_SoundEngine01_AttackSample", "Pipe_SoundEngine01_ReleaseSample",
    // keyboards / divisions / key actions
    "Division", "DivisionInput", "Keyboard", "KeyboardKey", "KeyImageSet",
    // stops / switches
    "Stop", "StopRank", "KeyAction", "Switch", "SwitchLinkage",
    // display
    "DisplayPage", "ImageSet", "ImageSetElement", "ImageSetInstance",
    "TextInstance", "TextStyle",
    // combinations
    "Combination", "CombinationElement",
    // physical modelling (parsed in M2/M3; recognised now so they don't warn)
    "Enclosure", "EnclosurePipe", "WindCompartment", "WindCompartmentLinkage",
    "ContinuousControl", "ContinuousControlDoubleLinkage",
    "ContinuousControlImageSetStage", "ContinuousControlLinkage",
    "ContinuousControlStageSwitch", "Tremulant", "TremulantWaveform",
    "TremulantWaveformPipe", "Temperament",
    // CODM family (compiled by MP-CODM-HWv9, see CodmCompiler)
    "General", "Package", "Coupler", "Noise",
  };
  return tables;
}

bool hasSuffixInsensitive(const std::string& value, const char* suffix) {
  const std::string suf(suffix);
  if (value.size() < suf.size()) return false;
  return std::equal(suf.rbegin(), suf.rend(), value.rbegin(),
                    [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

std::string trim(std::string s) {
  const auto isSpace = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                  [&](char c) { return !isSpace(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [&](char c) { return !isSpace(c); })
              .base(),
          s.end());
  return s;
}

// Field with full-name-first, compact-code fallback (OdfEdit dict).
// Reads <Field>value</Field> child text (trimmed for pretty-printed files).
std::string field(const pugi::xml_node& row, const char* full, const char* code = nullptr) {
  auto e = row.child(full);
  if (!e && code != nullptr) e = row.child(code);
  return e ? trim(e.child_value()) : std::string{};
}

int fieldInt(const pugi::xml_node& row, const char* full, const char* code, int dflt) {
  const std::string v = field(row, full, code);
  if (v.empty()) return dflt;
  char* end = nullptr;
  const long r = std::strtol(v.c_str(), &end, 10);
  return (end != v.c_str()) ? static_cast<int>(r) : dflt;
}

double fieldDouble(const pugi::xml_node& row, const char* full, const char* code, double dflt) {
  const std::string v = field(row, full, code);
  if (v.empty()) return dflt;
  char* end = nullptr;
  const double r = std::strtod(v.c_str(), &end);
  return (end != v.c_str()) ? r : dflt;
}

bool fieldBool(const pugi::xml_node& row, const char* full, const char* code, bool dflt) {
  const std::string v = field(row, full, code);
  if (v.empty()) return dflt;
  const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(v[0])));
  if (c == 'y' || c == '1' || c == 't') return true;
  if (c == 'n' || c == '0' || c == 'f') return false;
  return dflt;
}

std::string lower(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Hauptwerk installs each package under a SIX-DIGIT zero-padded directory:
// package 2514 lives in OrganInstallationPackages/002514. Getting the padding
// wrong resolves nothing and the organ loads silent.
std::string packageDirName(Id packageId) {
  std::string digits = std::to_string(packageId);
  if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
  return digits;
}

bool diskMissing(const std::string& rootDir, const std::string& rel) {
  if (rootDir.empty() || rel.empty()) return false;
  std::string p = rel;
  std::replace(p.begin(), p.end(), '\\', '/');
  std::error_code ec;
  return !std::filesystem::exists(std::filesystem::path(rootDir) / p, ec);
}

// Iterate rows of every <ObjectList ObjectType="table"> block (exact-case
// match; CODM matching lives in parseCodm which is case-insensitive).
template <typename Fn>
void forEachRow(const pugi::xml_node& odfRoot, const char* table, Fn&& fn) {
  for (pugi::xml_node list : odfRoot.children()) {
    if (list.type() != pugi::node_element) continue;
    if (lower(list.name()) != "objectlist") continue;
    if (std::string(list.attribute("ObjectType").value()) != table) continue;
    for (pugi::xml_node row : list.children()) {
      if (row.type() != pugi::node_element) continue;
      fn(row);
    }
  }
}

bool isEncryptedFile(const std::string& file) {
  return hasSuffixInsensitive(file, ".hbw") || hasSuffixInsensitive(file, ".hbx");
}

void reportEncrypted(OrganModel& model, OdfDiagnostics& diag, const std::string& file) {
  if (file.empty()) return;
  model.hasEncryptedSamples = true;
  if (std::find(diag.encryptedSamples.begin(), diag.encryptedSamples.end(), file) ==
      diag.encryptedSamples.end())
    diag.encryptedSamples.push_back(file);
}

} // namespace

// Forward: CODM ObjectList compiler (defined at end of file).
bool parseCodm(const pugi::xml_node& odfRoot, const OdfLoader::Options& odfOpts,
               OrganModel& model, OdfDiagnostics& diag);

OdfType OdfLoader::detectType(const std::string& xmlHead, const std::string& fileName) {
  auto has = [&](const char* s) { return xmlHead.find(s) != std::string::npos; };
  if (fileName.find("CustomOrgan") != std::string::npos ||
      has("FileFormat=\"CustomOrgan\"") || has("CustomOrgan_Hauptwerk"))
    return OdfType::Codm;
  if (fileName.find("Organ_Hauptwerk") != std::string::npos ||
      has("FileFormat=\"Organ\"") || has("Organ_Hauptwerk") || has("_General"))
    return OdfType::Full;
  return OdfType::Unknown;
}

bool OdfLoader::load(const std::string& odfPath, const Options& opts,
                     OrganModel& outModel, OdfDiagnostics& outDiag, ProgressFn progress) {
  (void)progress;
  std::ifstream f(odfPath, std::ios::binary);
  if (!f) {
    outDiag.errors.emplace_back("cannot open ODF file: " + odfPath);
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return loadFromXmlString(ss.str(), odfPath, opts, outModel, outDiag);
}

bool OdfLoader::loadFromXmlString(const std::string& xml, const std::string& fileNameHint,
                                  const Options& opts, OrganModel& outModel,
                                  OdfDiagnostics& outDiag) {
  outModel = OrganModel{};

  pugi::xml_document doc;
  const auto parseResult = doc.load_string(xml.c_str());
  if (!parseResult) {
    outDiag.errors.emplace_back(std::string("XML parse error at offset ") +
                                std::to_string(parseResult.offset) + ": " +
                                parseResult.description());
    return false;
  }

  // Root is <Hauptwerk> in both formats (FileFormat attr selects the
  // vocabulary). A bare table-root fragment (unit tests, SQLite exports)
  // is accepted as a degenerate single-table document.
  pugi::xml_node docEl = doc.document_element();
  pugi::xml_node odfRoot = docEl;
  const std::string docName = docEl.name();
  const std::string fileFormat = docEl.attribute("FileFormat").value();
  if (docName == "Hauptwerk" && fileFormat == "CustomOrgan")
    outModel.odfType = OdfType::Codm;
  else if (docName == "Hauptwerk")
    outModel.odfType = OdfType::Full;
  else if (docName == "Organ_Hauptwerk_xml")
    outModel.odfType = OdfType::Full;
  else if (docName == "CustomOrgan_Hauptwerk_xml")
    outModel.odfType = OdfType::Codm;
  else
    outModel.odfType = detectType(xml.substr(0, 4096), fileNameHint);

  if (outModel.odfType == OdfType::Unknown) {
    outDiag.errors.emplace_back("Unknown ODF type: not Full (*.Organ_Hauptwerk_xml) nor CODM (*.CustomOrgan_Hauptwerk_xml)");
    return false;
  }

  // CODM files take the ObjectList compiler, never the full-ODF tables.
  if (outModel.odfType == OdfType::Codm)
    return parseCodm(odfRoot, opts, outModel, outDiag);

  // Unknown tables: warn + record, never fail (ADR-002).
  {
    std::unordered_set<std::string> seen;
    for (pugi::xml_node list : odfRoot.children()) {
      if (list.type() != pugi::node_element) continue;
      if (lower(list.name()) != "objectlist") continue;
      const std::string name = list.attribute("ObjectType").value();
      if (name.empty() || knownTables().count(name) != 0 || seen.count(name) != 0) continue;
      seen.insert(name);
      outModel.unknownTables.push_back(name);
      // Warn only when the table has rows. Hauptwerk writes many object lists
      // as empty placeholders -- all 22 sets on hand carry ReversiblePiston,
      // ExternalRank, SwitchExclusiveSelectGroup and three more with nothing
      // in them -- and a warning for a list with no content reads as an
      // unsupported feature. It sent a whole implementation plan after six
      // features no set here uses. A table that DOES have rows is the case
      // worth hearing about, and it says how many, so it stands out.
      size_t rows = 0;
      for (pugi::xml_node row : list.children())
        if (row.type() == pugi::node_element) ++rows;
      if (rows > 0)
        outDiag.warnings.emplace_back("unknown table preserved (forward-compat): " +
                                      name + " -- " + std::to_string(rows) +
                                      " row(s) ignored");
    }
  }

  // ---- M1.1: _General (required for full ODF) ----
  {
    bool found = false;
    forEachRow(odfRoot, "_General", [&](pugi::xml_node row) {
      if (found) return;
      found = true;
      outModel.organName = field(row, "Identification_Name", "c");
      outModel.church = field(row, "OrganInfo_Location", "e");
      outModel.builder = field(row, "OrganInfo_Builder", "f");
      outModel.uniqueOrganId = fieldInt(row, "Identification_UniqueOrganID", "b", 0);
      outModel.organVersion = field(row, "Control_OrganVersion", "u");
      outModel.basePitchHz = fieldDouble(row, "AudioEngine_BasePitchHz", "n1", 440.0);
      // A zero here means "not stated", not "silence". The Barton theatre
      // consoles write 0, and taking it literally tuned every pipe to 0 Hz --
      // an organ that loads, draws and plays nothing audible.
      if (!(outModel.basePitchHz > 0.0)) outModel.basePitchHz = 440.0;
      // The producer's own output trim, so two sets recorded at different
      // levels play at a comparable loudness. Every set we have declares one
      // and they are not all zero: Azzio +2 dB, Raszczyce -4 dB. Ignoring it
      // leaves a 6 dB step between sets that the producer meant to remove.
      outModel.audioOutputTrimDb =
          fieldDouble(row, "AudioOut_AmplitudeLevelAdjustDecibels", nullptr, 0.0);
    });
    if (!found)
      outDiag.errors.emplace_back("missing required table: _General");
    // Asked only who this organ is; that is now known.
    if (opts.headerOnly) return outDiag.ok();
  }

  // ---- M1.1: Sample table -> registry ----
  forEachRow(odfRoot, "Sample", [&](pugi::xml_node row) {
    SampleRef s;
    s.sampleId = fieldInt(row, "SampleID", "a", 0);
    s.packageId = field(row, "InstallationPackageID", "b");
    s.fileName = field(row, "SampleFilename", "c");
    s.encrypted = isEncryptedFile(s.fileName) ||
                  fieldBool(row, "LicenceSerialNumRequiredForSampleFile", "h", false);
    s.pitchHz = fieldDouble(row, "Pitch_ExactSamplePitch", "g", 0.0);
    s.installationPackageId = fieldInt(row, "InstallationPackageID", "b", 0);
    s.midiNote = fieldInt(row, "Pitch_NormalMIDINoteNumber", "f", -1);
    // Which of those fields the set means. Friesach declares code 1 on 11400
    // of its 12146 samples and leaves every pitch field empty, so a loader
    // that reads the fields and not the code learns nothing from it at all.
    s.pitchMethodCode = fieldInt(row, "Pitch_SpecificationMethodCode", "d", -1);
    s.rankBasePitch64ftHarmonicNum = fieldInt(row, "Pitch_RankBasePitch64ftHarmonicNum", "e", 8);
    if (s.sampleId != 0) outModel.samples[s.sampleId] = s;
    if (s.encrypted) reportEncrypted(outModel, outDiag, s.fileName);
  });

  // Encrypted *Filename fields anywhere else (nested spots, ADR-003).
  for (auto node : doc.select_nodes("//*[contains(name(), 'Filename')]")) {
    const std::string file = trim(node.node().child_value());
    if (isEncryptedFile(file)) reportEncrypted(outModel, outDiag, file);
  }
  if (outModel.hasEncryptedSamples)
    // Said plainly, and without an internal issue number: a player reading
    // this needs to know the set cannot be opened by anything but the program
    // it was encrypted for, and that nothing here is broken.
    outDiag.warnings.emplace_back(
        "This set's samples are encrypted (.hbw/.hbx). Encrypted sets can only "
        "be played by the program they were encrypted for.");

  // ---- M1.2 validator: missing WAVs on disk ----
  // SampleFilename is relative to the sample's OWN installation package, not
  // to the set root (see resolvePath in SampleLibrary.cpp, which resolves the
  // same samples for real at load time) -- so a package-bearing sample has to
  // be checked under OrganInstallationPackages/<id>, the same place the
  // loader will actually look, or every packaged sample reports missing
  // regardless of whether it truly is.
  if (!opts.organRootDir.empty()) {
    for (const auto& [id, s] : outModel.samples) {
      (void)id;
      if (s.encrypted || s.fileName.empty()) continue;
      std::string rel = s.fileName;
      if (s.installationPackageId > 0)
        rel = "OrganInstallationPackages/" + packageDirName(s.installationPackageId) +
              "/" + rel;
      if (diskMissing(opts.organRootDir, rel))
        outDiag.missingSampleFiles.push_back(s.fileName);
    }
  }

  // ---- M1.2: Rank table ----
  forEachRow(odfRoot, "Rank", [&](pugi::xml_node row) {
    Rank r;
    r.rankId = fieldInt(row, "RankID", "a", 0);
    r.name = field(row, "Name", "b");
    if (r.rankId != 0) outModel.ranks[r.rankId] = std::move(r);
  });

  // ---- M1.2: Pipe table (linked into ranks) ----
  // Count first: vectors must not reallocate after pipeById takes addresses.
  {
    std::unordered_map<Id, size_t> pipeCount;
    forEachRow(odfRoot, "Pipe_SoundEngine01", [&](pugi::xml_node row) {
      pipeCount[fieldInt(row, "RankID", "b", 0)]++;
    });
    for (auto& [id, rank] : outModel.ranks) {
      const auto it = pipeCount.find(id);
      if (it != pipeCount.end()) rank.pipes.reserve(it->second);
    }
  }
  std::unordered_map<Id, Pipe*> pipeById;
  forEachRow(odfRoot, "Pipe_SoundEngine01", [&](pugi::xml_node row) {
    Pipe p;
    p.pipeId = fieldInt(row, "PipeID", "a", 0);
    const Id rankId = fieldInt(row, "RankID", "b", 0);
    p.midiNote = fieldInt(row, "NormalMIDINoteNumber", "d", 60);
    p.palletSwitchId = fieldInt(row, "ControllingPalletSwitchID", "c", 0);
    p.baseTuningDeviationCents =
        fieldDouble(row, "Pitch_Tempered_BaseTuningDeviation", "g", 0.0);
    // What this pipe does to the wind: where it draws from, where it exhausts
    // to, and how much it draws. The flow rate is the number that makes a
    // 32-foot reed sag the organ and a 2-foot flute not: on Nancy it varies by
    // more than an order of magnitude within a single rank.
    p.windMassFlowKgPerSec = fieldDouble(
        row, "WindSupply_MassFlowRateKilogramsPerSecAtReferencePressureDiff", "t",
        0.0);
    p.windAirConsumption = p.windMassFlowKgPerSec; // the CODM path's own name
    p.windSourceCompartmentId =
        fieldInt(row, "WindSupply_SourceWindCompartmentID", "r", 0);
    p.windOutputCompartmentId =
        fieldInt(row, "WindSupply_OutputWindCompartmentID", "s", 0);
    p.windRefPressureInches =
        fieldDouble(row, "WindSupply_ReferencePressureDifferenceInches", "u", 1.0);
    if (p.windRefPressureInches <= 0.0) p.windRefPressureInches = 1.0;
    // Absent, not "equal to 8": defaulting to 8 and then treating 8 as
    // permission to look at the other field cannot tell a rank that really is
    // a unison from one that said nothing. A set declaring a tempered 8 and a
    // plain 32 would be read as 32 -- two octaves up, on a rank whose own
    // definition says otherwise. Sentinel 0 means absent, and only absent
    // falls through.
    p.basePitch64ftHarmonicNum =
        fieldInt(row, "Pitch_Tempered_RankBasePitch64ftHarmonicNum", "f", 0);
    if (p.basePitch64ftHarmonicNum <= 0)
      p.basePitch64ftHarmonicNum =
          fieldInt(row, "Pitch_RankBasePitch64ftHarmonicNum", nullptr, 0);
    if (p.basePitch64ftHarmonicNum <= 0) p.basePitch64ftHarmonicNum = 8;
    // The pitch this pipe sounded on the instrument that was sampled. Kept
    // separate from the sample's own recorded pitch: the difference between
    // the two IS the pipe's detuning, and a set that records a pipe slightly
    // out of tune means it.
    p.originalOrganPitchHz =
        fieldDouble(row, "Pitch_OriginalOrgan_PitchHz", "m", 0.0);
    // M2: harmonicNum (f), original-organ pitch (l/m) feed the temperament solver.
    if (p.pipeId == 0) return;
    auto rankIt = outModel.ranks.find(rankId);
    if (rankIt == outModel.ranks.end()) {
      outDiag.danglingIds.push_back(rankId);
      return;
    }
    rankIt->second.pipes.push_back(std::move(p));
    pipeById[p.pipeId] = &rankIt->second.pipes.back();
  });

  // ---- M1.2: Layer table (linked into pipes; reserve like pipes above) ----
  {
    std::unordered_map<Id, size_t> layerCount;
    forEachRow(odfRoot, "Pipe_SoundEngine01_Layer", [&](pugi::xml_node row) {
      layerCount[fieldInt(row, "PipeID", "b", 0)]++;
    });
    for (auto& [id, rank] : outModel.ranks) {
      (void)id;
      for (auto& pipe : rank.pipes) {
        const auto it = layerCount.find(pipe.pipeId);
        if (it != layerCount.end()) pipe.layers.reserve(it->second);
      }
    }
  }
  std::unordered_map<Id, PipeLayer*> layerById;
  forEachRow(odfRoot, "Pipe_SoundEngine01_Layer", [&](pugi::xml_node row) {
    const Id layerId = fieldInt(row, "LayerID", "a", 0);
    const Id pipeId = fieldInt(row, "PipeID", "b", 0);
    if (layerId == 0) return;
    auto pipeIt = pipeById.find(pipeId);
    if (pipeIt == pipeById.end()) {
      outDiag.danglingIds.push_back(pipeId);
      return;
    }
    PipeLayer layer;
    layer.layerId = layerId;
    layer.gainDb = fieldDouble(row, "AmpLvl_LevelAdjustDecibels", "h", 0.0);
    layer.optimalChannel = fieldInt(row, "AudioOut_OptimalChannelFormatCode", "o", 0);
    layer.optimalResolution = fieldInt(row, "AudioOut_OptimalSampleResolutionCode", "p", 0);
    // How the organ's own level sliders reach this layer. Every layer of a
    // real set names one.
    layer.ampScalingControlId =
        fieldInt(row, "AmpLvl_ScalingContinuousControlID", nullptr, 0);
    layer.pitchControlId =
        fieldInt(row, "PitchLvl_IncrementingContinuousControlID", nullptr, 0);
    layer.pitchSensitivityHzPerUnit = fieldDouble(
        row, "PitchLvl_IncrementingCtsCtrlSensitivityHzPerCtrlUnit", nullptr, 0.0);
    // How hard the key is struck reaches the pipe's level, and how far this
    // layer trims the chest's tremulant depth. All four are stated per layer
    // and two thirds of the corpus fills them; unread they made every note
    // one level and every stop on a tremmed chest wobble alike.
    layer.velSensMaxAttenDb = fieldDouble(
        row, "AmpLvl_VelocitySensitivityMaxAttenuationDecibels", nullptr, 0.0);
    layer.invertVelocitySens =
        fieldBool(row, "AmpLvl_InvertVelocitySensitivity", nullptr, false);
    layer.tremAmpDepthAdjustDb = fieldDouble(
        row, "AmpLvl_TremulantModDepthAdjustDecibels", nullptr, 0.0);
    layer.tremPitchDepthAdjustPct = fieldDouble(
        row, "PitchLvl_TremulantModDepthAdjustPercent", nullptr, 100.0);
    pipeIt->second->layers.push_back(std::move(layer));
    layerById[layerId] = &pipeIt->second->layers.back();
  });

  // ---- M1.2: Attack samples (M1: first-match semantics; full matrix in M2.1) ----
  forEachRow(odfRoot, "Pipe_SoundEngine01_AttackSample", [&](pugi::xml_node row) {
    const Id layerId = fieldInt(row, "LayerID", "b", 0);
    const Id sampleId = fieldInt(row, "SampleID", "c", 0);
    auto layerIt = layerById.find(layerId);
    auto sampleIt = outModel.samples.find(sampleId);
    if (layerIt == layerById.end()) {
      outDiag.danglingIds.push_back(layerId);
      return;
    }
    if (sampleIt == outModel.samples.end()) {
      outDiag.danglingIds.push_back(sampleId);
      return;
    }
    AttackSample attack;
    attack.id = fieldInt(row, "UniqueID", "a", 0);
    attack.sample = sampleIt->second;
    attack.velHigh = fieldInt(row, "AttackSelCriteria_HighestVelocity", "h", 127);
    attack.minTimeSinceCloseMs =
        fieldInt(row, "AttackSelCriteria_MinTimeSincePrevPipeCloseMs", "i", 0);
    attack.ctsHigh = fieldInt(row, "AttackSelCriteria_HighestCtsCtrlValue", "j", 127);
    attack.loadStartType = fieldInt(row, "LoadSampleRange_StartPositionTypeCode", "d", 0);
    attack.loadStartValue = fieldInt(row, "LoadSampleRange_StartPositionValue", "e", 0);
    attack.loadEndType = fieldInt(row, "LoadSampleRange_EndPositionTypeCode", "f", 0);
    attack.loadEndValue = fieldInt(row, "LoadSampleRange_EndPositionValue", "g", 0);
    layerIt->second->attacks.push_back(std::move(attack));
    layerIt->second->loopCrossfadeMs =
        fieldDouble(row, "LoopCrossfadeLengthInSrcSampleMs", "k",
                    layerIt->second->loopCrossfadeMs);
  });

  // ---- M1.2: Release samples (coupled to the played attack per voice, M2.1) ----
  forEachRow(odfRoot, "Pipe_SoundEngine01_ReleaseSample", [&](pugi::xml_node row) {
    const Id layerId = fieldInt(row, "LayerID", "b", 0);
    const Id sampleId = fieldInt(row, "SampleID", "c", 0);
    auto layerIt = layerById.find(layerId);
    auto sampleIt = outModel.samples.find(sampleId);
    if (layerIt == layerById.end()) {
      outDiag.danglingIds.push_back(layerId);
      return;
    }
    if (sampleIt == outModel.samples.end()) {
      outDiag.danglingIds.push_back(sampleId);
      return;
    }
    ReleaseSample release;
    release.id = fieldInt(row, "UniqueID", "a", 0);
    release.sample = sampleIt->second;
    release.attackVelHigh =
        fieldInt(row, "AttackSelCriteria_HighestVelocity", "h", 127);
    release.attackCtsHigh =
        fieldInt(row, "AttackSelCriteria_HighestCtsCtrlValue", "j", 127);
    release.velHigh = fieldInt(row, "ReleaseSelCriteria_HighestVelocity", "p", 127);
    release.ctsHigh = fieldInt(row, "ReleaseSelCriteria_HighestCtsCtrlValue", "r", 127);
    release.scaleAmplitude = fieldBool(row, "ScaleAmplitudeAutomatically", "k", true);
    release.preferLinkedAttackId =
        fieldInt(row, "ReleaseSelCriteria_PreferThisRelForAttackID", "s", 0);
    release.loadStartType = fieldInt(row, "LoadSampleRange_StartPositionTypeCode", "d", 0);
    release.loadStartValue = fieldInt(row, "LoadSampleRange_StartPositionValue", "e", 0);
    release.loadEndType = fieldInt(row, "LoadSampleRange_EndPositionTypeCode", "f", 0);
    release.loadEndValue = fieldInt(row, "LoadSampleRange_EndPositionValue", "g", 0);
    release.holdTimeMsHigh =
        fieldInt(row, "ReleaseSelCriteria_LatestKeyReleaseTimeMs", "q", INT32_MAX);
    release.releaseCrossfadeMs =
        fieldDouble(row, "ReleaseCrossfadeLengthMs", "n", release.releaseCrossfadeMs);
    release.phaseAlign = fieldBool(row, "PhaseAlignAutomatically", "m", false);
    layerIt->second->releases.push_back(std::move(release));
  });

  // ---- M2.1 validator: velocity coverage + dead release branches ----
  // A velocity is covered when some attack's ceiling reaches it (ceilings
  // are the only velocity dimension HW stores). A prefer-linked release
  // naming a nonexistent attack id in its layer can never win pass 0 and
  // only sounds via file-order fallback — flagged, not failed.
  for (const auto& [rankId, rank] : outModel.ranks) {
    (void)rankId;
    for (const auto& pipe : rank.pipes) {
      for (const auto& layer : pipe.layers) {
        for (int v = 0; v < 128; ++v) {
          bool covered = false;
          for (const auto& a : layer.attacks)
            if (v <= a.velHigh) {
              covered = true;
              break;
            }
          if (!covered) {
            outDiag.layersUncoveredVelocity.push_back(layer.layerId);
            break;
          }
        }
        std::unordered_set<Id> attackIds;
        for (const auto& a : layer.attacks) attackIds.insert(a.id);
        for (const auto& r : layer.releases)
          if (r.preferLinkedAttackId != 0 && attackIds.count(r.preferLinkedAttackId) == 0)
            outDiag.deadReleaseBranches.push_back(r.id);
      }
    }
  }

  // ---- M1.3: Keyboard table ----
  forEachRow(odfRoot, "Keyboard", [&](pugi::xml_node row) {
    Keyboard k;
    k.keyboardId = fieldInt(row, "KeyboardID", "a", 0);
    k.name = field(row, "Name", "b");
    k.assignmentCode =
        fieldInt(row, "DefaultInputOutputKeyboardAsgnCode", "d", 0);
    k.primaryDivisionHint =
        fieldInt(row, "Hint_PrimaryAssociatedDivisionID", "x", 0);
    k.accessibleForInput = fieldBool(row, "AccessibleForInput", "e", true);
    k.numKeys = fieldInt(row, "KeyGen_NumberOfKeys", "h", 0);
    k.firstMidiNote = fieldInt(row, "KeyGen_MIDINoteNumberOfFirstKey", "i", 0);
    // Where the console draws this manual, if it draws it at all.
    k.keyImageSetId = fieldInt(row, "KeyGen_KeyImageSetID", "j", 0);
    k.displayPageId = fieldInt(row, "KeyGen_DisplayPageID", "k", 0);
    k.dispLeftPx = fieldInt(row, "KeyGen_DispKeyboardLeftXPos", "l", 0);
    k.dispTopPx = fieldInt(row, "KeyGen_DispKeyboardTopYPos", "m", 0);
    // The same manual on the other console layouts, with its own key artwork.
    static constexpr const char* kAltKb[3][3] = {
        {"KeyGen_AlternateScreenLayout1_KeyImageSetID",
         "KeyGen_AlternateScreenLayout1_DispKeyboardLeftXPos",
         "KeyGen_AlternateScreenLayout1_DispKeyboardTopYPos"},
        {"KeyGen_AlternateScreenLayout2_KeyImageSetID",
         "KeyGen_AlternateScreenLayout2_DispKeyboardLeftXPos",
         "KeyGen_AlternateScreenLayout2_DispKeyboardTopYPos"},
        {"KeyGen_AlternateScreenLayout3_KeyImageSetID",
         "KeyGen_AlternateScreenLayout3_DispKeyboardLeftXPos",
         "KeyGen_AlternateScreenLayout3_DispKeyboardTopYPos"}};
    static constexpr const char* kAltKbCodes[3][3] = {
        {"n", "p", "q"}, {"r", "s", "t"}, {"u", "v", "w"}};
    for (int a = 0; a < 3; ++a) {
      k.altKeyImageSetId[a] = fieldInt(row, kAltKb[a][0], kAltKbCodes[a][0], 0);
      if (k.altKeyImageSetId[a] == 0) continue;
      k.altLeftPx[a] = fieldInt(row, kAltKb[a][1], kAltKbCodes[a][1], 0);
      k.altTopPx[a] = fieldInt(row, kAltKb[a][2], kAltKbCodes[a][2], 0);
    }
    if (k.keyboardId != 0) outModel.keyboards[k.keyboardId] = k;
  });

  // ---- KeyImageSet: the per-shape artwork a drawn manual is assembled from.
  // Compact letter codes verified against the OdfEdit attribute dictionary in
  // specs/odfedit, not guessed. ----
  forEachRow(odfRoot, "KeyImageSet", [&](pugi::xml_node row) {
    KeyImageSet k;
    k.keyImageSetId = fieldInt(row, "KeyImageSetID", "a", 0);
    if (k.keyImageSetId == 0) return;
    k.name = field(row, "Name", "b");
    k.shapeCF = fieldInt(row, "KeyShapeImageSetID_CF", "c", 0);
    k.shapeD = fieldInt(row, "KeyShapeImageSetID_D", "d", 0);
    k.shapeEB = fieldInt(row, "KeyShapeImageSetID_EB", "e", 0);
    k.shapeG = fieldInt(row, "KeyShapeImageSetID_G", "f", 0);
    k.shapeA = fieldInt(row, "KeyShapeImageSetID_A", "g", 0);
    k.shapeWholeNatural =
        fieldInt(row, "KeyShapeImageSetID_WholeNatural", "h", 0);
    k.shapeSharp = fieldInt(row, "KeyShapeImageSetID_Sharp", "i", 0);
    k.shapeFirstKeyDA = fieldInt(row, "KeyShapeImageSetID_FirstKeyDA", "j", 0);
    k.shapeFirstKeyG = fieldInt(row, "KeyShapeImageSetID_FirstKeyG", "k", 0);
    k.shapeLastKeyDG = fieldInt(row, "KeyShapeImageSetID_LastKeyDG", "l", 0);
    k.shapeLastKeyA = fieldInt(row, "KeyShapeImageSetID_LastKeyA", "m", 0);
    k.indexEngaged = fieldInt(row, "ImageIndexWithinImageSets_Engaged", "n", 2);
    k.indexDisengaged =
        fieldInt(row, "ImageIndexWithinImageSets_Disengaged", "p", 1);
    k.spacingNaturalToNatural = fieldInt(
        row, "HorizSpacingPixels_LeftOfNaturalFromLeftOfNatural", "q", 0);
    k.spacingCFToSharp =
        fieldInt(row, "HorizSpacingPixels_LeftOfCFSharpFromLeftOfCF", "r", 0);
    k.spacingDAToSharp =
        fieldInt(row, "HorizSpacingPixels_LeftOfDASharpFromLeftOfDA", "s", 0);
    k.spacingGToSharp =
        fieldInt(row, "HorizSpacingPixels_LeftOfGSharpFromLeftOfG", "t", 0);
    k.spacingSharpToDG =
        fieldInt(row, "HorizSpacingPixels_LeftOfDGFromLeftOfCFSharp", "u", 0);
    k.spacingSharpToEB =
        fieldInt(row, "HorizSpacingPixels_LeftOfEBFromLeftOfDASharp", "v", 0);
    k.spacingSharpToA =
        fieldInt(row, "HorizSpacingPixels_LeftOfAFromLeftOfGSharp", "w", 0);
    outModel.keyImageSets[k.keyImageSetId] = std::move(k);
  });

  // ---- KeyboardKey: a manual's keys, as individual switches ----
  // A set whose manuals are part of a photographed backdrop draws each key as
  // its own clickable switch rather than assembling them from a KeyImageSet.
  // Both shapes have to be playable, and this is the one Nancy uses.
  forEachRow(odfRoot, "KeyboardKey", [&](pugi::xml_node row) {
    KeyboardKeyRef k;
    k.keyboardId = fieldInt(row, "KeyboardID", "a", 0);
    const Id switchId = fieldInt(row, "SwitchID", "b", 0);
    k.midiNote = fieldInt(row, "NormalMIDINoteNumber", "c", -1);
    if (switchId == 0 || k.keyboardId == 0 || k.midiNote < 0) return;
    outModel.keyboardKeys[switchId] = k;
  });

  // ---- M1.3: Division table ----
  forEachRow(odfRoot, "Division", [&](pugi::xml_node row) {
    Division d;
    d.divisionId = fieldInt(row, "DivisionID", "a", 0);
    d.name = field(row, "Name", "b");
    if (d.divisionId != 0) outModel.divisions[d.divisionId] = std::move(d);
  });

  // keyboard -> division links (Hint Primary/Second/ThirdAssociatedDivisionID).
  forEachRow(odfRoot, "Keyboard", [&](pugi::xml_node row) {
    const Id kbId = fieldInt(row, "KeyboardID", "a", 0);
    const Id hints[3] = {
      fieldInt(row, "Hint_PrimaryAssociatedDivisionID", "x", 0),
      fieldInt(row, "Hint_SecondAssociatedDivisionID", "y", 0),
      fieldInt(row, "Hint_ThirdAssociatedDivisionID", "z", 0),
    };
    for (Id divId : hints) {
      if (divId == 0) continue;
      auto divIt = outModel.divisions.find(divId);
      if (divIt == outModel.divisions.end()) {
        outDiag.danglingIds.push_back(divId);
        continue;
      }
      divIt->second.keyboardIds.push_back(kbId);
    }
  });

  // ---- M1.3: KeyAction table (full matrix incl. traps/pizz in M3) ----
  forEachRow(odfRoot, "KeyAction", [&](pugi::xml_node row) {
    KeyAction ka;
    ka.sourceKeyboard = fieldInt(row, "SourceKeyboardID", "a", 0);
    // Which of the two destination fields applies. Real organs mostly leave
    // DestIsKeyboardNotDivision unstated and simply write the one that
    // matters, so presence decides and the flag only breaks a tie. Trusting
    // the flag's default instead threw away every division-terminated edge —
    // which is every edge that actually reaches pipework.
    const int destKb = fieldInt(row, "DestKeyboardID", "c", 0);
    const int destDiv = fieldInt(row, "DestDivisionID", "d", 0);
    const bool flagged = fieldBool(row, "DestIsKeyboardNotDivision", "b", true);
    ka.destIsKeyboard = (destKb != 0 && destDiv != 0) ? flagged : (destKb != 0);
    ka.destKeyboard = ka.destIsKeyboard ? destKb : 0;
    ka.destDivision = ka.destIsKeyboard ? 0 : destDiv;
    ka.conditionSwitchId = fieldInt(row, "ConditionSwitchID", "f", 0);
    ka.conditionWhenEngaged =
        fieldBool(row, "ConditionSwitchLinkIfEngaged", "g", true);
    ka.actionType = fieldInt(row, "ActionTypeCode", "h", 1);
    ka.effectCode = fieldInt(row, "ActionEffectCode", "i", 0);
    // The source window. NumberOfKeys is routinely written without the note
    // it starts from, in which case it is anchored on the source keyboard's
    // own first key — every Lemmer edge is written this way. Anchoring it on
    // 0 instead makes the window 0..26 and the whole compass falls outside it,
    // so the keyboard reaches nothing at all.
    ka.firstSourceNote = fieldInt(row, "MIDINoteNumOfFirstSourceKey", "l", -1);
    ka.numKeys = fieldInt(row, "NumberOfKeys", "m", 0);
    if (ka.firstSourceNote < 0) {
      const auto kbIt = outModel.keyboards.find(ka.sourceKeyboard);
      if (kbIt != outModel.keyboards.end() && kbIt->second.numKeys > 0)
        ka.firstSourceNote = kbIt->second.firstMidiNote;
      else
        ka.numKeys = 0; // nothing says where the window starts: carry everything
    }
    ka.midiIncrement = fieldInt(row, "MIDINoteNumberIncrement", "n", 0);
    if (ka.actionType != 1 &&
        std::find(outDiag.unmappedActionTypes.begin(), outDiag.unmappedActionTypes.end(),
                  ka.actionType) == outDiag.unmappedActionTypes.end())
      outDiag.unmappedActionTypes.push_back(ka.actionType);
    if (!ka.destIsKeyboard && ka.destDivision != 0 &&
        outModel.divisions.count(ka.destDivision) == 0)
      outDiag.danglingIds.push_back(ka.destDivision);
    outModel.keyActions.push_back(ka);
  });

  // ---- M1.3: Stop table ----
  forEachRow(odfRoot, "Stop", [&](pugi::xml_node row) {
    Stop s;
    s.stopId = fieldInt(row, "StopID", "a", 0);
    s.name = field(row, "Name", "b");
    s.divisionId = fieldInt(row, "DivisionID", "c", 0);
    s.controllingSwitchId = fieldInt(row, "ControllingSwitchID", "d", 0);
    s.defaultAsgnCode =
        fieldInt(row, "Hint_DefaultAssignmentCodeOfAssocInputOutputSwitch", "e", 0);
    s.hintPrimaryRankId = fieldInt(row, "Hint_PrimaryAssociatedRankID", "f", 0);
    if (s.stopId != 0) outModel.stops[s.stopId] = std::move(s);
  });

  // ---- M1.3: StopRank table (note mapping + unification window) ----
  forEachRow(odfRoot, "StopRank", [&](pugi::xml_node row) {
    const Id stopId = fieldInt(row, "StopID", "a", 0);
    auto stopIt = outModel.stops.find(stopId);
    if (stopIt == outModel.stops.end()) {
      outDiag.danglingIds.push_back(stopId);
      return;
    }
    StopRankEntry e;
    e.rankId = fieldInt(row, "RankID", "d", 0);
    e.firstMappedDivisionNote =
        fieldInt(row, "MIDINoteNumOfFirstMappedDivisionInputNode", "h", 36);
    e.numMappedNotes = fieldInt(row, "NumberOfMappedDivisionInputNodes", "i", 61);
    e.midiIncrement = fieldInt(row, "MIDINoteNumIncrementFromDivisionToRank", "j", 0);
    e.alternateRankId = fieldInt(row, "AlternateRankID", "p", 0);
    e.retriggerOnAlternate = fieldBool(
        row, "RetriggerNotesWhenSwitchingBetweenNormalAndAlternateRanks", "n", false);
    stopIt->second.ranks.push_back(e);
  });

  // ---- M1.3: Switch table ----
  forEachRow(odfRoot, "Switch", [&](pugi::xml_node row) {
    Switch sw;
    sw.switchId = fieldInt(row, "SwitchID", "a", 0);
    sw.name = field(row, "Name", "b");
    sw.latching = fieldBool(row, "Latching", "d", true);
    sw.defaultEngaged = fieldBool(row, "DefaultToEngaged", "e", false);
    sw.asgnCode = fieldInt(row, "DefaultInputOutputSwitchAsgnCode", "c", 0);
    sw.rememberState = fieldBool(row, "RememberStateFromLastLoad", "f", false);
    sw.clickable = fieldBool(row, "Clickable", "i", true);
    // [verified against the Nancy ODF] the console binding.
    sw.dispInstanceId = fieldInt(row, "Disp_ImageSetInstanceID", "k", 0);
    sw.dispIndexEngaged = fieldInt(row, "Disp_ImageSetIndexEngaged", "l", 0);
    sw.dispIndexDisengaged = fieldInt(row, "Disp_ImageSetIndexDisengaged", "m", 0);
    if (sw.switchId != 0) outModel.switches[sw.switchId] = sw;
    if (sw.defaultEngaged) outDiag.defaultEngagedSwitches.push_back(sw.switchId);
  });

  // ---- M3.1: the wind system. Compartments, the pipes between them, and
  // (in the Pipe rows below) what each pipe draws. Nancy declares twelve
  // compartments including a 343 kg bellows, so this is real data rather than
  // a placeholder. ----
  forEachRow(odfRoot, "WindCompartment", [&](pugi::xml_node row) {
    WindCompartment wc;
    wc.compartmentId = fieldInt(row, "WindCompartmentID", "a", 0);
    if (wc.compartmentId == 0) return;
    wc.name = field(row, "Name", "b");
    wc.infiniteVolume = fieldBool(row, "InfiniteVolume", "c", true);
    wc.volumeM3 = fieldDouble(row, "StandardVolumeMetresCubed", "d", 0.0);
    wc.defaultPressureInches =
        fieldDouble(row, "DefaultAirPressureInches", "e", 0.0);
    wc.pressureOutputControlId =
        fieldInt(row, "PressureOutputContinuousControlID", "f", 0);
    wc.hasBellows = fieldBool(row, "Bellows_HasBellows", "g", false);
    wc.bellowsMassKg = fieldDouble(
        row, "Bellows_MassOfMovingBoardGivingRiseToInertiaKg", nullptr, 0.0);
    wc.bellowsDamping =
        fieldDouble(row, "Bellows_PositiveDampingCoefficient", nullptr, 0.0);
    wc.bellowsWidthM =
        fieldDouble(row, "Bellows_FrameBaseWidthMetres", nullptr, 0.0);
    wc.bellowsLengthM =
        fieldDouble(row, "Bellows_FrameBaseLengthMetres", nullptr, 0.0);
    wc.bellowsExtensionM =
        fieldDouble(row, "Bellows_MaximumExtensionMetres", nullptr, 0.0);
    // A finite compartment with no volume cannot hold air; treating it as
    // finite would divide by zero in the solver.
    if (!wc.infiniteVolume && wc.volumeM3 <= 0.0) wc.infiniteVolume = true;
    outModel.wind[wc.compartmentId] = std::move(wc);
  });

  forEachRow(odfRoot, "WindCompartmentLinkage", [&](pugi::xml_node row) {
    WindCompartmentLink l;
    l.firstCompartmentId = fieldInt(row, "FirstWindCompartmentID", "a", 0);
    l.secondCompartmentId = fieldInt(row, "SecondWindCompartmentID", "b", 0);
    if (l.firstCompartmentId == 0 || l.secondCompartmentId == 0) return;
    l.name = field(row, "Name", "c");
    l.valveControlTypeCode = fieldInt(row, "ValveControlTypeCode", "d", 0);
    l.valveSwitchId = fieldInt(row, "ValveControllingSwitchID", "e", 0);
    l.valveControlId =
        fieldInt(row, "ValveControllingContinuousControlID", "g", 0);
    l.valveOpenWhenEngaged =
        fieldBool(row, "ValveOpenIfControllingSwitchEngaged", "f", true);
    l.massFlowKgPerSec = fieldDouble(
        row, "MassFlowRateKilogramsPerSecAtReferencePressureDiff", "h", 0.0);
    l.refPressureInches =
        fieldDouble(row, "ReferencePressureDifferenceInches", "i", 1.0);
    if (l.refPressureInches <= 0.0) l.refPressureInches = 1.0;
    for (Id id : {l.firstCompartmentId, l.secondCompartmentId})
      if (outModel.wind.count(id) == 0) outDiag.danglingIds.push_back(id);
    outModel.windLinks.push_back(l);
  });

  // ---- M3: ContinuousControlStageSwitch — a shoe position that moves a
  // switch. The crescendo is nothing else: one control with a row per step. ----
  forEachRow(odfRoot, "ContinuousControlStageSwitch", [&](pugi::xml_node row) {
    ContinuousControlStageSwitch st;
    st.controlId = fieldInt(row, "ContinuousControlID", "b", 0);
    st.controlledSwitchId = fieldInt(row, "ControlledSwitchID", "d", 0);
    if (st.controlId == 0 || st.controlledSwitchId == 0) return;
    st.value = fieldInt(row, "ContinuousControlValue", "c", 0);
    st.engageWhenIncreasing = fieldBool(row, "EngageWhenValueIncreasing", "e", false);
    st.engageWhenDecreasing = fieldBool(row, "EngageWhenValueDecreasing", "f", false);
    st.disengageWhenIncreasing =
        fieldBool(row, "DisengageWhenValueIncreasing", "g", false);
    st.disengageWhenDecreasing =
        fieldBool(row, "DisengageWhenValueDecreasing", "h", false);
    if (outModel.switches.count(st.controlledSwitchId) == 0)
      outDiag.danglingIds.push_back(st.controlledSwitchId);
    outModel.controlStageSwitches.push_back(st);
  });

  // ---- M3: SwitchLinkage — the organ's own wiring between switches ----
  // The drawstop a player clicks is rarely the switch anything reads: it
  // drives an internal node through one of these. Without them a console
  // coupler moves on screen and couples nothing.
  forEachRow(odfRoot, "SwitchLinkage", [&](pugi::xml_node row) {
    SwitchLinkage l;
    l.sourceSwitchId = fieldInt(row, "SourceSwitchID", "a", 0);
    l.destSwitchId = fieldInt(row, "DestSwitchID", "b", 0);
    if (l.sourceSwitchId == 0 || l.destSwitchId == 0) return;
    l.conditionSwitchId = fieldInt(row, "ConditionSwitchID", "c", 0);
    l.sourceWhenEngaged = fieldBool(row, "SourceSwitchLinkIfEngaged", "d", true);
    l.conditionWhenEngaged =
        fieldBool(row, "ConditionSwitchLinkIfEngaged", "e", true);
    l.engageAction = fieldInt(row, "EngageLinkActionCode", "f", 1);
    l.disengageAction = fieldInt(row, "DisengageLinkActionCode", "g", 2);

    // Codes 1 and 4 engage the destination, 2 and 7 disengage it. Anything
    // else is a latching or momentary behaviour we do not model, and treating
    // it as a plain follow would rewire the organ behind the player's back.
    for (int code : {l.engageAction, l.disengageAction})
      if (code != 1 && code != 2 && code != 4 && code != 7 &&
          std::find(outDiag.unmappedLinkageCodes.begin(),
                    outDiag.unmappedLinkageCodes.end(),
                    code) == outDiag.unmappedLinkageCodes.end())
        outDiag.unmappedLinkageCodes.push_back(code);

    for (Id id : {l.sourceSwitchId, l.destSwitchId, l.conditionSwitchId})
      if (id != 0 && outModel.switches.count(id) == 0)
        outDiag.switchLinkageDangling.push_back(id);

    outModel.switchLinkages.push_back(l);
  });

  // ---- Stop -> Rank through Hint_PrimaryAssociatedRankID ----
  // A set — and every demo set that ships only part of its pipework — may
  // declare no StopRank rows at all and leave the stop's rank in this hint.
  // The reference converters follow it, and without it the stop draws, moves
  // its switch and plays nothing. It is applied only when the hinted rank's
  // pipes are NOT pallet-wired: those organs reach every pipe through the
  // switch network (key AND stop AND routing), and a synthesized direct path
  // would bypass the very wiring that decides when a pipe speaks. Ranks the
  // pallet index owns keep their wiring untouched.
  {
    std::unordered_set<Id> palletRanks;
    for (const auto& [rankId, rank] : outModel.ranks)
      for (const Pipe& p : rank.pipes)
        if (p.palletSwitchId != 0) {
          palletRanks.insert(rankId);
          break;
        }

    // The division's own keyboard gives the compass the hint maps over. A
    // division with no keyboard (or one of unstated size) gets Hauptwerk's
    // own default of 61 notes from 36, which is what an unstated StopRank
    // maps anyway.
    auto compassFor = [&outModel](Id divisionId, int& firstNote, int& numKeys) {
      firstNote = 36;
      numKeys = 61;
      const auto divIt = outModel.divisions.find(divisionId);
      if (divIt == outModel.divisions.end()) return;
      for (Id kbId : divIt->second.keyboardIds) {
        const auto kbIt = outModel.keyboards.find(kbId);
        if (kbIt == outModel.keyboards.end()) continue;
        if (kbIt->second.numKeys <= 0) continue;
        firstNote = kbIt->second.firstMidiNote;
        numKeys = kbIt->second.numKeys;
        return;
      }
    };

    int hintStops = 0;
    for (auto& [stopId, stop] : outModel.stops) {
      if (!stop.ranks.empty()) continue;
      if (stop.hintPrimaryRankId == 0) continue;
      const auto rankIt = outModel.ranks.find(stop.hintPrimaryRankId);
      if (rankIt == outModel.ranks.end()) {
        outDiag.danglingIds.push_back(stop.hintPrimaryRankId);
        continue;
      }
      // Only when the rank actually holds pipework. On every set surveyed the
      // hint names either a rank the demo does not ship (no Pipe rows) or one
      // the pallets own; mapping to an empty rank would sound nothing and
      // would erase the stopsWithoutRanks diagnostic that truthfully says the
      // stop can never play as installed.
      if (rankIt->second.pipes.empty()) continue;
      if (palletRanks.count(stop.hintPrimaryRankId) != 0) continue;
      StopRankEntry e;
      e.rankId = stop.hintPrimaryRankId;
      compassFor(stop.divisionId, e.firstMappedDivisionNote, e.numMappedNotes);
      stop.ranks.push_back(e);
      ++hintStops;
    }
    if (hintStops > 0)
      outDiag.warnings.emplace_back(
          std::to_string(hintStops) +
          " stop(s) reach their rank through Hint_PrimaryAssociatedRankID "
          "(no StopRank rows declared); the hint is followed as the reference "
          "converters do");
  }

  // ---- M1.3 validator: stops without StopRank rows (full ODF only;
  // CODM gains its rows at M1.5 compile time) ----
  for (const auto& [id, s] : outModel.stops)
    if (s.ranks.empty()) outDiag.stopsWithoutRanks.push_back(id);

  // ---- M2.2: Temperament table ----
  // The ODF may spell out twelve cent offsets, or just name a historical
  // tuning. A named tuning we do not know is reported rather than quietly
  // replaced with equal temperament, which would retune the organ silently.
  forEachRow(odfRoot, "Temperament", [&](pugi::xml_node row) {
    OrganTemperament t;
    t.temperamentId = fieldInt(row, "TemperamentID", "a", 0);
    t.name = field(row, "Name", "b");
    if (t.temperamentId == 0) return;

    // Explicit per-note offsets win over the name.
    static const char* const kNoteFields[12] = {
        "CentsOffsetC",  "CentsOffsetCsharp", "CentsOffsetD",  "CentsOffsetDsharp",
        "CentsOffsetE",  "CentsOffsetF",      "CentsOffsetFsharp", "CentsOffsetG",
        "CentsOffsetGsharp", "CentsOffsetA",  "CentsOffsetAsharp", "CentsOffsetB"};
    bool anyExplicit = false;
    std::vector<double> offsets(12, 0.0);
    for (int i = 0; i < 12; ++i) {
      const std::string raw = field(row, kNoteFields[i]);
      if (raw.empty()) continue;
      anyExplicit = true;
      offsets[static_cast<size_t>(i)] = std::strtod(raw.c_str(), nullptr);
    }

    if (anyExplicit) {
      t.centsOffset12 = std::move(offsets);
      t.resolved = true;
    } else if (const Temperament* lib = findTemperament(t.name)) {
      t.centsOffset12 = lib->centsOffset12;
      t.resolved = true;
    } else {
      // Unresolved: left empty so the solver falls back to equal temperament,
      // but recorded so the player is told the organ is not tuned as authored.
      if (std::find(outDiag.unknownTemperaments.begin(),
                    outDiag.unknownTemperaments.end(),
                    t.name) == outDiag.unknownTemperaments.end())
        outDiag.unknownTemperaments.push_back(t.name);
      outDiag.warnings.emplace_back(
          "Temperament '" + t.name + "' is not in the built-in library and the "
          "ODF gives no offsets; this organ will play in equal temperament");
    }
    if (outModel.defaultTemperamentId == 0)
      outModel.defaultTemperamentId = t.temperamentId;
    outModel.temperaments[t.temperamentId] = std::move(t);
  });

  // pipe-pitch-out-of-range: a pipe whose footage, tuning deviation and
  // temperament put it outside anything an organ can sound. Catches a
  // mis-declared harmonic number long before it becomes an aliasing scream.
  {
    Temperament tuning;
    const auto tIt = outModel.temperaments.find(outModel.defaultTemperamentId);
    if (tIt != outModel.temperaments.end() && tIt->second.resolved) {
      tuning.name = tIt->second.name;
      tuning.centsOffset12 = tIt->second.centsOffset12;
    }
    for (const auto& [rankId, rank] : outModel.ranks) {
      // Noise ranks are unpitched by construction — a piston click or a blower
      // has no note and no footage, and HW omits both fields. Running a pitch
      // check over them reports a fault that does not exist.
      if (rank.isNoise) continue;
      for (const auto& pipe : rank.pipes) {
        const double hz = pipeTargetHz(pipe.midiNote, pipe.basePitch64ftHarmonicNum,
                                       outModel.basePitchHz,
                                       pipe.baseTuningDeviationCents, tuning, 0);
        if (pipeHzInRange(hz)) continue;
        outDiag.pipesPitchOutOfRange.push_back(
            {pipe.pipeId, rankId, rank.name, pipe.midiNote,
             pipe.basePitch64ftHarmonicNum, hz});
      }
    }
  }

  // ---- M2.4: Noise table (key/stop/blower/tracker action noises) ----
  // A noise is an ordinary rank flagged so the engine can gate and route it
  // apart from speaking pipework. Options::includeKeyNoises drops them for
  // players who do not want mechanical sound.
  forEachRow(odfRoot, "Noise", [&](pugi::xml_node row) {
    const Id rankId = fieldInt(row, "RankID", "b", 0);
    auto it = outModel.ranks.find(rankId);
    if (it == outModel.ranks.end()) {
      if (rankId != 0) outDiag.danglingIds.push_back(rankId);
      return;
    }
    if (!opts.includeKeyNoises) {
      outModel.ranks.erase(it);
      return;
    }
    it->second.isNoise = true;
    it->second.noiseTriggerSwitchId = fieldInt(row, "TriggerSwitchID", "c", 0);
    if (it->second.noiseTriggerSwitchId != 0 &&
        outModel.switches.count(it->second.noiseTriggerSwitchId) == 0)
      outDiag.danglingIds.push_back(it->second.noiseTriggerSwitchId);
  });

  // ---- M2.4: ContinuousControl (swell shoes, crescendo, generic controls) ----
  forEachRow(odfRoot, "ContinuousControl", [&](pugi::xml_node row) {
    ContinuousControl c;
    // [verified] HW calls this ControlID here, NOT ContinuousControlID —
    // the table name and the field name do not match, and the wrong guess
    // silently dropped every row.
    c.controlId = fieldInt(row, "ControlID", "a", 0);
    if (c.controlId == 0) c.controlId = fieldInt(row, "ContinuousControlID", nullptr, 0);
    c.name = field(row, "Name", "b");
    // The only code on the row is the default console/MIDI assignment, and it
    // is normally empty — HW controls are plain 0..127 with no declared range.
    c.typeCode = fieldInt(row, "DefaultInputOutputContinuousCtrlAsgnCode", "c", 0);
    c.defaultValue = fieldInt(row, "DefaultValue", "d", 0);
    c.minValue = 0;
    c.maxValue = 127;
    // ClickingHigherIncreasesValue is about mouse direction on the console
    // image, not about inverting the value, so it is deliberately not read
    // into `inverted` — that would silently reverse every shoe it appears on.
    // It belongs to the drawn control instead, below.
    c.inverted = false;

    // What makes a control something a player can move rather than a number
    // the organ keeps to itself. Only the drawn ones have an instance: Nancy
    // declares 1420 controls and draws 55.
    c.imageSetInstanceId = fieldInt(row, "ImageSetInstanceID", nullptr, 0);
    c.clickable = fieldBool(row, "Clickable", nullptr, true);
    c.clickingHigherIncreasesValue =
        fieldBool(row, "ClickingHigherIncreasesValue", nullptr, true);
    c.rememberState =
        fieldBool(row, "RememberStateFromLastLoad", nullptr, false);
    if (c.controlId == 0) return;
    if (c.minValue > c.maxValue) {
      outDiag.warnings.emplace_back(
          "ContinuousControl " + std::to_string(c.controlId) +
          ": min above max; range swapped");
      std::swap(c.minValue, c.maxValue);
    }
    outModel.continuousControls[c.controlId] = std::move(c);
  });

  // ---- How a drawn control SHOWS its position ----
  // A staircase per image set: the frame to draw for each band of 0..127.
  // Keyed by image set rather than by control, so a hundred identical sliders
  // share one ladder.
  forEachRow(odfRoot, "ContinuousControlImageSetStage", [&](pugi::xml_node row) {
    const Id setId = fieldInt(row, "ImageSetID", nullptr, 0);
    if (setId == 0) return;
    ContinuousControlImageStage s;
    s.highestValue = fieldInt(row, "HighestContinuousControlValue", nullptr, 0);
    s.imageIndex = fieldInt(row, "ImageSetIndex", nullptr, 1);
    outModel.continuousControlStages[setId].push_back(s);
  });
  // The rows do NOT arrive in value order — a real set has the lowest band
  // last — and a lookup that walked them as written would pick the wrong
  // frame for most of the travel.
  for (auto& [setId, stages] : outModel.continuousControlStages) {
    (void)setId;
    std::sort(stages.begin(), stages.end(),
              [](const ContinuousControlImageStage& a,
                 const ContinuousControlImageStage& b) {
                return a.highestValue < b.highestValue;
              });
  }

  // ---- Two controls combined into a third ----
  // An organ builds a level out of several sliders this way: the audio-group
  // level times the noise level, renormalised. Without it, every level slider
  // on a set's own settings page moves a number that reaches no pipe.
  forEachRow(odfRoot, "ContinuousControlDoubleLinkage", [&](pugi::xml_node row) {
    ContinuousControlDoubleLinkage d;
    d.destControlId = fieldInt(row, "DestControl_ID", nullptr, 0);
    d.firstControlId = fieldInt(row, "FirstSourceControl_ID", nullptr, 0);
    d.secondControlId = fieldInt(row, "SecondSourceControl_ID", nullptr, 0);
    if (d.destControlId == 0 || d.firstControlId == 0 || d.secondControlId == 0)
      return;
    d.operationCode = fieldInt(row, "BinaryOperationCode", nullptr, 0);
    d.firstCoefficient = fieldDouble(row, "FirstSourceControl_Coefficient", nullptr, 1.0);
    d.firstIncrement = fieldDouble(row, "FirstSourceControl_Increment", nullptr, 0.0);
    d.secondCoefficient = fieldDouble(row, "SecondSourceControl_Coefficient", nullptr, 1.0);
    d.secondIncrement = fieldDouble(row, "SecondSourceControl_Increment", nullptr, 0.0);
    // The coefficient is routinely absent, and 1.0 is the identity that means
    // "no renormalisation" — which is right for an add and wrong for nothing.
    d.destCoefficient = fieldDouble(row, "DestControl_Coefficient", nullptr, 1.0);
    d.destIncrement = fieldDouble(row, "DestControl_Increment", nullptr, 0.0);

    if (d.operationCode < 1 || d.operationCode > 3) {
      outDiag.warnings.emplace_back(
          "ContinuousControlDoubleLinkage into control " +
          std::to_string(d.destControlId) + ": unknown operation code " +
          std::to_string(d.operationCode) + "; the linkage is ignored");
      return;
    }
    outModel.controlDoubleLinkages.push_back(d);
  });

  // ---- M2.4: ContinuousControlLinkage (one control driving another) ----
  forEachRow(odfRoot, "ContinuousControlLinkage", [&](pugi::xml_node row) {
    ContinuousControlLinkage l;
    l.linkageId = fieldInt(row, "ContinuousControlLinkageID", "a", 0);
    l.sourceControlId = fieldInt(row, "SourceControlID", "b", 0);
    l.destControlId = fieldInt(row, "DestControlID", "c", 0);
    l.conditionSwitchId = fieldInt(row, "ConditionSwitchID", "d", 0);
    l.conditionWhenEngaged =
        fieldBool(row, "ConditionSwitchLinkIfEngaged", nullptr, true);
    l.scale = fieldDouble(row, "SourceControlValueCoefficient", "e", 1.0);
    l.offset = fieldInt(row, "SourceControlValueIncrement", "f", 0);
    if (fieldBool(row, "InvertSourceControlValue", nullptr, false)) {
      // Invert about the 0..127 range rather than negating, which would drive
      // the destination out of range.
      l.scale = -l.scale;
      l.offset += 127;
    }
    for (Id ref : {l.sourceControlId, l.destControlId})
      if (ref != 0 && outModel.continuousControls.count(ref) == 0)
        outDiag.danglingIds.push_back(ref);
    if (l.conditionSwitchId != 0 &&
        outModel.switches.count(l.conditionSwitchId) == 0)
      outDiag.danglingIds.push_back(l.conditionSwitchId);
    outModel.controlLinkages.push_back(l);
  });

  // ---- Installation packages: where the audio actually lives ----
  // An ODF names the packages it is played from. Reporting a missing one by
  // name beats letting the organ load and come up mute, which is what happens
  // when every sample silently fails to resolve.
  forEachRow(odfRoot, "RequiredInstallationPackage", [&](pugi::xml_node row) {
    InstallationPackage pkg;
    pkg.packageId = fieldInt(row, "InstallationPackageID", "a", 0);
    pkg.name = field(row, "Name", "b");
    pkg.supplierName = field(row, "SupplierName", "d");
    if (pkg.packageId == 0) return;
    if (!opts.organRootDir.empty()) {
      std::error_code ec;
      pkg.presentOnDisk = std::filesystem::is_directory(
          std::filesystem::path(opts.organRootDir) / "OrganInstallationPackages" /
              packageDirName(pkg.packageId),
          ec);
      if (!pkg.presentOnDisk)
        outDiag.warnings.emplace_back(
            "the organ is played from package " + packageDirName(pkg.packageId) +
            " (\"" + pkg.name + "\"" +
            (pkg.supplierName.empty() ? "" : " by " + pkg.supplierName) +
            "), which is not installed under OrganInstallationPackages");
    }
    outModel.packages[pkg.packageId] = std::move(pkg);
  });

  // ---- M2.3: Enclosure + EnclosurePipe (swell boxes) ----
  forEachRow(odfRoot, "Enclosure", [&](pugi::xml_node row) {
    Enclosure e;
    e.enclosureId = fieldInt(row, "EnclosureID", "a", 0);
    e.name = field(row, "Name", "b");
    // [verified] the shoe that drives this box.
    e.continuousControlId =
        fieldInt(row, "ShutterPositionContinuousControlID", "c", 0);
    if (e.continuousControlId == 0)
      e.continuousControlId = fieldInt(row, "ContinuousControlID", nullptr, 0);
    // An Enclosure row carries no filter numbers at all — the dictionary gives
    // it exactly three attributes (id, name, shutter control). The filter is
    // stated per pipe on EnclosurePipe and gathered there, below.
    if (e.enclosureId == 0) return;
    if (e.continuousControlId != 0 &&
        outModel.continuousControls.count(e.continuousControlId) == 0)
      outDiag.danglingIds.push_back(e.continuousControlId);
    outModel.enclosures[e.enclosureId] = std::move(e);
  });

  // EnclosurePipe rows say which pipework each box encloses. This is what
  // makes expression per rank rather than a filter over the whole organ: an
  // unenclosed Great must stay unenclosed while the Swell shades move.
  //
  // The row also carries the box's filter, stated against each pipe's own
  // pitch: OverallAttnDb insertion loss, and the [MaxFreq, MinFreq] band the
  // shades move it between (closed one band, open higher). The engine filters
  // one bus per box rather than one filter per voice, so the box gets the
  // MEDIAN of its pipes' values — the representative figure for the box —
  // and the raw spread stays in the file where a per-voice filter can use it
  // later. Read as the dictionary numbers them: c..h.
  {
    // Per enclosure, the six values from every row, for a median at the end.
    std::unordered_map<Id, std::vector<std::array<double, 6>>> shadeParams;
    forEachRow(odfRoot, "EnclosurePipe", [&](pugi::xml_node row) {
      const Id encId = fieldInt(row, "EnclosureID", "a", 0);
      auto it = outModel.enclosures.find(encId);
      if (it == outModel.enclosures.end()) {
        if (encId != 0) outDiag.danglingIds.push_back(encId);
        return;
      }
      ++it->second.numShades;
      const Id pipeId = fieldInt(row, "PipeID", "b", 0);
      if (pipeId != 0) {
        // A pipe named by two boxes is an authoring error; first wins and the
        // conflict is reported rather than silently resolved.
        const auto existing = outModel.pipeEnclosure.find(pipeId);
        if (existing != outModel.pipeEnclosure.end() && existing->second != encId)
          outDiag.warnings.emplace_back(
              "Pipe " + std::to_string(pipeId) + " is enclosed by both " +
              std::to_string(existing->second) + " and " + std::to_string(encId) +
              "; keeping the first");
        else
          outModel.pipeEnclosure[pipeId] = encId;
      }
      std::array<double, 6> v{
          fieldDouble(row, "FiltParamWhenClsd_OverallAttnDb", "c", 0.0),
          fieldDouble(row, "FiltParamWhenClsd_MaxFreqHz", "d", 0.0),
          fieldDouble(row, "FiltParamWhenClsd_MinFreqHz", "e", 0.0),
          fieldDouble(row, "FiltParamWhenClsd_ExtraAttnAtMinDb", "f", 0.0),
          fieldDouble(row, "FiltParamWhenOpen_MaxFreqHz", "g", 0.0),
          fieldDouble(row, "FiltParamWhenOpen_MinFreqHz", "h", 0.0)};
      if (v[1] > 0.0 || v[4] > 0.0)
        shadeParams[encId].push_back(v);
    });

    for (auto& [encId, rows] : shadeParams) {
      auto encIt = outModel.enclosures.find(encId);
      if (encIt == outModel.enclosures.end() || rows.empty()) continue;
      // The 75th percentile, not the median. Each pipe's figure is stated
      // against its own pitch, so a box's values climb with the compass and
      // its median describes a pipe LOWER than most of what is heard: on
      // Bégard the open median is 1.7 kHz, which would leave a box that is
      // open sounding permanently closed. The upper quartile is the bus
      // filter's honest compromise — the open box stays close to
      // transparent, the closed one clearly muffled, and the numbers are
      // still the set's own (Bégard closed 932 Hz / open 3.7 kHz, against
      // invented 800 Hz / 12 kHz before). A per-pipe filter is the faithful
      // model and this is where it would go; see the Enclosure comment in
      // OrganModel.h.
      auto quantile = [&rows](size_t i, double q) {
        std::vector<double> col;
        col.reserve(rows.size());
        for (const auto& r : rows)
          if (r[i] > 0.0) col.push_back(r[i]);
        if (col.empty()) return 0.0;
        std::sort(col.begin(), col.end());
        const double pos = q * static_cast<double>(col.size() - 1);
        return col[static_cast<size_t>(pos + 0.5)];
      };
      Enclosure& e = encIt->second;
      const double closedMax = quantile(1, 0.75), closedMin = quantile(2, 0.75);
      const double openMax = quantile(4, 0.75), openMin = quantile(5, 0.75);
      if (closedMax > 0.0) e.closedFilterHz = closedMax;
      else if (closedMin > 0.0) e.closedFilterHz = closedMin;
      if (openMax > 0.0) e.openFilterHz = openMax;
      else if (openMin > 0.0) e.openFilterHz = openMin;
      // Closed attenuation is the insertion loss plus the extra at the bottom
      // of the band; an open box takes the insertion loss off entirely.
      auto meanPositive = [&rows](size_t i) {
        double sum = 0.0;
        size_t n = 0;
        for (const auto& r : rows) {
          if (r[i] > 0.0) { sum += r[i]; ++n; }
        }
        return n > 0 ? sum / static_cast<double>(n) : 0.0;
      };
      e.closedAttnDb = -(meanPositive(0) + meanPositive(3));
      e.openAttnDb = 0.0;
      e.filterParamsFromPipes = true;
    }
  }

  // ---- M2.3: Tremulant table ----
  forEachRow(odfRoot, "Tremulant", [&](pugi::xml_node row) {
    Tremulant t;
    t.tremulantId = fieldInt(row, "TremulantID", "a", 0);
    t.name = field(row, "Name", "b");
    t.controllingSwitchId = fieldInt(row, "ControllingSwitchID", "c", 0);
    t.engagedHz = fieldDouble(row, "FrequencyWhenEngagedHz", "d", t.engagedHz);
    t.disengagedHz =
        fieldDouble(row, "FrequencyWhenDisengagedHz", "e", t.disengagedHz);
    // StartRatePercent / StopRatePercent are how quickly the tremulant winds
    // up and down, which is what TremulantLfo::depthRampSeconds consumes.
    t.startPercent = fieldDouble(row, "StartRatePercent", "f", t.startPercent);
    t.stopPercent = fieldDouble(row, "StopRatePercent", "g", t.stopPercent);
    // There is NO depth field on the Tremulant row. HW puts modulation depth on
    // TremulantWaveformPipe, per pipe (AmplitudeModDepthAdjustDecibels), which
    // is a different model from one depth per tremulant. Linked below.
    t.waveformId = 0; // set from the TremulantWaveform table
    t.hasWaveform = false;
    if (t.tremulantId == 0) return;
    if (t.controllingSwitchId != 0 &&
        outModel.switches.count(t.controllingSwitchId) == 0)
      outDiag.danglingIds.push_back(t.controllingSwitchId);
    outModel.tremulants[t.tremulantId] = std::move(t);
  });

  // ---- M2.3: TremulantWaveform -> the tremulant it belongs to ----
  forEachRow(odfRoot, "TremulantWaveform", [&](pugi::xml_node row) {
    const Id waveformId = fieldInt(row, "TremulantWaveformID", "a", 0);
    // Field c, not b — b is the Name. Reading the tremulant id out of the name
    // field meant a waveform almost never found its tremulant, and the pipes
    // it modulates were never linked to anything.
    const Id tremId = fieldInt(row, "TremulantID", "c", 0);
    auto it = outModel.tremulants.find(tremId);
    if (it == outModel.tremulants.end()) {
      if (tremId != 0) outDiag.danglingIds.push_back(tremId);
      return;
    }
    it->second.waveformId = waveformId;
    it->second.hasWaveform = waveformId != 0;
  });

  // TremulantWaveformPipe says how far one tremulant moves one pipe. This is
  // the link between a tremulant and the pipework it actually reaches: a
  // tremulant belongs to a chest, and the stops on that chest wobble by
  // different amounts.
  {
    std::unordered_map<Id, Id> tremOfWaveform;
    for (const auto& [id, t] : outModel.tremulants)
      if (t.waveformId != 0) tremOfWaveform[t.waveformId] = id;

    forEachRow(odfRoot, "TremulantWaveformPipe", [&](pugi::xml_node row) {
      const Id pipeId = fieldInt(row, "PipeID", "a", 0);
      const Id waveformId = fieldInt(row, "TremulantWaveformID", "b", 0);
      if (pipeId == 0 || waveformId == 0) return;
      const auto tremIt = tremOfWaveform.find(waveformId);
      if (tremIt == tremOfWaveform.end()) {
        outDiag.danglingIds.push_back(waveformId);
        return;
      }
      TremulantPipeMod mod;
      mod.tremulantId = tremIt->second;
      mod.ampDepthDb =
          fieldDouble(row, "AmplitudeModDepthAdjustDecibels", "c", 0.0);
      mod.pitchDepthPct =
          fieldDouble(row, "PitchModDepthAdjustPercent", "d", 0.0);
      outModel.tremulantPipes[pipeId] = mod;

      // The strongest depth any pipe uses is the tremulant's nominal depth,
      // which is what the "enabled but does nothing" check tests against.
      auto& t = outModel.tremulants[mod.tremulantId];
      t.depthPercent = std::max(t.depthPercent, std::fabs(mod.ampDepthDb));
    });
  }

  // ---- M2.3/M2.4 validator queries ----
  // enclosure-without-shades: a box that encloses no pipework is inert, and a
  // dead swell pedal is the kind of fault a player notices before we do.
  for (const auto& [id, e] : outModel.enclosures)
    if (e.numShades == 0) outDiag.enclosuresWithoutShades.push_back(id);

  // trem-depth-zero-but-enabled: a tremulant wired to a drawstop but with no
  // depth does nothing when drawn.
  for (const auto& [id, t] : outModel.tremulants)
    if (t.controllingSwitchId != 0 && t.depthPercent == 0.0)
      outDiag.tremulantsDepthZero.push_back(id);

  // continuous-control-unmapped. The code here is HW's DEFAULT console/MIDI
  // assignment, and it is empty on most controls — that means "not assigned by
  // default", which is normal, not a fault. Flagging empty fired on all 1420
  // controls of the Nancy demo: noise, not signal. Only a code that is present
  // and that we have no behaviour for is worth reporting (ADR-002).
  for (const auto& [id, c] : outModel.continuousControls)
    if (c.typeCode != 0) outDiag.unmappedContinuousControls.push_back(id);

  // continuous-control-feedback-loop. This originally walked the linkage graph
  // for ANY cycle, which reported 124 loops on the Nancy demo — an organ that
  // works. Hauptwerk linkages are not a pure dataflow graph: they carry a
  // LinkTypeCode, condition switches and an inertia model, and mutually
  // referencing pairs are ordinary (a control and its inverse, for one). The
  // runtime settles them by bounded relaxation and cannot hang.
  //
  // What IS unambiguously broken is a control wired to itself, so that is all
  // this reports now. A general cycle is not a defect and saying so was wrong.
  for (const auto& l : outModel.controlLinkages)
    if (l.sourceControlId != 0 && l.sourceControlId == l.destControlId)
      outDiag.controlLinkageCycles.push_back(l.sourceControlId);

  // noise-rank-without-sample: a noise rank with no pipes (or pipes with no
  // layers) is silent; key/stop/blower noises are easy to declare and forget.
  for (const auto& [id, r] : outModel.ranks) {
    if (!r.isNoise) continue;
    bool hasAudio = false;
    for (const auto& pipe : r.pipes)
      for (const auto& layer : pipe.layers)
        if (!layer.attacks.empty()) { hasAudio = true; break; }
    if (!hasAudio) outDiag.noiseRanksWithoutSample.push_back(id);
  }

  // ---- M1.4: ImageSet table (shared artwork; elements carry bitmap files) ----
  forEachRow(odfRoot, "ImageSet", [&](pugi::xml_node row) {
    ImageSet set;
    set.imageSetId = fieldInt(row, "ImageSetID", "a", 0);
    set.name = field(row, "Name", "b");
    set.packageId = fieldInt(row, "InstallationPackageID", "c", 0);
    set.widthPx = fieldInt(row, "ImageWidthPixels", "d", 0);
    set.heightPx = fieldInt(row, "ImageHeightPixels", "e", 0);
    // The clickable sub-rectangle. Note these are NOT the width and height:
    // reading them as such would size a drawstop by its knob and a key by its
    // playable part.
    set.clickLeftPx = fieldInt(row, "ClickableAreaLeftRelativeXPosPixels", "f", -1);
    set.clickRightPx = fieldInt(row, "ClickableAreaRightRelativeXPosPixels", "g", -1);
    set.clickTopPx = fieldInt(row, "ClickableAreaTopRelativeYPosPixels", "h", -1);
    set.clickBottomPx =
        fieldInt(row, "ClickableAreaBottomRelativeYPosPixels", "i", -1);
    // Unstated left/top with a stated right/bottom means the area starts at
    // the image's own corner, which is how every real set writes it.
    if (set.clickRightPx > 0 && set.clickLeftPx < 0) set.clickLeftPx = 0;
    if (set.clickBottomPx > 0 && set.clickTopPx < 0) set.clickTopPx = 0;
    set.transparencyMaskFile = field(row, "TransparencyMaskBitmapFilename", "j");
    if (set.imageSetId != 0) outModel.imageSets[set.imageSetId] = std::move(set);
  });

  // ---- M1.4: ImageSetElement table (frames) ----
  forEachRow(odfRoot, "ImageSetElement", [&](pugi::xml_node row) {
    const Id setId = fieldInt(row, "ImageSetID", "a", 0);
    auto setIt = outModel.imageSets.find(setId);
    if (setIt == outModel.imageSets.end()) {
      outDiag.danglingIds.push_back(setId);
      return;
    }
    ImageSetElement el;
    // Hauptwerk's own default is 1, and sets rely on it: the Barton
    // theatre consoles write the index only on the second frame, so a
    // default of 0 left every "up" picture unreachable.
    el.index = fieldInt(row, "ImageIndexWithinSet", "b", 1);
    el.name = field(row, "Name", "c");
    el.bitmapFile = field(row, "BitmapFilename", "d");
    setIt->second.elements.push_back(std::move(el));
  });

  // ---- M1.4: DisplayPage table ----
  forEachRow(odfRoot, "DisplayPage", [&](pugi::xml_node row) {
    DisplayPage page;
    page.pageId = fieldInt(row, "PageID", "a", 0);
    page.name = field(row, "Name", "b");
    if (page.pageId != 0) outModel.displayPages[page.pageId] = std::move(page);
  });

  // ---- M1.4: ImageSetInstance table (placed controls; alt layouts in M4) ----
  forEachRow(odfRoot, "ImageSetInstance", [&](pugi::xml_node row) {
    const Id pageId = fieldInt(row, "DisplayPageID", "e", 0);
    const Id setId = fieldInt(row, "ImageSetID", "c", 0);
    auto pageIt = outModel.displayPages.find(pageId);
    if (pageIt == outModel.displayPages.end()) {
      outDiag.danglingIds.push_back(pageId);
      return;
    }
    if (setId != 0 && outModel.imageSets.count(setId) == 0) {
      outDiag.danglingIds.push_back(setId);
      return;
    }
    ImageSetInstance inst;
    inst.instanceId = fieldInt(row, "ImageSetInstanceID", "a", 0);
    inst.name = field(row, "Name", "b");
    inst.imageSetId = setId;
    inst.defaultImageIndex = fieldInt(row, "DefaultImageIndexWithinSet", "d", 0);
    inst.layer = fieldInt(row, "ScreenLayerNumber", "f", 0);
    inst.leftPx = fieldInt(row, "LeftXPosPixels", "g", 0);
    inst.tileRightPx = fieldInt(row, "RightXPosPixelsIfTiling", "i", -1);
    inst.tileBottomPx = fieldInt(row, "BottomYPosPixelsIfTiling", "j", -1);
    inst.topPx = fieldInt(row, "TopYPosPixels", "h", 0);

    // The same thing on the set's other console layouts. Each alternate names
    // its own image set as well as its position, because a stop knob drawn for
    // a wide console is not the one drawn for a narrow one. The letter codes
    // skip 'o' — Hauptwerk's compact form never uses it, presumably because it
    // reads as a zero.
    static constexpr const char* kAltSet[3] = {
        "AlternateScreenLayout1_ImageSetID", "AlternateScreenLayout2_ImageSetID",
        "AlternateScreenLayout3_ImageSetID"};
    static constexpr const char* kAltLeft[3] = {
        "AlternateScreenLayout1_LeftXPosPixels",
        "AlternateScreenLayout2_LeftXPosPixels",
        "AlternateScreenLayout3_LeftXPosPixels"};
    static constexpr const char* kAltTop[3] = {
        "AlternateScreenLayout1_TopYPosPixels",
        "AlternateScreenLayout2_TopYPosPixels",
        "AlternateScreenLayout3_TopYPosPixels"};
    static constexpr const char* kAltRight[3] = {
        "AlternateScreenLayout1_RightXPosPixelsIfTiling",
        "AlternateScreenLayout2_RightXPosPixelsIfTiling",
        "AlternateScreenLayout3_RightXPosPixelsIfTiling"};
    static constexpr const char* kAltBottom[3] = {
        "AlternateScreenLayout1_BottomYPosPixelsIfTiling",
        "AlternateScreenLayout2_BottomYPosPixelsIfTiling",
        "AlternateScreenLayout3_BottomYPosPixelsIfTiling"};
    static constexpr const char* kAltCodes[3][5] = {
        {"k", "l", "m", "n", "p"},
        {"q", "r", "s", "t", "u"},
        {"v", "w", "x", "y", "z"}};

    for (int a = 0; a < 3; ++a) {
      auto& alt = inst.alternates[a];
      alt.imageSetId = fieldInt(row, kAltSet[a], kAltCodes[a][0], 0);
      if (alt.imageSetId == 0) continue; // this set does not offer that layout
      alt.leftPx = fieldInt(row, kAltLeft[a], kAltCodes[a][1], 0);
      alt.topPx = fieldInt(row, kAltTop[a], kAltCodes[a][2], 0);
      alt.tileRightPx = fieldInt(row, kAltRight[a], kAltCodes[a][3], -1);
      alt.tileBottomPx = fieldInt(row, kAltBottom[a], kAltCodes[a][4], -1);
      if (outModel.imageSets.count(alt.imageSetId) == 0)
        outDiag.danglingIds.push_back(alt.imageSetId);
    }

    pageIt->second.instances.push_back(std::move(inst));
  });

  // ---- M1.4: TextInstance table ----
  forEachRow(odfRoot, "TextInstance", [&](pugi::xml_node row) {
    const Id pageId = fieldInt(row, "DisplayPageID", "e", 0);
    auto pageIt = outModel.displayPages.find(pageId);
    if (pageIt == outModel.displayPages.end()) {
      outDiag.danglingIds.push_back(pageId);
      return;
    }
    TextInstance t;
    t.textInstanceId = fieldInt(row, "TextInstanceID", "a", 0);
    t.name = field(row, "Name", "b");
    t.text = field(row, "Text", "d");
    t.styleId = fieldInt(row, "TextStyleID", "c", 0);
    t.xPx = fieldInt(row, "XPosPixels", "f", 0);
    t.yPx = fieldInt(row, "YPosPixels", "g", 0);
    t.boxWidthPx = fieldInt(row, "BoundingBoxWidthPixelsIfWordWrap", "h", 0);
    t.boxHeightPx = fieldInt(row, "BoundingBoxHeightPixelsIfWordWrap", "i", 0);
    // The flag and the ID are separate fields, and a set that fills in the ID
    // without setting the flag still means it.
    t.attachedInstanceId = fieldInt(row, "AttachedToImageSetInstanceID", "k", 0);
    if (!fieldBool(row, "AttachedToAnImageSetInstance", "j", true))
      t.attachedInstanceId = 0;
    t.posRelativeToInstance =
        fieldBool(row, "PosRelativeToTopLeftOfImageSetInstance", "l", false);
    pageIt->second.texts.push_back(std::move(t));
  });

  // ---- TextStyle: the font a TextInstance is drawn in ----
  // Defaults follow Hauptwerk's own: Arial, 10 pixels, normal weight, black,
  // centred horizontally and aligned to the top vertically.
  forEachRow(odfRoot, "TextStyle", [&](pugi::xml_node row) {
    TextStyle t;
    t.styleId = fieldInt(row, "StyleID", "a", 0);
    if (t.styleId == 0) return;
    t.name = field(row, "Name", "b");
    t.faceWindows = field(row, "Face_WindowsName", "c");
    t.faceMac = field(row, "Face_MacName", "d");
    t.faceLinux = field(row, "Face_LinuxName", "e");
    t.sizePx = fieldInt(row, "Font_SizePixels", "f", 10);
    t.weightCode = fieldInt(row, "Font_WeightCode", "g", 2);
    t.italic = fieldBool(row, "Font_Italic", "h", false);
    t.underline = fieldBool(row, "Font_Underline", "i", false);
    t.red = fieldInt(row, "Colour_Red", "j", 0);
    t.green = fieldInt(row, "Colour_Green", "k", 0);
    t.blue = fieldInt(row, "Colour_Blue", "l", 0);
    t.hAlignCode = fieldInt(row, "HorizontalAlignmentCode", "m", 0);
    t.vAlignCode = fieldInt(row, "VerticalAlignmentCode", "n", 1);
    outModel.textStyles[t.styleId] = std::move(t);
  });

  // ---- M1.4 validator: missing images / empty pages / unreferenced sets ----
  if (!opts.organRootDir.empty()) {
    auto checkImage = [&](const std::string& rel) {
      if (rel.empty()) return;
      if (diskMissing(opts.organRootDir, rel))
        outDiag.missingImageFiles.push_back(rel);
    };
    for (const auto& [id, set] : outModel.imageSets) {
      (void)id;
      checkImage(set.transparencyMaskFile);
      for (const auto& el : set.elements) checkImage(el.bitmapFile);
    }
  }
  for (const auto& [id, page] : outModel.displayPages)
    if (page.instances.empty() && page.texts.empty())
      outDiag.emptyDisplayPages.push_back(id);
  {
    std::unordered_set<Id> referenced;
    for (const auto& [id, page] : outModel.displayPages) {
      (void)id;
      for (const auto& inst : page.instances)
        if (inst.imageSetId != 0) referenced.insert(inst.imageSetId);
    }
    for (const auto& [id, set] : outModel.imageSets) {
      (void)set;
      if (referenced.count(id) == 0) outDiag.unreferencedImageSets.push_back(id);
    }
  }

  // ---- M1.5: Combination table ----
  forEachRow(odfRoot, "Combination", [&](pugi::xml_node row) {
    Combination c;
    c.combinationId = fieldInt(row, "CombinationID", "a", 0);
    c.name = field(row, "Name", "b");
    // Two things at once: 1-7 name a kind, 1xx/2xx/... name a specific piston.
    // Lemmer's ten generals are 101..110 and its general cancel is 100.
    c.type = fieldInt(row, "CombinationTypeCode", "c", 0);
    c.activatingSwitchId = fieldInt(row, "ActivatingSwitchID", "d", 0);
    c.canEngage = fieldBool(row, "CanEngageControlledSwitches", "e", true);
    c.canDisengage = fieldBool(row, "CanDisengageControlledSwitches", "f", true);
    // A cancel takes its behaviour from its TYPE, not from these flags.
    //
    // The flags are worth distrusting here. Lemmer sets exactly one of them to
    // N, and only on its four cancels — and under the field order the OdfEdit
    // dictionary gives, that one is "may not DISENGAGE", which would make a
    // cancel unable to cancel. The type codes are documented outright (6 is a
    // general cancel, 7 a divisional one, and the x00 members of the 1xx/2xx
    // families are cancels too), so the type decides and the flags do not get
    // to contradict it.
    if (c.isCancel()) {
      c.canEngage = false;
      c.canDisengage = true;
    }
    // AllowsCapture also carries a kind code (1 template, 2 general, ...) as
    // well as Y/N, so anything that is not an explicit N allows capture.
    c.allowsCapture = field(row, "AllowsCapture", "g") != "N";
    if (c.combinationId != 0) outModel.combinations[c.combinationId] = std::move(c);
  });

  // ---- M1.5: CombinationElement table (capture wiring; dangling = report) ----
  forEachRow(odfRoot, "CombinationElement", [&](pugi::xml_node row) {
    const Id comboId = fieldInt(row, "CombinationID", "b", 0);
    auto comboIt = outModel.combinations.find(comboId);
    if (comboIt == outModel.combinations.end()) {
      outDiag.danglingIds.push_back(comboId);
      return;
    }
    CombinationElement el;
    el.combinationId = comboId;
    el.controlledSwitchId = fieldInt(row, "ControlledSwitchID", "c", 0);
    el.capturedSwitchId = fieldInt(row, "CapturedSwitchID", "d", 0);
    // The state the organ shipped with, and a separate flag that flips it on
    // the way out. These are fields e and f — reading the stored state out of
    // f, as this did, both took the wrong field and inverted it, so every
    // piston would have recalled the opposite of what it was set to.
    el.storedEngaged = fieldBool(row, "InitialStoredStateIsEngaged", "e", false);
    el.invertWhenActivating =
        fieldBool(row, "InvertStoredStateWhenActivating", "f", false);
    if (el.controlledSwitchId != 0 && outModel.switches.count(el.controlledSwitchId) == 0)
      outDiag.danglingIds.push_back(el.controlledSwitchId);
    if (el.capturedSwitchId != 0 && outModel.switches.count(el.capturedSwitchId) == 0)
      outDiag.danglingIds.push_back(el.capturedSwitchId);
    comboIt->second.elements.push_back(el);
  });

  return outDiag.ok();
}

// ---- M1.5: CODM ObjectList compiler (MP-CODM-HWv9) ----
//
// CODM rows carry no UniqueID: Hauptwerk allocates object IDs when it compiles
// the custom-organ definition into a full model. We do the same, deterministically,
// from a per-table base plus the row ordinal, so a given CODM file always compiles
// to the same IDs (diffable validator output, and combination/voicing files stay
// valid across reloads).
namespace {

constexpr Id kCodmDivisionIdBase  = 1000;
constexpr Id kCodmKeyboardIdBase  = 1100;
constexpr Id kCodmRankIdBase      = 2000;
constexpr Id kCodmStopIdBase      = 3000;
constexpr Id kCodmSwitchIdBase    = 4000;
constexpr Id kCodmCouplerIdBase   = 5000;
constexpr Id kCodmTremulantIdBase = 6000;
constexpr Id kCodmTremSwitchBase  = 6500;
constexpr Id kCodmEnclosureIdBase = 7000;
constexpr Id kCodmWindIdBase      = 8000;

// A division named "pedal" becomes manual 0; the rest are numbered in file
// order (the CODM guide lists Pedal + Manual 1-6 as the standard set, but the
// row order is what fixes the console layout).
bool looksLikePedal(const std::string& name) {
  const std::string n = lower(name);
  return n.find("pedal") != std::string::npos || n.find("ped.") != std::string::npos;
}

} // namespace

bool parseCodm(const pugi::xml_node& odfRoot, const OdfLoader::Options& opts,
               OrganModel& model, OdfDiagnostics& diag) {
  (void)opts;

  auto noteUnhandled = [&](const std::string& table, const std::string& fieldName) {
    const std::string key = table + "." + fieldName;
    auto& v = diag.codmUnhandledFields;
    if (std::find(v.begin(), v.end(), key) == v.end()) v.push_back(key);
  };
  auto noteUnmapped = [&](const std::string& table) {
    auto& v = diag.codmUnmappedElements;
    if (std::find(v.begin(), v.end(), table) == v.end()) v.push_back(table);
  };
  // ADR-002: every field we did not consume is reported, never silently lost.
  auto reportUnconsumed = [&](const pugi::xml_node& row, const std::string& table,
                              const std::unordered_set<std::string>& handled) {
    for (pugi::xml_node f : row.children()) {
      if (f.type() != pugi::node_element) continue;
      if (handled.count(f.name()) == 0) noteUnhandled(table, f.name());
    }
  };

  // Registered CODM tables (codm-summary + guide prose), matched
  // case-insensitively (the guide shows both "division" and "CustomDisplayPage").
  auto isKnown = [](const std::string& t) {
    static const std::unordered_set<std::string> known = {
      "_general", "division", "rank", "stop", "stoprank", "coupler",
      "tremulant", "enclosure", "combination", "divisional", "general",
      "shortcutpiston", "reversiblepiston", "customdisplaypage",
      "customdisplaylabel", "customdisplaycontrolstyle",
      "customdisplaykeyboardstyle", "noise", "soundeffect", "package",
      "customorganrank", "customorgantremulantwaveformset",
      "tremulantwaveformset",
    };
    return known.count(t) != 0;
  };

  // Divisions must exist before stops and enclosures can attach to them, so the
  // compiler walks table-by-table in dependency order, not in file order.
  std::vector<pugi::xml_node> lists;
  for (pugi::xml_node list : odfRoot.children()) {
    if (list.type() != pugi::node_element) continue;
    if (lower(list.name()) != "objectlist") continue;
    const std::string type = lower(list.attribute("ObjectType").value());
    if (type.empty()) {
      diag.warnings.emplace_back("CODM: ObjectList without ObjectType (skipped)");
      continue;
    }
    if (!isKnown(type)) {
      diag.warnings.emplace_back("unknown CODM ObjectType preserved (forward-compat): " + type);
      continue;
    }
    lists.push_back(list);
  }

  auto forEachCodmRow = [&](const char* table,
                            const std::function<void(const pugi::xml_node&, int)>& fn) {
    int ordinal = 0;
    for (const pugi::xml_node& list : lists) {
      if (lower(list.attribute("ObjectType").value()) != table) continue;
      for (pugi::xml_node row : list.children()) {
        if (row.type() != pugi::node_element) continue;
        fn(row, ordinal++);
      }
    }
  };

  // ---- _general [guide]: UniqueOrganID + organ Name ----
  forEachCodmRow("_general", [&](const pugi::xml_node& row, int) {
    model.uniqueOrganId = fieldInt(row, "UniqueOrganID", nullptr, model.uniqueOrganId);
    const std::string n = field(row, "Name");
    if (!n.empty() && model.organName.empty()) model.organName = n;
    reportUnconsumed(row, "_general", {"UniqueOrganID", "Name"});
  });

  // ---- division [guide]: Name + windchest pressure drop ----
  // Each division also gets one keyboard (the CODM console is one manual per
  // division) and one wind compartment when a pressure drop is declared.
  int manualCounter = 0;
  std::vector<Id> divisionOrder; // file order -> division id, for stop/coupler lookup
  forEachCodmRow("division", [&](const pugi::xml_node& row, int ordinal) {
    Division d;
    d.divisionId = kCodmDivisionIdBase + ordinal;
    d.name = field(row, "Name");
    d.manualNumber = looksLikePedal(d.name) ? 0 : ++manualCounter;

    Keyboard kb;
    kb.keyboardId = kCodmKeyboardIdBase + ordinal;
    kb.name = d.name.empty() ? ("Keyboard " + std::to_string(ordinal + 1)) : d.name;
    // CODM console defaults: 32-note pedalboard, 61-note manuals, both from C1.
    kb.numKeys = (d.manualNumber == 0) ? 32 : 61;
    kb.firstMidiNote = 36;
    d.keyboardIds.push_back(kb.keyboardId);
    model.keyboards[kb.keyboardId] = std::move(kb);

    const double dropPct =
        fieldDouble(row, "WindModel_WindchestPressureDropPctAtMaxLoad", nullptr, -1.0);
    if (dropPct >= 0.0) {
      WindCompartment wc;
      wc.compartmentId = kCodmWindIdBase + ordinal;
      wc.name = d.name;
      // A declared pressure drop means the chest is modelled, not infinite.
      wc.infiniteVolume = (dropPct == 0.0);
      wc.defaultPressureInches = 1.0;
      model.wind[wc.compartmentId] = std::move(wc);
    }

    divisionOrder.push_back(d.divisionId);
    reportUnconsumed(row, "division",
                     {"Name", "WindModel_WindchestPressureDropPctAtMaxLoad"});
    model.divisions[d.divisionId] = std::move(d);
  });

  // ---- rank [guide]: Name + pitch/footage + tremulant code ----
  forEachCodmRow("rank", [&](const pugi::xml_node& row, int ordinal) {
    Rank r;
    r.rankId = kCodmRankIdBase + ordinal;
    r.name = field(row, "Name");
    const int outHarm =
        fieldInt(row, "Pitch_RankBaseOutputPitch64ftHarmonicNum", nullptr, 8);
    const int recHarm = fieldInt(
        row, "Samples_RankBasePitch64ftHarmNumIfAssumedTunedToConcertPitch", nullptr,
        outHarm);
    // Pipes come from the sample-set install, not from the CODM row: the row
    // only declares the rank's pitch contract, which every pipe inherits. The
    // contract is parked on a template SampleRef keyed by rank id so M2 pipe
    // generation reads it from one place.
    SampleRef pitchContract;
    pitchContract.sampleId = r.rankId;
    pitchContract.rankBasePitch64ftHarmonicNum = recHarm;
    model.samples[r.rankId] = pitchContract;
    if (outHarm != recHarm)
      diag.warnings.emplace_back(
          "CODM rank '" + r.name + "': output pitch 64ft harmonic " +
          std::to_string(outHarm) + " differs from sample-assumed " +
          std::to_string(recHarm) + " (transposed rank)");

    reportUnconsumed(row, "rank",
                     {"Name", "Pitch_RankBaseOutputPitch64ftHarmonicNum",
                      "Samples_RankBasePitch64ftHarmNumIfAssumedTunedToConcertPitch",
                      "Trem_TremulantCode"});
    model.ranks[r.rankId] = std::move(r);
  });

  // ---- stop [guide]: Name (+ optional explicit division / assignment code) ----
  forEachCodmRow("stop", [&](const pugi::xml_node& row, int ordinal) {
    Stop s;
    s.stopId = kCodmStopIdBase + ordinal;
    s.name = field(row, "Name");
    s.defaultAsgnCode = fieldInt(row, "DefaultInputOutputAsgnCode", nullptr, 0);
    const int explicitDiv = fieldInt(row, "DivisionID", nullptr, 0);
    if (explicitDiv > 0 && explicitDiv <= static_cast<int>(divisionOrder.size())) {
      s.divisionId = divisionOrder[static_cast<size_t>(explicitDiv - 1)];
    } else if (s.defaultAsgnCode != 0) {
      const int di = mp::codm::captureDivisionForStopCode(s.defaultAsgnCode);
      if (di >= 0 && di < static_cast<int>(divisionOrder.size()))
        s.divisionId = divisionOrder[static_cast<size_t>(di)];
      else
        diag.unmappedAsgnCodes.push_back(s.defaultAsgnCode);
    }
    if (s.divisionId == 0 && !divisionOrder.empty()) {
      // Nothing in the row pins the stop to a division. Rather than drop it,
      // park it on the first division and say so (ADR-002).
      s.divisionId = divisionOrder.front();
      diag.warnings.emplace_back(
          "CODM stop '" + s.name +
          "': no DivisionID/assignment code; parked on first division");
    }
    // Every CODM stop gets the drawstop switch the player pulls.
    Switch sw;
    sw.switchId = kCodmSwitchIdBase + ordinal;
    sw.name = s.name;
    model.switches[sw.switchId] = sw;
    s.controllingSwitchId = sw.switchId;

    // StopRank rows arrive with the sample-set install, not from CODM.
    if (s.ranks.empty()) diag.stopsWithoutRanks.push_back(s.stopId);

    reportUnconsumed(row, "stop", {"Name", "DivisionID", "DefaultInputOutputAsgnCode"});
    model.stops[s.stopId] = std::move(s);
  });

  // ---- coupler [guide]: Name (+ code) -> Switch + KeyAction ----
  forEachCodmRow("coupler", [&](const pugi::xml_node& row, int ordinal) {
    Switch sw;
    sw.switchId = kCodmCouplerIdBase + ordinal;
    sw.name = field(row, "Name");
    sw.isCoupler = true;
    model.switches[sw.switchId] = sw;

    KeyAction ka;
    ka.id = kCodmCouplerIdBase + ordinal;
    ka.conditionSwitchId = sw.switchId;
    ka.actionType = 1;
    const int code = fieldInt(row, "Coupler_CouplerCode", nullptr, 0);
    if (code != 0 && mp::codm::captureDivisionForCouplerCode(code) == -1)
      diag.unmappedActionTypes.push_back(code);
    // Source/destination divisions come from the coupler-code table, which is
    // M3 work; the linkage is recorded now so the switch is never orphaned.
    model.keyActions.push_back(ka);

    reportUnconsumed(row, "coupler", {"Name", "Coupler_CouplerCode"});
  });

  // ---- tremulant [guide]: Name (+ rates) ----
  forEachCodmRow("tremulant", [&](const pugi::xml_node& row, int ordinal) {
    Tremulant t;
    t.tremulantId = kCodmTremulantIdBase + ordinal;
    t.name = field(row, "Name");
    t.engagedHz = fieldDouble(row, "Trem_EngagedRateHz", nullptr, t.engagedHz);
    t.disengagedHz = fieldDouble(row, "Trem_DisengagedRateHz", nullptr, t.disengagedHz);
    model.tremulants[t.tremulantId] = t;

    Switch sw;
    sw.switchId = kCodmTremSwitchBase + ordinal;
    sw.name = t.name;
    sw.isTremulant = true;
    model.switches[sw.switchId] = std::move(sw);

    reportUnconsumed(row, "tremulant",
                     {"Name", "Trem_EngagedRateHz", "Trem_DisengagedRateHz"});
  });

  // ---- enclosure [guide]: Name + Encl_EnclosureCode ----
  forEachCodmRow("enclosure", [&](const pugi::xml_node& row, int ordinal) {
    Enclosure e;
    e.enclosureId = kCodmEnclosureIdBase + ordinal;
    e.name = field(row, "Name");
    // Encl_EnclosureCode selects which console shoe drives this enclosure. The
    // code-to-shoe table is HW-licensed detail (M2 verification); the raw code
    // is kept as the continuous-control id so nothing is lost.
    e.continuousControlId = fieldInt(row, "Encl_EnclosureCode", nullptr, 0);
    model.enclosures[e.enclosureId] = std::move(e);

    reportUnconsumed(row, "enclosure", {"Name", "Encl_EnclosureCode"});
  });

  // ---- customdisplaypage [guide]: DisplayPageID + Name ----
  forEachCodmRow("customdisplaypage", [&](const pugi::xml_node& row, int) {
    DisplayPage page;
    page.pageId = fieldInt(row, "DisplayPageID", nullptr, 0);
    page.name = field(row, "Name");
    reportUnconsumed(row, "customdisplaypage", {"DisplayPageID", "Name"});
    if (page.pageId != 0) model.displayPages[page.pageId] = std::move(page);
  });

  // ---- registered but not yet compiled ----
  // Tables whose semantics still need licensed-install verification. Their rows
  // are reported field by field, so the remaining gap is an explicit checklist.
  static const char* const kPending[] = {
      "stoprank", "combination", "divisional", "general", "shortcutpiston",
      "reversiblepiston", "customdisplaylabel", "customdisplaycontrolstyle",
      "customdisplaykeyboardstyle", "noise", "soundeffect", "package",
      "customorganrank", "customorgantremulantwaveformset", "tremulantwaveformset"};
  for (const char* table : kPending) {
    bool any = false;
    forEachCodmRow(table, [&](const pugi::xml_node& row, int) {
      any = true;
      reportUnconsumed(row, table, {});
    });
    if (any) noteUnmapped(table);
  }

  // MP-CODM-HWv9 fixed defaults (CodmCompiler): setter + jamb/divisional rules.
  mp::codm::applyCodmDefaults(model, diag);
  return diag.ok();
}

namespace {

// The definition's grandparent, unless the immediate parent is itself named
// OrganDefinitions (case-insensitively -- these sets are authored on
// Windows), in which case its parent is the root. A loose ODF with neither
// falls back to its own containing directory.
std::filesystem::path organRootFrom(const std::filesystem::path& odfPath) {
  std::filesystem::path root = odfPath.parent_path();
  const std::string name = lower(root.filename().string());
  if (name == "organdefinitions") root = root.parent_path();
  return root;
}

bool hasInstallationPackages(const std::filesystem::path& root) {
  std::error_code ec;
  return std::filesystem::is_directory(root / "OrganInstallationPackages", ec);
}

} // namespace

std::string findLibraryHolding(const std::vector<std::string>& roots,
                               const OrganModel& model) {
  std::vector<Id> wanted;
  for (const auto& [id, ref] : model.samples) {
    (void)id;
    if (ref.installationPackageId > 0 &&
        std::find(wanted.begin(), wanted.end(), ref.installationPackageId) == wanted.end())
      wanted.push_back(ref.installationPackageId);
    if (wanted.size() >= 4) break; // four is plenty to tell libraries apart
  }
  if (wanted.empty()) return {};

  std::error_code ec;
  for (const auto& root : roots) {
    const std::filesystem::path packages =
        std::filesystem::path(root) / "OrganInstallationPackages";
    if (!std::filesystem::is_directory(packages, ec)) continue;
    bool all = true;
    for (Id id : wanted) {
      std::string digits = std::to_string(id);
      if (digits.size() < 6) digits.insert(0, 6 - digits.size(), '0');
      if (!std::filesystem::is_directory(packages / digits, ec)) {
        all = false;
        break;
      }
    }
    if (all) return root;
  }
  return {};
}

std::string deriveOrganRoot(const std::string& odfPath) {
  const std::filesystem::path odf(odfPath);
  const std::filesystem::path logicalRoot = organRootFrom(odf);
  if (hasInstallationPackages(logicalRoot)) return logicalRoot.string();

  // The logical root has no OrganInstallationPackages sibling. A set whose
  // folders are linked in from elsewhere can hand back a path whose textual
  // parent is not where the packages live, so the search widens:
  //
  //   * the same path with its symlinks resolved -- OrganDefinitions moved to
  //     another drive and linked back in resolves to where it really is;
  //   * the ancestors of both, because a link can land the definition several
  //     levels below the folder that holds the packages.
  //
  // A set that genuinely has no packages keeps the logical answer, so nothing
  // about a loose ODF changes.
  std::error_code ec;
  std::vector<std::filesystem::path> starts{logicalRoot};
  const std::filesystem::path canonicalOdf =
      std::filesystem::weakly_canonical(odf, ec);
  if (!ec && canonicalOdf != odf) starts.push_back(organRootFrom(canonicalOdf));

  // Four levels is past any layout we have seen and stops well short of a
  // drive's root, where a stray folder of that name would be someone else's.
  constexpr int kMaxAncestors = 4;
  for (const auto& start : starts) {
    std::filesystem::path dir = start;
    for (int up = 0; up <= kMaxAncestors; ++up) {
      if (hasInstallationPackages(dir)) return dir.string();
      const std::filesystem::path parent = dir.parent_path();
      if (parent.empty() || parent == dir) break;
      dir = parent;
    }
  }
  return logicalRoot.string();
}

} // namespace mp
