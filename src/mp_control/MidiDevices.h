// Which physical console a message came from.
//
// A player with two manuals plugged in sends note 60 on channel 1 from both of
// them. Without knowing which box it came from, the two cannot be told apart,
// and "this keyboard plays the Great and that one plays the Swell" is
// unexpressible — which is the whole reason a rig has more than one input.
//
// A device is known by its NAME, and where the system gives one that lasts,
// by its IDENTIFIER: on Windows the device's interface path, which names the
// USB socket, on macOS CoreMIDI's persistent id, and on Linux the USB socket
// of the card behind the ALSA port (the ALSA client and port numbers follow
// the order the devices appear in). Ids are small integers meaningful only
// within one run.
//
// Matching follows GrandOrgue (GOMidiDeviceConfigList): a device that opens
// takes the saved one with its identifier, else the one with its exact name,
// else one whose name differs only by the numbering the system adds ("2- " in
// front, "-2" after), and each saved device is taken by one device at most.
// Two identical consoles therefore stay two: the second is named "<name>-1",
// as GrandOrgue names it, and keeps that name in the saved mappings.
//
// Id 0 is reserved and means ANY device, which is what an unqualified mapping
// wants: a player with one keyboard should not have to care.
#pragma once
#include <cctype>
#include <string>
#include <vector>

namespace mp {

class MidiDeviceMap {
public:
  static constexpr int kAnyDevice = 0;

  // A device opening now, as the system names it. Returns the id its saved
  // mappings use, taking the saved device it matches, or a new id.
  int claim(const std::string& name, const std::string& identifier = {}) {
    if (name.empty()) return kAnyDevice;
    // The same device opened again in this run.
    if (!identifier.empty())
      for (size_t i = 0; i < devices_.size(); ++i)
        if (devices_[i].present && devices_[i].identifier == identifier) return idOf(i);
    int found = 0;
    if (!identifier.empty()) found = find([&](const Device& d) { return d.identifier == identifier; }, false);
    if (found == 0) found = find([&](const Device& d) { return fits(d, identifier) && d.name == name; }, false);
    if (found == 0) found = find([&](const Device& d) { return fits(d, identifier) && sameConsole(d.name, name); }, false);
    // Plugged into a socket no saved device names: the keyboards were moved,
    // to other ports or another controller, and the name decides (#240).
    if (found == 0 && !identifier.empty() && !named(identifier)) {
      found = find([&](const Device& d) { return d.name == name; }, false);
      if (found == 0) found = find([&](const Device& d) { return sameConsole(d.name, name); }, false);
    }
    if (found != 0) {
      auto& d = devices_[static_cast<size_t>(found) - 1];
      d.present = true;
      if (!identifier.empty()) d.identifier = identifier;
      return found;
    }
    std::string unique = name;
    for (int n = 1; find([&](const Device& d) { return d.name == unique; }, true) != 0; ++n)
      unique = name + "-" + std::to_string(n);
    devices_.push_back({unique, identifier, true});
    names_.push_back(unique);
    return idOf(devices_.size() - 1);
  }

  // A device a saved mapping names: matched as claim() matches, without
  // taking it. A device not seen in this run gets an id of its own, ready for
  // when it opens.
  int idFor(const std::string& name, const std::string& identifier = {}) {
    if (name.empty()) return kAnyDevice;
    int found = 0;
    if (!identifier.empty()) found = find([&](const Device& d) { return d.identifier == identifier; }, true);
    if (found == 0) found = find([&](const Device& d) { return fits(d, identifier) && d.name == name; }, true);
    // The numbering a system adds is matched only against a device open in
    // this run. Two saved devices are two, "X" and "X-1" as much as any: read
    // from one map, they would otherwise become one.
    if (found == 0) found = find([&](const Device& d) { return d.present && fits(d, identifier) && sameConsole(d.name, name); }, true);
    // A saved socket no open device is plugged into: the keyboards were
    // moved, and an open device of that name takes the mapping (#240).
    if (found == 0 && !identifier.empty() && !openAt(identifier)) {
      found = find([&](const Device& d) { return d.present && d.name == name; }, true);
      if (found == 0)
        found = find([&](const Device& d) { return d.present && sameConsole(d.name, name); }, true);
    }
    if (found != 0) return found;
    devices_.push_back({name, identifier, false});
    names_.push_back(name);
    return idOf(devices_.size() - 1);
  }

