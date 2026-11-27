// Everything else a GrandOrgue settings file (.cmb) holds that Masterpiece can
// take, beside its MIDI events (GrandOrgueMidi): the registrations the player
// set, the voicing, and the organ's own settings (#1012).
//
// GrandOrgue keeps, for one organ, in one file:
//   - its own generals and divisionals as the player changed them
//     ([General001], [Divisional003]), and the setter's memory: its banked
//     generals ([SetterGeneral001]...), its sequencer frames
//     ([FrameGeneral001]...) and its divisionals per manual
//     ([Setter002Divisional000]...);
//   - the crescendo's stages ([SetterCrescendo1_001]...): bank A for the
//     player's crescendo, the other banks counted as left out;
//   - the voicing at four levels, organ, windchest group, rank and pipe:
//     Amplitude and UserGain, ManualTuning and AutoTuningCorrection,
//     ToneBalance, AudioGroup, and Delay and ReleaseTail, which have nothing
//     to go to here;
//   - the organ's volume and temperament, each swell's position, each
//     drawstop's state, and the metronome.
//
// Read here into plain data, against the model the organ was converted into
// (OrganModel::grandOrgueObjects, grandOrgueRanks, grandOrgueVoicing), and
// applied by the processor. JUCE-free, so the tests feed it a file directly.
#pragma once

#include "../mp_core/OrganModel.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mp {

// A registration as GrandOrgue stores it: the switches it names, each drawn
// or retired. What it does not name it leaves as it is.
struct GrandOrgueRegistration {
  std::vector<std::pair<Id, bool>> switches;
  bool full = false;  // IsFull: GrandOrgue's "sets everything"
};

// A change to one rank or pipe, in Masterpiece's voicing terms.
struct GrandOrgueVoice {
  double gainDb = 0.0;
  double tuningCents = 0.0;
  double brightnessDb = 0.0;
  double bassDb = 0.0;
  bool neutral() const { return gainDb == 0.0 && tuningCents == 0.0 && brightnessDb == 0.0 && bassDb == 0.0; }
};

struct GrandOrgueSettings {
  // The organ's own generals and divisionals, by the switch of their piston.
  std::map<Id, GrandOrgueRegistration> organPistons;
  // The setter's memory, in Masterpiece's player pistons: generals 1 to 100,
  // sequencer frames 1 to 999, divisionals 1 to 20 by division.
  std::map<int, GrandOrgueRegistration> generals;
  std::map<int, GrandOrgueRegistration> frames;
  std::map<std::pair<Id, int>, GrandOrgueRegistration> divisionals;
  // The crescendo's bank A, steps 1 to 32, for the player's crescendo.
  std::map<int, GrandOrgueRegistration> crescendo;
  // The voicing, by rank and by pipe. Every rank whose section the file holds
  // is here, neutral or not: its voicing becomes the file's. A pipe is here
  // when it differs from its rank.
  std::map<Id, GrandOrgueVoice> ranks;
  std::map<Id, GrandOrgueVoice> pipes;
  // A rank's audio group, by its name, for routing to an output of that name.
  std::map<Id, std::string> audioGroups;
  // The organ: its volume as a change from GrandOrgue's own default (-15 dB),
  // its temperament as GrandOrgue names it, each swell's position (0 to 127,
  // by its control), each drawstop's state at load, the metronome.
  std::optional<double> volumeChangeDb;
  std::string temperament;
  std::map<Id, int> swells;
  std::map<Id, bool> drawn;
  std::optional<int> metronomeBpm;
  std::optional<int> metronomeBeats;
};

struct GrandOrgueSettingsReport {
  bool ok = false;  // the text is a GrandOrgue settings file
  // Registrations taken, and those beyond what Masterpiece keeps.
  int organPistons = 0;
  int generals = 0, generalsLeftOut = 0;
  int frames = 0, framesLeftOut = 0;
  int divisionals = 0, divisionalsLeftOut = 0;
  int unknownElements = 0;      // entries naming nothing in this organ
  int divisionalCouplers = 0;   // GrandOrgue's divisional couplers, no counterpart
  int crescendoSteps = 0;       // bank A, taken by the player's crescendo
  int crescendoStages = 0;      // left out: banks B to D
  // Voicing.
  int voicedRanks = 0, voicedPipes = 0;
  int voicingClamped = 0;       // beyond Masterpiece's range, taken to its edge
  int delays = 0, releaseTails = 0;  // no counterpart
  // The organ.
  bool volume = false;
  std::string temperament;      // GrandOrgue's name, as read
  int swells = 0, drawn = 0;
  bool metronome = false;
  // Filled in as the settings are applied.
  int routedRanks = 0;
  std::vector<std::string> groupsWithoutOutput;  // audio groups no output is named for
  bool temperamentTaken = false;
};

GrandOrgueSettings readGrandOrgueSettings(const std::string& text, const OrganModel& model,
                                          GrandOrgueSettingsReport& report);

// GrandOrgue's ToneBalance (-100 to 100) as Masterpiece's shelves: a negative
// value is GrandOrgue's low-pass, taken as the treble shelf's cut at 6 kHz; a
// positive one its high-pass, taken as the bass shelf's cut at 100 Hz. Both
// from the one-pole filter GrandOrgue runs (GOSoundToneBalanceFilter).
void toneBalanceShelves(int toneBalance, double& brightnessDb, double& bassDb);

// GrandOrgue's temperament name as one of Masterpiece's (temperamentLibrary),
// "" for the organ's own, or nullopt when Masterpiece has no such temperament.
std::optional<std::string> temperamentFromGrandOrgue(const std::string& goName);

}  // namespace mp
