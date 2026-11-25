// The organ's network: everything between a key or a drawstop and a pipe.
//
// The processor asks one object for the state of every switch and continuous
// control, moves them, and lets time pass. Two implementations stand behind
// this interface, chosen at load:
//
//   * LegacyNetwork: the SwitchNetwork and ContinuousControlBank the engine
//     has always used, with every special case the organs at hand needed.
//     The default, and unchanged.
//   * EventNetwork: Hauptwerk's object model taken literally, event-driven on
//     a clock of its own. Switched on by the "useEventNetwork" setting or the
//     MP_EVENT_NETWORK=1 environment variable, so both can be played and
//     traced side by side (see TraceSimulator).
//
// The switch and control calls mirror what the processor already asked of
// SwitchNetwork and ContinuousControlBank, so wrapping the old ones changes
// nothing. The key calls and the listener are what the trace simulator and
// the event network add: the processor still walks its own key paths.
#pragma once
#include "../mp_core/OrganModel.h"

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mp {

// What a network reports as it moves, with the time on its own clock in
// seconds. Every call is made from inside the network's own calls, on the
// thread that made them; an implementation must not allocate to make them.
class NetworkListener {
public:
  virtual ~NetworkListener() = default;
  virtual void switchMoved(Id /*switchId*/, bool /*engaged*/, double /*time*/) {}
  virtual void controlMoved(Id /*controlId*/, int /*value*/, double /*time*/) {}
  // A division's note is played (by key flow or a division input) or let go.
  virtual void divisionNote(Id /*divisionId*/, int /*midiNote*/, bool /*on*/, double /*time*/) {}
  // A pipe starts or stops speaking: through a drawn stop and a played
  // division note, or through its pallet switch.
  virtual void pipeMoved(Id /*rankId*/, Id /*pipeId*/, bool /*on*/, double /*time*/, const Pipe* /*pipe*/) {}
};

// General rules on top of the literal object model: behaviour sample sets by
// well-known publishers rely on and Hauptwerk evidently has, written as rules
// for every organ, never keyed by an organ's id or name. Each can be switched
// off on its own for A/B (MP_NETWORK_RULES="-startPulse,-timedStart").
struct NetworkRules {
  // LoadSettle: at load every wire is judged once against the switches'
  // defaults. A firing wire asserts its engage action; one that does not fire
  // asserts its disengage action only where that engages (an inverting 7/4
  // wire holds a unison-off's destination on). Off: nothing is asserted
  // until a switch moves.
  bool loadSettle = true;
  // StartPulse: once loaded, the organ's start signal fires. Every control
  // that no linkage drives and no image draws moves: to its top when stage
  // switches hang on it (Nancy's __DelayBlower, Alessandria's blower init,
  // #1183), and from one end of its range to the other when it feeds a timed
  // linkage or a staged control (the "Hidden OffToOn"/"OnToOff" pair of
  // Nancy, Friesach and Oloron, #263). Replaces the legacy name match.
  bool startPulse = true;
  // TimedStart: the start pulse travels through timed linkages in time, as
  // at power-on, rather than settling at once. Ramps then land in the order
  // of their own times, which the legacy engine imitates by sorting the
  // staged controls by ramp time (#1183).
  bool timedStart = true;

  // All on, less those MP_NETWORK_RULES turns off.
  static NetworkRules fromEnvironment();
};

class OrganNetwork {
public:
  enum class Kind { Legacy, Event };
  virtual ~OrganNetwork() = default;
  virtual Kind kind() const = 0;

  // ---- switches ----
  // Build from a model: every switch at its declared default, and the wires
  // that fire at that state asserting what they assert.
  virtual void resetSwitches(const OrganModel& model) = 0;
  // Move a switch and let the change travel; everything it reached is in
  // lastChanges(), in the order it moved.
  virtual void set(Id switchId, bool engaged) = 0;
  // Switches nothing may engage (a demo's locked stops).
  virtual void holdOff(std::unordered_set<Id> switches) = 0;
  virtual bool engaged(Id switchId) const = 0;
  virtual const std::unordered_set<Id>& engagedSwitches() const = 0;
  virtual const std::vector<std::pair<Id, bool>>& lastChanges() const = 0;
  virtual size_t linkageCount() const = 0;
  virtual bool lastChangeRanAway() const = 0;

  // ---- continuous controls ----
  // Every control at its default. Timed linkages into `waitForTime` wait for
  // setTimed(true) (ContinuousControlBank::reset).
  virtual void resetControls(const OrganModel& model, const std::unordered_set<Id>* waitForTime) = 0;
  virtual void setValue(Id controlId, int value) = 0;
  virtual void setExact(Id controlId, double value, double wakeStep = 1.0 / 8.0) = 0;
  virtual int value(Id controlId) const = 0;
  virtual double readingOf(Id controlId) const = 0;
  virtual double normalised(Id controlId) const = 0;
  virtual double shutterFor(const Enclosure& e) const = 0;
  // Carry the controls along their linkages. `engagedSwitches` is the state
  // the conditions read; the event network reads its own and ignores it.
  virtual void propagate(Id pinned, const std::unordered_set<Id>* engagedSwitches) = 0;
  virtual void setTimed(bool on) = 0;
  virtual bool timed() const = 0;
  // Let `seconds` pass on the network's clock.
  virtual void advance(double seconds) = 0;
  virtual bool moving() const = 0;

  // ---- stage switches ----
  // Whether the network moves the switches its continuous controls' stages
  // name. Off inside the processor, which fires them itself
  // (fireMovedStages); on in the trace simulator.
  virtual void setDrivesStages(bool on) = 0;

  // ---- keys ----
  // A key of a keyboard goes down or comes up. The processor walks its own
  // key paths and does not call these yet; the trace simulator does.
  virtual void keyDown(Id keyboardId, int midiNote, int velocity) = 0;
  virtual void keyUp(Id keyboardId, int midiNote) = 0;
  // Every key up at once (a panic).
  virtual void releaseAllKeys() = 0;
  // Report every pipe speaking now as starting: for a listener that was not
  // ready to start voices when they began (the organ loading).
  virtual void replaySounding() {}

  // ---- start-up ----
  // The rules for the next reset (NetworkRules); the legacy network keeps
  // its own behaviour and ignores them.
  virtual void setRules(const NetworkRules& rules) { (void)rules; }
  // The organ is loaded: fire its start signal (NetworkRules::startPulse).
  // The legacy network's start-up is the processor's own and runs there.
  virtual void startUp() {}

  // ---- time and reporting ----
  virtual double now() const = 0;
  void setListener(NetworkListener* l) { listener_ = l; }
  NetworkListener* listener() const { return listener_; }

protected:
  NetworkListener* listener_ = nullptr;
};

std::unique_ptr<OrganNetwork> makeOrganNetwork(OrganNetwork::Kind kind);
// MP_EVENT_NETWORK=1 in the environment: for tests and the command line.
bool eventNetworkFromEnvironment();
const char* organNetworkName(OrganNetwork::Kind kind);

} // namespace mp
