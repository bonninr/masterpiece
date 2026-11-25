// Hauptwerk's object model, taken literally and driven by events.
//
// Every change is an event on a time-ordered queue, on a clock of the
// network's own: a switch moving, a linkage re-judged, a timed control taking
// one more step. Events at the same instant run in the order they were made,
// so a change travels breadth-first. The queue, the state and the wiring are
// flat arrays built at load; nothing is allocated while the organ plays,
// beyond the engaged-switch set the processor's interface still hands out.
//
// The chain, as the definition states it and with none of the special cases
// LegacyNetwork has gathered:
//
//   key -> keyboard (its key switch engages while the key is down, played or
//   coupled) -> KeyAction (keyboard or division, gated by its condition) ->
//   division note -> drawn stop -> StopRank window -> rank pipe.
//   A DivisionInput plays its division's note while its switch is engaged.
//   A pipe with a ControllingPalletSwitchID speaks while that switch is
//   engaged, and only then.
//
//   SwitchLinkage: a wire fires while its source (and its condition, if any)
//   is in the state it names; a condition switch moving re-judges it unless
//   the wire says ReevaluateIfCondSwitchChangesState=N. On the edge where it
//   starts firing it applies
//   its engage action, on the edge where it stops its disengage action:
//   1 and 4 engage, 2 and 7 disengage, 3 toggles, anything else does nothing.
//   (NetworkRules::code7Momentary: a wire stopping with a 7 leaves a latching
//   destination alone.)
//   No OR of parallel wires, no mirrors, no momentary-to-latching exception:
//   the last action applied wins.
//
//   ContinuousControlLinkage: while its condition holds, a change of its
//   source sets its destination to (source' + increment) * coefficient.
//   Type 1 at once; type 2 (Willis) ramps the destination evenly, one step
//   per event, the whole 127-step range in willisMsPerStep milliseconds;
//   type 3 (inertia) moves it
//   as a damped mass, 5 ms events of 1 ms sub-steps. Double linkages add,
//   subtract or multiply, at once.
//
//   ContinuousControlStageSwitch: a control crossing a threshold engages or
//   disengages its switch, as the row says for the direction of travel;
//   engage wins when a row says both.
//
// Not yet, against the engine as it plays with LegacyNetwork (stage 1):
//   * The processor still walks its own key paths (CouplerMatrix, coupled
//     and direct inputs, switch-fed keyboards, program and bass/melody
//     couplers) and fires stage switches itself, with either network. The
//     key flow and stages here run in the trace simulator only.
//   * SwitchNetwork's special cases are left out on purpose: the OR of
//     parallel wires, mirrors, condition wires judged after the move settles
//     (#546), a closing gate keeping 1/7 (#263), 2/7 leaving its destination
//     (#546), a momentary button's 1/7 release (#1146).
//   * Codes 4 and 7 act as 1 and 2; whatever else Hauptwerk means by them is
//     not modelled.
//   * ContinuousControlBank's pinned twins and change-only linkages into
//     drawn controls. Willis is the same even ramp, integrated per block
//     there, one event per step here.
//   * Key actions: ActionTypeCode 1 from a keyboard only; no pizzicato,
//     reiteration, bass/melody, division sources, effect codes, and no
//     every-division fallback for a keyboard with no key actions.
//   * Pipes: no action-noise rows, no release or percussive handling; a pipe
//     with a pallet is played by its pallet only (the engine plays a rank its
//     stops reach through the stops and ignores the pallet).
//   * engagedSwitches() and lastChanges() still allocate, for the
//     processor's interface; the sounding pipes are re-derived in O(pipes)
//     per instant that touched them.
#pragma once
#include "OrganNetwork.h"

#include <cstdint>
#include <unordered_map>

namespace mp {

class EventNetwork final : public OrganNetwork {
public:
  EventNetwork();
  Kind kind() const override { return Kind::Event; }

  void resetSwitches(const OrganModel& model) override;
  void set(Id switchId, bool engaged) override;
  void holdOff(std::unordered_set<Id> switches) override;
  bool engaged(Id switchId) const override;
  const std::unordered_set<Id>& engagedSwitches() const override { return engagedSet_; }
  const std::vector<std::pair<Id, bool>>& lastChanges() const override { return changes_; }
  size_t linkageCount() const override { return wires_.size(); }
  bool lastChangeRanAway() const override { return ranAway_; }

