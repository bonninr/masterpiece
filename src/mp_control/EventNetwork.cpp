#include "EventNetwork.h"

#include <algorithm>
#include <cmath>

namespace mp {

namespace {
constexpr int kNotes = 128;
// Generous, and only a guard: a console cannot reach it in one instant.
constexpr int kMaxSteps = 200000;
// The inertia model's integration step.
constexpr double kInertiaTick = 0.001;
// Keyboards chain through key actions; a file can loop them.
constexpr int kMaxKeyDepth = 8;
// No condition, and a condition naming a switch the organ does not declare
// (read as disengaged, as SwitchNetwork reads it).
constexpr int32_t kNoCondition = -1, kUnknownSwitch = -2;

template <typename Map>
std::vector<Id> sortedIds(const Map& m) {
  std::vector<Id> ids;
  ids.reserve(m.size());
  for (const auto& kv : m) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());
  return ids;
}
}  // namespace

void EventNetwork::Csr::build(size_t owners, const std::vector<std::pair<int32_t, int32_t>>& pairs) {
  start.assign(owners + 1, 0);
  for (const auto& p : pairs) ++start[static_cast<size_t>(p.first) + 1];
  for (size_t i = 0; i < owners; ++i) start[i + 1] += start[i];
  items.assign(pairs.size(), 0);
  std::vector<int32_t> fill(start.begin(), start.end() - 1);
  for (const auto& p : pairs) items[static_cast<size_t>(fill[static_cast<size_t>(p.first)]++)] = p.second;
}

EventNetwork::EventNetwork() {
  linksByCondition_.build(0, {});
  inputsBySwitch_.build(0, {});
  wiresBySource_.build(0, {});
  wiresByCondition_.build(0, {});
  linksBySource_.build(0, {});
  doublesByControl_.build(0, {});
  stageRowsByControl_.build(0, {});
  actionsByKeyboard_.build(0, {});
  stopRanksByDivision_.build(0, {});
  pipesByPallet_.build(0, {});
}

// ---------------------------------------------------------------- the queue

void EventNetwork::push(double time, Ev kind, int32_t target, int32_t value) {
  if (queue_.size() >= queueCap_) {
    ++dropped_;
    return;
  }
  queue_.push_back({time, seq_++, kind, target, value});
  std::push_heap(queue_.begin(), queue_.end(), Later{});
}

void EventNetwork::run(double until) {
  int steps = 0;
  for (;;) {
    // The keys and pipes settle when an instant is over: everything due at
    // this time has run, so a pallet opened and shut in one instant never
    // sounds.
    if (soundDirty_ && (queue_.empty() || queue_.front().time > now_)) {
      if (++steps > kMaxSteps) break;
      settleSounding();
      continue;
    }
    if (queue_.empty() || queue_.front().time > until) break;
    if (++steps > kMaxSteps) {
      ranAway_ = true;
      queue_.clear();
      break;
    }
    std::pop_heap(queue_.begin(), queue_.end(), Later{});
    const Event e = queue_.back();
    queue_.pop_back();
    if (e.time > now_) now_ = e.time;
    handle(e);
  }
  if (until > now_) now_ = until;
}

void EventNetwork::handle(const Event& e) {
  switch (e.kind) {
    case Ev::SetSwitch: {
      const bool want = e.value != 0;
      if ((on_[static_cast<size_t>(e.target)] != 0) == want) return;
      if (want && heldOff_[static_cast<size_t>(e.target)] != 0) return;
      applySwitch(e.target, want);
      return;
    }
    case Ev::ToggleSwitch: {
      const bool want = on_[static_cast<size_t>(e.target)] == 0;
      if (want && heldOff_[static_cast<size_t>(e.target)] != 0) return;
      applySwitch(e.target, want);
      return;
    }
    case Ev::EvalLink: evalLink(e.target); return;
    case Ev::EvalDouble: evalDouble(e.target); return;
    case Ev::WillisStep: willisStep(e.target); return;
    case Ev::InertiaTick: inertiaTick(e.target); return;
  }
}

// ---------------------------------------------------------------- switches

int32_t EventNetwork::switchSlot(Id id) const {
  const auto it = switchSlot_.find(id);
  return it == switchSlot_.end() ? -1 : it->second;
}

bool EventNetwork::engaged(Id switchId) const {
  const int32_t s = switchSlot(switchId);
  return s >= 0 && on_[static_cast<size_t>(s)] != 0;
}

