// Masterpiece ODF loader — HW full (*.Organ_Hauptwerk_xml) + CODM.
// Bootstrap: OdfEdit v2.22 object list (40 types) + HW2GO link lessons.
// Strategy per docs/format-spec/00-strategy.md:
//  - streaming XML parse (100MB+ files), preserve unknown tables (warn, not fail)
//  - ID-link phase (OdfEdit do_links_between_objects equivalent)
//  - CODM compile with HW defaults (MP-CODM-HWv9)
//  - WAV-only v1; detect HBW/HBX + licence-gated, report rank list.
#pragma once
#include "OrganModel.h"
#include <functional>
#include <string>
#include <vector>

namespace mp {

struct OdfDiagnostics {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<std::string> skipped; // OdfEdit SKIPPED lesson: never silent-drop
  std::vector<Id> danglingIds;
  std::vector<std::string> missingSampleFiles;
  std::vector<std::string> encryptedSamples;
  // Validation queries (format-spec/01-required-tables.md):
  std::vector<Id> switchesWithoutCombinationElements;
  std::vector<std::string> jambOverflows; // >38/division
  std::vector<int> unmappedAsgnCodes;
  std::vector<Id> stopsWithoutRanks; // M1.3: stop has no StopRank rows
  std::vector<int> unmappedActionTypes; // M1.3: KeyAction codes beyond the M1 matrix
  std::vector<Id> defaultEngagedSwitches; // M1.3: informational (capture baseline)
  // M3: SwitchLinkage action codes we do not model. Reported rather than
  // treated as a plain follow, because guessing here silently rewires an organ
  // (ADR-002).
  std::vector<int> unmappedLinkageCodes;
  std::vector<Id> switchLinkageDangling; // linkage naming a switch that is not there
  std::vector<std::string> missingImageFiles; // M1.4: element/mask bitmaps absent on disk
  std::vector<Id> emptyDisplayPages; // M1.4: page with no instances and no texts
  std::vector<Id> unreferencedImageSets; // M1.4: set never instanced on any page
  std::vector<std::string> codmUnmappedElements; // M1.5: CODM ObjectTypes without a compiler
  std::vector<std::string> codmUnhandledFields; // M1.5: CODM table.field names the compiler ignores
  std::vector<Id> layersUncoveredVelocity; // M2.1: layer ids with velocity gaps
  std::vector<Id> deadReleaseBranches; // M2.1: release ids linked to missing attacks
  std::vector<Id> enclosuresWithoutShades;   // M2.3: enclosure-without-shades
  std::vector<Id> tremulantsDepthZero;       // M2.3: trem-depth-zero-but-enabled
  std::vector<Id> unmappedContinuousControls; // M2.4: continuous-control-unmapped
  std::vector<Id> noiseRanksWithoutSample;   // M2.4: noise-rank-without-sample
  std::vector<Id> controlLinkageCycles;      // M2.4: shoe/crescendo feedback loop
  std::vector<std::string> unknownTemperaments; // M2.2: temperament-unknown-code
  // M2.2: pipe-pitch-out-of-range. A bare id cannot be triaged, so each entry
  // carries what produced the pitch.
  struct PipePitchFault {
    Id pipeId = 0;
    Id rankId = 0;
    std::string rankName;
    int midiNote = 0;
    int harmonicNum = 0;
    double hz = 0.0;
  };
  std::vector<PipePitchFault> pipesPitchOutOfRange;
  bool ok() const { return errors.empty(); }
};

class OdfLoader {
public:
  using ProgressFn = std::function<void(float, const std::string&)>;

  struct Options {
    bool buildAltLayouts = true;      // max layout 3 else 0 (OdfEdit flag)
    bool pitchFromMetadata = true;
    bool pitchFromFilename = true;    // 036-C.wav style
    bool includeKeyNoises = true;
    bool includeUnusedRanks = false;
    std::string organRootDir;         // for Sample file existence checks
    // Stop after _General. Enough to find out which organ this is — its name
    // and its UniqueOrganID — without parsing a 60 000-row Sample table to do
    // it. Used to locate an organ's saved settings before deciding to load it.
    bool headerOnly = false;
  };

  // Detect by header/extension: full vs CODM vs SQLite-exported XML.
  static OdfType detectType(const std::string& xmlHead, const std::string& fileName);

  // Load from file. Runs parse -> link -> compile(CODM) -> audio/control graph hooks.
  // Never throws on unknown tables; records warnings. Errors only for fatal XML/ID faults.
  bool load(const std::string& odfPath, const Options& opts,
            OrganModel& outModel, OdfDiagnostics& outDiag,
            ProgressFn progress = {});

  // Load from memory (tests + SQLite round-trip).
  bool loadFromXmlString(const std::string& xml, const std::string& fileNameHint,
                         const Options& opts, OrganModel& outModel,
                         OdfDiagnostics& outDiag);
};

// Given the path to a *.Organ_Hauptwerk_xml (or *.CustomOrgan_Hauptwerk_xml)
// file, return the directory OrganInstallationPackages and OrganDefinitions
// hang off. Ordinarily that is just the ODF's grandparent — <root>/
// OrganDefinitions/Foo.Organ_Hauptwerk_xml -> <root> — but a set someone has
// reorganised with symbolic links (moving OrganDefinitions, or
// OrganInstallationPackages, or a single package folder inside it, onto
// another drive) can hand back an odfPath whose plain parent-directory walk
// no longer lines up with where the audio actually lives, because a symlink
// resolved to a differently-named ancestor somewhere along the way. Tried as
// given first, then with the path's symlinks resolved, keeping whichever one
// actually has an OrganInstallationPackages sibling; falls back to the
// as-given answer if neither does, so a loose ODF with no installation
// packages at all is unaffected.
std::string deriveOrganRoot(const std::string& odfPath);

// Of these library roots, the first whose OrganInstallationPackages holds the
// packages this model names, or an empty string. For a definition whose path
// cannot lead to its audio: both standard folders linked to unrelated drives
// leave no shared parent to find. Checking the package ids the definition
// names is what makes the answer right rather than a guess -- a library that
// holds other organs is passed over.
std::string findLibraryHolding(const std::vector<std::string>& roots,
                               const OrganModel& model);

} // namespace mp