  void resetControls(const OrganModel& model, const std::unordered_set<Id>* waitForTime) override;
  void setValue(Id controlId, int value) override;
  void setExact(Id controlId, double value, double wakeStep) override;
  int value(Id controlId) const override;
  double readingOf(Id controlId) const override;
  double normalised(Id controlId) const override;
  double shutterFor(const Enclosure& e) const override;
  void propagate(Id pinned, const std::unordered_set<Id>* engagedSwitches) override;
  void setTimed(bool on) override {
    timed_ = on;
    inertiaTimed_ = on;
  }
  bool timed() const override { return timed_; }
  void advance(double seconds) override;
  bool moving() const override { return movingCount_ > 0; }

  void setDrivesStages(bool on) override { drivesStages_ = on; }
  void setRules(const NetworkRules& rules) override { rules_ = rules; }
  void startUp() override;
  const NetworkRules& rules() const { return rules_; }

  void keyDown(Id keyboardId, int midiNote, int velocity) override;
  void keyUp(Id keyboardId, int midiNote) override;
  void releaseAllKeys() override;
  void setProgramCouplers(const std::vector<ProgramCouplerSpec>& couplers) override;
  void replaySounding() override;

  double now() const override { return now_; }

  // Events dropped because the queue was full since the last reset. A
  // console cannot fill it; a runaway definition can.
  size_t droppedEvents() const { return dropped_; }
  size_t queueCapacity() const { return queueCap_; }
  // Pipes speaking now: for tests.
  size_t soundingPipes() const;

private:
  // ---- the queue ----
  enum class Ev : uint8_t { SetSwitch, ToggleSwitch, EvalLink, EvalDouble, WillisStep, InertiaTick, LandLink };
  struct Event {
    double time = 0.0;
    uint64_t seq = 0;
    Ev kind = Ev::SetSwitch;
    int32_t target = -1;
    int32_t value = 0;
    // Within one instant: 0 for every ordinary event; a timed linkage landing
    // at load carries its ramp time (NetworkRules::rampOrder).
    double order = 0.0;
  };
  struct Later {
    bool operator()(const Event& a, const Event& b) const {
      if (a.time != b.time) return a.time > b.time;
      if (a.order != b.order) return a.order > b.order;
      return a.seq > b.seq;
    }
  };
  void push(double time, Ev kind, int32_t target, int32_t value = 0, double order = 0.0);
  // Run every event due at or before `until`, then settle the keys and pipes.
  void run(double until);
  void handle(const Event& e);

  // ---- switches ----
  int32_t switchSlot(Id id) const;
  void applySwitch(int32_t s, bool on);
  bool fires(int32_t w) const;
  void judgeWire(int32_t w);
  static bool engages(int code) { return code == 1 || code == 4; }
  static bool disengages(int code) { return code == 2 || code == 7; }
  struct Wire {
    int32_t src = -1, dst = -1, cond = -1;
    bool srcWhen = true, condWhen = true;
    uint8_t engage = 1, disengage = 2;
    bool dstLatching = true;
  };

  // ---- controls ----
  int32_t controlSlot(Id id) const;
  // Write a reading to a control; wakes what reads it when it moved.
  void writeControl(int32_t c, double reading, double wakeStep = 0.0);
  void evalLink(int32_t l);
  void evalDouble(int32_t d);
  void willisStep(int32_t l);
  void inertiaTick(int32_t l);
  bool linkLive(int32_t l) const;
  void fireStageRows(int32_t c, int from, int to);
  struct Link {
    int32_t src = -1, dst = -1, cond = -1;
    bool condWhen = true;
    bool invert = false;
    double increment = 0.0, coefficient = 1.0;
    int type = 1;
    double msPerStep = 0.0, accel = 0.0, damping = 0.0;
    bool waitsForTime = false;
    // Motion of a timed linkage.
    double pos = 0.0, vel = 0.0, target = 0.0;
    bool scheduled = false;
    bool isTimed() const { return (type == 2 && msPerStep > 0.0) || (type == 3 && accel > 0.0); }
  };
  struct Double {
    int32_t a = -1, b = -1, dst = -1;
    int op = 0;
    double ac = 1, ai = 0, bc = 1, bi = 0, dc = 1, di = 0;
  };
  struct StageRow {
    int value = 0;
    int32_t sw = -1;
    bool engInc = false, engDec = false, disInc = false, disDec = false;
  };

