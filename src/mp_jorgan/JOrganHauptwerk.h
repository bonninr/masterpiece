// A translated jOrgan organ as a Hauptwerk definition, in memory, for the
// loader: keyboards and divisions, stops, couplers as key actions, ranks
// and pipes on the WAV files, enclosures, tremulants, and a console page
// drawn with the program's stock drawstops, keys and shoes.
#pragma once

#include "JOrganOrgan.h"

#include <string>

namespace mp {

std::string jorganHauptwerkXml(const JOrganOrgan& organ);

// Only the _General table: the organ's name and id, for a header-only load.
std::string jorganHauptwerkHeader(const std::string& name, long long id);

}  // namespace mp
