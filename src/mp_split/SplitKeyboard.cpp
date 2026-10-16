#include "SplitKeyboard.h"

#include <algorithm>

namespace mp::split {

namespace {

// How long a split the hands moved stays after the last key is released.
constexpr double kFollowHoldMs = 3000.0;

int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

Role clampRole(Role r) { return static_cast<int>(r) > kRoleCount ? Role::None : r; }

}  // namespace

int Layout::zoneOf(int note, int firstSplit) const {
  int zone = 0;
  if (splitCount >= 1 && note >= firstSplit) zone = 1;
  if (splitCount >= 2 && note >= splits[1]) zone = 2;
  if (splitCount >= 3 && note >= splits[2]) zone = 3;
  return zone;
}

void Layout::normalize() {
  splitCount = clampInt(splitCount, 0, kMaxSplits);
  for (int& s : splits) s = clampInt(s, 1, 127);
  std::sort(splits, splits + splitCount);
  for (int i = 1; i < splitCount; ++i)
    if (splits[i] <= splits[i - 1]) splits[i] = std::min(127, splits[i - 1] + 1);
  for (Zone& z : zones) {
    z.role = clampRole(z.role);
    z.layer = clampRole(z.layer);
    if (z.layer == z.role) z.layer = Role::None;
    z.octave = clampInt(z.octave, -3, 3);
    z.transpose = clampInt(z.transpose, -12, 12);
  }
  transpose = clampInt(transpose, -12, 12);
  bass.topKey = clampInt(bass.topKey, 0, 127);
  if (static_cast<int>(bass.sustain) > static_cast<int>(Sustain::Timed)) bass.sustain = Sustain::Off;
  bass.sustainMs = clampInt(bass.sustainMs, 50, 10000);
}

Layout factoryLayout(int index) {
  Layout l;
  switch (index) {
    case 0:
      // A 61-note keyboard from C2: C2 to E3 the pedal (17 notes), F3 to B4
      // the lower manual, C5 up the upper manual an octave down.
      l.splitCount = 2;
      l.splits[0] = 53;
      l.splits[1] = 72;
      l.zones[0].role = Role::Pedal;
      l.zones[1].role = Role::Lower;
      l.zones[2].role = Role::Upper;
      l.zones[2].octave = -1;
      break;
    case 1:
      // Split at middle C; the lowest left-hand key also plays the pedal.
      l.splitCount = 1;
      l.splits[0] = 60;
      l.zones[0].role = Role::Lower;
      l.zones[1].role = Role::Upper;
      l.bass.on = true;
      l.bass.topKey = 59;
      break;
    default:
      l.splitCount = 1;
      l.splits[0] = 60;
      l.zones[0].role = Role::Lower;
      l.zones[1].role = Role::Upper;
      break;
  }
  return l;
}

const char* factoryName(int index) {
  switch (index) {
    case 0: return "Two manuals and pedal";
    case 1: return "Two manuals and bass";
    default: return "Two manuals";
  }
}

SplitKeyboard::SplitKeyboard() {
  std::fill(std::begin(zoneOfKey_), std::end(zoneOfKey_), static_cast<int8_t>(-1));
  for (int i = 0; i < kFactoryPresets; ++i) {
    presets_.layouts[i] = factoryLayout(i);
    presets_.used[i] = true;
  }
  layout_ = factoryLayout(1);
}

// --- audio thread ----------------------------------------------------------

void SplitKeyboard::beginBlock(double nowMs) {
  nowMs_ = nowMs;
  Devices d;
  if (devicesBox_.take(d)) {
    if (d.source != sourceDevice_ && enabled_) {
      // Another keyboard: what the old one holds is let go.
      releaseAll();
      std::fill(std::begin(held_), std::end(held_), false);
    }
    sourceDevice_ = d.source;
    ownDevice_ = d.own;
  }
  Presets p;
  if (presetsBox_.take(p)) {
    for (int i = 0; i < kFactoryPresets; ++i) {
      p.layouts[i] = factoryLayout(i);
      p.used[i] = true;
    }
    for (Layout& l : p.layouts) l.normalize();
    presets_ = p;
    changed_ = true;
  }
  std::array<Trigger, kControlCount> t;
  if (triggersBox_.take(t)) {
    triggers_ = t;
    changed_ = true;
  }
  int learn = 0;
  if (learnBox_.take(learn)) {
    learning_ = learn >= 0 && learn < kControlCount ? learn : -1;
    changed_ = true;
  }
  Choice c;
  if (layoutBox_.take(c)) {
    c.layout.normalize();
    applyLayout(c.layout, c.preset, false);
  }
  int on = 0;
  if (enabledBox_.take(on)) setOn(on != 0);
}

void SplitKeyboard::endBlock() {
  if (!pedalHeld_ && pedalNote_ >= 0 && pedalUntilMs_ >= 0.0 && nowMs_ >= pedalUntilMs_) {
    pedalOff();
    changed_ = true;
  }
  if (followedSplit_ >= 0 && followUntilMs_ >= 0.0 && nowMs_ >= followUntilMs_) {
    followedSplit_ = -1;
    followUntilMs_ = -1.0;
    changed_ = true;
  }
  if (changed_) publish();
}

bool SplitKeyboard::feed(int device, const uint8_t* data, int size) {
  if (size < 1 || data == nullptr) return false;
  if (device == ownDevice_ && ownDevice_ != 0) return false;
  const uint8_t status = data[0];
  const uint8_t d1 = size > 1 ? data[1] : 0, d2 = size > 2 ? data[2] : 0;
  // From the host or a file playing: never split, never a control.
  if (device == 0) return false;

  if (learning_ >= 0) {
    const uint8_t kind = status & 0xF0;
    Trigger t;
    t.channel = static_cast<uint8_t>((status & 0x0F) + 1);
    t.number = d1;
    t.device = device;
    if (kind == 0x90 && d2 > 0) t.kind = Trigger::Note;
    else if (kind == 0xB0 && d2 > 0) t.kind = Trigger::Controller;
    else if (kind == 0xC0) t.kind = Trigger::Program;
    if (t.kind != Trigger::None) {
      // One message, one control.
      for (Trigger& other : triggers_)
        if (other == t) other = Trigger{};
      triggers_[static_cast<size_t>(learning_)] = t;
      learning_ = -1;
      ++triggerChanges_;
      changed_ = true;
      return true;
    }
  }
  if (matchTrigger(device, status, d1, d2)) return true;

  const bool fromSource = sourceDevice_ == 0 || device == sourceDevice_;
  const uint8_t kind = status & 0xF0;
  if (!fromSource || (kind != 0x90 && kind != 0x80) || d1 > 127) return false;
  const bool press = kind == 0x90 && d2 > 0;

  if (!enabled_) {
    raw_[d1] = press;
    return false;
  }
  // A key pressed before the split came on lets go the way it was pressed.
  if (!press && raw_[d1]) {
    raw_[d1] = false;
    return false;
  }
  if (press) {
    if (held_[d1]) keyOff(d1);
    keyOn(d1, d2);
  } else if (held_[d1]) {
    keyOff(d1);
  }
  return true;
}

bool SplitKeyboard::matchTrigger(int device, uint8_t status, uint8_t d1, uint8_t d2) {
  const uint8_t kind = status & 0xF0;
  const uint8_t channel = static_cast<uint8_t>((status & 0x0F) + 1);
  Trigger::Kind k = Trigger::None;
  if (kind == 0x90 || kind == 0x80) k = Trigger::Note;
  else if (kind == 0xB0) k = Trigger::Controller;
  else if (kind == 0xC0) k = Trigger::Program;
  if (k == Trigger::None) return false;
  for (int i = 0; i < kControlCount; ++i) {
    const Trigger& t = triggers_[static_cast<size_t>(i)];
    if (t.kind != k || t.channel != channel || t.number != d1) continue;
    if (t.device != 0 && t.device != device) continue;
    // A press acts; its release is taken too, so a key learned as a control
    // plays nothing.
    const bool press = (k == Trigger::Note && kind == 0x90 && d2 > 0) || (k == Trigger::Controller && d2 > 0) ||
                       k == Trigger::Program;
    if (press) doControl(i);
    return true;
  }
  return false;
}

void SplitKeyboard::doControl(int control) {
  if (control == static_cast<int>(Control::Toggle)) {
    setOn(!enabled_);
    return;
  }
  int target = -1;
  if (control == static_cast<int>(Control::Next) || control == static_cast<int>(Control::Previous)) {
    const int step = control == static_cast<int>(Control::Next) ? 1 : -1;
    int p = preset_ < 0 ? (step > 0 ? -1 : 0) : preset_;
    for (int n = 0; n < kPresetCount; ++n) {
      p = (p + step + kPresetCount) % kPresetCount;
      if (presets_.used[p]) {
        target = p;
        break;
      }
    }
  } else {
    const int p = control - static_cast<int>(Control::Preset1);
    if (p >= 0 && p < kPresetCount && presets_.used[p]) target = p;
  }
  if (target >= 0) applyLayout(presets_.layouts[target], target, true);
}

void SplitKeyboard::setOn(bool on) {
  if (on == enabled_) return;
  if (!on) {
    releaseAll();
    std::fill(std::begin(held_), std::end(held_), false);
  }
  enabled_ = on;
  changed_ = true;
}

void SplitKeyboard::applyLayout(const Layout& l, int preset, bool fromAudio) {
  if (l == layout_ && preset == preset_) return;
  const bool sounding = enabled_;
  if (sounding) releaseAll();
  layout_ = l;
  preset_ = preset;
  followedSplit_ = -1;
  followUntilMs_ = -1.0;
  if (sounding) resoundHeld();
  if (fromAudio) ++layoutChanges_;
  changed_ = true;
}

void SplitKeyboard::releaseAll() {
  for (int r = 1; r <= kRoleCount; ++r)
    for (int n = 0; n < 128; ++n)
      if (count_[r][n] > 0) {
        count_[r][n] = 0;
        emit(static_cast<uint8_t>(0x80 | (channelOf(static_cast<Role>(r)) - 1)), static_cast<uint8_t>(n), 0);
      }
  for (Sent& s : sent_) s = Sent{};
  std::fill(std::begin(zoneOfKey_), std::end(zoneOfKey_), static_cast<int8_t>(-1));
  pedalNote_ = -1;
  pedalKey_ = -1;
  pedalHeld_ = false;
  pedalUntilMs_ = -1.0;
  changed_ = true;
}

void SplitKeyboard::resoundHeld() {
  bool keys[128];
  std::copy(std::begin(held_), std::end(held_), keys);
  std::fill(std::begin(held_), std::end(held_), false);
  for (int n = 0; n < 128; ++n)
    if (keys[n]) keyOn(n, velocity_[n]);
}

int SplitKeyboard::lowestHeldUpTo(int top) const {
  for (int n = 0; n <= top && n < 128; ++n)
    if (held_[n]) return n;
  return -1;
}

void SplitKeyboard::keyOn(int note, int velocity) {
  held_[note] = true;
  velocity_[note] = static_cast<uint8_t>(velocity);
  followUntilMs_ = -1.0;
  if (layout_.followHands) followHands();
  const int first = followedSplit_ >= 0 ? followedSplit_ : layout_.splits[0];
  const int zone = layout_.zoneOf(note, first);
  zoneOfKey_[note] = static_cast<int8_t>(zone);

  const Bass& bass = layout_.bass;
  const bool inBass = bass.on && note <= bass.topKey;
  sent_[note] = Sent{};
  // The bass key plays only the pedal when the layout says so; it is the bass
  // key if it is the lowest held in the bass range.
  if (inBass && !bass.alsoOnManual && lowestHeldUpTo(bass.topKey) == note) sent_[note].silentOnManual = true;
  else soundOnManual(note);
  if (inBass) updateBass();
  changed_ = true;
}

void SplitKeyboard::soundOnManual(int note) {
  Sent& s = sent_[note];
  s.silentOnManual = false;
  const int zone = zoneOfKey_[note];
  if (zone < 0) return;
  const Zone& z = layout_.zones[zone];
  const int out = note + z.transpose + 12 * z.octave + layout_.transpose;
  if (out < 0 || out > 127) return;
  int k = 0;
  for (Role r : {z.role, z.layer}) {
    if (r == Role::None || (k == 1 && r == s.role[0])) continue;
    sendOn(r, out, velocity_[note]);
    s.role[k] = r;
    s.note[k] = static_cast<uint8_t>(out);
    ++k;
  }
}

void SplitKeyboard::keyOff(int note) {
  held_[note] = false;
  const Sent s = sent_[note];
  for (int k = 0; k < 2; ++k)
    if (s.role[k] != Role::None) sendOff(s.role[k], s.note[k]);
  sent_[note] = Sent{};
  zoneOfKey_[note] = -1;
  if (layout_.bass.on && note <= layout_.bass.topKey) updateBass();
  if (followedSplit_ >= 0 && std::none_of(std::begin(held_), std::end(held_), [](bool h) { return h; }))
    followUntilMs_ = nowMs_ + kFollowHoldMs;
  changed_ = true;
}

void SplitKeyboard::updateBass() {
  const Bass& bass = layout_.bass;
  const int low = lowestHeldUpTo(bass.topKey);
  // A key that gave the bass and no longer does, still held, plays its
  // manual again: a chord pressed from the top down keeps its notes.
  if (pedalKey_ >= 0 && pedalKey_ != low && held_[pedalKey_] && sent_[pedalKey_].silentOnManual)
    soundOnManual(pedalKey_);
  pedalKey_ = low;
  if (low >= 0) {
    pedalHeld_ = true;
    pedalUntilMs_ = -1.0;
    const int target = low + layout_.transpose;
    if (target < 0 || target > 127) return;
    // A new pedal note only when the lowest key changes.
    if (target != pedalNote_) {
      pedalOff();
      pedalOn(target, velocity_[low]);
    }
    return;
  }
  pedalHeld_ = false;
  switch (bass.sustain) {
    case Sustain::Off: pedalOff(); break;
    case Sustain::UntilNext: break;
    case Sustain::Timed: pedalUntilMs_ = nowMs_ + bass.sustainMs; break;
  }
}

void SplitKeyboard::pedalOn(int note, int velocity) {
  sendOn(Role::Pedal, note, velocity);
  pedalNote_ = note;
}

void SplitKeyboard::pedalOff() {
  if (pedalNote_ < 0) return;
  sendOff(Role::Pedal, pedalNote_);
  pedalNote_ = -1;
  pedalUntilMs_ = -1.0;
}

void SplitKeyboard::followHands() {
  if (layout_.splitCount < 1) return;
  // The widest gap between two keys held next to each other.
  int prev = -1, gapLow = -1, gapHigh = -1;
  for (int n = 0; n < 128; ++n) {
    if (!held_[n]) continue;
    if (prev >= 0 && n - prev > 12 && (gapLow < 0 || n - prev > gapHigh - gapLow)) {
      gapLow = prev;
      gapHigh = n;
    }
    prev = n;
  }
  if (gapLow < 0) return;  // one hand: the split stays where it is
  int split = gapLow + 1 + (gapHigh - gapLow) / 2;
  if (layout_.splitCount >= 2) split = std::min(split, layout_.splits[1] - 1);
  split = clampInt(split, 1, 127);
  if (split != followedSplit_) {
    followedSplit_ = split;
    changed_ = true;
  }
}

void SplitKeyboard::sendOn(Role r, int note, int velocity) {
  const int idx = static_cast<int>(r);
  if (count_[idx][note]++ == 0)
    emit(static_cast<uint8_t>(0x90 | (channelOf(r) - 1)), static_cast<uint8_t>(note),
         static_cast<uint8_t>(clampInt(velocity, 1, 127)));
}

void SplitKeyboard::sendOff(Role r, int note) {
  const int idx = static_cast<int>(r);
  if (count_[idx][note] == 0) return;
  if (--count_[idx][note] == 0)
    emit(static_cast<uint8_t>(0x80 | (channelOf(r) - 1)), static_cast<uint8_t>(note), 0);
}

void SplitKeyboard::emit(uint8_t status, uint8_t d1, uint8_t d2) {
  if (outCount_ < kOutCapacity) out_[outCount_++] = Msg{status, d1, d2};
}

void SplitKeyboard::publish() {
  State& s = state_;
  s.enabled = enabled_;
  s.layout = layout_;
  s.preset = preset_;
  s.firstSplit = followedSplit_ >= 0 ? followedSplit_ : layout_.splits[0];
  s.pedalNote = pedalNote_;
  s.held[0] = s.held[1] = 0;
  for (int n = 0; n < 128; ++n) {
    if (held_[n]) s.held[n / 64] |= uint64_t{1} << (n % 64);
    s.zoneOfKey[n] = static_cast<uint8_t>(zoneOfKey_[n] + 1);
  }
  std::copy(triggers_.begin(), triggers_.end(), s.triggers);
  s.layoutChanges = layoutChanges_;
  s.triggerChanges = triggerChanges_;
  s.learning = learning_;
  stateBox_.put(s);
  changed_ = false;
}

}  // namespace mp::split
