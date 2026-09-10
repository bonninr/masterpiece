// CODM compiler defaults (MP-CODM-HWv9) — must match Hauptwerk so CODM
// sets sound identical. Source: docs/reference/04-codm-summary.md.
#pragma once
#include "OdfLoader.h"
#include "OrganModel.h"

#include <algorithm>
#include <utility>

namespace mp::codm {

inline constexpr int kMaxStandardDivisions = 7; // Pedal + Manual 1-6
inline constexpr int kNumGenerals = 20;
inline constexpr int kNumDivisionalsPerDivision = 20;
inline constexpr int kMaxJambPerDivision = 38; // default console cap
inline constexpr int kDefaultConsolePages = 4;

// Stop/Coupler/Tremulant codes determine capture division + jamb sort + MIDI default.
// Full table lives in OdfEdit HwObjectsAttributesCodes.txt (vendored in specs/odfedit).
int captureDivisionForStopCode(int asgnCode);
int captureDivisionForCouplerCode(int couplerCode);

// Apply fixed CODM compile assumptions to a freshly parsed CODM model:
// keyboards/jamb columns/divisionals, master capture + General Cancel,
// 1 enclosure/division + optional General Swell, default 4-page console.
void applyCodmDefaults(OrganModel& model, OdfDiagnostics& diag);

} // namespace mp::codm
