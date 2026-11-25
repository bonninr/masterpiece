#include "LegacyNetwork.h"
#include "EventNetwork.h"

#include <cstdlib>
#include <cstring>

namespace mp {

std::unique_ptr<OrganNetwork> makeOrganNetwork(OrganNetwork::Kind kind) {
  if (kind == OrganNetwork::Kind::Event) return std::make_unique<EventNetwork>();
  return std::make_unique<LegacyNetwork>();
}

bool eventNetworkFromEnvironment() {
  const char* v = std::getenv("MP_EVENT_NETWORK");
  return v != nullptr && std::strcmp(v, "1") == 0;
}

const char* organNetworkName(OrganNetwork::Kind kind) {
  return kind == OrganNetwork::Kind::Event ? "event" : "legacy";
}

void LegacyNetwork::resetSwitches(const OrganModel& model) {
  model_ = &model;
  switches_.reset(model);
  clock_ = 0.0;
  heldKeys_.clear();
  keySwitch_.clear();
  flowReady_ = false;
}

void LegacyNetwork::resetControls(const OrganModel& model, const std::unordered_set<Id>* waitForTime) {
  model_ = &model;
  controls_.reset(model, waitForTime);
  // The processor fires the stages itself and pays nothing here.
  stageValues_.clear();
  if (!drivesStages_) return;
  stages_.reset(model);
  for (Id id : stages_.stagedControls()) stageValues_.emplace_back(id, controls_.value(id));
}

void LegacyNetwork::set(Id switchId, bool engaged) { switches_.set(switchId, engaged); }

void LegacyNetwork::propagate(Id pinned, const std::unordered_set<Id>* engagedSwitches) {
  controls_.propagate(pinned, engagedSwitches);
  if (drivesStages_) fireStages();
}

void LegacyNetwork::advance(double seconds) {
  clock_ += seconds;
  controls_.advance(seconds);
}

// The processor's fireMovedStages(), less the wind solver's relays: every
// staged control that moved fires the rows it swept past.
void LegacyNetwork::fireStages() {
  for (auto& [id, previous] : stageValues_) {
    const int now = controls_.value(id);
    if (now == previous) continue;
    stageScratch_.clear();
    stages_.moveControl(id, previous, now, stageScratch_);
    previous = now;
    for (const auto& change : stageScratch_)
      if (switches_.engaged(change.switchId) != change.engage) switches_.set(change.switchId, change.engage);
  }
}

void LegacyNetwork::keyDown(Id keyboardId, int midiNote, int velocity) {
  // Built on the first key, so the processor, which never plays one here,
  // pays nothing at load.
  if (!flowReady_ && model_ != nullptr) {
    for (const auto& [switchId, k] : model_->keyboardKeys)
      keySwitch_[static_cast<int>(k.keyboardId) * 256 + k.midiNote] = switchId;
    flow_.reset(*model_);
    flowReady_ = true;
  }
  const int key = static_cast<int>(keyboardId) * 256 + midiNote;
  if (heldKeys_.count(key) != 0) return;
  if (const auto sw = keySwitch_.find(key); sw != keySwitch_.end()) switches_.set(sw->second, true);
  auto reached = flow_.expand(static_cast<int>(keyboardId), midiNote, static_cast<float>(velocity) / 127.0f,
                              switches_.engagedSwitches());
  if (listener_ != nullptr)
    for (const ExpandedNote& n : reached)
      listener_->divisionNote(static_cast<Id>(n.divisionId), n.midiNote, true, clock_);
  heldKeys_[key] = std::move(reached);
}

void LegacyNetwork::keyUp(Id keyboardId, int midiNote) {
  const int key = static_cast<int>(keyboardId) * 256 + midiNote;
  const auto held = heldKeys_.find(key);
  if (held == heldKeys_.end()) return;
  if (const auto sw = keySwitch_.find(key); sw != keySwitch_.end()) switches_.set(sw->second, false);
  if (listener_ != nullptr)
    for (const ExpandedNote& n : held->second)
      listener_->divisionNote(static_cast<Id>(n.divisionId), n.midiNote, false, clock_);
  heldKeys_.erase(held);
}

} // namespace mp