void EventNetwork::resetSwitches(const OrganModel& model) {
  model_ = &model;
  now_ = 0.0;
  seq_ = 0;
  queue_.clear();
  dropped_ = 0;
  ranAway_ = false;
  soundDirty_ = false;
  recording_ = false;
  changes_.clear();
  engagedSet_.clear();

  // Switches, by id so that everything built from them has a stable order.
  const std::vector<Id> ids = sortedIds(model.switches);
  const size_t n = ids.size();
  switchSlot_.clear();
  switchSlot_.reserve(n);
  switchId_ = ids;
  for (size_t i = 0; i < n; ++i) switchSlot_.emplace(ids[i], static_cast<int32_t>(i));
  on_.assign(n, 0);
  heldOff_.assign(n, 0);
  soundSwitch_.assign(n, 0);
  groupOf_.assign(n, -1);
  groups_.clear();
  for (const auto& g : model.exclusiveGroups) {
    std::vector<int32_t> members;
    for (Id id : g)
      if (const int32_t s = switchSlot(id); s >= 0) members.push_back(s);
    for (int32_t s : members) groupOf_[static_cast<size_t>(s)] = static_cast<int32_t>(groups_.size());
    groups_.push_back(std::move(members));
  }
  auto condSlot = [this](Id id) {
    if (id == 0) return kNoCondition;
    const int32_t s = switchSlot(id);
    return s >= 0 ? s : kUnknownSwitch;
  };

  // Wires.
  wires_.clear();
  std::vector<std::pair<int32_t, int32_t>> bySrc, byCond;
  for (const SwitchLinkage& l : model.switchLinkages) {
    Wire w;
    w.src = switchSlot(l.sourceSwitchId);
    w.dst = switchSlot(l.destSwitchId);
    if (w.src < 0 || w.dst < 0) continue;
    w.cond = condSlot(l.conditionSwitchId);
    w.srcWhen = l.sourceWhenEngaged;
    w.condWhen = l.conditionWhenEngaged;
    w.engage = static_cast<uint8_t>(l.engageAction);
    w.disengage = static_cast<uint8_t>(l.disengageAction);
    const auto wi = static_cast<int32_t>(wires_.size());
    bySrc.emplace_back(w.src, wi);
    if (w.cond >= 0) byCond.emplace_back(w.cond, wi);
    wires_.push_back(w);
  }
  wiresBySource_.build(n, bySrc);
  wiresByCondition_.build(n, byCond);
  fired_.assign(wires_.size(), 0);
  linksByCondition_.build(n, {});

  // Keyboards and their key switches.
  const std::vector<Id> kbs = sortedIds(model.keyboards);
  keyboardSlot_.clear();
  for (size_t i = 0; i < kbs.size(); ++i) keyboardSlot_.emplace(kbs[i], static_cast<int32_t>(i));
  keySwitch_.assign(kbs.size() * kNotes, -1);
  for (const Id sw : sortedIds(model.keyboardKeys)) {
    const KeyboardKeyRef& k = model.keyboardKeys.at(sw);
    const auto kb = keyboardSlot_.find(k.keyboardId);
    const int32_t s = switchSlot(sw);
    if (kb == keyboardSlot_.end() || s < 0 || k.midiNote < 0 || k.midiNote >= kNotes) continue;
    keySwitch_[static_cast<size_t>(kb->second) * kNotes + static_cast<size_t>(k.midiNote)] = s;
  }
  keyHeld_.assign(kbs.size() * kNotes, 0);
  keyDown_.assign(kbs.size() * kNotes, 0);
  keyDownNext_.assign(kbs.size() * kNotes, 0);

  // Divisions.
  const std::vector<Id> divs = sortedIds(model.divisions);
  divisionSlot_.clear();
  divisionId_ = divs;
  for (size_t i = 0; i < divs.size(); ++i) divisionSlot_.emplace(divs[i], static_cast<int32_t>(i));
  divInput_.assign(divs.size() * kNotes, 0);
  divNote_.assign(divs.size() * kNotes, 0);
  divNoteNext_.assign(divs.size() * kNotes, 0);

  // Key actions: keys only (ActionTypeCode 1), from a keyboard.
  actions_.clear();
  std::vector<std::pair<int32_t, int32_t>> byKb;
  for (const KeyAction& a : model.keyActions) {
    if (a.actionType != 1) continue;
    const auto src = keyboardSlot_.find(static_cast<Id>(a.sourceKeyboard));
    if (src == keyboardSlot_.end()) continue;
    Action x;
    if (a.destIsKeyboard) {
      const auto d = keyboardSlot_.find(static_cast<Id>(a.destKeyboard));
      if (d == keyboardSlot_.end()) continue;
      x.destKb = d->second;
    } else {
      const auto d = divisionSlot_.find(static_cast<Id>(a.destDivision));
      if (d == divisionSlot_.end()) continue;
      x.destDiv = d->second;
    }
    x.cond = condSlot(a.conditionSwitchId);
    x.condWhen = a.conditionWhenEngaged;
    x.first = a.firstSourceNote;
    x.num = a.numKeys;
    x.inc = a.midiIncrement;
    if (x.cond >= 0) soundSwitch_[static_cast<size_t>(x.cond)] = 1;
    byKb.emplace_back(src->second, static_cast<int32_t>(actions_.size()));
    actions_.push_back(x);
  }
  actionsByKeyboard_.build(kbs.size(), byKb);

  // Division inputs.
  inputs_.clear();
  std::vector<std::pair<int32_t, int32_t>> bySwitch;
  for (const DivisionInput& in : model.divisionInputs) {
    const auto d = divisionSlot_.find(in.divisionId);
    const int32_t s = switchSlot(in.switchId);
    if (d == divisionSlot_.end() || s < 0 || in.midiNote < 0 || in.midiNote >= kNotes) continue;
    bySwitch.emplace_back(s, static_cast<int32_t>(inputs_.size()));
    inputs_.emplace_back(d->second, in.midiNote);
  }
  inputsBySwitch_.build(n, bySwitch);

  // Ranks and pipes.
  const std::vector<Id> ranks = sortedIds(model.ranks);
  rankSlot_.clear();
  for (size_t i = 0; i < ranks.size(); ++i) rankSlot_.emplace(ranks[i], static_cast<int32_t>(i));
  rankPipeAt_.assign(ranks.size() * kNotes, -1);
  pipeRank_.clear();
  pipeId_.clear();
  pipeHasPallet_.clear();
  std::vector<std::pair<int32_t, int32_t>> byPallet;
  for (size_t r = 0; r < ranks.size(); ++r) {
    for (const Pipe& p : model.ranks.at(ranks[r]).pipes) {
      const auto pi = static_cast<int32_t>(pipeId_.size());
      pipeRank_.push_back(ranks[r]);
      pipeId_.push_back(p.pipeId);
      const int32_t pallet = p.palletSwitchId != 0 ? switchSlot(p.palletSwitchId) : -1;
      pipeHasPallet_.push_back(p.palletSwitchId != 0 ? 1 : 0);
      if (pallet >= 0) {
        byPallet.emplace_back(pallet, pi);
        soundSwitch_[static_cast<size_t>(pallet)] = 1;
      }
      if (p.midiNote >= 0 && p.midiNote < kNotes) {
        int32_t& at = rankPipeAt_[r * kNotes + static_cast<size_t>(p.midiNote)];
        if (at < 0) at = pi;
      }
    }
  }
  pipesByPallet_.build(n, byPallet);
  palletSwitches_.clear();
  for (int32_t s = 0; s < static_cast<int32_t>(n); ++s)
    if (pipesByPallet_.begin(s) != pipesByPallet_.end(s)) palletSwitches_.push_back(s);
  pipeOn_.assign(pipeId_.size(), 0);
  pipeNext_.assign(pipeId_.size(), 0);

  // Stops: each StopRank row the keys hold, under the stop's switch.
  stopRanks_.clear();
  std::vector<std::pair<int32_t, int32_t>> byDiv;
  for (const Id stopId : sortedIds(model.stops)) {
    const Stop& stop = model.stops.at(stopId);
    const auto d = divisionSlot_.find(stop.divisionId);
    if (d == divisionSlot_.end()) continue;
    const int32_t sw = stop.controllingSwitchId != 0 ? switchSlot(stop.controllingSwitchId) : -1;
    if (sw < 0) continue;  // a stop with no switch is never drawn
    soundSwitch_[static_cast<size_t>(sw)] = 1;
    for (const StopRankEntry& e : stop.ranks) {
      if (e.isActionNoise() || e.actionType != StopRankEntry::kKeys) continue;
      const auto r = rankSlot_.find(e.rankId);
      if (r == rankSlot_.end()) continue;
      StopRankRef ref;
      ref.stopSwitch = sw;
      ref.rank = r->second;
      if (e.alternateRankId != 0 && e.alternateSwitchId != 0) {
        const auto alt = rankSlot_.find(e.alternateRankId);
        ref.altRank = alt == rankSlot_.end() ? -1 : alt->second;
        ref.altSwitch = switchSlot(e.alternateSwitchId);
        if (ref.altSwitch >= 0) soundSwitch_[static_cast<size_t>(ref.altSwitch)] = 1;
      }
      ref.first = e.firstMappedDivisionNote;
      ref.num = e.numMappedNotes;
      ref.inc = e.midiIncrement;
      byDiv.emplace_back(d->second, static_cast<int32_t>(stopRanks_.size()));
      stopRanks_.push_back(ref);
    }
  }
  stopRanksByDivision_.build(divs.size(), byDiv);

  queueCap_ = std::max<size_t>(65536, 8 * (wires_.size() + model.controlLinkages.size()));
  queue_.reserve(queueCap_);
  engagedSet_.reserve(n);
  changes_.reserve(4096);

  // The organ's defaults, then every wire judged once against them: a wire
  // that fires asserts its engage action; one that does not fire asserts its
  // disengage action only when that engages (an inverting 7/4 wire holding
  // its destination on). A toggle does nothing: loading is not a press.
  // The defaults are the starting state, not moves: no wire sees an edge.
  for (size_t i = 0; i < n; ++i) {
    if (!model.switches.at(ids[i]).defaultEngaged) continue;
    on_[i] = 1;
    engagedSet_.insert(ids[i]);
    const auto s = static_cast<int32_t>(i);
    for (const int32_t* in = inputsBySwitch_.begin(s); in != inputsBySwitch_.end(s); ++in) {
      const auto& [div, note] = inputs_[static_cast<size_t>(*in)];
      ++divInput_[static_cast<size_t>(div) * kNotes + static_cast<size_t>(note)];
    }
    soundDirty_ = true;
  }
  for (size_t w = 0; w < wires_.size(); ++w) {
    const Wire& x = wires_[w];
    const bool f = fires(static_cast<int32_t>(w));
    fired_[w] = f ? 1 : 0;
    if (x.engage == 3) continue;
    if (!f) {
      if (engages(x.disengage)) push(now_, Ev::SetSwitch, x.dst, 1);
      continue;
    }
    if (engages(x.engage)) push(now_, Ev::SetSwitch, x.dst, 1);
    else if (disengages(x.engage)) push(now_, Ev::SetSwitch, x.dst, 0);
  }
  run(now_);
  changes_.clear();
}

