#include "MidiMap.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace mp {
namespace {

const char* triggerName(MidiTrigger t) {
  switch (t) {
    case MidiTrigger::Momentary: return "momentary";
    case MidiTrigger::EngageOnly: return "on";
    case MidiTrigger::DisengageOnly: return "off";
    case MidiTrigger::Toggle: break;
  }
  return "toggle";
}

// Device names have spaces in them and the file is whitespace-separated, so
// they travel with underscores. A name that genuinely contains an underscore
// round-trips because the escape is doubled.
std::string encodeName(const std::string& in) {
  std::string out;
  for (char c : in) {
    if (c == '_') out += "__";
    else if (c == ' ') out += '_';
    else out += c;
  }
  return out.empty() ? "any" : out;
}

std::string decodeName(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '_') { out += in[i]; continue; }
    if (i + 1 < in.size() && in[i + 1] == '_') { out += '_'; ++i; }
    else out += ' ';
  }
  return out;
}

MidiTrigger triggerFrom(const std::string& s) {
  if (s == "momentary") return MidiTrigger::Momentary;
  if (s == "on") return MidiTrigger::EngageOnly;
  if (s == "off") return MidiTrigger::DisengageOnly;
  return MidiTrigger::Toggle;
}

} // namespace

namespace {

const char* kindName(MidiSourceKind k) {
  switch (k) {
    case MidiSourceKind::Note: return "note";
    case MidiSourceKind::ControlChange: return "cc";
    case MidiSourceKind::ProgramChange: return "pgm";
    case MidiSourceKind::SysEx: return "sysex";
    case MidiSourceKind::None: break;
  }
  return "none";
}

MidiSourceKind sourceKindFrom(const std::string& s) {
  if (s == "note") return MidiSourceKind::Note;
  if (s == "cc") return MidiSourceKind::ControlChange;
  if (s == "pgm") return MidiSourceKind::ProgramChange;
  if (s == "sysex") return MidiSourceKind::SysEx;
  return MidiSourceKind::None;
}

const char* targetName(MidiTargetKind k) {
  switch (k) {
    case MidiTargetKind::Switch: return "switch";
    case MidiTargetKind::ContinuousControl: return "control";
    case MidiTargetKind::Keyboard: return "keyboard";
    case MidiTargetKind::StepperNext: return "stepper-next";
    case MidiTargetKind::StepperPrev: return "stepper-prev";
    case MidiTargetKind::ConsoleNextPage: return "console-next-page";
    case MidiTargetKind::ConsolePrevPage: return "console-prev-page";
    case MidiTargetKind::ConsoleNextLayout: return "console-next-layout";
    case MidiTargetKind::ConsoleToggleStopList: return "console-stop-list";
    case MidiTargetKind::ConsoleToggleKeyboard: return "console-keyboard";
    case MidiTargetKind::ConsoleToggleCombinations: return "console-combinations";
    case MidiTargetKind::TransposeUp: return "transpose-up";
    case MidiTargetKind::TransposeDown: return "transpose-down";
    case MidiTargetKind::TemperamentNext: return "temperament-next";
    case MidiTargetKind::TemperamentPrev: return "temperament-prev";
    case MidiTargetKind::PlayerGeneral: return "general";
    case MidiTargetKind::PlayerGeneralCancel: return "general-cancel";
    case MidiTargetKind::PlayerDivisional: return "divisional";
    case MidiTargetKind::PlayerDivisionalCancel: return "divisional-cancel";
    case MidiTargetKind::Setter: return "setter";
    case MidiTargetKind::RouteKeyboard: return "route-keyboard";
    case MidiTargetKind::None: break;
  }
  return "none";
}

MidiTargetKind targetKindFrom(const std::string& s) {
  if (s == "switch") return MidiTargetKind::Switch;
  if (s == "route-keyboard") return MidiTargetKind::RouteKeyboard;
  if (s == "control") return MidiTargetKind::ContinuousControl;
  if (s == "keyboard") return MidiTargetKind::Keyboard;
  if (s == "stepper-next") return MidiTargetKind::StepperNext;
  if (s == "stepper-prev") return MidiTargetKind::StepperPrev;
  if (s == "console-next-page") return MidiTargetKind::ConsoleNextPage;
  if (s == "console-prev-page") return MidiTargetKind::ConsolePrevPage;
  if (s == "console-next-layout") return MidiTargetKind::ConsoleNextLayout;
  if (s == "console-stop-list") return MidiTargetKind::ConsoleToggleStopList;
  if (s == "console-keyboard") return MidiTargetKind::ConsoleToggleKeyboard;
  if (s == "console-combinations") return MidiTargetKind::ConsoleToggleCombinations;
  if (s == "transpose-up") return MidiTargetKind::TransposeUp;
  if (s == "transpose-down") return MidiTargetKind::TransposeDown;
  if (s == "temperament-next") return MidiTargetKind::TemperamentNext;
  if (s == "temperament-prev") return MidiTargetKind::TemperamentPrev;
  if (s == "general") return MidiTargetKind::PlayerGeneral;
  if (s == "general-cancel") return MidiTargetKind::PlayerGeneralCancel;
  if (s == "divisional") return MidiTargetKind::PlayerDivisional;
  if (s == "divisional-cancel") return MidiTargetKind::PlayerDivisionalCancel;
  if (s == "setter") return MidiTargetKind::Setter;
  return MidiTargetKind::None;
}

} // namespace

