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
    case MidiSourceKind::None: break;
  }
  return "none";
}

MidiSourceKind sourceKindFrom(const std::string& s) {
  if (s == "note") return MidiSourceKind::Note;
  if (s == "cc") return MidiSourceKind::ControlChange;
  if (s == "pgm") return MidiSourceKind::ProgramChange;
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
    case MidiTargetKind::None: break;
  }
  return "none";
}

MidiTargetKind targetKindFrom(const std::string& s) {
  if (s == "switch") return MidiTargetKind::Switch;
  if (s == "control") return MidiTargetKind::ContinuousControl;
  if (s == "keyboard") return MidiTargetKind::Keyboard;
  if (s == "stepper-next") return MidiTargetKind::StepperNext;
  if (s == "stepper-prev") return MidiTargetKind::StepperPrev;
  if (s == "console-next-page") return MidiTargetKind::ConsoleNextPage;
  if (s == "console-prev-page") return MidiTargetKind::ConsolePrevPage;
  if (s == "console-next-layout") return MidiTargetKind::ConsoleNextLayout;
  if (s == "console-stop-list") return MidiTargetKind::ConsoleToggleStopList;
  if (s == "console-keyboard") return MidiTargetKind::ConsoleToggleKeyboard;
  return MidiTargetKind::None;
}

} // namespace

void MidiMap::clear() {
  bySource_.clear();
  ordered_.clear();
  latchState_.clear();
  cancelLearn();
}

void MidiMap::rebuildOrdered() {
  ordered_.clear();
  ordered_.reserve(bySource_.size());
  for (const auto& [src, b] : bySource_) {
    (void)src;
    ordered_.push_back(b);
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
  // stacking a second meaning onto the same button.
  bySource_[binding.source] = binding;
  rebuildOrdered();
}

void MidiMap::unbind(const MidiSource& source) {
  if (bySource_.erase(source) > 0) rebuildOrdered();
}

void MidiMap::unbindTarget(MidiTargetKind kind, Id targetId) {
  bool changed = false;
  for (auto it = bySource_.begin(); it != bySource_.end();) {
    if (it->second.targetKind == kind && it->second.targetId == targetId) {
      it = bySource_.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
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
  if (it == bySource_.end()) return {};
  const MidiBinding& b = it->second;

  MidiAction action;
  action.kind = b.targetKind;
  action.targetId = b.targetId;

  switch (b.targetKind) {
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
      // Across the binding's own window, so a shoe that only travels 20..100
      // still reaches both ends of the swell.
      int v = b.scale(value);
      if (b.invert) v = 127 - v;
      action.value = v;
      break;
    }
    case MidiTargetKind::Keyboard:
      action.value = value;
      break;
    case MidiTargetKind::StepperNext:
    case MidiTargetKind::StepperPrev:
    case MidiTargetKind::ConsoleNextPage:
    case MidiTargetKind::ConsolePrevPage:
    case MidiTargetKind::ConsoleNextLayout:
    case MidiTargetKind::ConsoleToggleStopList:
    case MidiTargetKind::ConsoleToggleKeyboard:
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
  if (!directional) {
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

// ------------------------------------------------------------ persistence

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
        << '\n';
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
        << '\n';
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
        if ((kl >> dev) && dev != "any") b.deviceId = devices_.idFor(decodeName(dev));
        addKeyboardBinding(b);
      } else {
        anyBad = true;
      }
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
        if ((kl >> dev) && dev != "any") b.deviceId = devices_.idFor(decodeName(dev));
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
      b.source.deviceId = devices_.idFor(decodeName(dev));
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