bool EventNetwork::fires(int32_t w) const {
  const Wire& x = wires_[static_cast<size_t>(w)];
  if ((on_[static_cast<size_t>(x.src)] != 0) != x.srcWhen) return false;
  if (x.cond == kNoCondition) return true;
  const bool c = x.cond >= 0 && on_[static_cast<size_t>(x.cond)] != 0;
  return c == x.condWhen;
}

void EventNetwork::judgeWire(int32_t w) {
  const bool now = fires(w);
  if (now == (fired_[static_cast<size_t>(w)] != 0)) return;
  fired_[static_cast<size_t>(w)] = now ? 1 : 0;
  const Wire& x = wires_[static_cast<size_t>(w)];
  if (x.engage == 3) {
    if (now) push(now_, Ev::ToggleSwitch, x.dst);
    return;
  }
  const int code = now ? x.engage : x.disengage;
  if (engages(code)) push(now_, Ev::SetSwitch, x.dst, 1);
  else if (disengages(code)) push(now_, Ev::SetSwitch, x.dst, 0);
}

void EventNetwork::applySwitch(int32_t s, bool on) {
  const auto i = static_cast<size_t>(s);
  on_[i] = on ? 1 : 0;
  if (on) engagedSet_.insert(switchId_[i]);
  else engagedSet_.erase(switchId_[i]);
  if (recording_) changes_.emplace_back(switchId_[i], on);
  if (listener_ != nullptr) listener_->switchMoved(switchId_[i], on, now_);

  if (on && groupOf_[i] >= 0)
    for (int32_t other : groups_[static_cast<size_t>(groupOf_[i])])
      if (other != s && on_[static_cast<size_t>(other)] != 0) push(now_, Ev::SetSwitch, other, 0);

  for (const int32_t* w = wiresBySource_.begin(s); w != wiresBySource_.end(s); ++w) judgeWire(*w);
  for (const int32_t* w = wiresByCondition_.begin(s); w != wiresByCondition_.end(s); ++w) judgeWire(*w);
  for (const int32_t* l = linksByCondition_.begin(s); l != linksByCondition_.end(s); ++l)
    push(now_, Ev::EvalLink, *l);
  for (const int32_t* in = inputsBySwitch_.begin(s); in != inputsBySwitch_.end(s); ++in) {
    const auto& [div, note] = inputs_[static_cast<size_t>(*in)];
    uint16_t& count = divInput_[static_cast<size_t>(div) * kNotes + static_cast<size_t>(note)];
    if (on) ++count;
    else if (count > 0) --count;
    soundDirty_ = true;
  }
  if (soundSwitch_[i] != 0) soundDirty_ = true;
}

