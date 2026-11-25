// The network the engine has always used, behind OrganNetwork: SwitchNetwork
// for the switches, ContinuousControlBank for the controls. Every call goes
// straight through, so the processor behaves exactly as before.
//
// Two things only the trace simulator asks for, both off inside the
// processor: stage switches fired as the processor's fireMovedStages() fires
// them, and keys, which engage their keyboard key switch and report the
// divisions the CouplerMatrix walk reaches. The processor's own key paths
// (coupled inputs, direct inputs, pallet index, switch-fed keyboards) stay in
// the processor and are not reproduced here.
#pragma once
#include "Control.h"
#include "OrganNetwork.h"
#include "StageSwitches.h"
#include "SwitchNetwork.h"

#include <unordered_map>

namespace mp {

class LegacyNetwork final : public OrganNetwork {
public:
  Kind kind() const override { return Kind::Legacy; }

  void resetSwitches(const OrganModel& model) override;
  void set(Id switchId, bool engaged) override;
  void holdOff(std::unordered_set<Id> switches) override { switches_.holdOff(std::move(switches)); }
  bool engaged(Id switchId) const override { return switches_.engaged(switchId); }
  const std::unordered_set<Id>& engagedSwitches() const override { return switches_.engagedSwitches(); }
  const std::vector<std::pair<Id, bool>>& lastChanges() const override { return switches_.lastChanges(); }
  size_t linkageCount() const override { return switches_.linkageCount(); }
  bool lastChangeRanAway() const override { return switches_.lastChangeRanAway(); }

  void resetControls(const OrganModel& model, const std::unordered_set<Id>* waitForTime) override;
  void setValue(Id controlId, int value) override { controls_.setValue(controlId, value); }
  void setExact(Id controlId, double value, double wakeStep) override {
    controls_.setExact(controlId, value, wakeStep);
  }
  int value(Id controlId) const override { return controls_.value(controlId); }
  double readingOf(Id controlId) const override { return controls_.readingOf(controlId); }
  double normalised(Id controlId) const override { return controls_.normalised(controlId); }
  double shutterFor(const Enclosure& e) const override { return controls_.shutterFor(e); }
  void propagate(Id pinned, const std::unordered_set<Id>* engagedSwitches) override;
  void setTimed(bool on) override { controls_.setTimed(on); }
  bool timed() const override { return controls_.timed(); }
  void advance(double seconds) override;
  bool moving() const override { return controls_.moving(); }

  void setDrivesStages(bool on) override { drivesStages_ = on; }

  void keyDown(Id keyboardId, int midiNote, int velocity) override;
  void keyUp(Id keyboardId, int midiNote) override;

  double now() const override { return clock_; }

  // The wrapped objects, for tools that want to look inside.
  const SwitchNetwork& switchNetwork() const { return switches_; }
  const ContinuousControlBank& controlBank() const { return controls_; }

private:
  void fireStages();

  const OrganModel* model_ = nullptr;
  SwitchNetwork switches_;
  ContinuousControlBank controls_;
  double clock_ = 0.0;

  // Standalone only (trace simulator).
  bool drivesStages_ = false;
  StageSwitchBank stages_;
  std::vector<std::pair<Id, int>> stageValues_;
  std::vector<StageSwitchBank::Change> stageScratch_;
  CouplerMatrix flow_;
  bool flowReady_ = false;
  std::unordered_map<int, Id> keySwitch_;  // keyboard * 256 + note -> key switch
  std::unordered_map<int, std::vector<ExpandedNote>> heldKeys_;
};

} // namespace mp
