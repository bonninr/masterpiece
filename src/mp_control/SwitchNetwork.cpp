#include "SwitchNetwork.h"

namespace mp {

void SwitchNetwork::reset(const OrganModel& model) {
  model_ = &model;
  links_ = model.switchLinkages;
  bySource_.clear();
  byCondition_.clear();
  engaged_.clear();
  changes_.clear();
  work_.clear();
  ranAway_ = false;

  for (const SwitchLinkage& l : links_) {
    bySource_[l.sourceSwitchId].push_back(&l);
    if (l.conditionSwitchId != 0)
      byCondition_[l.conditionSwitchId].push_back(&l);
  }

  // An organ ships with some switches already made: a blower that is running,
  // a unison coupler that is drawn. Starting everything at rest would leave
  // those organs silent until the player found a switch nobody told them
  // about. The defaults travel like any other change, so whatever they drive
  // comes up too.
  for (const auto& [id, sw] : model.switches)
    if (sw.defaultEngaged) propagate(id, true);

  // Wires that assert something with nothing engaged — an inverting one holds
  // its destination ON while its source is out — have to be given their say at
  // load, or the organ starts in a state its own wiring says is impossible.
  for (const SwitchLinkage& l : links_) {
    bool engage = false;
    if (assertion(l, engage) && engage) propagate(l.destSwitchId, true);
  }

  changes_.clear(); // the initial state is a baseline, not a pile of changes
}

bool SwitchNetwork::assertion(const SwitchLinkage& l, bool& outEngage) const {
  const bool sourceOn = engaged_.count(l.sourceSwitchId) != 0;
  bool fires = (sourceOn == l.sourceWhenEngaged);
  if (fires && l.conditionSwitchId != 0) {
    const bool condOn = engaged_.count(l.conditionSwitchId) != 0;
    fires = (condOn == l.conditionWhenEngaged);
  }
  const int action = fires ? l.engageAction : l.disengageAction;
  if (actionEngages(action)) {
    outEngage = true;
    return true;
  }
  if (actionDisengages(action)) {
    outEngage = false;
    return true;
  }
  // An action we do not model leaves the destination alone rather than
  // guessing at it; the loader has already reported the code.
  return false;
}

void SwitchNetwork::set(Id switchId, bool engaged) {
  changes_.clear();
  ranAway_ = false;
  propagate(switchId, engaged);
}

void SwitchNetwork::propagate(Id switchId, bool engaged) {
  work_.clear();
  work_.emplace_back(switchId, engaged);

  // Generous, and only a guard: a console cannot reach it. A file whose wiring
  // flip-flops can, and the audio thread must not spin on it.
  constexpr int kMaxSteps = 200000;
  int steps = 0;

  while (!work_.empty()) {
    if (++steps > kMaxSteps) {
      ranAway_ = true;
      break;
    }
    const auto [id, want] = work_.front();
    work_.pop_front();

    // Already there: stop. This is the whole reason a console's mutual pairs —
    // the drawn knob and the logical switch driving each other — settle
    // instead of ringing.
    if ((engaged_.count(id) != 0) == want) continue;

    if (want) engaged_.insert(id);
    else engaged_.erase(id);
    changes_.emplace_back(id, want);

    // Everything this switch feeds, and everything it gates.
    const auto pushFrom = [&](const std::vector<const SwitchLinkage*>& list) {
      for (const SwitchLinkage* l : list) {
        bool engage = false;
        if (assertion(*l, engage)) work_.emplace_back(l->destSwitchId, engage);
      }
    };
    const auto srcIt = bySource_.find(id);
    if (srcIt != bySource_.end()) pushFrom(srcIt->second);
    const auto condIt = byCondition_.find(id);
    if (condIt != byCondition_.end()) pushFrom(condIt->second);
  }
}

} // namespace mp