void EventNetwork::set(Id switchId, bool engaged) {
  changes_.clear();
  ranAway_ = false;
  const int32_t s = switchSlot(switchId);
  if (s < 0) return;
  recording_ = true;
  push(now_, Ev::SetSwitch, s, engaged ? 1 : 0);
  run(now_);
  recording_ = false;
}

void EventNetwork::holdOff(std::unordered_set<Id> switches) {
  std::fill(heldOff_.begin(), heldOff_.end(), 0);
  changes_.clear();
  ranAway_ = false;
  recording_ = true;
  for (Id id : switches)
    if (const int32_t s = switchSlot(id); s >= 0) {
      heldOff_[static_cast<size_t>(s)] = 1;
      if (on_[static_cast<size_t>(s)] != 0) push(now_, Ev::SetSwitch, s, 0);
    }
  run(now_);
  recording_ = false;
}

// ---------------------------------------------------------------- controls

int32_t EventNetwork::controlSlot(Id id) const {
  const auto it = controlSlot_.find(id);
  return it == controlSlot_.end() ? -1 : it->second;
}

void EventNetwork::resetControls(const OrganModel& model, const std::unordered_set<Id>* waitForTime) {
  model_ = &model;
  timed_ = false;
  movingCount_ = 0;
  const std::vector<Id> ids = sortedIds(model.continuousControls);
  const size_t n = ids.size();
  controlSlot_.clear();
  controlSlot_.reserve(n);
  controlId_ = ids;
  lo_.assign(n, 0);
  hi_.assign(n, 127);
  val_.assign(n, 0);
  reading_.assign(n, 0.0);
  woken_.assign(n, 0.0);
  inverted_.assign(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const ContinuousControl& c = model.continuousControls.at(ids[i]);
    controlSlot_.emplace(ids[i], static_cast<int32_t>(i));
    lo_[i] = std::min(c.minValue, c.maxValue);
    hi_[i] = std::max(c.minValue, c.maxValue);
    val_[i] = std::clamp(c.defaultValue, lo_[i], hi_[i]);
    reading_[i] = woken_[i] = val_[i];
    inverted_[i] = c.inverted ? 1 : 0;
  }

  links_.clear();
  std::vector<std::pair<int32_t, int32_t>> bySrc, byCond;
  for (const ContinuousControlLinkage& l : model.controlLinkages) {
    Link k;
    k.src = controlSlot(l.sourceControlId);
    k.dst = controlSlot(l.destControlId);
    if (k.src < 0 || k.dst < 0) continue;
    if (l.conditionSwitchId != 0) {
      const int32_t s = switchSlot(l.conditionSwitchId);
      k.cond = s >= 0 ? s : kUnknownSwitch;
    }
    k.condWhen = l.conditionWhenEngaged;
    k.invert = l.invert;
    k.increment = l.increment;
    k.coefficient = l.coefficient;
    k.type = l.linkType;
    k.msPerStep = l.willisMsPerStep;
    k.accel = l.inertiaAccel;
    k.damping = l.inertiaDamping;
    k.waitsForTime = waitForTime != nullptr && waitForTime->count(l.destControlId) != 0 && k.isTimed();
    const auto li = static_cast<int32_t>(links_.size());
    bySrc.emplace_back(k.src, li);
    if (k.cond >= 0) byCond.emplace_back(k.cond, li);
    links_.push_back(k);
  }
  linksBySource_.build(n, bySrc);
  linksByCondition_.build(switchId_.size(), byCond);

  doubles_.clear();
  std::vector<std::pair<int32_t, int32_t>> byControl;
  for (const ContinuousControlDoubleLinkage& d : model.controlDoubleLinkages) {
    Double k;
    k.a = controlSlot(d.firstControlId);
    k.b = controlSlot(d.secondControlId);
    k.dst = controlSlot(d.destControlId);
    if (k.a < 0 || k.b < 0 || k.dst < 0) continue;
    k.op = d.operationCode;
    k.ac = d.firstCoefficient;
    k.ai = d.firstIncrement;
    k.bc = d.secondCoefficient;
    k.bi = d.secondIncrement;
    k.dc = d.destCoefficient;
    k.di = d.destIncrement;
    const auto di = static_cast<int32_t>(doubles_.size());
    byControl.emplace_back(k.a, di);
    if (k.b != k.a) byControl.emplace_back(k.b, di);
    doubles_.push_back(k);
  }
  doublesByControl_.build(n, byControl);

  // Stage rows, per control, in threshold order.
  stageRows_.clear();
  std::vector<std::pair<int32_t, StageRow>> rows;
  for (const ContinuousControlStageSwitch& r : model.controlStageSwitches) {
    const int32_t c = controlSlot(r.controlId);
    const int32_t s = switchSlot(r.controlledSwitchId);
    if (c < 0 || s < 0) continue;
    rows.push_back({c, {r.value, s, r.engageWhenIncreasing, r.engageWhenDecreasing, r.disengageWhenIncreasing,
                        r.disengageWhenDecreasing}});
  }
  std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : a.second.value < b.second.value;
  });
  std::vector<std::pair<int32_t, int32_t>> byStage;
  for (const auto& [c, row] : rows) {
    byStage.emplace_back(c, static_cast<int32_t>(stageRows_.size()));
    stageRows_.push_back(row);
  }
  stageRowsByControl_.build(n, byStage);

  queueCap_ = std::max(queueCap_, 8 * (wires_.size() + links_.size()));
  queue_.reserve(queueCap_);

  // Settle once: every linkage judged against the defaults and the switches,
  // at once, since nothing is timed yet. Loading crosses no stage rows.
  const bool stages = drivesStages_;
  drivesStages_ = false;
  for (size_t l = 0; l < links_.size(); ++l) push(now_, Ev::EvalLink, static_cast<int32_t>(l));
  for (size_t d = 0; d < doubles_.size(); ++d) push(now_, Ev::EvalDouble, static_cast<int32_t>(d));
  run(now_);
  drivesStages_ = stages;
}

