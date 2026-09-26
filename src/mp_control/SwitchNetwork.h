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
// Each wire asserts its destination when the source (and the condition, if
// there is one) is in the state that fires it: do `engageAction`; when it stops
// firing, do `disengageAction`. Codes 1 and 4 engage, 2 and 7 disengage, and
// an engage action of 3 toggles the destination on each firing (a reversible
// piston) and does nothing when the wire stops firing. Together these cover
// every wiring real organs use — a plain follow (1/2 or 4/7), a reversible
// (3/7) and an
// inverting one (7/4, or 1/2 driven from the source's OFF state).
//
// A wire that is NOT firing asserts NOTHING. This is the rule that matters for
// organs whose pipework hangs off pallet switches: a pallet carries one wire
// per tremulant state — one conditioned on "tremulant off", one on "tremulant
// on" — and with the tremulant off the first one fires while the second sits
// idle. Driving the idle wire's disengage action anyway slammed the pallet
// shut in the same breath as the key opened it, and the pipe, started and
// released in one instant, sounded as an attack followed by a release tail.
// GrandOrgue's Hauptwerk importer reads the same wiring the same way: a
// destination is the OR of the wires currently firing into it.
//
// Verified on Alessandria, Erfurt Predigerkirche and Swieta Lipka, whose
// pipework is reached only through pallets, and against the switch tests.
//
// So a disengage assertion is applied only when no other wire firing into the
// same destination is asserting engagement. That OR is also what two parallel
// wires into one stop mean on an organ that reaches a switch by two routes.
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
  // Build from a model. Every switch starts at its declared default and every
  // wire firing at that state asserts its destination, so an organ that ships
  // with its blower running or its unison couplers drawn comes up that way.
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
  // Move every queued assertion and re-evaluate the wires it touches, until
  // nothing moves. Shared by set() and reset().
  void drain();
  // Engage action 3 flips the destination each time the wire fires.
  static constexpr int kToggle = 3;
  static bool actionEngages(int code) { return code == 1 || code == 4; }
  static bool actionDisengages(int code) { return code == 2 || code == 7; }
  // Does this wire presently fire: its source, and its condition when it has
  // one, in the states the organ requires.
  bool fires(const SwitchLinkage& l) const;
  // The wire's firing state changed: assert what its action says, subject to
  // the OR rule for a disengage.
  void reevaluate(const SwitchLinkage& l);
  // Is any wire other than `except` currently firing into `dest` and asserting
  // engagement? If so a disengage against `dest` must not be applied.
  bool anotherEngagingWire(Id dest, const SwitchLinkage* except) const;

  const OrganModel* model_ = nullptr;
  std::vector<SwitchLinkage> links_;
  // Wires indexed by what makes them re-evaluate: their source, and their
  // condition. Both matter — a condition switch moving changes whether a wire
  // fires just as surely as its source moving does.
  std::unordered_map<Id, std::vector<const SwitchLinkage*>> bySource_;
  std::unordered_map<Id, std::vector<const SwitchLinkage*>> byCondition_;
  // Wires indexed by destination, for the OR rule above.
  std::unordered_map<Id, std::vector<const SwitchLinkage*>> byDest_;
  // Whether each wire fired at the last evaluation, parallel to links_. A
  // wire asserts its destination on the edges, not on the level.
  std::vector<uint8_t> fired_;

  std::unordered_set<Id> engaged_;
  std::vector<std::pair<Id, bool>> changes_;
  std::deque<std::pair<Id, bool>> work_;
  bool ranAway_ = false;
};

} // namespace mp
