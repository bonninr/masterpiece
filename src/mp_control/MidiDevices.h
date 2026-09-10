// Which physical console a message came from.
//
// A player with two manuals plugged in sends note 60 on channel 1 from both of
// them. Without knowing which box it came from, the two cannot be told apart,
// and "this keyboard plays the Great and that one plays the Swell" is
// unexpressible — which is the whole reason a rig has more than one input.
//
// Devices are identified by their NAME, not by their position in the list.
// Unplug a keyboard and plug it back into a different port and the operating
// system renumbers everything; the name is what survives, so that is what a
// saved mapping stores. Ids are small integers assigned in first-seen order
// and are meaningful only within one run — the same approach GrandOrgue takes,
// and for the same reason.
//
// Id 0 is reserved and means ANY device, which is what an unqualified mapping
// wants: a player with one keyboard should not have to care.
#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

class MidiDeviceMap {
public:
  static constexpr int kAnyDevice = 0;

  // The id for this device, assigning one if it is new. Names are matched
  // exactly; a device that has never been seen gets the next id.
  int idFor(const std::string& name) {
    if (name.empty()) return kAnyDevice;
    const auto it = byName_.find(name);
    if (it != byName_.end()) return it->second;
    names_.push_back(name);
    const int id = static_cast<int>(names_.size()); // 1-based; 0 is "any"
    byName_[name] = id;
    return id;
  }

  // The id for a device already seen, or 0. Does not assign.
  int lookup(const std::string& name) const {
    const auto it = byName_.find(name);
    return it == byName_.end() ? kAnyDevice : it->second;
  }

  std::string nameFor(int id) const {
    if (id <= 0 || id > static_cast<int>(names_.size())) return {};
    return names_[static_cast<size_t>(id) - 1];
  }

  const std::vector<std::string>& names() const { return names_; }
  size_t size() const { return names_.size(); }
  void clear() {
    names_.clear();
    byName_.clear();
  }

private:
  std::vector<std::string> names_;
  std::unordered_map<std::string, int> byName_;
};

} // namespace mp
