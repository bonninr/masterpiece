// A player's MIDI settings brought over from GrandOrgue (#865).
//
// GrandOrgue keeps them in the organ's settings file (.cmb), an INI text
// whose sections are the .organ file's own: [Manual001], [Stop012],
// [Enclosure001], [General003]. Each lists its events as MIDIEventType001,
// MIDIChannel001, MIDIKey001 and so on, and the messages it sends as
// MIDISendEventType001 and the rest. The GrandOrgue import records which
// object each section became (OrganModel::grandOrgueObjects), so a file
// saved for the same organ finds every stop, coupler, piston, swell and
// manual it names.
//
// The events Masterpiece has a binding for are taken; the others are counted
// and left out, and the report says how many. Bindings for an object the file
// names replace the ones it had; every other object keeps its own.
//
// JUCE-free: the file arrives as text, already unpacked when GrandOrgue
// compressed it.
#pragma once

#include "MidiMap.h"

#include <string>

namespace mp {

struct GrandOrgueMidiReport {
  bool ok = false;           // the text is a GrandOrgue settings file
  std::string organName;     // the organ it was saved for, [Organ] ChurchName
  int bindings = 0;          // events taken for stops, pistons and swells
  int keyboards = 0;         // events taken for manuals
  int sends = 0;             // messages sent, taken
  int leftOut = 0;           // events and sends of a kind Masterpiece does not take
  int otherObjects = 0;      // sections with events this organ does not have
  int shortcuts = 0;         // computer key shortcuts, left out
  int anyDevice = 0;         // devices not connected now, taken as any console
};

GrandOrgueMidiReport importGrandOrgueMidi(const std::string& text, const OrganModel& model, MidiMap& map);

}  // namespace mp