bool EventNetwork::linkLive(int32_t l) const {
  const Link& k = links_[static_cast<size_t>(l)];
  if (k.cond == kNoCondition) return true;
  const bool c = k.cond >= 0 && on_[static_cast<size_t>(k.cond)] != 0;
  return c == k.condWhen;
}

void EventNetwork::writeControl(int32_t c, double reading, double wakeStep) {
  const auto i = static_cast<size_t>(c);
  const double r = std::clamp(reading, static_cast<double>(lo_[i]), static_cast<double>(hi_[i]));
  const int v = std::clamp(static_cast<int>(std::lround(r)), lo_[i], hi_[i]);
  const int was = val_[i];
  if (v == was && r == reading_[i]) return;
  val_[i] = v;
  reading_[i] = r;
  if (v != was) {
    if (listener_ != nullptr) listener_->controlMoved(controlId_[i], v, now_);
    if (drivesStages_) fireStageRows(c, was, v);
  }
  if (v == was && std::abs(r - woken_[i]) < wakeStep) return;
  woken_[i] = r;
  for (const int32_t* l = linksBySource_.begin(c); l != linksBySource_.end(c); ++l) push(now_, Ev::EvalLink, *l);
  for (const int32_t* d = doublesByControl_.begin(c); d != doublesByControl_.end(c); ++d)
    push(now_, Ev::EvalDouble, *d);
}

