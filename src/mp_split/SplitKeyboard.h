// One keyboard playing a whole organ (discussion #358).
//
// The split sits between the MIDI inputs and the organ. It takes the notes of
// one keyboard and sends them on as notes of its own input, "Split keyboard",
// one MIDI channel per role: Lower, Upper, Solo and Pedal. The player's MIDI
// map sends each of those channels to a manual, as it does any keyboard's, so
// nothing here knows about the organ.
//
// What a layout does:
// - Zones: up to three split points, each zone sent to a role, with a second
//   role layered on it, an octave shift and a transposition. A global
//   transposition on top.
// - A split that follows the hands: with keys held more than an octave apart,
//   the gap between the hands moves the first split point, and it stays there
//   a few seconds after the keys are released.
// - Bass from the hands: the lowest key held from the bottom of the keyboard
//   up to a chosen key plays the pedal, one note at a time. The key sounds on
//   its manual too, or only in the pedal. The pedal note can hold after the
//   hand lifts, until the next bass note or for a set time.
//
// Presets: nine slots shared by every organ. The first three are the factory
// layouts and cannot be changed; the others are the player's own. Recalling a
// preset copies it into the organ's own layout.
//
// The split's own controls -- on and off, next and previous preset, presets 1
// to 9 -- are learned here, from any input, and kept with the split.
//
// Threads. Everything under "audio thread" runs inside the audio callback and
// neither allocates nor locks. The message thread hands layouts and presets
// over through mailboxes, and reads back what the audio thread did through a
// snapshot. JUCE-free.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace mp::split {

enum class Role : uint8_t { None = 0, Lower, Upper, Solo, Pedal };
constexpr int kRoleCount = 4;
// The channels of the split's input, one per role. High channels, so that a
// recording of what the split sent, replayed without its input's name, is
// still known as the split's: a keyboard or a MIDI file seldom uses them.
constexpr int kFirstRoleChannel = 11;
constexpr int channelOf(Role r) { return r == Role::None ? 0 : kFirstRoleChannel - 1 + static_cast<int>(r); }
constexpr Role roleOfChannel(int channel) {
  return channel >= kFirstRoleChannel && channel < kFirstRoleChannel + kRoleCount
             ? static_cast<Role>(channel - kFirstRoleChannel + 1)
             : Role::None;
}

struct Zone {
  Role role = Role::None;
  Role layer = Role::None;  // a second role on the same keys
  int octave = 0;           // -3..3
  int transpose = 0;        // semitones, -12..12
  bool operator==(const Zone&) const = default;
};

enum class Sustain : uint8_t { Off = 0, UntilNext, Timed };

struct Bass {
  bool on = false;
  int topKey = 59;  // the bass is taken from the bottom of the keyboard up to this key
  bool alsoOnManual = true;
  Sustain sustain = Sustain::Off;
  int sustainMs = 1500;
  bool operator==(const Bass&) const = default;
};

constexpr int kMaxSplits = 3;
constexpr int kMaxZones = kMaxSplits + 1;

struct Layout {
  int splitCount = 1;
  // Ascending. Split i is the first key of zone i + 1.
  int splits[kMaxSplits] = {60, 72, 84};
  Zone zones[kMaxZones];
  int transpose = 0;  // all zones and the bass, -12..12
  bool followHands = false;
  Bass bass;
  bool operator==(const Layout&) const = default;

  // The zone a key falls in, with the first split point at `firstSplit`.
  int zoneOf(int note, int firstSplit) const;
  // Within range and in order, whatever was read from a file.
  void normalize();
};

static_assert(std::is_trivially_copyable_v<Layout>);

constexpr int kPresetCount = 9;
constexpr int kFactoryPresets = 3;
// Factory layouts: 0 "Two manuals and a pedal zone", 1 "Two manuals and bass
// from the left hand", 2 "Two manuals".
Layout factoryLayout(int index);
const char* factoryName(int index);

// One learned message.
struct Trigger {
  enum Kind : uint8_t { None = 0, Note, Controller, Program };
  Kind kind = None;
  uint8_t channel = 0;  // 1..16
  uint8_t number = 0;
  int device = 0;  // 0: any input
  bool operator==(const Trigger&) const = default;
};

enum class Control : uint8_t { Toggle = 0, Next, Previous, Preset1 };
constexpr int kControlCount = 3 + kPresetCount;  // Toggle, Next, Previous, Preset1..Preset9

// Hands a value from one thread to the other, newest wins. Three slots: the
// writer fills one, the reader holds one, the third is the latest finished.
template <typename T>
class Mailbox {
  static_assert(std::is_trivially_copyable_v<T>);

public:
  // Writer's thread.
  void put(const T& value) {
    slots_[back_] = value;
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex;
  }
  // Reader's thread. True and `out` set if something newer arrived.
  bool take(T& out) {
    if (!(middle_.load(std::memory_order_acquire) & kFresh)) return false;
    front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
    out = slots_[front_];
    return true;
  }

private:
  static constexpr int kFresh = 4, kIndex = 3;
  T slots_[3]{};
  int back_ = 0, front_ = 1;
  std::atomic<int> middle_{2};
};

