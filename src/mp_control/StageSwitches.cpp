#include "StageSwitches.h"

#include <algorithm>

namespace mp {

void StageSwitchBank::reset(const OrganModel& model) {
  model_ = &model;
  rows_.clear();
  engagedStep_.clear();
  pistonSwitches_.clear();
  for (const auto& [id, combination] : model.combinations) {
    (void)id;
    if (combination.activatingSwitchId != 0) pistonSwitches_.insert(combination.activatingSwitchId);
  }
  total_ = model.controlStageSwitches.size();

  for (const auto& row : model.controlStageSwitches)
    rows_[row.controlId].push_back(row);

  // Ascending by threshold. A sweep walks them forwards or backwards from
  // here, which is what puts the crossings in the order the shoe met them.
  for (auto& [id, list] : rows_) {
    (void)id;
    std::stable_sort(list.begin(), list.end(),
                     [](const ContinuousControlStageSwitch& a,
                        const ContinuousControlStageSwitch& b) {
                       return a.value < b.value;
                     });
  }
}

std::vector<Id> StageSwitchBank::stagedControls() const {
  std::vector<Id> out;
  out.reserve(rows_.size());
  for (const auto& [id, list] : rows_) {
    (void)list;
    out.push_back(id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

size_t StageSwitchBank::stepCount(Id controlId) const {
  const auto it = rows_.find(controlId);
  return it == rows_.end() ? 0 : it->second.size();
}

size_t StageSwitchBank::stepMax(Id controlId) const {
  const auto it = rows_.find(controlId);
  if (it == rows_.end()) return 0;
  size_t n = 0;
  // Only rows that engage on the way up count as steps. A blower's
  // disengage-on-the-way-down row sits on the same kind of control and would
  // otherwise be numbered as though it were a registration.
  for (const auto& row : it->second)
    if (row.engageWhenIncreasing) ++n;
  return n;
}

size_t StageSwitchBank::currentStep(Id controlId) const {
  const auto engaged = engagedStep_.find(controlId);
  if (engaged == engagedStep_.end()) return 0;
  const auto it = rows_.find(controlId);
  if (it == rows_.end()) return 0;

  // Rows are held sorted by threshold, so counting engaging rows up to the one
  // holding the engaged switch gives its ordinal directly.
  size_t n = 0;
  for (const auto& row : it->second) {
    if (!row.engageWhenIncreasing) continue;
    ++n;
    if (!engaged->second.switches.empty() && row.controlledSwitchId == engaged->second.switches.front())
      return n;
  }
  return 0;
}

Id StageSwitchBank::crescendoControl() const {
  Id best = 0;
  size_t bestRows = 0;
  for (const auto& [id, rows] : rows_) {
    if (rows.size() <= bestRows) continue;
    bestRows = rows.size();
    best = id;
  }
  // One or two rows is a blower or a noise trigger, not a crescendo. A real
  // crescendo has a step per registration and there are never fewer than a
  // handful.
  return bestRows >= 4 ? best : 0;
}

void StageSwitchBank::moveControl(Id controlId, int fromValue, int toValue,
                                  std::vector<Change>& out) {
  if (fromValue == toValue) return;
  const auto it = rows_.find(controlId);
  if (it == rows_.end()) return;
  const auto& list = it->second;
  const bool rising = toValue > fromValue;

  // The engaging rows of the highest threshold below `left`, each a switch
  // that fires a combination: one step, its switches engaged together.
  const auto engageStepBelow = [&](const ContinuousControlStageSwitch& left) {
    // Rows sharing the threshold left are one step: the first already did it.
    if (const auto now = engagedStep_.find(controlId);
        now != engagedStep_.end() && !now->second.switches.empty() && now->second.value < left.value)
      return;
    int below = -1;
    for (const auto& row : list)
      if (row.value < left.value && row.engageWhenIncreasing && row.controlledSwitchId != left.controlledSwitchId &&
          pistonSwitches_.count(row.controlledSwitchId) != 0)
        below = std::max(below, row.value);
    if (below < 0) return;
    Step& step = engagedStep_[controlId];
    for (Id was : step.switches) out.push_back({was, false});
    step.switches.clear();
    step.value = below;
    for (const auto& row : list)
      if (row.value == below && row.engageWhenIncreasing && pistonSwitches_.count(row.controlledSwitchId) != 0 &&
          std::find(step.switches.begin(), step.switches.end(), row.controlledSwitchId) == step.switches.end()) {
        step.switches.push_back(row.controlledSwitchId);
        out.push_back({row.controlledSwitchId, true});
      }
  };

  // Walk the thresholds in the order the shoe met them.
  const auto handle = [&](const ContinuousControlStageSwitch& row) {
    // Crossed, half-open so that arriving exactly on a threshold counts and
    // leaving it does not count twice.
    //
    // Except for a row that engages one way and releases the other: it says
    // its switch is on while the value is at or past the threshold. Its
    // release counts on leaving the threshold, not on arriving at it, or a
    // step reached exactly on the way up (Düren's 13, #1274; a swell's open
    // detent at 123) would stay engaged when the shoe moves back from there.
    const bool onFromBelow = row.engageWhenIncreasing && row.disengageWhenDecreasing && !row.engageWhenDecreasing;
    const bool onFromAbove = row.engageWhenDecreasing && row.disengageWhenIncreasing && !row.engageWhenIncreasing;
    bool crossed = rising ? (fromValue < row.value && row.value <= toValue)
                          : (toValue <= row.value && row.value < fromValue);
    if (rising && onFromAbove) crossed = fromValue <= row.value && row.value < toValue;
    if (!rising && onFromBelow) crossed = toValue < row.value && row.value <= fromValue;
    if (!crossed) return;

    const bool engage =
        rising ? row.engageWhenIncreasing : row.engageWhenDecreasing;
    const bool disengage =
        rising ? row.disengageWhenIncreasing : row.disengageWhenDecreasing;

    // Engage wins when a row claims both, which the crescendo rows do: they
    // mean "this step is the one now", and the step being left is dealt with
    // below rather than by the row that is arriving.
    // Rows sharing a threshold are one step: each engages beside the others.
    if (engage) {
      Step& step = engagedStep_[controlId];
      if (!step.switches.empty() && step.value != row.value) {
        for (Id was : step.switches)
          if (was != row.controlledSwitchId) out.push_back({was, false});
        step.switches.clear();
      }
      step.value = row.value;
      if (std::find(step.switches.begin(), step.switches.end(), row.controlledSwitchId) == step.switches.end())
        step.switches.push_back(row.controlledSwitchId);
      out.push_back({row.controlledSwitchId, true});
    } else if (disengage) {
      if (const auto prev = engagedStep_.find(controlId); prev != engagedStep_.end()) {
        auto& sw = prev->second.switches;
        sw.erase(std::remove(sw.begin(), sw.end(), row.controlledSwitchId), sw.end());
        if (sw.empty()) engagedStep_.erase(prev);
      }
      out.push_back({row.controlledSwitchId, false});
      // A crescendo step left on the way down: the step below is the one now,
      // and its combination is what the shoe should play (#1274). Only for a
      // step that fires a combination: a blower's or a noise's own release
      // row means just that.
      if (!rising && row.engageWhenIncreasing && pistonSwitches_.count(row.controlledSwitchId) != 0)
        engageStepBelow(row);
    }
  };

  if (rising)
    for (const auto& row : list) handle(row);
  else
    for (auto r = list.rbegin(); r != list.rend(); ++r) handle(*r);
}

} // namespace mp
