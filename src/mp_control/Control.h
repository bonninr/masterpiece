// Control graph: couplers/key actions + combinations/stepper/crescendo.
// MIDI event expansion happens pre-voice: note -> list of (division, midiNote, velScale)
// honouring condition switches, melody/bass pick, increment/shift, pizz/reit modes.
// Combinations drive switch states; audio only sees switch->rank-enable + continuous values.
#pragma once
#include "../mp_core/OdfLoader.h"
#include "../mp_core/OrganModel.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <vector>

namespace mp {

struct ExpandedNote { int divisionId = 0; int midiNote = 60; float velScale = 1.0f; };

// One sounding pipe for a key press: stop -> StopRank mapping window ->
// rank pipe by MIDI note (unification falls out: several division notes
// may resolve to the same pipe when the rank is short).
struct ResolvedPipe {
  Id stopId = 0;
  Id rankId = 0;
  Id pipeId = 0;
  int pipeMidiNote = 60;
};

// M1.3 key-on path (no audio): division + key + engaged stop set -> pipes.
// A stop sounds when its stopId is engaged AND it belongs to the division.
// Each StopRankEntry maps [first, first+num) division notes to rank notes
// via midiIncrement; missing rank pipe note = silent (no fallback).
// With `engagedSwitches`, a stop rank whose alternate-rank switch is engaged
// resolves to its alternate rank instead.
std::vector<ResolvedPipe> resolvePipes(const OrganModel& model, int divisionId,
                                       int midiNote,
                                       const std::unordered_set<Id>& engagedStops,
                                       const std::unordered_set<Id>* engagedSwitches = nullptr);

// Key flow: which divisions a key press actually reaches, and at what pitch.
//
// A Hauptwerk organ has no coupler objects. It has KeyAction edges from a
// keyboard to another keyboard or to a division, each optionally gated on a
// switch. A manual's own keys reach its division through an ungated edge; a
// coupler is the same edge gated on a drawstop. So coupling is a graph walk,
// not a matrix, and "Great to Pedal 8" is one row that happens to be gated.
//
// The walk is breadth-first from the source keyboard, carrying the accumulated
// transposition, and terminates at divisions. Keyboards chain (input keyboard
// -> organ keyboard -> division is the ordinary shape) and a badly authored
// file can loop, so the walk is depth-bounded and remembers where it has been.
//
// It runs on the audio thread, on every note-on, so it allocates nothing: the
// caller owns the scratch and it keeps its capacity between notes.
struct KeyFlowScratch {
  std::vector<std::pair<Id, int>> frontier, next, seen;
  std::vector<std::pair<int, int>> emitted;

  // Called once, off the audio thread. The sizes are generous for an organ:
  // five manuals plus pedal, four or five hops of coupling.
  void reserve(size_t n = 64) {
    frontier.reserve(n);
    next.reserve(n);
    seen.reserve(n);
    emitted.reserve(n);
  }
  void clear() {
    frontier.clear();
    next.clear();
    seen.clear();
    emitted.clear();
  }
};

class CouplerMatrix {
public:
  void reset(const OrganModel& model);

  // Every division this key press reaches, from the keyboard it was played on,
  // given which switches are currently engaged. Appends to `out`.
  //
  // A keyboard the organ's key flow never explains — because its manuals are
  // wired to their pipework through the switch graph, which this does not walk
  // yet — gets every division at unison. That is what this did before the
  // graph existed, and an organ that plays too much is a far smaller fault
  // than one that plays nothing.
  //
  // The fallback is decided at load, from what the keyboard reaches with
  // NOTHING engaged, never per note. Deciding it per note would make a unison
  // off — an edge that is live only while its switch is out — silence a
  // manual and then immediately un-silence it by falling back.
  void expandInto(int sourceKeyboardId, int midiNote, float velocity,
                  const std::unordered_set<Id>& engagedSwitches,
                  KeyFlowScratch& scratch, std::vector<ExpandedNote>& out) const;

  // Same, allocating its own scratch and result. For tests and tools, never
  // for the audio thread.
  std::vector<ExpandedNote> expand(
      int sourceKeyboardId, int midiNote, float velocity,
      const std::unordered_set<Id>& engagedSwitches) const;

  // Keyboards a player can play ON: those that drive the key-flow graph but
  // are never driven by it. Everything downstream is the organ's own internal
  // wiring. Sorted by keyboard id, so the order is stable across loads.
  const std::vector<Id>& inputKeyboards() const { return inputKeyboards_; }
  // The division a keyboard is really for, used to name it and to order the
  // manuals. 0 when the graph does not reach one.
  Id primaryDivisionFor(Id keyboardId) const;
  // Hauptwerk's default MIDI assignment code for a playable keyboard (1 =
  // pedal, 2 = first manual, ...). The drawn keyboards a player touches do not
  // carry it; the organ's internal keyboards they feed do, so this follows the
  // ungated key flow until it finds one. 0 when nothing declares it.
  int assignmentCodeFor(Id keyboardId) const;
  // The keyboard a player plays that this one stands for. A console draws
  // its manuals as keyboards of their own, joined to the playable ones by
  // plain unison key flow, and it is the playable one the player assigns to
  // a MIDI channel. A playable keyboard is its own; 0 when none is joined.
  Id inputKeyboardFor(Id keyboardId) const;
  // A display name for a playable keyboard, taken from the division it
  // reaches rather than from its own name, which is a layout label
  // ("CustPg1_InputKbd_DivCode2") and no use to a player.
  std::string keyboardName(Id keyboardId) const;

