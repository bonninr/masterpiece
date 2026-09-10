#include "Stepper.h"

#include <algorithm>

namespace mp {

void Stepper::reset(const OrganModel& model) {
  frames_.clear();
  frame_ = 0;

  // Generals, in playing order. Two spellings exist and organs use both: the
  // plain type code 2, and the 1xx family where the number IS the piston.
  // Ordering by the type code puts 101, 102, 103 in the order an organist
  // would press them; combinations sharing a code fall back to their id so the
  // answer never depends on a hash table's layout.
  std::vector<std::pair<int, Id>> ordered;
  for (const auto& [id, combo] : model.combinations) {
    if (combo.isCancel()) continue; // stepping onto a cancel wipes the organ
    const bool plainGeneral = combo.type == 2;
    const bool numberedGeneral = combo.type > 100 && combo.type < 200;
    if (!plainGeneral && !numberedGeneral) continue;
    if (combo.elements.empty()) continue; // nothing to recall
    ordered.emplace_back(combo.type, id);
  }
  std::sort(ordered.begin(), ordered.end());

  frames_.reserve(ordered.size());
  for (const auto& [type, id] : ordered) {
    (void)type;
    frames_.push_back(id);
  }
}

Id Stepper::current() const {
  if (frame_ < 1 || frame_ > static_cast<int>(frames_.size())) return 0;
  return frames_[static_cast<size_t>(frame_ - 1)];
}

Id Stepper::next() {
  if (frames_.empty()) return 0;
  if (frame_ >= static_cast<int>(frames_.size())) return 0; // held at the end
  ++frame_;
  return current();
}

Id Stepper::prev() {
  if (frames_.empty() || frame_ <= 1) return 0; // held at the beginning
  --frame_;
  return current();
}

Id Stepper::gotoFrame(int oneBased) {
  if (oneBased < 1 || oneBased > static_cast<int>(frames_.size())) return 0;
  frame_ = oneBased;
  return current();
}

} // namespace mp
