// Pistons: recall a registration, capture one, cancel everything.
//
// A combination is a list of switches and the state each should be in. Pressing
// its piston sets them; pressing it with the setter held stores what is
// currently drawn instead. That is the whole mechanism, and it is the one part
// of an organ that is genuinely the player's rather than the builder's — so
// what a player captures is saved beside their MIDI map, in their own data,
// and never written back into the sample set.
//
// How a piston is wired to its combination is the only surprising part.
// Hauptwerk lets a combination name its `ActivatingSwitchID` outright, and
// Nancy does that for its crescendo steps. But an organ with ordinary generals
// usually names nothing: the combination's TYPE code says which piston it is
// (101 is "General 01", 100 is "General cancel"), and the switch that fires it
// is the one whose DefaultInputOutputSwitchAsgnCode matches. Lemmer wires all
// ten of its console pistons that way.
//
// This is JUCE-free and holds no state the audio thread needs, so it can be
// tested against a hand-built model in the fast loop.
#pragma once
#include "../mp_core/OrganModel.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

class CombinationSystem {
public:
  // What a recall does to one switch. The system does not touch switches
  // itself: it hands them back, because setting one has to go through the
  // organ's own wiring, which lives in SwitchNetwork.
  struct Change {
    Id switchId = 0;
    bool engage = false;
  };

  void reset(const OrganModel& model);

  // Which combination this switch fires, or 0. Built once at reset.
  Id combinationForSwitch(Id switchId) const;
  // Every switch that fires a combination, so a console can tell a piston from
  // a drawstop without asking what it does.
  const std::unordered_map<Id, Id>& activatingSwitches() const {
    return bySwitch_;
  }

  // Recall: what this combination wants every switch it controls to be.
  // Respects the combination's own permissions — a cancel may only disengage,
  // and a fixed combination may only engage — so a piston that is allowed to
  // do neither returns nothing rather than silently doing both.
  void recall(Id combinationId, std::vector<Change>& out) const;

  // Capture: store the current state of every switch this combination watches.
  // `isEngaged` reads the live console. Does nothing to a combination the
  // organ marks as fixed, which is what "fixed" means.
  bool capture(Id combinationId, const std::function<bool(Id)>& isEngaged);

  // Setter mode: while this is on, pressing a piston stores instead of
  // recalling. It is a switch on the console like any other; the caller says
  // when it moves.
  void setCaptureMode(bool on) { captureMode_ = on; }
  bool captureMode() const { return captureMode_; }

  size_t combinationCount() const { return stored_.size(); }
  // How many combinations hold at least one engaged switch — the honest
  // measure of whether a player has anything saved.
  int programmedCount() const;

  // Persistence. A flat text form, deliberately not the organ file: what a
  // player captures is theirs, and the sample set is read-only.
  std::string toText() const;
  bool fromText(const std::string& text);

private:
  const OrganModel* model_ = nullptr;
  // Stored state per combination, keyed by controlled switch. Seeded from the
  // organ's own InitialStoredStateIsEngaged and replaced by capture.
  std::unordered_map<Id, std::unordered_map<Id, bool>> stored_;
  std::unordered_map<Id, Id> bySwitch_; // activating switch -> combination
  bool captureMode_ = false;
};

} // namespace mp