void MidiMap::clear() {
  bySource_.clear();
  alsoDrives_.clear();
  ordered_.clear();
  latchState_.clear();
  sends_.clear();
  shortcuts_.clear();
  cancelLearn();
}

void MidiMap::rebuildOrdered() {
  ordered_.clear();
  ordered_.reserve(bySource_.size());
  for (const auto& [src, b] : bySource_) {
    (void)src;
    ordered_.push_back(b);
  }
  for (const auto& [src, more] : alsoDrives_) {
    (void)src;
    ordered_.insert(ordered_.end(), more.begin(), more.end());
  }
  // Stable presentation: by target, then by what triggers it.
  std::sort(ordered_.begin(), ordered_.end(),
            [](const MidiBinding& a, const MidiBinding& b) {
              if (a.targetKind != b.targetKind)
                return static_cast<int>(a.targetKind) < static_cast<int>(b.targetKind);
              if (a.targetId != b.targetId) return a.targetId < b.targetId;
              if (a.source.kind != b.source.kind)
                return static_cast<int>(a.source.kind) < static_cast<int>(b.source.kind);
              if (a.source.channel != b.source.channel)
                return a.source.channel < b.source.channel;
              return a.source.number < b.source.number;
            });
}

void MidiMap::bind(const MidiBinding& binding) {
  if (binding.source.kind == MidiSourceKind::None) return;
  if (binding.targetKind == MidiTargetKind::None) return;
  // One source drives one target: re-learning a control moves it rather than
  // stacking a second meaning onto the same button. Except a controller
  // learned for another continuous control: it drives both (#90, one pedal
  // for two swell boxes). Clear mapping on either one to part them.
  //
  // And a control learned for a second manual on a keyboard: one spare stop
  // that steps the keyboard through several manuals (#90, a Johannus whose
  // spare tab moves the lower keyboard between Great and Choir).
  const auto existing = bySource_.find(binding.source);
  const bool shares = binding.targetKind == MidiTargetKind::ContinuousControl ||
                      binding.targetKind == MidiTargetKind::RouteKeyboard;
  if (shares && existing != bySource_.end() &&
      existing->second.targetKind == binding.targetKind &&
      existing->second.targetId != binding.targetId) {
    auto& more = alsoDrives_[binding.source];
    more.erase(std::remove_if(more.begin(), more.end(),
                              [&](const MidiBinding& b) { return b.targetId == binding.targetId; }),
               more.end());
    more.push_back(binding);
    rebuildOrdered();
    return;
  }
  alsoDrives_.erase(binding.source);
  bySource_[binding.source] = binding;
  rebuildOrdered();
}

void MidiMap::removeBindings(const std::function<bool(const MidiBinding&)>& pick) {
  for (auto it = bySource_.begin(); it != bySource_.end();)
    it = pick(it->second) ? bySource_.erase(it) : std::next(it);
  for (auto it = alsoDrives_.begin(); it != alsoDrives_.end();) {
    auto& more = it->second;
    more.erase(std::remove_if(more.begin(), more.end(), pick), more.end());
    it = more.empty() ? alsoDrives_.erase(it) : std::next(it);
  }
  rebuildOrdered();
}

bool MidiMap::replace(const MidiBinding& binding) {
  const auto it = bySource_.find(binding.source);
  if (it != bySource_.end() && it->second.targetKind == binding.targetKind &&
      it->second.targetId == binding.targetId) {
    it->second = binding;
    rebuildOrdered();
    return true;
  }
  const auto more = alsoDrives_.find(binding.source);
  if (more != alsoDrives_.end())
    for (auto& b : more->second)
      if (b.targetKind == binding.targetKind && b.targetId == binding.targetId) {
        b = binding;
        rebuildOrdered();
        return true;
      }
  return false;
}

void MidiMap::unbind(const MidiSource& source) {
  const bool any = bySource_.erase(source) > 0;
  if (alsoDrives_.erase(source) > 0 || any) rebuildOrdered();
}

