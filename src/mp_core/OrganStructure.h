// The loaded organ's wind system and tremulants as text, for the Diagnostics
// report: what a player copies and sends when a set sounds wrong with the wind
// model on. It says what the program read from the definition (compartments,
// linkages and the switches and controls that work their valves, tremulants,
// the ranks a switch swaps in) by id and name, and nothing of the definition's
// own file.
#pragma once

#include "OrganModel.h"

#include <string>

namespace mp {

// One line per object, lowest id first, each section headed by its count.
// A section longer than maxRows lines ends with the number left out.
std::string windAndTremulantReport(const OrganModel& m, int maxRows = 400);

}  // namespace mp
