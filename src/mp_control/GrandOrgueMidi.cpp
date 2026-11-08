#include "GrandOrgueMidi.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace mp {

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// The file's sections in order, each with its keys.
struct Section {
  std::string name;
  std::unordered_map<std::string, std::string> keys;

  bool has(const std::string& key) const { return keys.count(key) != 0; }
  std::string str(const std::string& key, const std::string& fallback = {}) const {
    const auto it = keys.find(key);
    return it == keys.end() ? fallback : it->second;
  }
  int num(const std::string& key, int fallback) const {
    const auto it = keys.find(key);
    if (it == keys.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    return end == it->second.c_str() ? fallback : static_cast<int>(v);
  }
};

std::vector<Section> readSections(const std::string& text) {
  std::vector<Section> out;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first && line.rfind("\xef\xbb\xbf", 0) == 0) line.erase(0, 3);
    first = false;
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      out.push_back({line.substr(1, line.size() - 2), {}});
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos || out.empty()) continue;
    out.back().keys[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return out;
}

std::string n3(int i) {
  char b[8];
  std::snprintf(b, sizeof b, "%03d", i);
  return b;
}

// GrandOrgue's channel: 1 to 16, or -1 for any.
int channelOf(int goChannel) { return goChannel >= 1 && goChannel <= 16 ? goChannel : 0; }

// A device as GrandOrgue names it, with the system's decorations taken off:
// "winmm: " in front from its older Windows builds, the ALSA port " 20:0" or
// " CA3DC798:0" after it on Linux.
std::string plainDeviceName(std::string name) {
  const size_t colon = name.find(": ");
  if (colon != std::string::npos && colon > 0 &&
      std::all_of(name.begin(), name.begin() + static_cast<long>(colon),
                  [](unsigned char c) { return std::isalpha(c) != 0; }))
    name = name.substr(colon + 2);
  const size_t space = name.rfind(' ');
  if (space != std::string::npos) {
    const std::string tail = name.substr(space + 1);
    const size_t c = tail.find(':');
    const bool port = c != std::string::npos && c > 0 && c + 1 < tail.size() &&
                      std::all_of(tail.begin(), tail.begin() + static_cast<long>(c),
                                  [](unsigned char ch) { return std::isxdigit(ch) != 0; }) &&
                      std::all_of(tail.begin() + static_cast<long>(c) + 1, tail.end(),
                                  [](unsigned char ch) { return std::isdigit(ch) != 0; });
    if (port) name = name.substr(0, space);
  }
  return trim(name);
}

// A console this map knows by the same name, or 0: a device GrandOrgue named
// that is not known here is taken as any console, so the events work with
// whatever the keyboard is called on this computer.
int deviceOf(const std::string& goName, MidiMap& map, std::set<std::string>& unknown) {
  const std::string plain = lower(plainDeviceName(goName));
  if (plain.empty()) return 0;
  for (const std::string& known : map.devices().names()) {
    const std::string k = lower(known);
    if (MidiDeviceMap::sameConsole(k, plain) || k.find(plain) != std::string::npos ||
        plain.find(k) != std::string::npos)
      return map.devices().idFor(known);
  }
  unknown.insert(plain);
  return 0;
}

// One receive event, as GrandOrgue's GOMidiReceiver::Load reads it.
struct Event {
  std::string type;
  std::string device;
  int channel = -1;
  int key = 0;
  int low = 1, high = 127;
  int lowKey = 0, highKey = 127;
};

Event readEvent(const Section& s, int i, const std::string& fallbackType, bool manual) {
  const std::string k = n3(i);
  Event e;
  e.type = s.str("MIDIEventType" + k, fallbackType);
  e.device = s.str("MIDIDevice" + k);
  e.channel = s.num("MIDIChannel" + k, -1);
  e.key = s.num(manual ? "MIDIKeyShift" + k : "MIDIKey" + k, 0);
  e.low = s.num("MIDILowerLimit" + k, s.num("MIDILowerVelocity" + k, 1));
  e.high = s.num("MIDIUpperLimit" + k, s.num("MIDIUpperVelocity" + k, 127));
  e.lowKey = s.num("MIDILowerKey" + k, 0);
  e.highKey = s.num("MIDIUpperKey" + k, 127);
  return e;
}

