// Perspectives: the microphone positions a sample set was recorded from.
//
// A set recorded from several places -- close, front, middle, rear; direct,
// diffuse, rear -- carries the whole organ once per position, each as ranks
// of its own, and says which is which only in the rank names:
// "Sousbasse 32 (close)", "Principal 8 Rear". Loading every perspective of
// a large set takes three or four times the memory of one, so a player with
// less RAM than the set was made for leaves some out. Stops cannot say this:
// every stop plays one rank of each perspective.
#pragma once

#include "OrganModel.h"

#include <map>
#include <string>
#include <vector>

namespace mp {

// The perspective a rank name names, lower-case, or empty: the text in
// trailing brackets ("Flute 8 (close)" -> "close"), or else a last word that
// only ever names a microphone position ("Principal 8 Rear" -> "rear").
std::string perspectiveOf(const std::string& rankName);

// The organ's perspectives and the ranks in each, when it has them: at least
// two, each holding a fair share of the ranks. A bracket that says something
// else ("Cornet (5 rgs)") names a handful of ranks at most, and does not
// make a perspective of them. Empty for an organ recorded from one place.
//
// A GrandOrgue set has no perspectives; its windchest groups stand in for
// them, the grouping its own stop tree uses (#136).
std::map<std::string, std::vector<Id>> perspectivesOf(const OrganModel& model);

// A GrandOrgue set's windchest groups and their ranks; empty with fewer than
// two, or for a set from anywhere else.
std::map<std::string, std::vector<Id>> windchestsOf(const OrganModel& model);

// Whether perspectivesOf() answered with windchest groups, which the stop
// list then names as such.
bool groupedByWindchest(const OrganModel& model);

}  // namespace mp