void EventNetwork::evalLink(int32_t l) {
  if (!linkLive(l)) return;
  Link& k = links_[static_cast<size_t>(l)];
  const double s = reading_[static_cast<size_t>(k.src)];
  const double target = ((k.invert ? 127.0 - s : s) + k.increment) * k.coefficient;
  if (k.isTimed()) {
    if (!timed_) {
      if (k.waitsForTime) return;
    } else {
      k.target = target;
      if (!k.scheduled) {
        k.pos = reading_[static_cast<size_t>(k.dst)];
        k.vel = 0.0;
        if (k.pos == target) return;
        k.scheduled = true;
        ++movingCount_;
        if (k.type == 2) push(now_ + k.msPerStep / 1000.0, Ev::WillisStep, l);
        else push(now_ + kInertiaTick, Ev::InertiaTick, l);
      }
      return;
    }
  }
  writeControl(k.dst, target);
}

// Willis: one step every msPerStep milliseconds, toward where the source
// last sent it.
void EventNetwork::willisStep(int32_t l) {
  Link& k = links_[static_cast<size_t>(l)];
  k.scheduled = false;
  --movingCount_;
  if (!linkLive(l)) return;
  const double left = k.target - k.pos;
  k.pos = std::abs(left) <= 1.0 ? k.target : k.pos + (left > 0 ? 1.0 : -1.0);
  writeControl(k.dst, k.pos);
  if (k.pos != k.target) {
    k.scheduled = true;
    ++movingCount_;
    push(now_ + k.msPerStep / 1000.0, Ev::WillisStep, l);
  }
}

// Inertia: a mass pulled toward the target by accel per step left, damped
// per second, integrated in 1 ms steps until it comes to rest.
void EventNetwork::inertiaTick(int32_t l) {
  Link& k = links_[static_cast<size_t>(l)];
  k.scheduled = false;
  --movingCount_;
  if (!linkLive(l)) return;
  k.vel += (k.accel * (k.target - k.pos) - k.damping * k.vel) * kInertiaTick;
  k.pos += k.vel * kInertiaTick;
  const bool rest = std::abs(k.target - k.pos) < 1e-3 && std::abs(k.vel) < 1e-2;
  if (rest) k.pos = k.target;
  writeControl(k.dst, k.pos);
  if (!rest) {
    k.scheduled = true;
    ++movingCount_;
    push(now_ + kInertiaTick, Ev::InertiaTick, l);
  }
}