void MidiMap::unbindTarget(MidiTargetKind kind, Id targetId) {
  bool changed = false;
  for (auto& [src, more] : alsoDrives_) {
    (void)src;
    const auto before = more.size();
    more.erase(std::remove_if(more.begin(), more.end(),
                              [&](const MidiBinding& b) { return b.targetKind == kind && b.targetId == targetId; }),
               more.end());
    changed = changed || more.size() != before;
  }
  for (auto it = bySource_.begin(); it != bySource_.end();) {
    if (it->second.targetKind == kind && it->second.targetId == targetId) {
      // A controller that also drives other controls keeps driving them.
      const auto more = alsoDrives_.find(it->first);
      if (more != alsoDrives_.end() && !more->second.empty()) {
        it->second = more->second.front();
        more->second.erase(more->second.begin());
        ++it;
      } else {
        it = bySource_.erase(it);
      }
      changed = true;
    } else {
      ++it;
    }
  }
  for (auto it = alsoDrives_.begin(); it != alsoDrives_.end();)
    it = it->second.empty() ? alsoDrives_.erase(it) : std::next(it);
  if (changed) rebuildOrdered();
}

const MidiBinding* MidiMap::bindingFor(MidiTargetKind kind, Id targetId) const {
  for (const auto& b : ordered_)
    if (b.targetKind == kind && b.targetId == targetId) return &b;
  return nullptr;
}

MidiAction MidiMap::actionFor(const MidiSource& source, int value) const {
  // Most specific first. A mapping that names a device and a channel beats one
  // that names only a device, which beats one that names neither — so a player
  // can map their whole rig loosely and then pin one keyboard exactly, without
  // the loose mapping swallowing it.
  auto it = bySource_.end();
  for (int step = 0; step < 4 && it == bySource_.end(); ++step) {
    MidiSource probe = source;
    if (step & 1) probe.channel = 0;
    if (step & 2) probe.deviceId = MidiDeviceMap::kAnyDevice;
    it = bySource_.find(probe);
  }
  // Then any note, last: a mapping for one note beats one for every note.
  if (it == bySource_.end() && source.kind == MidiSourceKind::Note)
    for (int step = 0; step < 4 && it == bySource_.end(); ++step) {
      MidiSource probe = source;
      probe.number = kAnyNote;
      if (step & 1) probe.channel = 0;
      if (step & 2) probe.deviceId = MidiDeviceMap::kAnyDevice;
      it = bySource_.find(probe);
    }
  if (it == bySource_.end()) return {};
  const MidiBinding& b = it->second;

  MidiAction action;
  action.kind = b.targetKind;
  action.targetId = b.targetId;

  switch (b.targetKind) {
    // The setter is held or toggled exactly like a drawstop. Its latch is
    // kept under an id no switch can have.
    case MidiTargetKind::Setter:
    case MidiTargetKind::Switch: {
      // A console that sends 64 for a press, or whose button only travels part
      // of the range, still has to register.
      const bool pressed = b.isOn(value);
      // A message that only ever engages, or only ever disengages. This is how
      // a console with separate "draw" and "cancel" messages per stop is
      // mapped, and it cannot be expressed by toggling.
      if (b.trigger == MidiTrigger::EngageOnly ||
          b.trigger == MidiTrigger::DisengageOnly) {
        if (!pressed) {
          action.kind = MidiTargetKind::None; // nothing to do on release
          return action;
        }
        action.engage = b.trigger == MidiTrigger::EngageOnly;
        latchState_[b.targetId] = action.engage;
        break;
      }
      if (b.trigger == MidiTrigger::Toggle) {
        // A latching control toggles on the press and ignores the release,
        // which is what a drawstop button on a console does.
        if (!pressed) {
          action.kind = MidiTargetKind::None; // nothing to do on release
          return action;
        }
        bool& state = latchState_[b.targetId];
        state = !state;
        action.engage = state;
      } else {
        // Momentary: held is on, released is off. This is a piston.
        action.engage = pressed;
        latchState_[b.targetId] = action.engage;
      }
      break;
    }
    case MidiTargetKind::ContinuousControl: {
      // A note sets a control by its velocity as it is struck (#199: the
      // swell written into a sequencer's part as note velocities). Its
      // note-off carries velocity 0 and would shut the swell after every
      // note, so it does nothing.
      if (b.source.kind == MidiSourceKind::Note && value <= 0) {
        action.kind = MidiTargetKind::None;
        return action;
      }
      // Across the binding's own window, so a shoe that only travels 20..100
      // still reaches both ends of the swell.
      action.value = controlValue(b, value);
      if (const auto more = alsoDrives_.find(it->first); more != alsoDrives_.end())
        action.alsoDrives = &more->second;
      break;
    }
    case MidiTargetKind::Keyboard:
      action.value = value;
      break;
    case MidiTargetKind::RouteKeyboard:
      // Every manual this control is learned for, to step through.
      if (const auto more = alsoDrives_.find(it->first); more != alsoDrives_.end())
        action.alsoDrives = &more->second;
      // A piston fires on the press. A drawstop or tab used for this sends
      // one message going on and another going off, and each is a press.
      if (b.trigger != MidiTrigger::Toggle && value <= 0) action.kind = MidiTargetKind::None;
      break;
    case MidiTargetKind::StepperNext:
    case MidiTargetKind::StepperPrev:
    case MidiTargetKind::ConsoleNextPage:
    case MidiTargetKind::ConsolePrevPage:
    case MidiTargetKind::ConsoleNextLayout:
    case MidiTargetKind::ConsoleToggleStopList:
    case MidiTargetKind::ConsoleToggleKeyboard:
    case MidiTargetKind::ConsoleToggleCombinations:
    case MidiTargetKind::TransposeUp:
    case MidiTargetKind::TransposeDown:
    case MidiTargetKind::TemperamentNext:
    case MidiTargetKind::TemperamentPrev:
    case MidiTargetKind::PlayerGeneral:
    case MidiTargetKind::PlayerGeneralCancel:
    case MidiTargetKind::PlayerDivisional:
    case MidiTargetKind::PlayerDivisionalCancel:
      // These fire on the press. Acting on the release too would move two
      // frames, or turn a page and turn it straight back -- exactly the
      // failure an organist would notice mid-piece and could not explain.
      if (value <= 0) action.kind = MidiTargetKind::None;
      break;
    case MidiTargetKind::None:
      break;
  }
  return action;
}