  // The id for a device already known by this name, or 0. Does not assign.
  int lookup(const std::string& name) const {
    for (size_t i = 0; i < devices_.size(); ++i)
      if (devices_[i].name == name) return idOf(i);
    return kAnyDevice;
  }

  std::string nameFor(int id) const {
    return valid(id) ? devices_[static_cast<size_t>(id) - 1].name : std::string();
  }
  std::string identifierFor(int id) const {
    return valid(id) ? devices_[static_cast<size_t>(id) - 1].identifier : std::string();
  }

  // The name a person reads. The saved name of a second device of one model,
  // "GarageKey MIDI 1-1", reads like a version number when the model's own
  // name ends in a digit. Where devices share a name, each is shown with the
  // USB socket it is plugged into, "GarageKey MIDI 1 (USB 1.1.2)", or
  // numbered "#2" where the system gives no socket.
  std::string displayName(int id) const {
    if (!valid(id)) return {};
    const Device& d = devices_[static_cast<size_t>(id) - 1];
    const std::string base = unnumbered(d.name);
    int twins = 0, position = 0;
    for (size_t i = 0; i < devices_.size(); ++i)
      if (unnumbered(devices_[i].name) == base) {
        ++twins;
        if (static_cast<int>(i) + 1 == id) position = twins;
      }
    if (twins < 2) return d.name;
    if (d.identifier.rfind("usb-", 0) == 0) {
      const std::string path = d.identifier.substr(4, d.identifier.find('/') - 4);
      const size_t dash = path.rfind('-');
      if (dash != std::string::npos && dash + 1 < path.size())
        return base + " (USB " + path.substr(dash + 1) + ")";
    }
    return base + " #" + std::to_string(position);
  }

  const std::vector<std::string>& names() const { return names_; }
  size_t size() const { return devices_.size(); }
  void clear() {
    devices_.clear();
    names_.clear();
  }

  // Two names for one console: the same, or one of them with the numbering a
  // system adds to tell duplicates apart ("2- Name" on Windows, "Name-2" from
  // JUCE). "Roland A-49" and "Roland A-88" are not taken for one another:
  // only a name that loses its numbering to become the other matches.
  static bool sameConsole(const std::string& a, const std::string& b) {
    return a == b || unnumbered(a) == b || unnumbered(b) == a;
  }
  static std::string unnumbered(const std::string& name) {
    std::string s = name;
    // "2- Name": Windows' prefix for a second device of the same name.
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i > 0 && i + 1 < s.size() && s[i] == '-' && s[i + 1] == ' ') s.erase(0, i + 2);
    // "Name-2": JUCE's suffix for the same.
    size_t j = s.size();
    while (j > 0 && std::isdigit(static_cast<unsigned char>(s[j - 1]))) --j;
    if (j < s.size() && j > 1 && s[j - 1] == '-') s.erase(j - 1);
    return s;
  }

private:
  struct Device {
    std::string name;
    std::string identifier;
    bool present = false;  // opened in this run
  };

  // A name may stand in for an identifier only where the two do not
  // disagree: two saved consoles with their own identifiers are two, however
  // alike their names.
  static bool fits(const Device& d, const std::string& identifier) {
    return identifier.empty() || d.identifier.empty() || d.identifier == identifier;
  }

  // Whether a saved device, open or not, names this identifier.
  bool named(const std::string& identifier) const {
    for (const auto& d : devices_)
      if (d.identifier == identifier) return true;
    return false;
  }
  // Whether a device open in this run is plugged in there.
  bool openAt(const std::string& identifier) const {
    for (const auto& d : devices_)
      if (d.present && d.identifier == identifier) return true;
    return false;
  }

  template <typename Pred>
  int find(Pred pred, bool includePresent) const {
    for (size_t i = 0; i < devices_.size(); ++i)
      if ((includePresent || !devices_[i].present) && pred(devices_[i])) return idOf(i);
    return 0;
  }
  static int idOf(size_t index) { return static_cast<int>(index) + 1; }  // 0 is "any"
  bool valid(int id) const { return id > 0 && id <= static_cast<int>(devices_.size()); }

  std::vector<Device> devices_;
  std::vector<std::string> names_;
};

} // namespace mp
