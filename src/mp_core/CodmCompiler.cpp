#include "CodmCompiler.h"

#include <string>

namespace mp::codm {

int captureDivisionForStopCode(int asgnCode) {
  // 20xx-26xx/30xx stop codes -> one of 7 standard divisions.
  // Full matrix in specs/odfedit HwObjectsAttributesCodes.txt; M1: coarse map.
  if (asgnCode >= 2000 && asgnCode < 2700) return (asgnCode / 100) - 20; // 20xx->0(Ped)..26xx->6
  if (asgnCode >= 3000 && asgnCode < 3100) return 0;
  return -1; // unmapped -> validator logs full code for triage
}

int captureDivisionForCouplerCode(int couplerCode) {
  if (couplerCode >= 1000 && couplerCode <= 1634) return 0; // refined in M3
  if (couplerCode >= 10000) return -2; // custom coupler -> full-ODF path
  return -1;
}

namespace {

// Combination id space for compiled CODM registration objects. Kept clear of
// the object-id bases the CODM parser uses (1000-8999) so a compiled model can
// be diffed against a full ODF without id collisions.
constexpr Id kSetterId = 9000;
constexpr Id kGeneralCancelId = 9001;
constexpr Id kGeneralsBase = 9100;      // 9100..9119 -> generals 1..20
constexpr Id kDivisionalsBase = 9200;   // 9200 + div*100 + n

// Combination::type — local encoding until the HW type codes are verified
// against a licensed install (tracked in docs/GAP_REGISTER.md).
constexpr int kTypeSetter = 1;
constexpr int kTypeGeneralCancel = 2;
constexpr int kTypeGeneral = 3;
constexpr int kTypeDivisional = 4;

void addCombination(OrganModel& model, Id id, int type, std::string name) {
  if (model.combinations.count(id) != 0) return; // author-declared wins
  Combination c;
  c.combinationId = id;
  c.type = type;
  c.name = std::move(name);
  model.combinations.emplace(id, std::move(c));
}

} // namespace

void applyCodmDefaults(OrganModel& model, OdfDiagnostics& diag) {
  if (static_cast<int>(model.divisions.size()) > kMaxStandardDivisions)
    diag.warnings.emplace_back(
        "CODM: more than 7 standard divisions; extras need full-ODF authoring");

  // Master capture + General Cancel + the fixed general piston bank.
  addCombination(model, kSetterId, kTypeSetter, "Setter");
  addCombination(model, kGeneralCancelId, kTypeGeneralCancel, "General Cancel");
  for (int n = 1; n <= kNumGenerals; ++n)
    addCombination(model, kGeneralsBase + n - 1, kTypeGeneral,
                   "General " + std::to_string(n));

  // Per-division divisional bank, plus the jamb cap check. Divisions are
  // walked in manual order so the compiled ids do not depend on hash order.
  std::vector<std::pair<int, Id>> byManual;
  byManual.reserve(model.divisions.size());
  for (const auto& kv : model.divisions)
    byManual.emplace_back(kv.second.manualNumber, kv.first);
  std::sort(byManual.begin(), byManual.end());

  int divIndex = 0;
  for (const auto& entry : byManual) {
    const Division& d = model.divisions.at(entry.second);
    for (int n = 1; n <= kNumDivisionalsPerDivision; ++n)
      addCombination(model, kDivisionalsBase + divIndex * 100 + n - 1, kTypeDivisional,
                     d.name + " " + std::to_string(n));

    int stopsHere = 0;
    for (const auto& sv : model.stops)
      if (sv.second.divisionId == d.divisionId) ++stopsHere;
    if (stopsHere > kMaxJambPerDivision)
      diag.jambOverflows.push_back(d.name + ": " + std::to_string(stopsHere) +
                                   " stops exceeds the " +
                                   std::to_string(kMaxJambPerDivision) +
                                   "-per-division jamb cap");
    ++divIndex;
  }
}

} // namespace mp::codm