// ------------------------------------------------------------------ learn

void MidiMap::beginLearnAs(MidiTargetKind kind, Id targetId,
                          MidiTrigger trigger) {
  beginLearn(kind, targetId, trigger == MidiTrigger::Toggle);
  learnTrigger_ = trigger;
}

std::vector<const MidiBinding*> MidiMap::bindingsFor(MidiTargetKind kind,
                                                     Id targetId) const {
  std::vector<const MidiBinding*> out;
  for (const auto& b : ordered_)
    if (b.targetKind == kind && b.targetId == targetId) out.push_back(&b);
  return out;
}

void MidiMap::beginLearn(MidiTargetKind kind, Id targetId, bool latching) {
  learnTrigger_ = latching ? MidiTrigger::Toggle : MidiTrigger::Momentary;
  learnKind_ = kind;
  learnTarget_ = targetId;
  learnLatching_ = latching;
}

void MidiMap::cancelLearn() {
  learnKind_ = MidiTargetKind::None;
  learnTarget_ = 0;
}

bool MidiMap::learnFrom(const MidiSource& source) {
  if (learnKind_ == MidiTargetKind::None) return false;
  if (source.kind == MidiSourceKind::None) return false;
  // A shoe or a level is moved by a controller. A key pressed while one is
  // armed is the player playing, not teaching: it plays, and the learn waits
  // for the pedal.
  if (learnKind_ == MidiTargetKind::ContinuousControl &&
      source.kind != MidiSourceKind::ControlChange)
    return false;

  // Learning normally replaces whatever that target had: a player re-touching
  // a stop and moving a different control means "use this one instead".
  //
  // Except for the one-directional behaviours. A console that sends one
  // message to draw a stop and a different one to cancel it needs BOTH
  // bindings on the same stop, so learning an "on only" leaves an existing
  // "off only" alone and vice versa — otherwise the second one you teach
  // erases the first and the stop can only ever move one way.
  const bool directional = learnTrigger_ == MidiTrigger::EngageOnly ||
                           learnTrigger_ == MidiTrigger::DisengageOnly;
  // A program button can have several: consoles carry two or more + and -
  // thumb pistons. Learning one more adds it; Clear removes them all.
  if (isConsoleTarget(learnKind_)) {
    // nothing to replace
  } else if (!directional) {
    unbindTarget(learnKind_, learnTarget_);
  } else {
    for (const MidiBinding* existing : bindingsFor(learnKind_, learnTarget_))
      if (existing->trigger == learnTrigger_ ||
          (existing->trigger != MidiTrigger::EngageOnly &&
           existing->trigger != MidiTrigger::DisengageOnly)) {
        unbind(existing->source);
        break;
      }
  }

  MidiBinding b;
  b.source = source;
  b.targetKind = learnKind_;
  b.targetId = learnTarget_;
  b.trigger = learnTrigger_;
  b.latching = learnTrigger_ == MidiTrigger::Toggle;
  bind(b);
  cancelLearn();
  return true;
}

