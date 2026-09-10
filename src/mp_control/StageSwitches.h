// Shoe positions that move switches — and therefore the crescendo.
//
// A crescendo is not a special mechanism. It is one continuous control with a
// `ContinuousControlStageSwitch` row per step, each naming the switch that
// fires that step's registration; on Nancy that is control 51, "03. Crescendo
// pedal", with 74 rows and 74 type-4 combinations behind them. The same
// mechanism starts the blower, triggers enclosure noises and delays things at
// load, so implementing it once covers all of them.
//
// Each row is a THRESHOLD CROSSING rather than a state, which is what lets an
// organ give a switch hysteresis: Nancy starts her blower when the value rises
// through 120 and stops it when the value falls back through 126, two rows
// naming one switch. So this needs to know where the shoe WAS, not only where
// it is.
//
// Sweeping the shoe fires every threshold it passes, in the order it passes
// them. That order is the point: dragging a crescendo from nothing to full
// fires each step in turn and the topmost one lands last, and dragging it back
// fires them in reverse so the lowest lands last. In both directions the
// registration that survives is the one belonging to where the shoe stopped.
#pragma once
#include "../mp_core/OrganModel.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace mp {

class StageSwitchBank {
public:
  // One switch to move, and which way.
  struct Change {
    Id switchId = 0;
    bool engage = false;
  };

  void reset(const OrganModel& model);

  // Move a control from where it was to where it is, and report every switch
  // the sweep moves, in crossing order. Appends to `out`.
  //
  // Also disengages the previous step of the same control before engaging a
  // new one: a crescendo's steps are alternatives, not an accumulation, and
  // leaving the old one latched would stop the shoe from ever firing it again.
  void moveControl(Id controlId, int fromValue, int toValue,
                   std::vector<Change>& out);

  // Whether this control has any stage rows at all — most do not, and a shoe
  // that has none must not pay for the lookup.
  bool drives(Id controlId) const { return rows_.count(controlId) != 0; }
  size_t rowCount() const { return total_; }
  // Controls with stage rows, so a tool can say which shoe is the crescendo.
  std::vector<Id> stagedControls() const;
  size_t stepCount(Id controlId) const;

private:
  const OrganModel* model_ = nullptr;
  // Rows per control, sorted by threshold so a sweep can walk them in order.
  std::unordered_map<Id, std::vector<ContinuousControlStageSwitch>> rows_;
  // The switch each control most recently engaged through a stage row.
  std::unordered_map<Id, Id> engagedStep_;
  size_t total_ = 0;
};

} // namespace mp
