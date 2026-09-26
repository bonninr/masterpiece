#include "SwitchNetwork.h"

namespace mp {

void SwitchNetwork::reset(const OrganModel& model) {
  model_ = &model;
  links_ = model.switchLinkages;
  bySource_.clear();
  byCondition_.clear();
  byDest_.clear();
  engaged_.clear();
  changes_.clear();
  work_.clear();
  ranAway_ = false;
  fired_.assign(links_.size(), 0);

  for (const SwitchLinkage& l : links_) {
    bySource_[l.sourceSwitchId].push_back(&l);
    if (l.conditionSwitchId != 0)
      byCondition_[l.conditionSwitchId].push_back(&l);
    byDest_[l.destSwitchId].push_back(&l);
  }

  // An organ ships with some switches already made: a blower that is running,
  // a unison coupler that is drawn. Starting everything at rest would leave
  // those organs silent until the player found a switch nobody told them
  // about. The defaults travel like any other change, so whatever they drive
  // comes up too.
  for (const auto& [id, sw] : model.switches)
    if (sw.defaultEngaged) {
      if (engaged_.insert(id).second) changes_.emplace_back(id, true);
    }

  // Every wire is evaluated once against that state, and each one that fires
  // asserts what its engage action says. Wires that are not firing assert
  // nothing, which is also what leaves an organ's off-state alone.
  for (const SwitchLinkage& l : links_) {
    // A toggle acts on a press, and loading the organ is not one.
    if (l.engageAction == kToggle) {
      fired_[static_cast<size_t>(&l - links_.data())] = fires(l) ? 1 : 0;
      continue;
    }
    // An inverting wire (7/4) that is not firing holds its destination ON:
    // a unison-off coupler with its knob in is the usual case. It has to be
    // given its say here, or the organ comes up in a state its own wiring
    // rules out.
    if (!fires(l)) {
      if (actionEngages(l.disengageAction))
        work_.emplace_back(l.destSwitchId, true);
      continue;
    }
    const auto index = static_cast<size_t>(&l - links_.data());
    fired_[index] = 1;
    if (actionEngages(l.engageAction))
      work_.emplace_back(l.destSwitchId, true);
    else if (actionDisengages(l.engageAction) &&
             !anotherEngagingWire(l.destSwitchId, &l))
      work_.emplace_back(l.destSwitchId, false);
  }
  drain();

  changes_.clear(); // the initial state is a baseline, not a pile of changes
}

bool SwitchNetwork::fires(const SwitchLinkage& l) const {
  const bool sourceOn = engaged_.count(l.sourceSwitchId) != 0;
  bool fires = (sourceOn == l.sourceWhenEngaged);
  if (fires && l.conditionSwitchId != 0) {
    const bool condOn = engaged_.count(l.conditionSwitchId) != 0;
    fires = (condOn == l.conditionWhenEngaged);
  }
  return fires;
}

bool SwitchNetwork::anotherEngagingWire(Id dest,
                                        const SwitchLinkage* except) const {
  const auto it = byDest_.find(dest);
  if (it == byDest_.end()) return false;
  for (const SwitchLinkage* l : it->second) {
    if (l == except) continue;
    const auto index = static_cast<size_t>(l - links_.data());
    if (index < fired_.size() && fired_[index] != 0 &&
        actionEngages(l->engageAction))
      return true;
  }
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
  drain();
}

void SwitchNetwork::drain() {
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

    // Everything this switch can swing: the wires it drives, and the wires it
    // gates. Each is re-evaluated from its own source and condition, and
    // asserts only if its firing state just changed.
    const auto reeval = [&](const std::vector<const SwitchLinkage*>& list) {
      for (const SwitchLinkage* l : list) reevaluate(*l);
    };
    const auto srcIt = bySource_.find(id);
    if (srcIt != bySource_.end()) reeval(srcIt->second);
    const auto condIt = byCondition_.find(id);
    if (condIt != byCondition_.end()) reeval(condIt->second);
  }
}

// Asserts on the wire's firing EDGE, not on its level; a
// wire that is not firing says nothing. See the header for why, and the pallet
// chain in the Alessandria ODF for the case that forced this.
void SwitchNetwork::reevaluate(const SwitchLinkage& l) {
  const auto index = static_cast<size_t>(&l - links_.data());
  const bool now = fires(l);
  if (now == (fired_[index] != 0)) return;
  fired_[index] = now ? 1 : 0;

  // A reversible piston: each press flips what it controls, and letting
  // go does nothing -- a toggle has no state of its own to undo. Every
  // reversible in the sets at hand is wired 3/7 from a momentary piston.
  if (l.engageAction == kToggle) {
    if (now) work_.emplace_back(l.destSwitchId, engaged_.count(l.destSwitchId) == 0);
    return;
  }

  const int action = now ? l.engageAction : l.disengageAction;
  if (actionEngages(action)) {
    work_.emplace_back(l.destSwitchId, true);
    return;
  }
  if (actionDisengages(action)) {
    // The OR rule: a disengage only counts when no other firing wire into the
    // same destination is still asserting engagement.
    if (!anotherEngagingWire(l.destSwitchId, &l))
      work_.emplace_back(l.destSwitchId, false);
  }
  // An action we do not model leaves the destination alone rather than
  // guessing at it; the loader has already reported the code.
}

} // namespace mp
