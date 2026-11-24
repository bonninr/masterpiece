// The loaded organ's wind system and tremulants as text, for the Diagnostics
// report: what a player copies and sends when a set sounds wrong with the wind
// model on. It says what the program read from the definition (compartments,
// linkages and the switches and controls that work their valves, tremulants,
// the ranks a switch swaps in) by id and name, and nothing of the definition's
// own file.
#pragma once

#include "OrganModel.h"

#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

namespace mp {

// The ranks a player hears as the organ's noises, not its pipework: a rank of
// the Noise table or a noise effect (Rank::noiseEffect), and a rank that only
// the action sounds, every stop row that plays it an action noise
// (StopRankEntry::isActionNoise): a blower that runs while its stop is drawn,
// a drawstop's clicks, a key's thump. Lemmer, Cheltenham, St Davids and
// Wildervank put their blower in a Noises division this way, tuned like a
// pipe (#1245).
std::unordered_set<Id> noiseRanks(const OrganModel& m);

// The working pressure a compartment settles to and its pressure now, in
// inches of water, from the running wind solver.
using CompartmentPressures = std::function<std::pair<double, double>(Id)>;

// One line per object, lowest id first, each section headed by its count.
// A section longer than maxRows lines ends with the number left out. With
// pressures, each compartment also says its working pressure and its
// pressure now.
std::string windAndTremulantReport(const OrganModel& m, const CompartmentPressures& pressures = {},
                                   int maxRows = 400);

}  // namespace mp
