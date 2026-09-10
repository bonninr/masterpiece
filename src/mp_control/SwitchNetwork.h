// The organ's switch network: what a drawstop actually does.
//
// On a Hauptwerk console the switch a player clicks is rarely the switch
// anything reads. Lemmer's "Pedaal koppel" is switch 1006; every key action
// that cares about it looks at switch 10101, "CouplerNode_1006", one
// SwitchLinkage away. Nancy declares 7555 of these wires. Without following
// them a console coupler moves on screen and couples nothing, a piston lights
// up and registers nothing, and the crescendo is inert.
//
// Switches LATCH and changes PROPAGATE. A switch holds its state until
// something moves it; moving it fires its outgoing wires, which move their
// destinations, and so on until nothing more changes. That is how the console
// behaves and it is not the same as recomputing every switch from its inputs:
//
//   * A general cancel has to push the stop knobs back OUT. It does that by
//     turning the logical stop switches off, and the drawn knobs follow
//     through the reverse wires. A model that derives each switch from its
//     inputs cannot express that — the knob is still drawn, so it would
//     immediately turn the stop back on, and the cancel would do nothing.
//   * Consoles are full of loops by design: the drawn drawstop and the logical
//     switch behind it drive each other, so that moving either moves both.
//     Propagation stops as soon as a switch is already in the state it is
//     being set to, which is what makes those loops terminate.
//
// Each wire says: when the source (and the condition, if there is one) is in
// the state that fires it, do `engageAction` to the destination; otherwise do
// `disengageAction`. Codes 1 and 4 engage, 2 and 7 disengage, which covers
// every wiring real organs use — a plain follow (1/2 or 4/7) and an inverting
// one (7/4, or 1/2 driven from the source's OFF state) — with no special cases.
#pragma once
#include "../mp_core/OrganModel.h"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mp {

class SwitchNetwork {
public:
  // Build from a model. Every switch starts at its declared default and those
  // defaults propagate, so an organ that ships with its blower running or its
  // unison couplers drawn comes up that way.
  void reset(const OrganModel& model);

  // Move a switch, and let the change travel. Everything the move reached is
  // in lastChanges().
  void set(Id switchId, bool engaged);

  bool engaged(Id switchId) const { return engaged_.count(switchId) != 0; }
  const std::unordered_set<Id>& engagedSwitches() const { return engaged_; }

  // Every switch that moved during the last set(), which on a wired console is
  // usually more than the one asked for: a drawstop's noise belongs to the
  // switch that actually moved.
  const std::vector<std::pair<Id, bool>>& lastChanges() const {
    return changes_;
  }

  // How many wires the organ declared. Zero means every switch stands alone,
  // which is what a small or hand-built organ looks like.
  size_t linkageCount() const { return links_.size(); }
  // True when the last change hit the propagation limit. A console cannot
  // reach it; a file whose wiring flip-flops can, and it is recorded rather
  // than ignored so it is visible if one ever does.
  bool lastChangeRanAway() const { return ranAway_; }

private:
  void propagate(Id switchId, bool engaged);
  static bool actionEngages(int code) { return code == 1 || code == 4; }
  static bool actionDisengages(int code) { return code == 2 || code == 7; }
  // What this wire is currently asserting about its destination, given the
  // state of its source and its condition. Returns false when the wire says
  // nothing, which is what an action code we do not model means.
  bool assertion(const SwitchLinkage& l, bool& outEngage) const;

  const OrganModel* model_ = nullptr;
  std::vector<SwitchLinkage> links_;
  // Wires indexed by what makes them re-evaluate: their source, and their
  // condition. Both matter — a condition switch moving changes what a wire is
  // asserting just as surely as its source moving does.
  std::unordered_map<Id, std::vector<const SwitchLinkage*>> bySource_;
  std::unordered_map<Id, std::vector<const SwitchLinkage*>> byCondition_;

  std::unordered_set<Id> engaged_;
  std::vector<std::pair<Id, bool>> changes_;
  std::deque<std::pair<Id, bool>> work_;
  bool ranAway_ = false;
};

} // namespace mp