void EventNetwork::evalDouble(int32_t d) {
  const Double& k = doubles_[static_cast<size_t>(d)];
  const double x = reading_[static_cast<size_t>(k.a)] * k.ac + k.ai;
  const double y = reading_[static_cast<size_t>(k.b)] * k.bc + k.bi;
  double v = 0.0;
  switch (k.op) {
    case 1: v = x + y; break;
    case 2: v = x - y; break;
    case 3: v = x * y; break;
    case 7: v = x * y / 127.0; break;
    default: return;
  }
  writeControl(k.dst, v * k.dc + k.di);
}

void EventNetwork::fireStageRows(int32_t c, int from, int to) {
  const int32_t* b = stageRowsByControl_.begin(c);
  const int32_t* e = stageRowsByControl_.end(c);
  if (b == e || from == to) return;
  const bool rising = to > from;
  auto row = [&](int32_t r) {
    const StageRow& x = stageRows_[static_cast<size_t>(r)];
    const bool crossed = rising ? (from < x.value && x.value <= to) : (to <= x.value && x.value < from);
    if (!crossed) return;
    if (rising ? x.engInc : x.engDec) push(now_, Ev::SetSwitch, x.sw, 1);
    else if (rising ? x.disInc : x.disDec) push(now_, Ev::SetSwitch, x.sw, 0);
  };
  if (rising)
    for (const int32_t* r = b; r != e; ++r) row(*r);
  else
    for (const int32_t* r = e; r != b;) row(*--r);
}

void EventNetwork::setValue(Id controlId, int v) {
  const int32_t c = controlSlot(controlId);
  if (c < 0) return;
  writeControl(c, static_cast<double>(v));
  run(now_);
}

void EventNetwork::setExact(Id controlId, double v, double wakeStep) {
  const int32_t c = controlSlot(controlId);
  if (c < 0) return;
  writeControl(c, v, wakeStep);
  run(now_);
}

int EventNetwork::value(Id controlId) const {
  const int32_t c = controlSlot(controlId);
  return c < 0 ? 0 : val_[static_cast<size_t>(c)];
}

double EventNetwork::readingOf(Id controlId) const {
  const int32_t c = controlSlot(controlId);
  return c < 0 ? 0.0 : reading_[static_cast<size_t>(c)];
}

double EventNetwork::normalised(Id controlId) const {
  const int32_t c = controlSlot(controlId);
  if (c < 0) return 0.0;
  const auto i = static_cast<size_t>(c);
  if (hi_[i] <= lo_[i]) return 0.0;
  const double t = static_cast<double>(val_[i] - lo_[i]) / static_cast<double>(hi_[i] - lo_[i]);
  return inverted_[i] != 0 ? 1.0 - t : t;
}

double EventNetwork::shutterFor(const Enclosure& e) const {
  if (e.continuousControlId == 0 || controlSlot(e.continuousControlId) < 0) return 1.0;
  return normalised(e.continuousControlId);
}

void EventNetwork::propagate(Id /*pinned*/, const std::unordered_set<Id>* /*engagedSwitches*/) { run(now_); }

void EventNetwork::advance(double seconds) {
  if (seconds <= 0.0) return run(now_);
  run(now_ + seconds);
}

// ---------------------------------------------------------------- keys

void EventNetwork::keyDown(Id keyboardId, int midiNote, int /*velocity*/) {
  const auto kb = keyboardSlot_.find(keyboardId);
  if (kb == keyboardSlot_.end() || midiNote < 0 || midiNote >= kNotes) return;
  uint8_t& held = keyHeld_[static_cast<size_t>(kb->second) * kNotes + static_cast<size_t>(midiNote)];
  if (held != 0) return;
  held = 1;
  soundDirty_ = true;
  run(now_);
}

void EventNetwork::keyUp(Id keyboardId, int midiNote) {
  const auto kb = keyboardSlot_.find(keyboardId);
  if (kb == keyboardSlot_.end() || midiNote < 0 || midiNote >= kNotes) return;
  uint8_t& held = keyHeld_[static_cast<size_t>(kb->second) * kNotes + static_cast<size_t>(midiNote)];
  if (held == 0) return;
  held = 0;
  soundDirty_ = true;
  run(now_);
}