  // True when the organ declared no usable key flow and expand() is therefore
  // sounding every division. Worth surfacing: it means couplers do nothing.
  bool usingFallback() const { return edges_.empty(); }

  void setMasterCoupler(int toDiv, int fromDiv, int pitchKind, bool engaged);

private:
  bool conditionMet(const KeyAction& a,
                    const std::unordered_set<Id>& engaged) const;
  // The walk itself, with no fallback. Used to decide which keyboards need one.
  void walk(int sourceKeyboardId, int midiNote, float velocity,
            const std::unordered_set<Id>& engagedSwitches,
            KeyFlowScratch& scratch, std::vector<ExpandedNote>& out) const;

  const OrganModel* model_ = nullptr;
  // Key-flow edges indexed by source keyboard, so a note-on never scans the
  // whole table: a large organ declares thousands of them.
  std::unordered_map<Id, std::vector<const KeyAction*>> edges_;
  std::vector<Id> inputKeyboards_;
  std::unordered_map<Id, Id> primaryDivision_;
  std::unordered_map<Id, int> assignmentCode_;
  std::unordered_map<Id, Id> inputFor_;
  // The division each keyboard sounds. Usually the organ's own hint; for the
  // internal coupling keyboards, inherited from whatever feeds them.
  std::unordered_map<Id, Id> divisionOfKeyboard_;
  // Keyboards whose key flow reaches no division at all, and which therefore
  // sound every division rather than nothing.
  std::unordered_set<Id> needsFallback_;
  bool master_[8][8][6] = {};
};

// Which keyboard the on-screen fallback piano (and an unmapped MIDI channel)
// should play when nobody has chosen one.
//
// When the organ declares a compass, this is the widest one — a manual, never
// the pedalboard. Plenty of sets declare none at all (Nancy leaves
// KeyGen_NumberOfKeys empty on every keyboard), and the widest-of-nothing
// tie-break lands on the pedal, so the piano plays the one division the
// player almost certainly did not draw stops for. In that case it is the
// manual whose division ships the most playable pipework, preferring a manual
// over the pedal and an unenclosed division over an enclosed one — a shut
// swell shoe must never be able to silence the default.
Id defaultKeyboard(const OrganModel& model, const CouplerMatrix& flow);

// True when the organ draws at least one manual key-by-key from a KeyImageSet.
// A set whose manuals are part of a photographed backdrop (Nancy) draws none,
// and then the fallback piano is the only thing playable with the mouse.
bool hasDrawnManuals(const OrganModel& model);

// M2.4 runtime: current position of every continuous control (swell shoes,
// crescendo wheels, generic assignable controls), plus the linkages that let
// one drive another.
//
// Values are held in the ODF's own 0..127 domain because that is what the file
// and MIDI both speak; consumers that need a normalised position ask for it.
// Linkages are resolved by repeated relaxation rather than a topological sort:
// the loader already rejects cycles (continuous-control-feedback-loop), so a
// bounded number of passes settles the graph, and a file that slipped a cycle
// past the validator degrades to a stale value instead of hanging the engine.
class ContinuousControlBank {
public:
  void reset(const OrganModel& model);

  // Set a control from MIDI or the UI. Out-of-range input is clamped to the
  // control's declared range, never wrapped.
  void setValue(Id controlId, int value);
  int value(Id controlId) const;
  // Position within the control's own min..max, as 0..1. Returns 0 for an
  // unknown control. Honours the ODF's inverted flag, so a reversed shoe
  // reads 1.0 when it is open, like every other shoe.
  double normalised(Id controlId) const;

  // Recompute every control fed by a linkage. Called after setValue()s, once
  // per control block — never per sample.
  //
  // `pinned` is the control the player just moved, and nothing may overwrite
  // it. Consoles wire a shoe and its internal twin to follow EACH OTHER, so
  // that either can be moved — Nancy's crescendo pedal and its "extension" are
  // such a pair. Without a pin the twin's old value wins on the very first
  // pass and the shoe snaps straight back to where it was, which looks exactly
  // like a crescendo that was never wired up.
  // `engagedSwitches` decides which linkages are live. A linkage that names a
  // ConditionSwitchID is NOT a permanent wire: it is what a preset Load or a
  // "reset to defaults" button is made of, and it must fire only while its
  // switch is engaged. Running them unconditionally holds every control the
  // organ offers a preset for at its stored value, so dragging one snaps
  // straight back and the panel looks broken rather than wrongly wired.
  //
  // Passing nothing means "no switch is engaged", which is the safe reading:
  // an unconditional wire still runs, a conditional one waits to be asked.
  void propagate(Id pinned = 0,
                 const std::unordered_set<Id>* engagedSwitches = nullptr);

  // Convenience for the audio graph: shutter position 0..1 for an enclosure,
  // via whichever continuous control drives it. An enclosure with no control
  // is fully open, which is the safe reading — a silent organ is worse than an
  // unexpressive one.
  double shutterFor(const Enclosure& e) const;

  size_t size() const { return values_.size(); }

private:
  const OrganModel* model_ = nullptr;
  std::unordered_map<Id, int> values_;

  int clampToRange(const ContinuousControl& c, int v) const;
};

} // namespace mp
