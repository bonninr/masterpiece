#include "SplitText.h"

#include <sstream>

namespace mp::split {

namespace {

const char* sustainName(Sustain s) {
  switch (s) {
    case Sustain::UntilNext: return "next";
    case Sustain::Timed: return "timed";
    default: return "off";
  }
}

Sustain sustainFromName(const std::string& n) {
  if (n == "next") return Sustain::UntilNext;
  if (n == "timed") return Sustain::Timed;
  return Sustain::Off;
}

const char* kindName(Trigger::Kind k) {
  switch (k) {
    case Trigger::Note: return "note";
    case Trigger::Controller: return "cc";
    case Trigger::Program: return "pc";
    default: return "none";
  }
}

Trigger::Kind kindFromName(const std::string& n) {
  if (n == "note") return Trigger::Note;
  if (n == "cc") return Trigger::Controller;
  if (n == "pc") return Trigger::Program;
  return Trigger::None;
}

// The rest of a line after what was read, trimmed: a name may hold spaces.
std::string rest(std::istringstream& in) {
  std::string r;
  std::getline(in, r);
  const size_t a = r.find_first_not_of(" \t");
  if (a == std::string::npos) return {};
  const size_t b = r.find_last_not_of(" \t\r");
  return r.substr(a, b - a + 1);
}

bool layoutLine(const std::string& key, std::istringstream& in, Layout& l) {
  if (key == "splits") {
    int n = 0;
    if (in >> n) {
      l.splitCount = n;
      for (int& s : l.splits) in >> s;
    }
  } else if (key == "zone") {
    int i = -1;
    std::string role, layer;
    int octave = 0, transpose = 0;
    if (in >> i >> role >> layer >> octave >> transpose && i >= 0 && i < kMaxZones)
      l.zones[i] = Zone{roleFromName(role), roleFromName(layer), octave, transpose};
  } else if (key == "transpose") {
    in >> l.transpose;
  } else if (key == "follow") {
    int f = 0;
    if (in >> f) l.followHands = f != 0;
  } else if (key == "bass") {
    int on = 0, also = 1;
    std::string sustain;
    Bass b = l.bass;
    if (in >> on >> b.topKey >> also >> sustain >> b.sustainMs) {
      b.on = on != 0;
      b.alsoOnManual = also != 0;
      b.sustain = sustainFromName(sustain);
      l.bass = b;
    }
  } else {
    return false;
  }
  return true;
}

}  // namespace

const char* roleName(Role r) {
  switch (r) {
    case Role::Lower: return "lower";
    case Role::Upper: return "upper";
    case Role::Solo: return "solo";
    case Role::Pedal: return "pedal";
    default: return "none";
  }
}

Role roleFromName(const std::string& n) {
  if (n == "lower") return Role::Lower;
  if (n == "upper") return Role::Upper;
  if (n == "solo") return Role::Solo;
  if (n == "pedal") return Role::Pedal;
  return Role::None;
}

const char* controlName(int control) {
  static const char* kNames[kControlCount] = {"toggle",  "next",    "previous", "preset1",
                                              "preset2", "preset3", "preset4",  "preset5",
                                              "preset6", "preset7", "preset8",  "preset9"};
  return control >= 0 && control < kControlCount ? kNames[control] : "";
}

std::string layoutToText(const Layout& l) {
  std::ostringstream out;
  out << "splits " << l.splitCount;
  for (int s : l.splits) out << ' ' << s;
  out << '\n';
  for (int i = 0; i < kMaxZones; ++i) {
    const Zone& z = l.zones[i];
    out << "zone " << i << ' ' << roleName(z.role) << ' ' << roleName(z.layer) << ' ' << z.octave << ' '
        << z.transpose << '\n';
  }
  out << "transpose " << l.transpose << '\n';
  out << "follow " << (l.followHands ? 1 : 0) << '\n';
  out << "bass " << (l.bass.on ? 1 : 0) << ' ' << l.bass.topKey << ' ' << (l.bass.alsoOnManual ? 1 : 0) << ' '
      << sustainName(l.bass.sustain) << ' ' << l.bass.sustainMs << '\n';
  return out.str();
}

void layoutFromText(const std::string& text, Layout& l) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream in(line);
    std::string key;
    if (in >> key) layoutLine(key, in, l);
  }
  l.normalize();
}