// ------------------------------------------------------- the MIDI window

void MidiMap::setBindingsFor(MidiTargetKind kind, Id targetId,
                             const std::vector<MidiBinding>& bindings) {
  unbindTarget(kind, targetId);
  for (MidiBinding b : bindings) {
    if (b.source.kind == MidiSourceKind::None) continue;
    b.targetKind = kind;
    b.targetId = targetId;
    b.latching = b.trigger == MidiTrigger::Toggle;
    bind(b);
  }
}

std::vector<MidiSend> MidiMap::sendsFor(MidiTargetKind kind, Id targetId) const {
  std::vector<MidiSend> out;
  for (const auto& s : sends_)
    if (s.targetKind == kind && s.targetId == targetId) out.push_back(s);
  return out;
}

bool MidiMap::hasSends(MidiTargetKind kind, Id targetId) const {
  for (const auto& s : sends_)
    if (s.targetKind == kind && s.targetId == targetId) return true;
  return false;
}

void MidiMap::setSendsFor(MidiTargetKind kind, Id targetId,
                          const std::vector<MidiSend>& sends) {
  sends_.erase(std::remove_if(sends_.begin(), sends_.end(),
                              [&](const MidiSend& s) {
                                return s.targetKind == kind && s.targetId == targetId;
                              }),
               sends_.end());
  for (MidiSend s : sends) {
    if (s.kind == MidiSourceKind::None) continue;
    s.targetKind = kind;
    s.targetId = targetId;
    sends_.push_back(s);
  }
}

namespace {

uint8_t clamp7(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 127 ? 127 : v)); }
uint8_t channelBits(int channel) {
  return static_cast<uint8_t>((channel < 1 ? 1 : (channel > 16 ? 16 : channel)) - 1);
}

bool message(MidiSourceKind kind, int channel, int number, int value, RawMidi& out) {
  const uint8_t ch = channelBits(channel);
  switch (kind) {
    case MidiSourceKind::Note:
      // A note-on at velocity 0 is the usual "lamp off": consoles that use
      // the velocity for a colour or a brightness read it the same way.
      out = {{static_cast<uint8_t>(0x90 | ch), clamp7(number), clamp7(value)}, 3};
      return true;
    case MidiSourceKind::ControlChange:
      out = {{static_cast<uint8_t>(0xB0 | ch), clamp7(number), clamp7(value)}, 3};
      return true;
    case MidiSourceKind::ProgramChange:
      out = {{static_cast<uint8_t>(0xC0 | ch), clamp7(value), 0}, 2};
      return true;
    case MidiSourceKind::SysEx:  // received only; nothing is sent this way
    case MidiSourceKind::None:
      break;
  }
  return false;
}

} // namespace

bool MidiMap::switchMessage(const MidiSend& s, bool on, RawMidi& out) {
  // A program change has no "off": it is a button that says which program.
  if (s.kind == MidiSourceKind::ProgramChange)
    return on && message(s.kind, s.channel, 0, s.number, out);
  const int value = on ? s.high : s.low;
  if (value < 0) return false;
  return message(s.kind, s.channel, s.number, value, out);
}

bool MidiMap::controlMessage(const MidiSend& s, int value, RawMidi& out) {
  const int v = clamp7(value);
  const int scaled = s.low + ((s.high - s.low) * v) / 127;
  if (s.kind == MidiSourceKind::ProgramChange)
    return message(s.kind, s.channel, 0, scaled, out);
  return message(s.kind, s.channel, s.number, scaled, out);
}

bool MidiMap::keyMessage(const MidiSend& s, int note, int velocity, bool on, RawMidi& out) {
  const int n = note + s.number;
  if (n < 0 || n > 127) return false;
  if (on) return message(MidiSourceKind::Note, s.channel, n, velocity < 1 ? 1 : velocity, out);
  out = {{static_cast<uint8_t>(0x80 | channelBits(s.channel)), clamp7(n), 0}, 3};
  return true;
}

std::vector<KeyShortcut> MidiMap::shortcutsFor(MidiTargetKind kind, Id targetId) const {
  std::vector<KeyShortcut> out;
  for (const auto& k : shortcuts_)
    if (k.targetKind == kind && k.targetId == targetId) out.push_back(k);
  return out;
}

void MidiMap::setShortcutsFor(MidiTargetKind kind, Id targetId,
                              const std::vector<KeyShortcut>& shortcuts) {
  shortcuts_.erase(std::remove_if(shortcuts_.begin(), shortcuts_.end(),
                                  [&](const KeyShortcut& k) {
                                    return k.targetKind == kind && k.targetId == targetId;
                                  }),
                   shortcuts_.end());
  for (KeyShortcut k : shortcuts) {
    if (k.key.empty()) continue;
    k.targetKind = kind;
    k.targetId = targetId;
    shortcuts_.push_back(k);
  }
}

