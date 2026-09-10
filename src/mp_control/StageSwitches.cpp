#include "StageSwitches.h"

#include <algorithm>

namespace mp {

void StageSwitchBank::reset(const OrganModel& model) {
  model_ = &model;
  rows_.clear();
  engagedStep_.clear();
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

void StageSwitchBank::moveControl(Id controlId, int fromValue, int toValue,
                                  std::vector<Change>& out) {
  if (fromValue == toValue) return;
  const auto it = rows_.find(controlId);
  if (it == rows_.end()) return;
  const auto& list = it->second;
  const bool rising = toValue > fromValue;

  // Walk the thresholds in the order the shoe met them.
  const auto handle = [&](const ContinuousControlStageSwitch& row) {
    // Crossed, half-open so that arriving exactly on a threshold counts and
    // leaving it does not count twice.
    const bool crossed = rising ? (fromValue < row.value && row.value <= toValue)
                                : (toValue <= row.value && row.value < fromValue);
    if (!crossed) return;

    const bool engage =
        rising ? row.engageWhenIncreasing : row.engageWhenDecreasing;
    const bool disengage =
        rising ? row.disengageWhenIncreasing : row.disengageWhenDecreasing;

    // Engage wins when a row claims both, which the crescendo rows do: they
    // mean "this step is the one now", and the step being left is dealt with
    // below rather than by the row that is arriving.
    if (engage) {
      const auto prev = engagedStep_.find(controlId);
      if (prev != engagedStep_.end() && prev->second != row.controlledSwitchId)
        out.push_back({prev->second, false});
      engagedStep_[controlId] = row.controlledSwitchId;
      out.push_back({row.controlledSwitchId, true});
    } else if (disengage) {
      const auto prev = engagedStep_.find(controlId);
      if (prev != engagedStep_.end() && prev->second == row.controlledSwitchId)
        engagedStep_.erase(prev);
      out.push_back({row.controlledSwitchId, false});
    }
  };

  if (rising)
    for (const auto& row : list) handle(row);
  else
    for (auto r = list.rbegin(); r != list.rend(); ++r) handle(*r);
}

} // namespace mp