void EventNetwork::walkKey(int32_t kb, int note, int depth) {
  ++keyDownNext_[static_cast<size_t>(kb) * kNotes + static_cast<size_t>(note)];
  if (depth >= kMaxKeyDepth) return;
  for (const int32_t* a = actionsByKeyboard_.begin(kb); a != actionsByKeyboard_.end(kb); ++a) {
    const Action& x = actions_[static_cast<size_t>(*a)];
    if (x.num > 0 && (note < x.first || note >= x.first + x.num)) continue;
    if (x.cond != kNoCondition) {
      const bool c = x.cond >= 0 && on_[static_cast<size_t>(x.cond)] != 0;
      if (c != x.condWhen) continue;
    }
    const int to = note + x.inc;
    if (to < 0 || to >= kNotes) continue;
    if (x.destKb >= 0) walkKey(x.destKb, to, depth + 1);
    else ++divNoteNext_[static_cast<size_t>(x.destDiv) * kNotes + static_cast<size_t>(to)];
  }
}

// Keys, divisions and pipes, from the keys held and the switches as they
// stand: a pure function of the state, re-run when an instant that touched
// any of it is over.
void EventNetwork::settleSounding() {
  soundDirty_ = false;
  std::fill(keyDownNext_.begin(), keyDownNext_.end(), 0);
  std::copy(divInput_.begin(), divInput_.end(), divNoteNext_.begin());
  const auto keyboards = static_cast<int32_t>(keyHeld_.size() / kNotes);
  for (int32_t kb = 0; kb < keyboards; ++kb)
    for (int n = 0; n < kNotes; ++n)
      if (keyHeld_[static_cast<size_t>(kb) * kNotes + static_cast<size_t>(n)] != 0) walkKey(kb, n, 0);

  // A key down, played or coupled, engages its key switch.
  for (size_t k = 0; k < keyDown_.size(); ++k) {
    const bool down = keyDownNext_[k] != 0;
    if (down != (keyDown_[k] != 0) && keySwitch_[k] >= 0) push(now_, Ev::SetSwitch, keySwitch_[k], down ? 1 : 0);
  }
  keyDown_.swap(keyDownNext_);

  for (size_t k = 0; k < divNote_.size(); ++k) {
    const bool on = divNoteNext_[k] != 0;
    if (on != (divNote_[k] != 0) && listener_ != nullptr)
      listener_->divisionNote(divisionId_[k / kNotes], static_cast<int>(k % kNotes), on, now_);
  }
  divNote_.swap(divNoteNext_);

  std::fill(pipeNext_.begin(), pipeNext_.end(), 0);
  const auto divisions = static_cast<int32_t>(divisionId_.size());
  for (int32_t d = 0; d < divisions; ++d)
    for (int n = 0; n < kNotes; ++n) {
      if (divNote_[static_cast<size_t>(d) * kNotes + static_cast<size_t>(n)] == 0) continue;
      for (const int32_t* r = stopRanksByDivision_.begin(d); r != stopRanksByDivision_.end(d); ++r) {
        const StopRankRef& x = stopRanks_[static_cast<size_t>(*r)];
        if (on_[static_cast<size_t>(x.stopSwitch)] == 0) continue;
        if (n < x.first || n >= x.first + x.num) continue;
        const int pn = n + x.inc;
        if (pn < 0 || pn >= kNotes) continue;
        const int32_t rank =
            x.altRank >= 0 && x.altSwitch >= 0 && on_[static_cast<size_t>(x.altSwitch)] != 0 ? x.altRank : x.rank;
        const int32_t p = rankPipeAt_[static_cast<size_t>(rank) * kNotes + static_cast<size_t>(pn)];
        if (p >= 0 && pipeHasPallet_[static_cast<size_t>(p)] == 0) ++pipeNext_[static_cast<size_t>(p)];
      }
    }
  for (int32_t s : palletSwitches_)
    if (on_[static_cast<size_t>(s)] != 0)
      for (const int32_t* p = pipesByPallet_.begin(s); p != pipesByPallet_.end(s); ++p)
        ++pipeNext_[static_cast<size_t>(*p)];
  for (size_t p = 0; p < pipeOn_.size(); ++p) {
    const bool on = pipeNext_[p] != 0;
    if (on != (pipeOn_[p] != 0) && listener_ != nullptr) listener_->pipeMoved(pipeRank_[p], pipeId_[p], on, now_);
  }
  pipeOn_.swap(pipeNext_);
}

size_t EventNetwork::soundingPipes() const {
  return static_cast<size_t>(std::count_if(pipeOn_.begin(), pipeOn_.end(), [](uint16_t c) { return c != 0; }));
}

}  // namespace mp
