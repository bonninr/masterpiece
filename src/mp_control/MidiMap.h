// MIDI mapping: what a console's controls do to this organ.
//
// A player's hardware is not the organ's. Their drawstop sends note 36 on
// channel 3, their swell shoe sends CC 11, their pistons send program changes;
// none of that is in the ODF, and every player's rig differs. So the mapping
// lives OUTSIDE the organ model, is edited by learning rather than typing, and
// is saved per organ so it survives a reload.
//
// Deliberately JUCE-free so the fast loop tests it: this is lookup and state,
// not audio.
#pragma once
#include "../mp_core/OrganModel.h"
#include "MidiDevices.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

// What a MIDI message is aimed at.
enum class MidiTargetKind {
  None,
  Switch,            // a drawstop, coupler or tremulant: on/off
  ContinuousControl, // a shoe or wheel: 0..127
  Keyboard,          // a manual: notes play through it
  // The registration sequencer. Not in the organ file — the program provides
  // it and the player maps it — so it is a target with no id: the two thumb
  // pistons an organist actually uses.
  StepperNext,
  StepperPrev,

  // Console actions. Also not in the organ file, and for the same reason a
  // real console has thumb pistons that change nothing about the instrument:
  // a player whose hands are on the keys cannot reach for a mouse to turn a
  // page. These carry no id either — there is one of each.
  ConsoleNextPage,
  ConsolePrevPage,
  ConsoleNextLayout,
  ConsoleToggleStopList,
  ConsoleToggleKeyboard,
};

// The kind of message, reduced to what a mapping needs to match on.
enum class MidiSourceKind { None, Note, ControlChange, ProgramChange };

struct MidiSource {
  MidiSourceKind kind = MidiSourceKind::None;
  int channel = 0;  // 1..16; 0 = any channel, which is what most rigs want
  int number = 0;   // note number, CC number, or program number
  // Which physical console it came from; 0 means any. A rig with two manuals
  // plugged in sends the same note on the same channel from both, and without
  // this they cannot be told apart. See MidiDevices.h.
  int deviceId = 0;

  bool operator==(const MidiSource& o) const {
    return kind == o.kind && channel == o.channel && number == o.number &&
           deviceId == o.deviceId;
  }
};

struct MidiSourceHash {
  size_t operator()(const MidiSource& s) const {
    return ((static_cast<size_t>(s.kind) * 131u +
             static_cast<size_t>(s.channel)) *
                257u +
            static_cast<size_t>(s.number)) *
               521u +
           static_cast<size_t>(s.deviceId);
  }
};

// What one message does to a switch. Every physical console does this
// differently — a drawstop that sends one note to go out and another to come
// in, a piston that only ever engages, a rocker that sends on and off on the
// same note — so it belongs to the player's mapping and not to the organ.
//
// The set of behaviours is GrandOrgue's, which documents them properly:
// ON/OFF/ON_OFF variants of note and controller messages. Hauptwerk detects
// most of this for you and offers no equivalent list to copy.
enum class MidiTrigger {
  Toggle,          // one press flips it; the release is ignored
  Momentary,       // held is on, released is off
  EngageOnly,      // this message only ever draws it
  DisengageOnly,   // and this one only ever pushes it back
};

struct MidiBinding {
  MidiSource source;
  MidiTargetKind targetKind = MidiTargetKind::None;
  Id targetId = 0;
  MidiTrigger trigger = MidiTrigger::Toggle;
  // Kept in step with `trigger` for the older text form; Toggle is latching.
  bool latching = true;
  // A shoe wired backwards is common enough to deserve a flag rather than a
  // rewiring.
  bool invert = false;
  // The window of values this binding responds to, and the range it maps onto.
  // A console whose shoe only travels 20..100 still has to reach both ends of
  // the swell, and a button that sends 64 rather than 127 still has to count
  // as pressed. 0..127 is the whole range and the usual case.
  int lowValue = 0;
  int highValue = 127;

