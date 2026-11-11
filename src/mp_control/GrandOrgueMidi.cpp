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
  char b[16];
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

// What a section's events work here: an object of the organ, or a button,
// display or lamp of GrandOrgue's own panel.
struct Target {
  enum Role { Press, Control, Keyboard, Display, Lamp };
  MidiTargetKind kind = MidiTargetKind::None;
  Id id = 0;
  Role role = Press;
};

// GrandOrgue's panel by its section names (GOSetter, GOMetronome), lower
// case. The crescendo banks, the file buttons, frame insertion and deletion,
// the hundred-frame steps and the general banks have no counterpart and are
// left out.
std::optional<Target> panelTarget(const std::string& name) {
  using K = MidiTargetKind;
  static const std::map<std::string, Target> fixed = {
      {"setterprev", {K::StepperPrev, 0, Target::Press}},
      {"setternext", {K::StepperNext, 0, Target::Press}},
      {"setterm1", {K::StepperPrev, 0, Target::Press}},
      {"setterp1", {K::StepperNext, 0, Target::Press}},
      {"setterm10", {K::StepperBack10, 0, Target::Press}},
      {"setterp10", {K::StepperForward10, 0, Target::Press}},
      {"setterhome", {K::StepperFirst, 0, Target::Press}},
      {"settercurrent", {K::StepperCurrent, 0, Target::Press}},
      {"setterset", {K::Setter, kSetterTarget, Target::Press}},
      {"settergc", {K::PlayerGeneralCancel, 0, Target::Press}},
      {"settertemperamentnext", {K::TemperamentNext, 0, Target::Press}},
      {"settertemperamentprev", {K::TemperamentPrev, 0, Target::Press}},
      {"settertransposeup", {K::TransposeUp, 0, Target::Press}},
      {"settertransposedown", {K::TransposeDown, 0, Target::Press}},
      {"setterpitchm1", {K::PitchStep, -1, Target::Press}},
      {"setterpitchm10", {K::PitchStep, -10, Target::Press}},
      {"setterpitchm100", {K::PitchStep, -100, Target::Press}},
      {"setterpitchp1", {K::PitchStep, 1, Target::Press}},
      {"setterpitchp10", {K::PitchStep, 10, Target::Press}},
      {"setterpitchp100", {K::PitchStep, 100, Target::Press}},
      {"audiopanic", {K::Panic, 0, Target::Press}},
      {"exitgo", {K::QuitProgram, 0, Target::Press}},
      {"settermastervolume", {K::MasterVolume, 0, Target::Control}},
      {"metronomeon", {K::MetronomeOn, kMetronomeTarget, Target::Press}},
      {"metronomemp1", {K::MetronomeBeats, 1, Target::Press}},
      {"metronomemm1", {K::MetronomeBeats, -1, Target::Press}},
      {"metronomebpmp1", {K::MetronomeTempo, 1, Target::Press}},
      {"metronomebpmm1", {K::MetronomeTempo, -1, Target::Press}},
      {"metronomebpmp10", {K::MetronomeTempo, 10, Target::Press}},
      {"metronomebpmm10", {K::MetronomeTempo, -10, Target::Press}},
      {"settername", {K::ConsoleDisplay, kTextOrganName, Target::Display}},
      {"settermastertemperament", {K::ConsoleDisplay, kTextTemperament, Target::Display}},
      {"settermasterpitch", {K::ConsoleDisplay, kTextPitch, Target::Display}},
      {"settertranspose", {K::ConsoleDisplay, kTextTranspose, Target::Display}},
      {"settercurrentposition", {K::ConsoleDisplay, kTextFrame, Target::Display}},
      {"metronomebpm", {K::ConsoleDisplay, kTextMetronomeTempo, Target::Display}},
      {"metronomemeasure", {K::ConsoleDisplay, kTextMetronomeBeats, Target::Display}},
      {"setteron", {K::None, 0, Target::Lamp}},
  };
  if (const auto it = fixed.find(name); it != fixed.end()) return it->second;
  // SetterL0 to SetterL9, the digits; SetterGeneral00 to 49, the generals.
  auto digitsAfter = [&](const std::string& prefix, size_t count) -> int {
    if (name.size() != prefix.size() + count || name.rfind(prefix, 0) != 0) return -1;
    int n = 0;
    for (size_t i = prefix.size(); i < name.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(name[i]))) return -1;
      n = n * 10 + (name[i] - '0');
    }
    return n;
  };
  if (const int d = digitsAfter("setterl", 1); d >= 0) return Target{K::StepperDigit, d, Target::Press};
  if (const int g = digitsAfter("settergeneral", 2); g >= 0 && g < 50)
    return Target{K::PlayerGeneral, g + 1, Target::Press};
  return std::nullopt;
}