  // ---- keys, divisions, pipes ----
  void settleSounding();
  // `extreme`: kLowest / kHighest when the key played is the lowest or the
  // highest held on its keyboard (bass and melody couplers).
  static constexpr int kLowest = 1, kHighest = 2;
  void walkKey(int32_t kb, int note, int depth, int extreme);
  struct Action {
    int32_t destKb = -1, destDiv = -1, cond = -1;
    bool condWhen = true;
    int first = 0, num = 0, inc = 0;
    int type = 1;  // ActionTypeCode: 1 every key, 2 the lowest held, 3 the highest
  };
  struct StopRankRef {
    int32_t stopSwitch = -1, rank = -1, altRank = -1, altSwitch = -1;
    int first = 0, num = 0, inc = 0;
  };
  // Compressed adjacency: offsets per owner into one flat list.
  struct Csr {
    std::vector<int32_t> start, items;
    void build(size_t owners, const std::vector<std::pair<int32_t, int32_t>>& pairs);
    const int32_t* begin(int32_t o) const { return items.data() + start[static_cast<size_t>(o)]; }
    const int32_t* end(int32_t o) const { return items.data() + start[static_cast<size_t>(o) + 1]; }
  };

  const OrganModel* model_ = nullptr;
  double now_ = 0.0;
  uint64_t seq_ = 0;
  std::vector<Event> queue_;
  size_t queueCap_ = 0;
  size_t dropped_ = 0;
  bool ranAway_ = false;
  // Willis ramps in time (from the load, NetworkRules::timedStart), and
  // inertia linkages in time (once the organ is live).
  bool timed_ = false;
  bool inertiaTimed_ = false;
  bool drivesStages_ = false;
  NetworkRules rules_;
  bool recording_ = false;  // a set() or holdOff() is filling changes_
  bool soundDirty_ = false;
  int movingCount_ = 0;

  // Switches.
  std::unordered_map<Id, int32_t> switchSlot_;
  std::vector<Id> switchId_;
  std::vector<uint8_t> on_, heldOff_;
  std::vector<int32_t> groupOf_;
  std::vector<std::vector<int32_t>> groups_;
  std::vector<Wire> wires_;
  std::vector<uint8_t> fired_;
  Csr wiresBySource_, wiresByCondition_;
  // What else a switch moves: control linkages it gates, division inputs it
  // plays, whether a stop, pallet or key action reads it.
  Csr linksByCondition_, inputsBySwitch_;
  std::vector<uint8_t> soundSwitch_;
  std::unordered_set<Id> engagedSet_;
  std::vector<std::pair<Id, bool>> changes_;

  // Controls.
  std::unordered_map<Id, int32_t> controlSlot_;
  std::vector<Id> controlId_;
  std::vector<int> lo_, hi_, val_;
  std::vector<double> reading_, woken_;
  std::vector<uint8_t> inverted_;
  std::vector<Link> links_;
  std::vector<Double> doubles_;
  Csr linksBySource_, doublesByControl_;
  std::vector<StageRow> stageRows_;
  Csr stageRowsByControl_;

  // Keys, divisions, pipes. Notes are 0..127 per keyboard and per division.
  std::unordered_map<Id, int32_t> keyboardSlot_, divisionSlot_, rankSlot_;
  std::vector<Id> divisionId_;
  std::vector<Action> actions_;
  Csr actionsByKeyboard_;
  struct ProgramCoupler {
    int32_t sw = -1, srcDiv = -1;
    int shift = 0, extreme = 0;
  };
  std::vector<ProgramCoupler> programCouplers_;
  Csr programByKeyboard_;
  std::vector<int32_t> keySwitch_;            // keyboard * 128 + note -> switch, -1
  std::vector<uint8_t> keyHeld_;              // played keys
  std::vector<uint16_t> keyDown_, keyDownNext_;   // played or coupled, per keyboard note
  std::vector<uint16_t> divInput_;            // division inputs engaged, per division note
  std::vector<uint16_t> divNote_, divNoteNext_;
  std::vector<uint16_t> divKeyed_, divKeyedNext_;  // reached by key actions, per division note
  Csr inputSwitchesByNote_;                       // division note -> its input switches
  std::vector<std::pair<int32_t, int>> inputs_;  // division input -> division slot, note
  std::vector<StopRankRef> stopRanks_;
  Csr stopRanksByDivision_;
  std::vector<int32_t> rankPipeAt_;           // rank * 128 + note -> pipe, -1
  std::vector<Id> pipeRank_, pipeId_;
  std::vector<const Pipe*> pipePtr_;
  std::vector<uint8_t> pipeHasPallet_;
  std::vector<uint16_t> pipeOn_, pipeNext_;
  std::vector<int32_t> onList_, nextList_;    // pipes speaking; reached this settle
  std::vector<int32_t> palletSwitches_;       // switches that are some pipe's pallet
  Csr pipesByPallet_;                         // indexed by switch slot
};

} // namespace mp