std::vector<KeyShortcut> MidiMap::shortcutsForKey(const std::string& key) const {
  std::vector<KeyShortcut> out;
  for (const auto& k : shortcuts_)
    if (k.key == key) out.push_back(k);
  return out;
}

void MidiMap::keepOnlyConsoleSendsAndShortcuts() {
  sends_.erase(std::remove_if(sends_.begin(), sends_.end(),
                              [](const MidiSend& s) { return !isConsoleTarget(s.targetKind); }),
               sends_.end());
  shortcuts_.erase(std::remove_if(shortcuts_.begin(), shortcuts_.end(),
                                  [](const KeyShortcut& k) {
                                    return !isConsoleTarget(k.targetKind);
                                  }),
                   shortcuts_.end());
}

// ------------------------------------------------------------ persistence

// The device's identifier as a last field, when the system gave one that lasts
// (MidiDevices.h). A reader from before it stops at the name.
std::string MidiMap::deviceIdentifierField(int deviceId) const {
  const std::string id = devices_.identifierFor(deviceId);
  return id.empty() ? std::string() : " " + encodeName(id);
}

int MidiMap::readDevice(const std::string& name, std::istream& rest) {
  std::string identifier;
  rest >> identifier;
  return devices_.idFor(decodeName(name), identifier.empty() ? std::string() : decodeName(identifier));
}

std::string MidiMap::toText() const {
  std::ostringstream out;
  out << "# Masterpiece MIDI map\n";
  out << "# <source> <channel> <number> <target> <id> <latching> <invert>\n";
  for (const auto& b : ordered_) {
    out << kindName(b.source.kind) << ' ' << b.source.channel << ' '
        << b.source.number << ' ' << targetName(b.targetKind) << ' '
        << b.targetId << ' ' << (b.latching ? 1 : 0) << ' '
        << (b.invert ? 1 : 0) << ' ' << triggerName(b.trigger) << ' '
        << b.lowValue << ' ' << b.highValue << ' '
        // The device by NAME, not by id: ids are assigned in first-seen order
        // and mean nothing across runs. Underscores stand in for spaces so the
        // line stays one whitespace-separated record.
        << (b.source.deviceId == MidiDeviceMap::kAnyDevice
                ? std::string("any")
                : encodeName(devices_.nameFor(b.source.deviceId)))
        << deviceIdentifierField(b.source.deviceId) << '\n';
  }
  // Channel assignments last, so an older reader that does not know the line
  // still gets every binding before it hits one it skips.
  for (const auto& b : keyboardBindings_)
    out << "manual " << b.keyboardId << ' ' << b.channel << ' ' << b.lowKey
        << ' ' << b.highKey << ' ' << b.transpose << ' ' << b.lowVelocity << ' '
        << b.highVelocity << ' ' << (b.ignoreVelocity ? 1 : 0) << ' '
        << (b.shortOctave ? 1 : 0) << ' ' << b.debounceMs << ' '
        << (b.deviceId == MidiDeviceMap::kAnyDevice
                ? std::string("any")
                : encodeName(devices_.nameFor(b.deviceId)))
        << deviceIdentifierField(b.deviceId) << '\n';
  // What the console is sent, and the computer keys. Last for the same reason.
  for (const auto& s : sends_)
    out << "send " << targetName(s.targetKind) << ' ' << s.targetId << ' '
        << kindName(s.kind) << ' ' << s.channel << ' ' << s.number << ' ' << s.low
        << ' ' << s.high << '\n';
  for (const auto& k : shortcuts_)
    out << "shortcut " << targetName(k.targetKind) << ' ' << k.targetId << ' ' << k.step
        << ' ' << encodeName(k.key) << '\n';
  return out.str();
}

void MidiMap::addKeyboardBinding(const KeyboardBinding& b) {
  if (b.keyboardId == 0) return;
  keyboardBindings_.push_back(b);
}

bool MidiMap::hasChannelBinding(int deviceId, int channel) const {
  for (const auto& b : keyboardBindings_) {
    if (b.deviceId != MidiDeviceMap::kAnyDevice && b.deviceId != deviceId)
      continue;
    // Channel 0 means any channel, which is what a fresh binding claims.
    if (b.channel != 0 && b.channel != channel) continue;
    return true;
  }
  return false;
}

void MidiMap::removeKeyboardBindingsFor(Id keyboardId) {
  keyboardBindings_.erase(
      std::remove_if(keyboardBindings_.begin(), keyboardBindings_.end(),
                     [keyboardId](const KeyboardBinding& b) {
                       return b.keyboardId == keyboardId;
                     }),
      keyboardBindings_.end());
}