// Whether a target follows its messages as a drawstop does, on and off, or
// fires on a press and ignores the rest.
bool latches(MidiTargetKind k) {
  return k == MidiTargetKind::Switch || k == MidiTargetKind::Setter ||
         k == MidiTargetKind::PedalCombinationsCoupled || k == MidiTargetKind::MetronomeOn;
}

// One receive event, as GrandOrgue's GOMidiReceiver::Load reads it.
struct Event {
  std::string type;
  std::string device;
  int channel = -1;
  int key = 0;
  int low = 1, high = 127;
  int lowKey = 0, highKey = 127;
  int debounce = 0;
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
  e.debounce = std::clamp(s.num("MIDIDebounce" + k, 0), 0, 3000);
  return e;
}

// A system exclusive message's body, F0 and F7 left off, as a source.
MidiSource sysExSource(const std::vector<uint8_t>& body, int device) {
  MidiSource src;
  src.kind = MidiSourceKind::SysEx;
  src.number = sysExId(body.data(), static_cast<int>(body.size()));
  src.deviceId = device;
  return src;
}

// The bindings for a press event: a drawstop's, a piston's, a panel
// button's. Each GrandOrgue kind as GOMidiReceiver::Match reads it. Empty
// when the kind has none here.
std::vector<MidiBinding> pressBindings(const Event& e, int device, MidiTargetKind kind, Id id) {
  const std::string& t = e.type;
  MidiBinding b;
  b.targetKind = kind;
  b.targetId = id;
  b.source.channel = channelOf(e.channel);
  b.source.deviceId = device;
  b.source.number = e.key;
  b.debounceMs = e.debounce;
  const bool latching = latches(kind);
  // From `at` up, through isOn's half-way rule.
  auto pressAt = [](MidiBinding& row, int at) {
    at = std::clamp(at, 1, 127);
    row.lowValue = at - 1;
    row.highValue = at;
  };
  // At `at` and below.
  auto releaseAt = [](MidiBinding& row, int at) {
    at = std::clamp(at, 0, 126);
    row.lowValue = at + 1;
    row.highValue = at;
  };
  auto sourceKind = [&](const std::string& base) {
    if (base == "Note") return MidiSourceKind::Note;
    if (base == "ControlChange") return MidiSourceKind::ControlChange;
    if (base == "RPN") return MidiSourceKind::Rpn;
    if (base == "NRPN") return MidiSourceKind::Nrpn;
    return MidiSourceKind::None;
  };
  auto ends = [&](const std::string& suffix) {
    return t.size() > suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  const int keyLimit = (t.rfind("RPN", 0) == 0 || t.rfind("NRPN", 0) == 0) ? 16383 : 127;
  const bool fixed = t.find("Fixed") != std::string::npos;
  std::vector<MidiBinding> out;

  // Held: on at the upper limit, off at the lower, either way round.
  if (t == "Note" || t == "ControlChange" || t == "RPN" || t == "NRPN") {
    b.source.kind = sourceKind(t);
    b.trigger = MidiTrigger::Momentary;
    b.lowValue = e.low;
    b.highValue = e.high;
    b.debounceMs = t == "ControlChange" ? e.debounce : 0;
    out.push_back(b);
  } else if (fixed) {
    // One value each: a console that sends one controller for many tabs.
    if (!latching) return {};
    b.source.kind = t.rfind("Note", 0) == 0 ? MidiSourceKind::Note : MidiSourceKind::ControlChange;
    b.exactValues = true;
    if (t == "ControlChangeFixed") {
      b.trigger = MidiTrigger::Momentary;
      b.lowValue = e.low;
      b.highValue = e.high;
    } else if (ends("FixedOnOff")) {
      b.trigger = MidiTrigger::ToggleBoth;
      b.lowValue = e.low;
      b.highValue = e.high;
    } else if (ends("FixedOn")) {
      b.trigger = MidiTrigger::EngageOnly;
      b.lowValue = -1;
      b.highValue = e.high;
    } else if (ends("FixedOff")) {
      b.trigger = MidiTrigger::DisengageOnly;
      b.lowValue = -1;
      b.highValue = e.low;
    } else {
      return {};
    }
    out.push_back(b);
  } else if (ends("OnOff")) {
    // A change at the upper limit and at the lower: each flips it.
    b.source.kind = sourceKind(t.substr(0, t.size() - 5));
    if (!latching) return {};
    b.trigger = MidiTrigger::ToggleBoth;
    b.lowValue = e.low;
    b.highValue = e.high;
    out.push_back(b);
  } else if (ends("On")) {
    b.source.kind = sourceKind(t.substr(0, t.size() - 2));
    b.trigger = MidiTrigger::Toggle;
    pressAt(b, e.high);
    out.push_back(b);
  } else if (ends("Off")) {
    b.source.kind = sourceKind(t.substr(0, t.size() - 3));
    if (!latching) return {};
    b.trigger = MidiTrigger::Toggle;
    releaseAt(b, e.low);
    out.push_back(b);
  } else if (t == "ControlChangeBitfield") {
    if (!latching || e.low < 0 || e.low > 6) return {};
    b.source.kind = MidiSourceKind::ControlChange;
    b.trigger = MidiTrigger::Momentary;
    b.bit = e.low;
    out.push_back(b);
  } else if (t == "ProgramChange") {
    // GrandOrgue counts programs from 1, with the bank in the bits above.
    b.source.kind = MidiSourceKind::ProgramChange;
    b.source.number = e.key - 1;
    b.trigger = MidiTrigger::Toggle;
    if (b.source.number < 0 || b.source.number >= (1 << 21)) return {};
    out.push_back(b);
    return out;
  } else if (t == "ProgramRange") {
    // The upper program draws it, the lower one cancels it.
    b.source.kind = MidiSourceKind::ProgramChange;
    b.trigger = latching ? MidiTrigger::EngageOnly : MidiTrigger::Momentary;
    b.source.number = e.high - 1;
    if (b.source.number < 0 || b.source.number >= (1 << 21)) return {};
    out.push_back(b);
    if (latching && e.low >= 1 && e.low != e.high) {
      MidiBinding off = b;
      off.trigger = MidiTrigger::DisengageOnly;
      off.source.number = e.low - 1;
      out.push_back(off);
    }
    return out;
  } else if (t == "RPNRange" || t == "NRPNRange") {
    // The value is the key; the upper parameter draws it, the lower cancels.
    b.source.kind = t == "RPNRange" ? MidiSourceKind::Rpn : MidiSourceKind::Nrpn;
    b.exactValues = true;
    b.lowValue = -1;
    b.highValue = std::clamp(e.key, 0, 127);
    b.trigger = latching ? MidiTrigger::EngageOnly : MidiTrigger::Momentary;
    b.source.number = std::clamp(e.high, 0, 16383);
    if (!latching) b.exactValues = false;
    out.push_back(b);
    if (latching && e.low != e.high) {
      MidiBinding off = b;
      off.trigger = MidiTrigger::DisengageOnly;
      off.source.number = std::clamp(e.low, 0, 16383);
      out.push_back(off);
    }
    return out;
  } else if (t == "SysExJohannus") {
    b.source = sysExSource({0x00, 0x4A, 0x4F, 0x48, 0x41, 0x53, static_cast<uint8_t>(e.key & 127)}, device);
    b.trigger = MidiTrigger::Toggle;
    out.push_back(b);
    return out;
  } else if (t == "SysExJohannus11") {
    // Each value in the window is a press.
    const int lo = std::clamp(e.low, 0, 127), hi = std::clamp(e.high, 0, 127);
    for (int v = lo; v <= hi; ++v) {
      MidiBinding row = b;
      row.source = sysExSource({0x00, 0x4A, 0x4F, 0x48, 0x41, 0x53, static_cast<uint8_t>(v), 0x10,
                                static_cast<uint8_t>(e.key & 127)},
                               device);
      row.trigger = MidiTrigger::Toggle;
      out.push_back(row);
    }
    return out;
  } else if (t == "SysExViscount" || t == "SysExViscountToggle") {
    auto body = [](int v) {
      return std::vector<uint8_t>{0x31, static_cast<uint8_t>((v >> 14) & 127), static_cast<uint8_t>((v >> 7) & 127),
                                  static_cast<uint8_t>(v & 127)};
    };
    if (t == "SysExViscountToggle") {
      b.source = sysExSource(body(e.low), device);
      b.trigger = MidiTrigger::Toggle;
      out.push_back(b);
      return out;
    }
    b.source = sysExSource(body(e.high), device);
    b.trigger = latching ? MidiTrigger::EngageOnly : MidiTrigger::Momentary;
    out.push_back(b);
    if (latching) {
      MidiBinding off = b;
      off.source = sysExSource(body(e.low), device);
      off.trigger = MidiTrigger::DisengageOnly;
      out.push_back(off);
    }
    return out;
  } else if (t == "SysExAhlbornGalanti" || t == "SysExAhlbornGalantiToggle") {
    auto body = [](int v) {
      return std::vector<uint8_t>{0x35, static_cast<uint8_t>((v >> 7) & 127), static_cast<uint8_t>(v & 127)};
    };
    const bool toggle = t == "SysExAhlbornGalantiToggle";
    b.source = sysExSource(body(e.high), device);
    b.trigger = toggle ? MidiTrigger::Toggle : latching ? MidiTrigger::EngageOnly : MidiTrigger::Momentary;
    out.push_back(b);
    if (toggle || latching) {
      MidiBinding off = b;
      off.source = sysExSource(body(e.low), device);
      off.trigger = toggle ? MidiTrigger::Toggle : MidiTrigger::DisengageOnly;
      out.push_back(off);
    }
    return out;
  } else {
    return {};
  }
  // The note, controller and parameter kinds: a number in range.
  for (const MidiBinding& row : out)
    if (row.source.kind == MidiSourceKind::None || row.source.number < 0 || row.source.number > keyLimit) return {};
  // A button that fires: a press only, at any value above nothing.
  if (!latching)
    for (MidiBinding& row : out)
      if (row.trigger != MidiTrigger::Momentary && row.trigger != MidiTrigger::Toggle) return {};
  return out;
}

// A shoe: a controller or parameter across its window, the other way round
// when GrandOrgue's lower limit is the higher number; or a range of programs.
std::optional<MidiBinding> controlBinding(const Event& e, int device, MidiTargetKind kind, Id id) {
  MidiBinding b;
  b.targetKind = kind;
  b.targetId = id;
  b.source.channel = channelOf(e.channel);
  b.source.deviceId = device;
  b.source.number = e.key;
  if (e.type == "ProgramRange") {
    if (e.low < 1 || e.high < 1) return std::nullopt;
    b.source.kind = MidiSourceKind::ProgramChange;
    b.source.number = kProgramRange;
    b.lowValue = std::min(e.low, e.high) - 1;
    b.highValue = std::max(e.low, e.high) - 1;
    b.invert = e.low > e.high;
    return b;
  }
  if (e.type == "ControlChange") b.source.kind = MidiSourceKind::ControlChange;
  else if (e.type == "RPN") b.source.kind = MidiSourceKind::Rpn;
  else if (e.type == "NRPN") b.source.kind = MidiSourceKind::Nrpn;
  else return std::nullopt;
  const int keyLimit = b.source.kind == MidiSourceKind::ControlChange ? 127 : 16383;
  if (e.key < 0 || e.key > keyLimit) return std::nullopt;
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

// One send event, as GrandOrgue's GOMidiSender::Load reads it.
struct SendEvent {
  std::string type;
  int channel = 1;
  int key = 0;
  int low = 0, high = 127;
  int start = 0, length = 0;
  bool noteOff = false;
  std::string device;
};

SendEvent readSend(const Section& s, int i) {
  const std::string k = n3(i);
  SendEvent e;
  e.type = s.str("MIDISendEventType" + k, "Note");
  e.channel = std::clamp(s.num("MIDISendChannel" + k, 1), 1, 16);
  e.key = s.num("MIDISendKey" + k, 0);
  e.low = s.num("MIDISendLowValue" + k, 0);
  e.high = s.num("MIDISendHighValue" + k, 127);
  const std::string off = lower(s.str("MIDISendNoteOff" + k));
  e.noteOff = off == "y" || off == "1" || off == "true";
  // The output it goes to, by the port's name with GrandOrgue's decorations
  // taken off (outputForSend); none for every output.
  e.device = plainDeviceName(s.str("MIDISendDevice" + k));
  return e;
}

std::vector<uint8_t> parameterBytes(bool registered, int channel, int parameter, int value) {
  const auto cc = static_cast<uint8_t>(0xB0 | ((channel - 1) & 15));
  const int p = std::clamp(parameter, 0, 16383);
  return {cc, static_cast<uint8_t>(registered ? 101 : 99), static_cast<uint8_t>((p >> 7) & 127),
          cc, static_cast<uint8_t>(registered ? 100 : 98), static_cast<uint8_t>(p & 127),
          cc, 6, static_cast<uint8_t>(std::clamp(value, 0, 127))};
}

std::vector<uint8_t> programBytes(int channel, int program) {
  const auto ch = static_cast<uint8_t>((channel - 1) & 15);
  std::vector<uint8_t> out;
  if (program > 127)
    out = {static_cast<uint8_t>(0xB0 | ch), 0, static_cast<uint8_t>(bankMsbPart(program)),
           static_cast<uint8_t>(0xB0 | ch), 32, static_cast<uint8_t>(bankLsbPart(program))};
  out.push_back(static_cast<uint8_t>(0xC0 | ch));
  out.push_back(static_cast<uint8_t>(programPart(program)));
  return out;
}

// The send for a drawstop, piston, panel button or lamp: its message as it
// comes on and as it goes off, in GrandOrgue's terms (GOMidiSender::SetDisplay).
std::optional<MidiSend> switchSend(const SendEvent& e) {
  const std::string& t = e.type;
  MidiSend m;
  m.channel = e.channel;
  m.number = e.key;
  m.low = std::clamp(e.low, 0, 127);
  m.high = std::clamp(e.high, 0, 127);
  m.noteOff = e.noteOff;
  m.device = e.device;
  // A whole message for each state, for the kinds the plain fields do not
  // hold: kept as hex.
  auto hex = [&](std::vector<uint8_t> on, std::vector<uint8_t> off) {
    m.kind = MidiSourceKind::SysEx;
    m.sysExOn = std::move(on);
    m.sysExOff = std::move(off);
    return std::optional<MidiSend>(m);
  };
  if (t == "Note" || t == "NoteNoVelocity" || t == "NoteOn" || t == "NoteOff") {
    m.kind = MidiSourceKind::Note;
    if (t == "NoteOn") m.low = -1;
    if (t == "NoteOff") m.high = -1;
  } else if (t == "ControlChange" || t == "ControlOn" || t == "ControlOff") {
    m.kind = MidiSourceKind::ControlChange;
    if (t == "ControlOn") m.low = -1;
    if (t == "ControlOff") m.high = -1;
  } else if (t == "RPN" || t == "NRPN" || t == "RPNOn" || t == "RPNOff" || t == "NRPNOn" || t == "NRPNOff") {
    m.kind = MidiSourceKind::ControlChange;
    m.parameter = t.rfind("RPN", 0) == 0 ? MidiParameter::Registered : MidiParameter::NonRegistered;
    if (t.size() > 3 && t.compare(t.size() - 2, 2, "On") == 0) m.low = -1;
    if (t.size() > 3 && t.compare(t.size() - 3, 3, "Off") == 0) m.high = -1;
    if (e.key < 0 || e.key > 16383) return std::nullopt;
    return m;
  } else if (t == "ProgramOn" || t == "ProgramOff" || t == "ProgramRange") {
    m.kind = MidiSourceKind::ProgramChange;
    const int on = t == "ProgramRange" ? e.high - 1 : e.key - 1;
    const int off = t == "ProgramRange" ? e.low - 1 : -1;
    if (on < 0 || on >= (1 << 21) || off >= (1 << 21)) return std::nullopt;
    if (t == "ProgramOff") return hex({}, programBytes(e.channel, on));
    m.number = on;
    m.offProgram = off < 0 ? -1 : off;
    return m;
  } else if (t == "RPNRange" || t == "NRPNRange") {
    // The parameter is the state, the value the key.
    const bool registered = t == "RPNRange";
    return hex(parameterBytes(registered, e.channel, e.high, e.key), parameterBytes(registered, e.channel, e.low, e.key));
  } else if (t == "HWNameLCD" || t == "HWNameString") {
    m.kind = MidiSourceKind::SysEx;
    m.display = t == "HWNameLCD" ? MidiDisplay::Lcd : MidiDisplay::Text;
    m.number = std::clamp(e.key, 0, t == "HWNameLCD" ? 16383 : 127);
    m.low = std::clamp(e.low, 0, 127);
    m.high = 0;
    return m;
  } else if (t == "HWLCD" || t == "HWString") {
    // "ON" and "OFF", as GrandOrgue writes a switch's state.
    MidiSend d;
    d.display = t == "HWLCD" ? MidiDisplay::Lcd : MidiDisplay::Text;
    d.number = std::clamp(e.key, 0, t == "HWLCD" ? 16383 : 127);
    d.low = std::clamp(e.low, 0, 127);
    return hex(MidiMap::displayBytes(d, "ON"), MidiMap::displayBytes(d, "OFF"));
  } else {
    return std::nullopt;
  }
  if (m.number < 0 || m.number > 127) return std::nullopt;
  return m;
}

// The send for a shoe or the master volume: its value across the window.
std::optional<MidiSend> controlSend(const SendEvent& e) {
  const std::string& t = e.type;
  MidiSend m;
  m.channel = e.channel;
  m.number = e.key;
  m.low = std::clamp(e.low, 0, 127);
  m.high = std::clamp(e.high, 0, 127);
  m.device = e.device;
  if (t == "ControlChange") {
    m.kind = MidiSourceKind::ControlChange;
    if (m.number < 0 || m.number > 127) return std::nullopt;
  } else if (t == "RPN" || t == "NRPN") {
    m.kind = MidiSourceKind::ControlChange;
    m.parameter = t == "RPN" ? MidiParameter::Registered : MidiParameter::NonRegistered;
    if (m.number < 0 || m.number > 16383) return std::nullopt;
  } else if (t == "ProgramRange") {
    if (e.low < 1 || e.high < 1) return std::nullopt;
    m.kind = MidiSourceKind::ProgramChange;
    m.number = 0;
    m.low = e.low - 1;
    m.high = e.high - 1;
  } else if (t == "HWLCD" || t == "HWString") {
    // The value as a percentage, as GrandOrgue writes it.
    m.kind = MidiSourceKind::SysEx;
    m.display = t == "HWLCD" ? MidiDisplay::Lcd : MidiDisplay::Text;
    m.number = std::clamp(e.key, 0, t == "HWLCD" ? 16383 : 127);
    m.low = std::clamp(e.low, 0, 127);
    m.high = 1;
  } else if (t == "HWNameLCD" || t == "HWNameString") {
    m.kind = MidiSourceKind::SysEx;
    m.display = t == "HWNameLCD" ? MidiDisplay::Lcd : MidiDisplay::Text;
    m.number = std::clamp(e.key, 0, t == "HWNameLCD" ? 16383 : 127);
    m.low = std::clamp(e.low, 0, 127);
    m.high = 0;
  } else {
    return std::nullopt;
  }
  return m;
}

// A display's send: the text the program shows there.
std::optional<MidiSend> displaySend(const SendEvent& e) {
  const std::string& t = e.type;
  const bool lcd = t == "HWLCD" || t == "HWNameLCD";
  if (!lcd && t != "HWString" && t != "HWNameString") return std::nullopt;
  MidiSend m;
  m.kind = MidiSourceKind::SysEx;
  m.display = lcd ? MidiDisplay::Lcd : MidiDisplay::Text;
  m.channel = e.channel;
  m.number = std::clamp(e.key, 0, lcd ? 16383 : 127);
  m.low = std::clamp(e.low, 0, 127);
  m.high = 1;
  m.device = e.device;
  return m;
}

// A manual's keys going out again as notes (GOMidiSender::SetKey).
std::optional<MidiSend> keyboardSend(const SendEvent& e) {
  if (e.type != "Note" && e.type != "NoteNoVelocity") return std::nullopt;
  MidiSend m;
  m.kind = MidiSourceKind::Note;
  m.channel = e.channel;
  m.number = 0;
  // At the velocity played, or at the fixed one.
  m.low = e.type == "NoteNoVelocity" ? std::clamp(e.high, 1, 127) : 0;
  m.high = 127;
  m.noteOff = true;
  m.device = e.device;
  return m;
}

}  // namespace

std::string grandOrgueKeyName(int code) {
  // GOKeyConvert's table, in the words of the program's shortcuts.
  static const std::map<int, std::string> named = {
      {8, "backspace"}, {9, "tab"}, {13, "return"}, {27, "escape"}, {32, "spacebar"},
      {33, "page up"}, {34, "page down"}, {35, "end"}, {36, "home"}, {37, "cursor left"},
      {38, "cursor up"}, {39, "cursor right"}, {40, "cursor down"}, {45, "insert"}, {46, "delete"},
      {106, "numpad *"}, {107, "numpad +"}, {108, "numpad separator"}, {109, "numpad -"},
      {110, "numpad ."}, {111, "numpad /"}, {186, ";"}, {187, "="}, {188, ","}, {189, "-"},
      {190, "."}, {191, "/"}, {192, "`"}, {219, "["}, {220, "\\"}, {221, "]"}, {222, "'"},
  };
  if (const auto it = named.find(code); it != named.end()) return it->second;
  if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z')) return std::string(1, static_cast<char>(code));
  if (code >= 96 && code <= 105) return "numpad " + std::to_string(code - 96);
  if (code >= 112 && code <= 135) return "F" + std::to_string(code - 111);
  return {};
}

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
    const int shortcutKey = s.num("ShortcutKey", 0);
    const int plusKey = s.num("PlusKey", 0), minusKey = s.num("MinusKey", 0);
    if (received == 0 && sent == 0 && shortcutKey == 0 && plusKey == 0 && minusKey == 0) continue;

    // The organ's object of that name, or GrandOrgue's panel.
    std::optional<Target> target;
    if (const auto found = model.grandOrgueObjects.find(lower(s.name)); found != model.grandOrgueObjects.end()) {
      const OrganModel::GrandOrgueObject& object = found->second;
      if (object.kind == OrganModel::GrandOrgueObject::Keyboard && model.keyboards.count(object.id) != 0)
        target = Target{MidiTargetKind::Keyboard, object.id, Target::Keyboard};
      else if (object.kind == OrganModel::GrandOrgueObject::Control && model.continuousControls.count(object.id) != 0)
        target = Target{MidiTargetKind::ContinuousControl, object.id, Target::Control};
      else if (object.kind == OrganModel::GrandOrgueObject::Switch && model.switches.count(object.id) != 0)
        target = Target{MidiTargetKind::Switch, object.id, Target::Press};
    } else {
      target = panelTarget(lower(s.name));
    }
    if (!target) {
      if (received > 0 || sent > 0) ++report.otherObjects;
      continue;
    }

    if (target->role == Target::Keyboard) {
      std::vector<MidiMap::KeyboardBinding> taken;
      for (int i = 1; i <= received; ++i) {
        const Event e = readEvent(s, i, "Note", true);
        auto k = keyboardBinding(e, deviceOf(e.device, map, unknownDevices));
        if (!k) {
          ++report.leftOut;
          continue;
        }
        k->keyboardId = target->id;
        taken.push_back(*k);
      }
      if (!taken.empty()) {
        map.removeKeyboardBindingsFor(target->id);
        for (const auto& k : taken) map.addKeyboardBinding(k);
        report.keyboards += static_cast<int>(taken.size());
      }
      std::vector<MidiSend> sends;
      for (int i = 1; i <= sent; ++i) {
        auto m = keyboardSend(readSend(s, i));
        if (!m) {
          ++report.leftOut;
          continue;
        }
        m->targetKind = MidiTargetKind::Keyboard;
        m->targetId = target->id;
        sends.push_back(*m);
      }
      if (!sends.empty()) {
        map.setSendsFor(MidiTargetKind::Keyboard, target->id, sends);
        report.sends += static_cast<int>(sends.size());
      }
      continue;
    }

    // GrandOrgue's On lamp: lit while an organ is ready to play, which is
    // the ready lamp's job here. Its received events work nothing in
    // GrandOrgue either.
    if (target->role == Target::Lamp) {
      std::vector<uint8_t> lit, dark;
      for (int i = 1; i <= sent; ++i) {
        auto m = switchSend(readSend(s, i));
        if (!m) {
          ++report.leftOut;
          continue;
        }
        m->targetKind = MidiTargetKind::Switch;
        const auto on = MidiMap::sendBytes(*m, 1), off = MidiMap::sendBytes(*m, 0);
        lit.insert(lit.end(), on.begin(), on.end());
        dark.insert(dark.end(), off.begin(), off.end());
        ++report.sends;
      }
      if (!lit.empty() || !dark.empty()) {
        report.readyLampLit = midiText(lit);
        report.readyLampDark = midiText(dark);
      }
      continue;
    }

    const bool control = target->role == Target::Control;
    if (target->role != Target::Display) {
      std::vector<MidiBinding> bindings;
      int events = 0;
      for (int i = 1; i <= received; ++i) {
        const Event e = readEvent(s, i, control ? "ControlChange" : "ProgramChange", false);
        const int device = deviceOf(e.device, map, unknownDevices);
        std::vector<MidiBinding> rows;
        if (control) {
          if (auto b = controlBinding(e, device, target->kind, target->id)) rows.push_back(*b);
        } else {
          rows = pressBindings(e, device, target->kind, target->id);
        }
        // The same message twice for one object is one binding here.
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const MidiBinding& r) {
                                    return std::any_of(bindings.begin(), bindings.end(),
                                                       [&](const MidiBinding& o) { return o.source == r.source; });
                                  }),
                   rows.end());
        if (rows.empty()) {
          ++report.leftOut;
          continue;
        }
        ++events;
        bindings.insert(bindings.end(), rows.begin(), rows.end());
      }
      if (!bindings.empty()) {
        map.setBindingsFor(target->kind, target->id, bindings);
        report.bindings += events;
      }
    } else {
      report.leftOut += received;
    }

    std::vector<MidiSend> sends;
    for (int i = 1; i <= sent; ++i) {
      const SendEvent e = readSend(s, i);
      auto m = target->role == Target::Display ? displaySend(e) : control ? controlSend(e) : switchSend(e);
      if (!m) {
        ++report.leftOut;
        continue;
      }
      m->targetKind = target->kind;
      m->targetId = target->id;
      sends.push_back(*m);
    }
    if (!sends.empty()) {
      map.setSendsFor(target->kind, target->id, sends);
      report.sends += static_cast<int>(sends.size());
    }

    // Computer keys: a button's, or a shoe's pair that opens and closes it
    // a step of 8, as GrandOrgue moves it.
    std::vector<KeyShortcut> keys;
    auto addKey = [&](int code, int step) {
      if (code == 0) return;
      KeyShortcut k;
      k.key = grandOrgueKeyName(code);
      k.step = step;
      if (k.key.empty() || target->role == Target::Display) {
        ++report.leftOut;
        return;
      }
      keys.push_back(k);
    };
    if (control) {
      addKey(plusKey, 8);
      addKey(minusKey, -8);
    } else {
      addKey(shortcutKey, 0);
    }
    if (!keys.empty()) {
      map.setShortcutsFor(target->kind, target->id, keys);
      report.shortcuts += static_cast<int>(keys.size());
    }
  }
  report.anyDevice = static_cast<int>(unknownDevices.size());
  return report;
}

}  // namespace mp
