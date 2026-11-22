// A jOrgan organ, translated: what the disposition and its SoundFonts mean
// as an organ, in plain data. JOrganImport fills it; JOrganHauptwerk writes
// it as a Hauptwerk definition.
#pragma once

#include <string>
#include <vector>

namespace mp {

struct JOrganOrgan {
  std::string name;
  long long id = 0;  // from the disposition's bytes: the same file, the same organ

  struct Manual {
    std::string name;
    bool pedal = false;
    int firstMidi = 36, keys = 61;
  };
  std::vector<Manual> manuals;  // the pedal first, when there is one

  // A drawstop: the switch a stop, coupler or tremulant hangs on.
  struct Drawstop {
    std::string name, label;
    bool engaged = false;
  };
  std::vector<Drawstop> drawstops;  // in console order

  struct Pipe {
    int midi = 0;
    std::string file;       // relative to the work folder
    double cents = 0.0;     // from the pipe's nominal pitch
    double db = 0.0;
    bool percussive = false;
    std::string tremulantFile;  // the take with the tremulant, when it has one
  };
  struct Rank {
    std::string name;
    int chest = 0;          // index into chests
    double db = 0.0;        // the rank's own level
    std::vector<Pipe> pipes;
  };
  std::vector<Rank> ranks;

  struct StopRank {
    int rank = 0;
    int firstKey = 36;      // the first MIDI note of the manual it is mapped from
    int count = 0;
    int increment = 0;      // rank note = key + increment
  };
  struct Stop {
    std::string name;
    int drawstop = 0;
    int manual = 0;
    std::vector<StopRank> ranks;
  };
  std::vector<Stop> stops;

  struct Coupler {
    std::string name;
    int drawstop = 0;
    int manual = 0, dest = 0;
    int shift = 0;
    bool unisonOff = false;
  };
  std::vector<Coupler> couplers;

  struct Enclosure {
    std::string name;
    double closedPct = 1.0;  // the level with the shoe closed
  };
  std::vector<Enclosure> enclosures;

  struct Tremulant {
    std::string name;
    int drawstop = 0;
    bool wave = false;       // swaps in the tremulant takes
    double periodMs = 122.0;
    double ampPct = 6.0;     // level swing
    double pitchCents = 0.0; // pitch swing
  };
  std::vector<Tremulant> tremulants;

  struct Chest {
    std::vector<int> enclosures, tremulants;  // indexes
  };
  std::vector<Chest> chests;
};

}  // namespace mp