void MidiMap::releaseChannel(int channel, int deviceId, Id keepKeyboardId) {
  if (channel <= 0) return;
  keyboardBindings_.erase(
      std::remove_if(keyboardBindings_.begin(), keyboardBindings_.end(),
                     [channel, deviceId, keepKeyboardId](const KeyboardBinding& b) {
                       if (b.keyboardId == keepKeyboardId) return false;
                       if (b.channel != channel) return false;
                       // "Any console" overlaps every console, either way round.
                       return b.deviceId == deviceId || b.deviceId == 0 ||
                              deviceId == 0;
                     }),
      keyboardBindings_.end());
}

int MidiMap::repairKeyboardBindings(const std::vector<Id>& playableKeyboards) {
  const size_t before = keyboardBindings_.size();
  auto playable = [&playableKeyboards](Id kb) {
    return std::find(playableKeyboards.begin(), playableKeyboards.end(), kb) !=
           playableKeyboards.end();
  };

  // The same binding twice over. Two manuals on one channel is a choice a
  // player can make -- one keyboard playing two divisions is a coupler of
  // their own making -- but the same manual, channel, console and compass
  // listed twice only doubles the work of every note.
  auto identical = [](const KeyboardBinding& x, const KeyboardBinding& y) {
    return x.keyboardId == y.keyboardId && x.channel == y.channel &&
           x.deviceId == y.deviceId && x.lowKey == y.lowKey &&
           x.highKey == y.highKey && x.transpose == y.transpose;
  };
  std::vector<bool> drop(keyboardBindings_.size(), false);
  for (size_t i = 0; i < keyboardBindings_.size(); ++i) {
    if (!playable(keyboardBindings_[i].keyboardId)) drop[i] = true;
    for (size_t j = i + 1; j < keyboardBindings_.size(); ++j)
      if (identical(keyboardBindings_[i], keyboardBindings_[j])) drop[j] = true;
  }
  std::vector<KeyboardBinding> kept;
  kept.reserve(keyboardBindings_.size());
  for (size_t i = 0; i < keyboardBindings_.size(); ++i)
    if (!drop[i]) kept.push_back(keyboardBindings_[i]);
  keyboardBindings_ = std::move(kept);
  return static_cast<int>(before - keyboardBindings_.size());
}

int MidiMap::matchKeyboards(int deviceId, int channel, int note, int velocity,
                            double timeMs, std::vector<KeyHit>& out) const {
  int added = 0;
  for (size_t i = 0; i < keyboardBindings_.size(); ++i) {
    const KeyboardBinding& b = keyboardBindings_[i];
    if (b.deviceId != MidiDeviceMap::kAnyDevice && b.deviceId != deviceId)
      continue;
    if (b.channel != 0 && b.channel != channel) continue;
    if (note < b.lowKey || note > b.highKey) continue;

    int key = note;
    if (b.shortOctave) {
      // The bottom octave of a historic keyboard has no accidentals, and the
      // notes that would have been there sit on the keys below. GrandOrgue's
      // mapping, which is the only written-down one: the first four keys are
      // dead, and three of the next five come down by a fourth.
      const int offset = note - b.lowKey;
      if (offset <= 3) continue;
      if (offset == 4 || offset == 6 || offset == 8) key -= 4;
    }
    key += b.transpose;
    if (key < 0 || key > 127) continue; // transposed off the end of MIDI

    // The velocity window. An inverted one (low above high) reverses the
    // sense, which is how a normally-closed key contact is handled.
    bool on;
    if (b.lowVelocity <= b.highVelocity)
      on = velocity >= b.lowVelocity && velocity <= b.highVelocity;
    else
      on = !(velocity >= b.highVelocity && velocity <= b.lowVelocity);

    // Contacts chatter. Without this one press retriggers the pipe.
    if (b.debounceMs > 0 && on) {
      const int64_t slot = static_cast<int64_t>(i) * 256 + key;
      auto& last = keyLastMs_[slot];
      if (timeMs - last < b.debounceMs) continue;
      last = timeMs;
    }

    KeyHit hit;
    hit.keyboardId = b.keyboardId;
    hit.midiNote = key;
    hit.on = on;
    // Tracker action has no velocity to report; a sample set that switches on
    // it would pick the wrong layer from a number the console never sent.
    if (b.ignoreVelocity) {
      hit.velocity = on ? 127 : 0;
    } else if (b.lowVelocity < b.highVelocity) {
      // Scaled across the console's own travel, so a keyboard that bottoms
      // out at 100 still reaches full.
      const int lo = b.lowVelocity, hi = b.highVelocity;
      const int clamped = velocity < lo ? lo : (velocity > hi ? hi : velocity);
      hit.velocity = ((clamped - lo) * 127) / (hi - lo);
    } else {
      hit.velocity = velocity;
    }
    out.push_back(hit);
    ++added;
  }
  return added;
}