// A switch event: GrandOrgue's drawstop or button receiver. A note or a
// controller with an off and an on value follows them as a held binding
// does; the "On" kinds flip it at the on value; a program change flips it.
// The kinds that act on a release only, on both edges, or on exactly one
// value have no binding of their own here yet.
std::optional<MidiBinding> switchBinding(const Event& e, int device) {
  MidiBinding b;
  b.targetKind = MidiTargetKind::Switch;
  b.source.channel = channelOf(e.channel);
  b.source.deviceId = device;
  b.source.number = e.key;
  // The press from `at` up, through isOn's half-way rule.
  auto pressAt = [&](int at) {
    at = std::clamp(at, 1, 127);
    b.lowValue = at - 1;
    b.highValue = at;
  };
  if (e.type == "Note" || e.type == "ControlChange") {
    if (e.low > e.high) return std::nullopt;
    b.source.kind = e.type == "Note" ? MidiSourceKind::Note : MidiSourceKind::ControlChange;
    b.trigger = MidiTrigger::Momentary;
    b.lowValue = e.low;
    b.highValue = e.high;
  } else if (e.type == "NoteOn" || e.type == "ControlChangeOn") {
    b.source.kind = e.type == "NoteOn" ? MidiSourceKind::Note : MidiSourceKind::ControlChange;
    b.trigger = MidiTrigger::Toggle;
    pressAt(e.high);
  } else if (e.type == "ProgramChange") {
    // GrandOrgue counts programs from 1, and a bank in the bits above.
    if (e.key < 1 || e.key > 128) return std::nullopt;
    b.source.kind = MidiSourceKind::ProgramChange;
    b.source.number = e.key - 1;
    b.trigger = MidiTrigger::Toggle;
  } else {
    return std::nullopt;
  }
  if (b.source.number < 0 || b.source.number > 127) return std::nullopt;
  b.latching = b.trigger == MidiTrigger::Toggle;
  return b;
}

// A swell shoe: a controller across its window, the other way round when
// GrandOrgue's lower limit is the higher number.
std::optional<MidiBinding> controlBinding(const Event& e, int device) {
  if (e.type != "ControlChange" || e.key < 0 || e.key > 127) return std::nullopt;
  MidiBinding b;
  b.targetKind = MidiTargetKind::ContinuousControl;
  b.source.kind = MidiSourceKind::ControlChange;
  b.source.channel = channelOf(e.channel);
  b.source.deviceId = device;
  b.source.number = e.key;
  b.lowValue = std::clamp(std::min(e.low, e.high), 0, 127);
  b.highValue = std::clamp(std::max(e.low, e.high), 0, 127);
  b.invert = e.low > e.high;
  return b;
}

std::optional<MidiMap::KeyboardBinding> keyboardBinding(const Event& e, int device) {
  if (e.type != "Note" && e.type != "NoteNoVelocity" && e.type != "NoteShortOctave" && e.type != "NoteNormal")
    return std::nullopt;
  MidiMap::KeyboardBinding k;
  k.deviceId = device;
  k.channel = channelOf(e.channel);
  k.lowKey = std::clamp(e.lowKey, 0, 127);
  k.highKey = std::clamp(e.highKey, 0, 127);
  k.transpose = e.key;
  k.lowVelocity = std::clamp(e.low, 0, 127);
  k.highVelocity = std::clamp(e.high, 0, 127);
  k.ignoreVelocity = e.type == "NoteNoVelocity";
  k.shortOctave = e.type == "NoteShortOctave";
  return k;
}

// A message sent, as GrandOrgue's GOMidiSender::Load reads it: a lamp's note
// or controller at its on and off values, or one sent only when the object
// comes on.
std::optional<MidiSend> sendOf(const Section& s, int i, bool control) {
  const std::string k = n3(i);
  const std::string type = s.str("MIDISendEventType" + k, "Note");
  MidiSend m;
  m.channel = std::clamp(s.num("MIDISendChannel" + k, 1), 1, 16);
  m.number = s.num("MIDISendKey" + k, 0);
  m.low = std::clamp(s.num("MIDISendLowValue" + k, 0), 0, 127);
  m.high = std::clamp(s.num("MIDISendHighValue" + k, 127), 0, 127);
  const std::string off = lower(s.str("MIDISendNoteOff" + k));
  m.noteOff = off == "y" || off == "1" || off == "true";
  // The output it goes to, by the port's name with GrandOrgue's decorations
  // taken off (outputForSend); none for every output.
  m.device = plainDeviceName(s.str("MIDISendDevice" + k));
  if (type == "Note" || type == "NoteOn") {
    if (control) return std::nullopt;
    m.kind = MidiSourceKind::Note;
    if (type == "NoteOn") m.low = -1;
  } else if (type == "ControlChange" || type == "ControlOn") {
    m.kind = MidiSourceKind::ControlChange;
    if (type == "ControlOn") {
      if (control) return std::nullopt;
      m.low = -1;
    }
  } else if (type == "ProgramOn") {
    if (control || m.number < 1 || m.number > 128) return std::nullopt;
    m.kind = MidiSourceKind::ProgramChange;
    m.number -= 1;
    m.offProgram = -1;
  } else {
    return std::nullopt;
  }
  if (m.number < 0 || m.number > 127) return std::nullopt;
  return m;
}

}  // namespace