Shared::Shared() {
  for (int i = 0; i < kFactoryPresets; ++i) {
    presets.layouts[i] = factoryLayout(i);
    presets.used[i] = true;
  }
}

std::string sharedToText(const Shared& s, const NameOf& nameOf) {
  std::ostringstream out;
  out << "on " << (s.on ? 1 : 0) << '\n';
  if (!s.source.empty()) out << "source " << s.source << '\n';
  for (int i = 0; i < kControlCount; ++i) {
    const Trigger& t = s.triggers[static_cast<size_t>(i)];
    if (t.kind == Trigger::None) continue;
    out << "control " << controlName(i) << ' ' << kindName(t.kind) << ' ' << int(t.channel) << ' '
        << int(t.number);
    const std::string device = t.device != 0 && nameOf ? nameOf(t.device) : std::string();
    if (!device.empty()) out << ' ' << device;
    out << '\n';
  }
  for (int i = kFactoryPresets; i < kPresetCount; ++i) {
    if (!s.presets.used[i]) continue;
    out << "[preset " << (i + 1) << "] " << s.names[i] << '\n' << layoutToText(s.presets.layouts[i]);
  }
  return out.str();
}

Shared sharedFromText(const std::string& text, const IdOf& idOf) {
  Shared s;
  std::istringstream lines(text);
  std::string line;
  int preset = -1;
  while (std::getline(lines, line)) {
    if (line.rfind("[preset ", 0) == 0) {
      std::istringstream in(line.substr(8));
      int n = 0;
      preset = -1;
      if (in >> n && n > kFactoryPresets && n <= kPresetCount) {
        preset = n - 1;
        std::string name = rest(in);
        if (!name.empty() && name.front() == ']') name.erase(0, 1);
        const size_t a = name.find_first_not_of(' ');
        s.names[preset] = a == std::string::npos ? std::string() : name.substr(a);
        s.presets.used[preset] = true;
        s.presets.layouts[preset] = Layout{};
      }
      continue;
    }
    std::istringstream in(line);
    std::string key;
    if (!(in >> key)) continue;
    if (preset >= 0 && layoutLine(key, in, s.presets.layouts[preset])) continue;
    if (key == "on") {
      int on = 0;
      if (in >> on) s.on = on != 0;
    } else if (key == "source") {
      s.source = rest(in);
    } else if (key == "control") {
      std::string name, kind;
      int channel = 0, number = 0;
      if (!(in >> name >> kind >> channel >> number)) continue;
      int c = -1;
      for (int i = 0; i < kControlCount; ++i)
        if (name == controlName(i)) c = i;
      if (c < 0 || channel < 1 || channel > 16 || number < 0 || number > 127) continue;
      Trigger t;
      t.kind = kindFromName(kind);
      t.channel = static_cast<uint8_t>(channel);
      t.number = static_cast<uint8_t>(number);
      const std::string device = rest(in);
      t.device = device.empty() || !idOf ? 0 : idOf(device);
      s.triggers[static_cast<size_t>(c)] = t;
    }
  }
  for (Layout& l : s.presets.layouts) l.normalize();
  return s;
}

std::string organToText(const Layout& l, int preset) {
  std::string text = layoutToText(l);
  if (preset >= 0) text += "preset " + std::to_string(preset + 1) + "\n";
  return text;
}

void organFromText(const std::string& text, Layout& l, int& preset) {
  layoutFromText(text, l);
  preset = -1;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream in(line);
    std::string key;
    int n = 0;
    if (in >> key && key == "preset" && in >> n && n >= 1 && n <= kPresetCount) preset = n - 1;
  }
}

}  // namespace mp::split