  // A source value, normalised to 0..127 across this binding's own window.
  int scale(int raw) const {
    const int lo = std::min(lowValue, highValue);
    const int hi = std::max(lowValue, highValue);
    if (hi <= lo) return raw;
    const int clamped = raw < lo ? lo : (raw > hi ? hi : raw);
    return ((clamped - lo) * 127) / (hi - lo);
  }
  // Whether a raw value counts as "pressed" for a switch. Half-way up its own
  // window, so a console that sends 64 for a press still registers.
  bool isOn(int raw) const {
    const int lo = std::min(lowValue, highValue);
    const int hi = std::max(lowValue, highValue);
    return raw > lo + (hi - lo) / 2;
  }
};

// What a matched message wants done. The engine applies these; the map itself
// never touches audio state, which keeps it testable without one.
struct MidiAction {
  MidiTargetKind kind = MidiTargetKind::None;
  Id targetId = 0;
  bool engage = false;  // for Switch
  int value = 0;        // for ContinuousControl, 0..127
  bool valid() const { return kind != MidiTargetKind::None; }
};

class MidiMap {
public:
  void clear();
  void bind(const MidiBinding& binding);
  void unbind(const MidiSource& source);
  void unbindTarget(MidiTargetKind kind, Id targetId);

  // Look up what a message should do. `value` is velocity for a note, the
  // controller value for a CC. A note-off arrives as value 0.
  MidiAction actionFor(const MidiSource& source, int value) const;

  const std::vector<MidiBinding>& bindings() const { return ordered_; }
  size_t size() const { return ordered_.size(); }
  const MidiBinding* bindingFor(MidiTargetKind kind, Id targetId) const;

  // --- learn -------------------------------------------------------------
  // Arm a target, then feed it the next message that arrives. This is how a
  // player maps a console: touch the thing on screen, then move the control.
  void beginLearn(MidiTargetKind kind, Id targetId, bool latching = true);
  // Learn with an explicit behaviour, which is what the console's right-click
  // menu offers.
  void beginLearnAs(MidiTargetKind kind, Id targetId, MidiTrigger trigger);
  // Every binding aimed at this target, so the console can say what a drawstop
  // is currently mapped to. A stop can have two: one to draw it, one to cancel.
  std::vector<const MidiBinding*> bindingsFor(MidiTargetKind kind,
                                              Id targetId) const;
  void cancelLearn();
  bool learning() const { return learnKind_ != MidiTargetKind::None; }
  MidiTargetKind learningKind() const { return learnKind_; }
  Id learningTarget() const { return learnTarget_; }
  // Returns true when the message completed a binding.
  bool learnFrom(const MidiSource& source);

  // --- persistence -------------------------------------------------------
  // A flat text form, one binding per line. Deliberately not the organ file:
  // the mapping belongs to the player's hardware, and must never be written
  // back into a licensed sample set.
  std::string toText() const;
  bool fromText(const std::string& text);

  // Which keyboard each MIDI channel plays. Not a binding — a binding turns one
  // message into one action, and this decides where a whole channel's keys go —
  // but it belongs in the same file, because it is the same kind of thing: the
  // player's own console wiring, which is theirs and not the organ's.
  // --- keyboards -----------------------------------------------------------
  // What reaches a manual, and how. A channel number alone is nowhere near
  // enough for a real rig, so this follows GrandOrgue's manual receiver, which
  // is the one place any of this is written down.
  //
  // Every field earns its place on a console someone actually owns:
  //
  //   deviceId/channel  Two keyboards both send note 60 on channel 1.
  //   lowKey/highKey    Splitting ONE physical keyboard across two manuals, or
  //                     taking the bottom octave of a 61-note board as a pedal.
  //   transpose         The same split again, moved back into range — and an
  //                     organ whose compass starts somewhere other than the
  //                     keyboard's.
  //   velocity window   A console whose keys bottom out at 100, or one that
  //                     sends a fixed 64. An INVERTED window (low > high)
  //                     flips the sense, which is how a normally-closed
  //                     contact is handled.
  //   ignoreVelocity    Tracker action: the key is down or it is not, and a
  //                     velocity-sensitive sample would be wrong.
  //   shortOctave       Historic keyboards whose bottom octave omits the
  //                     accidentals and puts other notes on those keys.
  //   debounceMs        Old contacts chatter. Without this one key press
  //                     retriggers the pipe several times.
  struct KeyboardBinding {
    int deviceId = 0;  // 0 = any console
    int channel = 0;   // 0 = any channel
    int lowKey = 0, highKey = 127;
    int transpose = 0;
    int lowVelocity = 1, highVelocity = 127;
    bool ignoreVelocity = false;
    bool shortOctave = false;
    int debounceMs = 0;
    Id keyboardId = 0;
  };

