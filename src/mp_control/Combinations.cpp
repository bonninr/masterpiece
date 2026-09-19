#include "Combinations.h"

#include <sstream>

namespace mp {

void CombinationSystem::reset(const OrganModel& model) {
  model_ = &model;
  stored_.clear();
  bySwitch_.clear();
  reversibleBySwitch_.clear();
  captureMode_ = false;

  // Switches indexed by their assignment code, which is how a piston finds its
  // combination when the combination names no switch.
  std::unordered_map<int, Id> byAsgnCode;
  for (const auto& [id, sw] : model.switches)
    if (sw.asgnCode > 0) byAsgnCode.emplace(sw.asgnCode, id);

  for (const auto& [id, combo] : model.combinations) {
    auto& states = stored_[id];
    for (const CombinationElement& el : combo.elements)
      if (el.controlledSwitchId != 0)
        states[el.controlledSwitchId] =
            el.storedEngaged != el.invertWhenActivating;

    Id activating = combo.activatingSwitchId;
    if (activating == 0) {
      const auto it = byAsgnCode.find(combo.type);
      if (it != byAsgnCode.end()) activating = it->second;
    }
    // First combination wins a contested switch, in id order, so the mapping
    // does not depend on the hash table's layout.
    if (activating != 0) {
      const auto existing = bySwitch_.find(activating);
      if (existing == bySwitch_.end() || id < existing->second)
        bySwitch_[activating] = id;
    }
  }

  // First piston wins a contested switch, same rule as combinations above, so
  // two badly-authored rows sharing a switch do not depend on hash order.
  for (const ReversiblePiston& p : model.reversiblePistons) {
    if (p.activatingSwitchId == 0 || p.controlledSwitchId == 0) continue;
    reversibleBySwitch_.emplace(p.activatingSwitchId, p.controlledSwitchId);
  }
}

Id CombinationSystem::combinationForSwitch(Id switchId) const {
  const auto it = bySwitch_.find(switchId);
  return it == bySwitch_.end() ? 0 : it->second;
}

Id CombinationSystem::reversiblePistonTarget(Id switchId) const {
  const auto it = reversibleBySwitch_.find(switchId);
  return it == reversibleBySwitch_.end() ? 0 : it->second;
}

void CombinationSystem::recall(Id combinationId,
                               std::vector<Change>& out) const {
  if (model_ == nullptr) return;
  const auto comboIt = model_->combinations.find(combinationId);
  if (comboIt == model_->combinations.end()) return;
  const Combination& combo = comboIt->second;

  const auto statesIt = stored_.find(combinationId);
  if (statesIt == stored_.end()) return;

  for (const CombinationElement& el : combo.elements) {
    if (el.controlledSwitchId == 0) continue;
    const auto s = statesIt->second.find(el.controlledSwitchId);
    if (s == statesIt->second.end()) continue;
    const bool want = s->second;
    // A cancel is not a special kind of object: it is a combination that is
    // permitted to disengage and not to engage. Honouring the permissions is
    // what makes it cancel instead of recalling an empty registration over
    // the top of what is drawn.
    if (want && !combo.canEngage) continue;
    if (!want && !combo.canDisengage) continue;
    out.push_back({el.controlledSwitchId, want});
  }
}

bool CombinationSystem::capture(Id combinationId,
                                const std::function<bool(Id)>& isEngaged) {
  if (model_ == nullptr || !isEngaged) return false;
  const auto comboIt = model_->combinations.find(combinationId);
  if (comboIt == model_->combinations.end()) return false;
  const Combination& combo = comboIt->second;
  if (!combo.allowsCapture) return false;

  auto& states = stored_[combinationId];
  for (const CombinationElement& el : combo.elements) {
    if (el.controlledSwitchId == 0) continue;
    // What the piston LOOKS AT can differ from what it moves; when the organ
    // says nothing, it watches what it drives.
    const Id watched =
        el.capturedSwitchId != 0 ? el.capturedSwitchId : el.controlledSwitchId;
    states[el.controlledSwitchId] = isEngaged(watched);
  }
  return true;
}

int CombinationSystem::programmedCount() const {
  int n = 0;
  for (const auto& [id, states] : stored_) {
    (void)id;
    for (const auto& [sw, on] : states) {
      (void)sw;
      if (on) {
        ++n;
        break;
      }
    }
  }
  return n;
}

std::string CombinationSystem::toText() const {
  std::ostringstream out;
  out << "# Masterpiece combinations\n";
  out << "# <combination> <switch> <engaged>\n";
  // Only what is engaged. A combination is mostly zeros, and writing them all
  // would turn a ten-piston organ's file into thousands of lines saying
  // nothing; anything absent reads back as out.
  for (const auto& [comboId, states] : stored_)
    for (const auto& [switchId, on] : states)
      if (on) out << comboId << ' ' << switchId << " 1\n";
  return out.str();
}

bool CombinationSystem::fromText(const std::string& text) {
  // Clear only the states, not the wiring: the organ's own elements still
  // define which switches each combination controls.
  for (auto& [id, states] : stored_) {
    (void)id;
    for (auto& [sw, on] : states) {
      (void)sw;
      on = false;
    }
  }

  std::istringstream in(text);
  std::string line;
  bool anyBad = false;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    long long comboId = 0, switchId = 0;
    int on = 0;
    if (!(ls >> comboId >> switchId >> on)) {
      anyBad = true;
      continue;
    }
    // A line naming a combination this organ does not have is dropped rather
    // than invented: the file may have been saved against another version of
    // the set.
    const auto it = stored_.find(static_cast<Id>(comboId));
    if (it == stored_.end()) {
      anyBad = true;
      continue;
    }
    const auto sw = it->second.find(static_cast<Id>(switchId));
    if (sw == it->second.end()) {
      anyBad = true;
      continue;
    }
    sw->second = on != 0;
  }
  return !anyBad;
}

} // namespace mp
