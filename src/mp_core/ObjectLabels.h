// What a player calls the things on a console: the name shown beside a switch
// in a menu, a window title or a list.
//
// The stop or tremulant a switch works is the name to show, because a set
// often names the switch behind a drawn knob by an internal tag. Where one
// switch works several things — a Sesquialtera with a knob per variant, both
// knobs reaching the one switch that turns the stop on — the wiring cannot
// tell the controls apart, and the set's own name for the switch is what
// distinguishes them.
#pragma once

#include "OrganModel.h"

#include <string>
#include <vector>

namespace mp {

// A name as a player would read it: a set's internal prefix (`__Midm_`) taken
// off, runs of spaces squeezed, both ends trimmed.
std::string readableName(const std::string& raw);

// What a switch works: the names of the stops and tremulants at the nearest
// ring of the wiring, forwards through the linkages first, then what drives
// the switch. Lowest id first, each name once; empty when the wiring leads to
// nothing the console names.
std::vector<std::string> switchWorkedNames(const OrganModel& m, Id switchId);

// The name to show for a switch. Empty when there is nothing to go on, which
// leaves "Switch 1187" to the caller.
std::string switchLabel(const OrganModel& m, Id switchId);

}  // namespace mp
