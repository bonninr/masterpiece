#include "ConsoleProfiles.h"

#include <set>

#include "../mp_audio/DataFolder.h"
#include "Panels.h"

namespace mp::ui {

namespace {

const char* const kExtension = ".mpconsole";

// A tremulant drawn as a stop with no ranks, known by its name.
bool namedTremulant(const std::string& name) {
  const auto f = foldName(name);
  return f.find("trem") != std::string::npos || f.find("schweb") != std::string::npos;
}

// A message the organ's map already uses: the same source, and for a
// switch worked by one value of it (#923), the same value.
bool sourceTaken(const MidiMap& organ, const MidiBinding& b) {
  for (const auto& o : organ.bindings())
    if (o.source == b.source && (!(o.exactValues && b.exactValues) || o.highValue == b.highValue)) return true;
  return false;
}

}  // namespace

juce::File consoleProfilesFolder() { return mp::dataFolder().getChildFile("consoles"); }

juce::Array<juce::File> consoleProfileFiles() {
  auto files = consoleProfilesFolder().findChildFiles(juce::File::findFiles, false,
                                                      juce::String("*") + kExtension);
  files.sort();
  return files;
}

juce::File activeConsoleProfile() {
  const auto name = consoleProfilesFolder().getChildFile("active.txt").loadFileAsString().trim();
  if (name.isEmpty()) return {};
  const auto f = consoleProfilesFolder().getChildFile(name);
  return f.existsAsFile() ? f : juce::File();
}

void setActiveConsoleProfile(const juce::File& file) {
  consoleProfilesFolder().createDirectory();
  consoleProfilesFolder().getChildFile("active.txt").replaceWithText(file.existsAsFile() ? file.getFileName()
                                                                                          : juce::String());
}

bool loadConsoleProfile(const juce::File& file, ConsoleProfile& out) {
  if (!file.existsAsFile()) return false;
  return out.fromText(file.loadFileAsString().toStdString());
}

bool saveConsoleProfile(const juce::File& file, const ConsoleProfile& profile) {
  file.getParentDirectory().createDirectory();
  return file.replaceWithText(juce::String::fromUTF8(profile.toText().c_str()));
}

juce::File newConsoleProfileFile(const juce::String& name) {
  auto base = juce::File::createLegalFileName(name.trim());
  if (base.isEmpty()) base = "Console";
  auto f = consoleProfilesFolder().getChildFile(base + kExtension);
  for (int n = 2; f.existsAsFile(); ++n) f = consoleProfilesFolder().getChildFile(base + " " + juce::String(n) + kExtension);
  return f;
}

std::vector<OrganControl> organControls(const MasterpieceProcessor& p) {
  const auto& m = p.organModel();
  std::vector<std::pair<std::string, int>> divisions;
  for (const auto& [id, d] : m.divisions) divisions.emplace_back(d.name, divisionKeyboard(m, id));

  std::vector<OrganControl> out;
  std::set<Id> seen;
  auto add = [&](const PanelSection& s, const PanelElement& e, Id sw, ConsoleTabKind kind) {
    if (sw == 0) return;
    sw = p.midiSwitchFor(sw);
    if (!seen.insert(sw).second) return;
    OrganControl c;
    c.section = s.title;
    c.tab.kind = kind;
    c.tab.label = e.footage.empty() ? e.name : e.name + " " + e.footage;
    c.tab.switchId = sw;
    const int own = s.kind == PanelSection::Kind::Division ? divisionKeyboard(m, s.divisionId) : -1;
    switch (kind) {
      case ConsoleTabKind::Coupler: {
        int onto = -1, from = -1, shift = 0;
        if (couplerKeyboards(m, sw, onto, from, shift) ||
            couplerKeyboardsFromName(c.tab.label, divisions, onto, from, shift)) {
          c.tab.keyboard = onto;
          c.tab.fromKeyboard = from;
          c.tab.shift = shift;
        } else {
          c.tab.keyboard = own;
        }
        break;
      }
      case ConsoleTabKind::Tremulant:
        c.tab.keyboard = own >= 0 ? own : keyboardFromName(c.tab.label, divisions);
        break;
      case ConsoleTabKind::Stop:
        c.tab.keyboard = own;
        break;
      case ConsoleTabKind::Piston:
        c.tab.keyboard = keyboardFromName(c.tab.label, divisions);
        break;
    }
    c.mapped = !p.midiMap().bindingsFor(MidiTargetKind::Switch, sw).empty();
    out.push_back(std::move(c));
  };

  const auto sections = panelSections(p);
  for (const auto& s : sections) {
    if (s.kind == PanelSection::Kind::Controls || s.kind == PanelSection::Kind::Presets) continue;
    for (const auto& e : s.elements) {
      if (e.kind == PanelElement::Kind::Stop) {
        const Id sw = p.stopKnobFor(e.id);
        const auto st = m.stops.find(e.id);
        const bool noRanks = st != m.stops.end() && st->second.ranks.empty();
        const auto kind = e.coupler                       ? ConsoleTabKind::Coupler
                          : noRanks && namedTremulant(e.name) ? ConsoleTabKind::Tremulant
                                                              : ConsoleTabKind::Stop;
        add(s, e, sw, kind);
      } else if (e.kind == PanelElement::Kind::Switch) {
        const Id sw = p.playerSwitchFor(e.id);
        const auto it = m.switches.find(e.id);
        const bool tremulant =
            (it != m.switches.end() && it->second.isTremulant) || namedTremulant(e.name);
        const bool coupler = e.coupler || (it != m.switches.end() && it->second.isCoupler);
        add(s, e, sw,
            coupler ? ConsoleTabKind::Coupler : tremulant ? ConsoleTabKind::Tremulant : ConsoleTabKind::Stop);
      }
    }
  }
  for (const auto& s : sections)
    for (const auto& e : s.elements)
      if (e.kind == PanelElement::Kind::Piston) add(s, e, p.playerSwitchFor(e.id), ConsoleTabKind::Piston);
  return out;
}

std::vector<ProfileMatch> profileMatches(const MasterpieceProcessor& p, const ConsoleProfile& profile,
                                         const std::vector<OrganControl>& controls) {
  // Only what can be used: tabs with a message, controls not mapped.
  std::vector<ConsoleTab> tabs, open;
  std::vector<size_t> tabIndex, controlIndex;
  // The organ's map with the profile's devices named in it, to see which
  // messages it already uses.
  MidiMap probe = p.midiMap();
  for (size_t i = 0; i < profile.tabs.size(); ++i) {
    auto rows = profile.bindingsOf(i);
    if (rows.empty()) continue;
    bool free = true;
    for (auto& b : rows) {
      if (b.source.deviceId != MidiDeviceMap::kAnyDevice)
        b.source.deviceId = probe.devices().idFor(profile.messages.devices().nameFor(b.source.deviceId),
                                                  profile.messages.devices().identifierFor(b.source.deviceId));
      if (sourceTaken(p.midiMap(), b)) free = false;
    }
    if (!free) continue;
    tabs.push_back(profile.tabs[i]);
    tabIndex.push_back(i);
  }
  for (size_t c = 0; c < controls.size(); ++c) {
    if (controls[c].mapped) continue;
    open.push_back(controls[c].tab);
    controlIndex.push_back(c);
  }
  auto matches = matchProfile(tabs, open);
  for (auto& mt : matches) {
    mt.tab = tabIndex[mt.tab];
    mt.control = controlIndex[mt.control];
  }
  return matches;
}

int applyProfileMatches(MasterpieceProcessor& p, const ConsoleProfile& profile,
                        const std::vector<OrganControl>& controls, const std::vector<ProfileMatch>& chosen) {
  std::vector<MidiBinding> rows;
  int mapped = 0;
  for (const auto& mt : chosen) {
    if (mt.tab >= profile.tabs.size() || mt.control >= controls.size()) continue;
    const Id target = controls[mt.control].tab.switchId;
    if (target == 0 || !p.midiMap().bindingsFor(MidiTargetKind::Switch, target).empty()) continue;
    bool any = false;
    for (auto b : profile.bindingsOf(mt.tab)) {
      b.targetKind = MidiTargetKind::Switch;
      b.targetId = target;
      rows.push_back(b);
      any = true;
    }
    if (any) ++mapped;
  }
  p.addBindingsFrom(profile.messages, rows);
  return mapped;
}

int addOrganToProfile(const MasterpieceProcessor& p, ConsoleProfile& into) {
  int added = 0;
  for (const auto& c : organControls(p)) {
    if (!c.mapped) continue;
    bool known = false;
    for (const auto& t : into.tabs)
      if (t.kind == c.tab.kind && t.label == c.tab.label && t.keyboard == c.tab.keyboard) known = true;
    if (known) continue;
    ConsoleTab tab = c.tab;
    tab.switchId = 0;
    into.tabs.push_back(tab);
    const size_t index = into.tabs.size() - 1;
    for (const MidiBinding* b : p.midiMap().bindingsFor(MidiTargetKind::Switch, c.tab.switchId)) {
      MidiBinding copy = *b;
      copy.targetKind = MidiTargetKind::Switch;
      copy.targetId = ConsoleProfile::targetOf(index);
      into.messages.bindFrom(p.midiMap(), copy);
    }
    ++added;
  }
  return added;
}

juce::String organProfileStamp(const MasterpieceProcessor& p) {
  const auto f = p.organFile(p.loadedOrganFile(), "consoles/organs", ".mpstamp");
  return f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
}

void setOrganProfileStamp(const MasterpieceProcessor& p, const juce::String& stamp) {
  const auto f = p.organFileForSaving("consoles/organs", ".mpstamp");
  if (f == juce::File()) return;
  f.getParentDirectory().createDirectory();
  f.replaceWithText(stamp);
}

juce::String profileStampFor(const juce::File& file, const ConsoleProfile& profile) {
  return file.getFileName() + " " + juce::String(profile.stamp());
}

}  // namespace mp::ui