bool MidiMap::fromText(const std::string& text) {
  clear();
  keyboardBindings_.clear();
  keyLastMs_.clear();
  std::istringstream in(text);
  std::string line;
  bool anyBad = false;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    // A manual assignment, which has its own shape.
    if (line.rfind("manual ", 0) == 0) {
      std::istringstream kl(line);
      std::string tag, dev;
      long long keyboardId = 0;
      KeyboardBinding b;
      int ignoreVel = 0, shortOct = 0;
      if (kl >> tag >> keyboardId >> b.channel >> b.lowKey >> b.highKey >>
          b.transpose >> b.lowVelocity >> b.highVelocity >> ignoreVel >>
          shortOct >> b.debounceMs) {
        b.keyboardId = static_cast<Id>(keyboardId);
        b.ignoreVelocity = ignoreVel != 0;
        b.shortOctave = shortOct != 0;
        if ((kl >> dev) && dev != "any") b.deviceId = readDevice(dev, kl);
        addKeyboardBinding(b);
      } else {
        anyBad = true;
      }
      continue;
    }

    if (line.rfind("send ", 0) == 0) {
      std::istringstream sl(line);
      std::string tag, tgt, kind;
      long long id = 0;
      MidiSend snd;
      if (sl >> tag >> tgt >> id >> kind >> snd.channel >> snd.number >> snd.low >> snd.high) {
        snd.targetKind = targetKindFrom(tgt);
        snd.targetId = static_cast<Id>(id);
        snd.kind = sourceKindFrom(kind);
        if (snd.targetKind != MidiTargetKind::None && snd.kind != MidiSourceKind::None) {
          sends_.push_back(snd);
          continue;
        }
      }
      anyBad = true;
      continue;
    }
    if (line.rfind("shortcut ", 0) == 0) {
      std::istringstream sl(line);
      std::string tag, tgt, key;
      long long id = 0;
      KeyShortcut k;
      if (sl >> tag >> tgt >> id >> k.step >> key) {
        k.targetKind = targetKindFrom(tgt);
        k.targetId = static_cast<Id>(id);
        k.key = decodeName(key);
        if (k.targetKind != MidiTargetKind::None && !k.key.empty()) {
          shortcuts_.push_back(k);
          continue;
        }
      }
      anyBad = true;
      continue;
    }

    // The old one-line channel form. Read so a mapping made before manuals
    // grew a shape of their own still opens; written back in the new form.
    if (line.rfind("keyboard ", 0) == 0) {
      std::istringstream kl(line);
      std::string tag, dev;
      int channel = 0;
      long long keyboardId = 0;
      if (kl >> tag >> channel >> keyboardId && channel > 0) {
        KeyboardBinding b;
        b.channel = channel;
        b.keyboardId = static_cast<Id>(keyboardId);
        if ((kl >> dev) && dev != "any") b.deviceId = readDevice(dev, kl);
        addKeyboardBinding(b);
      } else {
        anyBad = true;
      }
      continue;
    }

    std::string src, tgt;
    int channel = 0, number = 0, latching = 1, invert = 0;
    long long id = 0;
    std::string trig;
    if (!(ls >> src >> channel >> number >> tgt >> id >> latching >> invert)) {
      // A malformed line is skipped rather than aborting the load: losing one
      // binding beats losing the whole map.
      anyBad = true;
      continue;
    }
    MidiBinding b;
    b.source.kind = sourceKindFrom(src);
    b.source.channel = channel;
    b.source.number = number;
    b.targetKind = targetKindFrom(tgt);
    b.targetId = static_cast<Id>(id);
    b.latching = latching != 0;
    b.invert = invert != 0;
    // The behaviour is the eighth field. A mapping saved before it existed has
    // seven, and falls back to what `latching` used to mean.
    b.trigger = (ls >> trig) ? triggerFrom(trig)
                             : (b.latching ? MidiTrigger::Toggle
                                           : MidiTrigger::Momentary);
    int lo = 0, hi = 127;
    if (ls >> lo >> hi) {
      b.lowValue = lo;
      b.highValue = hi;
    }
    std::string dev;
    if ((ls >> dev) && dev != "any")
      b.source.deviceId = readDevice(dev, ls);
    if (b.source.kind == MidiSourceKind::None ||
        b.targetKind == MidiTargetKind::None) {
      anyBad = true;
      continue;
    }
    bind(b);
  }
  return !anyBad;
}

} // namespace mp