// What the message thread sees of the split.
struct State {
  bool enabled = false;
  Layout layout;
  int preset = -1;        // the preset last recalled and not changed since, or -1
  int firstSplit = 60;    // where the first split point is now, hands followed
  int pedalNote = -1;     // the note the bass sends to the pedal, or -1
  uint64_t held[2] = {};  // source keys down
  // Per key, the zone it went to when pressed, +1; 0 when not held.
  uint8_t zoneOfKey[128] = {};
  Trigger triggers[kControlCount];
  uint32_t layoutChanges = 0;   // counts changes made on the audio thread
  uint32_t triggerChanges = 0;  // counts learned controls
  int learning = -1;            // the control being learned, or -1
};

class SplitKeyboard {
public:
  struct Msg {
    uint8_t status = 0, data1 = 0, data2 = 0;
  };
  static constexpr int kOutCapacity = 2048;

  SplitKeyboard();

  // --- message thread ---------------------------------------------------
  void setEnabled(bool on) { enabledBox_.put(on ? 1 : 0); }
  // The organ's layout, and the preset it was recalled from (-1: none).
  void setLayout(const Layout& l, int preset = -1) { layoutBox_.put({l, preset}); }
  // The nine presets; slot i < kFactoryPresets is always the factory layout.
  struct Presets {
    Layout layouts[kPresetCount];
    bool used[kPresetCount] = {};
  };
  void setPresets(const Presets& p) { presetsBox_.put(p); }
  void setTriggers(const std::array<Trigger, kControlCount>& t) { triggersBox_.put(t); }
  // Everything the split holds let go: the Panic button.
  void panic() { panicBox_.put(1); }
  // The next press on any input is learned for this control (-1: stop).
  void learn(int control) { learnBox_.put(control); }
  // Which input the split reads (0: any input but its own), and its own.
  void setDevices(int source, int own) { devicesBox_.put({source, own}); }
  // The latest state the audio thread published.
  bool state(State& out) { return stateBox_.take(out); }

  // --- audio thread -----------------------------------------------------
  // Takes the mailboxes' news and the time, in milliseconds. Call once per
  // block, before feed().
  void beginBlock(double nowMs);
  // One incoming message. True if the split took it: then output() holds what
  // to send in its place, from the split's own input. False: pass it on as it
  // is (output() may still hold note-offs to send).
  bool feed(int device, const uint8_t* data, int size);
  // Timed work: pedal sustain running out, the followed split returning.
  // Call once per block, after the messages.
  void endBlock();
  const Msg* output(int& count) const {
    count = outCount_;
    return out_;
  }
  void clearOutput() { outCount_ = 0; }

  // For tests and the glue alike.
  bool enabled() const { return enabled_; }
  const Layout& layout() const { return layout_; }
  int ownDevice() const { return ownDevice_; }

private:
  struct Sent {
    Role role[2] = {Role::None, Role::None};
    uint8_t note[2] = {0, 0};
    bool silentOnManual = false;  // the bass key, playing only the pedal
  };

  void applyLayout(const Layout& l, int preset, bool fromAudio);
  void setOn(bool on);
  void resoundHeld();
  void releaseAll();
  void forgetKeys();
  void keyOn(int note, int velocity);
  void keyOff(int note);
  void sendOn(Role r, int note, int velocity);
  void sendOff(Role r, int note);
  void updateBass();
  void soundOnManual(int note);
  void pedalOn(int note, int velocity);
  void pedalOff();
  void followHands();
  bool matchTrigger(int device, uint8_t status, uint8_t d1, uint8_t d2);
  void doControl(int control);
  void publish();
  void emit(uint8_t status, uint8_t d1, uint8_t d2);
  int lowestHeldUpTo(int top) const;

  // Mailboxes in.
  struct Choice {
    Layout layout;
    int preset = -1;
  };
  Mailbox<int> enabledBox_;
  Mailbox<Choice> layoutBox_;
  Mailbox<Presets> presetsBox_;
  Mailbox<std::array<Trigger, kControlCount>> triggersBox_;
  Mailbox<int> learnBox_;
  Mailbox<int> panicBox_;
  struct Devices {
    int source = 0, own = 0;
  };
  Mailbox<Devices> devicesBox_;
  Mailbox<State> stateBox_;

  // Audio thread state.
  bool enabled_ = false;
  Layout layout_;
  Presets presets_;
  int preset_ = -1;
  std::array<Trigger, kControlCount> triggers_{};
  int learning_ = -1;
  int sourceDevice_ = 0, ownDevice_ = 0;
  double nowMs_ = 0.0;
  bool changed_ = true;
  uint32_t layoutChanges_ = 0, triggerChanges_ = 0;

  bool held_[128] = {};
  uint8_t velocity_[128] = {};
  bool raw_[128] = {};      // passed on unsplit while the split was off
  int8_t zoneOfKey_[128];   // -1 when not held
  Sent sent_[128];
  uint16_t count_[kRoleCount + 1][128] = {};  // notes sounding per role, by how many keys

  int pedalNote_ = -1;
  int pedalKey_ = -1;  // the source key giving the bass
  bool pedalHeld_ = false;       // a bass key is down
  double pedalUntilMs_ = -1.0;   // a timed sustain ends here
  int followedSplit_ = -1;       // the first split point the hands moved, or -1
  double followUntilMs_ = -1.0;

  Msg out_[kOutCapacity];
  int outCount_ = 0;
  State state_;
};

}  // namespace mp::split