GrandOrgueMidiReport importGrandOrgueMidi(const std::string& text, const OrganModel& model, MidiMap& map) {
  GrandOrgueMidiReport report;
  const std::vector<Section> sections = readSections(text);
  for (const Section& s : sections) {
    if (s.name == "Organ") report.organName = s.str("ChurchName");
    for (const auto& kv : s.keys)
      if (kv.first == "NumberOfMIDIEvents" || kv.first == "NumberOfMIDISendEvents") report.ok = true;
  }
  if (!report.ok) return report;

  std::set<std::string> unknownDevices;
  for (const Section& s : sections) {
    const int received = std::clamp(s.num("NumberOfMIDIEvents", 0), 0, 255);
    const int sent = std::clamp(s.num("NumberOfMIDISendEvents", 0), 0, 255);
    if (s.num("ShortcutKey", 0) != 0) ++report.shortcuts;
    if (received == 0 && sent == 0) continue;
    const auto found = model.grandOrgueObjects.find(lower(s.name));
    if (found == model.grandOrgueObjects.end()) {
      ++report.otherObjects;
      continue;
    }
    const OrganModel::GrandOrgueObject object = found->second;

    if (object.kind == OrganModel::GrandOrgueObject::Keyboard) {
      if (model.keyboards.count(object.id) == 0) {
        ++report.otherObjects;
        continue;
      }
      std::vector<MidiMap::KeyboardBinding> taken;
      for (int i = 1; i <= received; ++i) {
        const Event e = readEvent(s, i, "Note", true);
        auto k = keyboardBinding(e, deviceOf(e.device, map, unknownDevices));
        if (!k) {
          ++report.leftOut;
          continue;
        }
        k->keyboardId = object.id;
        taken.push_back(*k);
      }
      // A manual's notes going out again: not taken yet.
      report.leftOut += sent;
      if (!taken.empty()) {
        map.removeKeyboardBindingsFor(object.id);
        for (const auto& k : taken) map.addKeyboardBinding(k);
        report.keyboards += static_cast<int>(taken.size());
      }
      continue;
    }

    const bool control = object.kind == OrganModel::GrandOrgueObject::Control;
    const MidiTargetKind kind = control ? MidiTargetKind::ContinuousControl : MidiTargetKind::Switch;
    if ((control && model.continuousControls.count(object.id) == 0) ||
        (!control && model.switches.count(object.id) == 0)) {
      ++report.otherObjects;
      continue;
    }
    std::vector<MidiBinding> bindings;
    for (int i = 1; i <= received; ++i) {
      const Event e = readEvent(s, i, control ? "ControlChange" : "ProgramChange", false);
      const int device = deviceOf(e.device, map, unknownDevices);
      auto b = control ? controlBinding(e, device) : switchBinding(e, device);
      // The same message twice for one object is one binding here.
      const bool twice = b && std::any_of(bindings.begin(), bindings.end(),
                                          [&](const MidiBinding& o) { return o.source == b->source; });
      if (!b || twice) {
        ++report.leftOut;
        continue;
      }
      b->targetId = object.id;
      bindings.push_back(*b);
    }
    if (!bindings.empty()) {
      map.setBindingsFor(kind, object.id, bindings);
      report.bindings += static_cast<int>(bindings.size());
    }
    std::vector<MidiSend> sends;
    for (int i = 1; i <= sent; ++i) {
      auto m = sendOf(s, i, control);
      if (!m) {
        ++report.leftOut;
        continue;
      }
      m->targetKind = kind;
      m->targetId = object.id;
      sends.push_back(*m);
    }
    if (!sends.empty()) {
      map.setSendsFor(kind, object.id, sends);
      report.sends += static_cast<int>(sends.size());
    }
  }
  report.anyDevice = static_cast<int>(unknownDevices.size());
  return report;
}

}  // namespace mp