  // One key press, after a binding has had its say.
  struct KeyHit {
    Id keyboardId = 0;
    int midiNote = 60;
    int velocity = 64;
    bool on = true;
  };

  // Every manual this key press reaches. More than one is legitimate: a split
  // keyboard sends the same note to two manuals, and so does a coupled rig.
  // Appends to `out` and returns how many it added.
  int matchKeyboards(int deviceId, int channel, int note, int velocity,
                     double timeMs, std::vector<KeyHit>& out) const;

  // True when any manual binding could answer for this console and channel:
  // a binding names a device (or any) and a channel (or any). A note on a
  // channel NO binding claims keeps the organ's own default assignment
  // rather than going silent, so a partially mapped rig still plays every
  // manual — and so do the on-screen keys, which arrive with no device and
  // match no device-specific binding.
  bool hasChannelBinding(int deviceId, int channel) const;

  void addKeyboardBinding(const KeyboardBinding& b);
  void removeKeyboardBindingsFor(Id keyboardId);  // Drop every claim on this channel from a DIFFERENT keyboard, for the same
  // console. Two manuals on one channel make one of them unreachable, and
  // nothing on screen says which, so an assignment takes the channel rather
  // than sharing it. `deviceId` 0 (any console) collides with everything.
  void releaseChannel(int channel, int deviceId, Id keepKeyboardId);
  // Repair a mapping that cannot be what anyone meant, and say how many
  // bindings went. Two things are dropped:
  //   - a binding for a keyboard this organ does not have (a stale file, or
  //     one written for a different set), and
  //   - a binding listed twice over, identical in every field, which only
  //     doubles the work of each note.
  //
  // Manuals sharing a channel are NOT a fault. One keyboard playing two
  // divisions is a coupler a player can build for themselves, and a rig with
  // more manuals than keyboards has no other way to reach them. Up to 0.5.3
  // every binding in such a collision was dropped, which made the mapping
  // impossible to keep.
  int repairKeyboardBindings(const std::vector<Id>& playableKeyboards);
  const std::vector<KeyboardBinding>& keyboardBindings() const {
    return keyboardBindings_;
  }
  void clearKeyboardBindings() { keyboardBindings_.clear(); }
  // True when nothing is mapped, in which case the caller falls back to the
  // organ's own default assignment rather than going silent.
  bool keyboardBindingsEmpty() const { return keyboardBindings_.empty(); }

  // The consoles this map knows about, by name. Owned here because a saved
  // mapping refers to them by name and has to resolve them on load.
  MidiDeviceMap& devices() { return devices_; }
  const MidiDeviceMap& devices() const { return devices_; }

private:
  MidiDeviceMap devices_;
  std::vector<KeyboardBinding> keyboardBindings_;
  // Last time each (binding, note) fired, for debouncing. Mutable because
  // matching is const from the caller's point of view — it asks a question —
  // but debouncing is inherently stateful.
  mutable std::unordered_map<int64_t, double> keyLastMs_;
  std::unordered_map<MidiSource, MidiBinding, MidiSourceHash> bySource_;
  std::vector<MidiBinding> ordered_; // stable order for the UI and for saving
  // Latching switches remember their own state: the console sends "button
  // pressed", not "stop is now on".
  mutable std::unordered_map<Id, bool> latchState_;

  MidiTargetKind learnKind_ = MidiTargetKind::None;
  Id learnTarget_ = 0;
  bool learnLatching_ = true;
  MidiTrigger learnTrigger_ = MidiTrigger::Toggle;

  void rebuildOrdered();
};

} // namespace mp
