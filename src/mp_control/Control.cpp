#include "Control.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace mp {

void CouplerMatrix::reset(const OrganModel& m) {
  model_ = &m;
  edges_.clear();
  inputKeyboards_.clear();
  primaryDivision_.clear();
  assignmentCode_.clear();
  divisionOfKeyboard_.clear();
  needsFallback_.clear();

  std::unordered_set<Id> sources, destinations;
  for (const KeyAction& a : m.keyActions) {
    if (a.sourceKeyboard == 0) continue;
    // Action types beyond the plain one (pizzicato, reiteration, traps) are
    // not modelled yet. Carrying them as ordinary key flow would sound the
    // right pipes in the wrong manner, which is worse than the loader's
    // existing report that they were seen and skipped.
    if (a.actionType != 1) continue;
    edges_[static_cast<Id>(a.sourceKeyboard)].push_back(&a);
    sources.insert(static_cast<Id>(a.sourceKeyboard));
    if (a.destIsKeyboard && a.destKeyboard != 0)
      destinations.insert(static_cast<Id>(a.destKeyboard));
  }

  // Which keyboards a player can actually play on.
  //
  // The organ says so directly: AccessibleForInput, plus a default assignment
  // code. Codes at 6 and above are not manuals at all — they are the noise and
  // miscellaneous keyboards an organ uses internally, and the reference
  // conversion skips them for the same reason.
  for (const auto& [id, kb] : m.keyboards) {
    if (!kb.accessibleForInput) continue;
    if (kb.assignmentCode <= 0 || kb.assignmentCode >= 6) continue;
    inputKeyboards_.push_back(id);
  }
  // A model that says nothing (hand-built, or a CODM set) falls back to the
  // shape of the graph: a keyboard that drives key flow but is never driven by
  // it is one a player plays on.
  if (inputKeyboards_.empty())
    for (Id kb : sources)
      if (destinations.count(kb) == 0) inputKeyboards_.push_back(kb);
  std::sort(inputKeyboards_.begin(), inputKeyboards_.end());

  // Which division each keyboard sounds.
  //
  // Usually the organ says so outright. It does not for the internal coupling
  // keyboards a large organ uses as a hub — Nancy's "GrandOrgue Ex" is fed
  // unison from the Grand Orgue and is where every coupler off that manual
  // starts, but it names no division of its own, because Hauptwerk reaches its
  // pipework through the switch graph instead. A keyboard that is fed
  // unconditionally by exactly one keyboard with a division IS that division,
  // and resolving it this way is what makes those couplers sound.
  for (const auto& [id, kb] : m.keyboards)
    if (kb.primaryDivisionHint != 0) divisionOfKeyboard_[id] = kb.primaryDivisionHint;

  for (int pass = 0; pass < 4; ++pass) {
    bool changed = false;
    for (const auto& [src, list] : edges_) {
      const auto srcDiv = divisionOfKeyboard_.find(src);
      if (srcDiv == divisionOfKeyboard_.end()) continue;
      for (const KeyAction* a : list) {
        if (!a->destIsKeyboard || a->destKeyboard == 0) continue;
        // Only an ungated, untransposed feed: a coupler tells you nothing
        // about what the destination keyboard IS.
        if (a->conditionSwitchId != 0 || a->midiIncrement != 0) continue;
        const Id dst = static_cast<Id>(a->destKeyboard);
        if (divisionOfKeyboard_.count(dst)) continue;
        divisionOfKeyboard_[dst] = srcDiv->second;
        changed = true;
      }
    }
    if (!changed) break;
  }

  // Which keyboards the key flow does not explain. Decided here, once, from
  // the bare walk with nothing engaged — never per note. A key in the middle of
  // the compass is the fair question to ask, since an edge may carry only part
  // of it.
  const std::unordered_set<Id> nothingEngaged;
  {
    KeyFlowScratch probe;
    std::vector<ExpandedNote> reached;
    for (const auto& [id, kbRow] : m.keyboards) {
      (void)kbRow;
      reached.clear();
      walk(static_cast<int>(id), 60, 1.0f, nothingEngaged, probe, reached);
      if (reached.empty()) needsFallback_.insert(id);
    }
  }

  // What each input keyboard reaches with NOTHING engaged is its own division:
  // its own keys are ungated and every coupler is gated. This is how a
  // keyboard gets a name to show the player.
  for (Id kb : inputKeyboards_) {
    const auto reached = expand(static_cast<int>(kb), 60, 1.0f, nothingEngaged);
    if (!reached.empty()) {
      Id best = static_cast<Id>(reached.front().divisionId);
      for (const auto& n : reached)
        best = std::min(best, static_cast<Id>(n.divisionId));
      primaryDivision_[kb] = best;
    }

  }

  // An assignment code for EVERY keyboard, including the drawn console manuals,
  // which never carry one themselves: follow the ungated key flow until a
  // keyboard that does. This is what lets a click on a drawn manual arrive as
  // if it had been played on the keyboard that manual represents.
  for (const auto& [id, unusedRow] : m.keyboards) {
    (void)unusedRow;
    Id at = id;
    for (int hop = 0; hop < 8; ++hop) {
      const auto kbIt = m.keyboards.find(at);
      if (kbIt != m.keyboards.end() && kbIt->second.assignmentCode > 0) {
        assignmentCode_[id] = kbIt->second.assignmentCode;
        break;
      }
      const auto e = edges_.find(at);
      if (e == edges_.end()) break;
      Id nextKb = 0;
      for (const KeyAction* a : e->second)
        if (a->destIsKeyboard && a->destKeyboard != 0 &&
            a->conditionSwitchId == 0) {
          nextKb = static_cast<Id>(a->destKeyboard);
          break;
        }
      if (nextKb == 0 || nextKb == at) break;
      at = nextKb;
    }
  }
}

int CouplerMatrix::assignmentCodeFor(Id keyboardId) const {
  const auto it = assignmentCode_.find(keyboardId);
  return it == assignmentCode_.end() ? 0 : it->second;
}

std::string CouplerMatrix::keyboardName(Id keyboardId) const {
  if (model_ != nullptr) {
    const Id div = primaryDivisionFor(keyboardId);
    const auto d = model_->divisions.find(div);
    if (d != model_->divisions.end() && !d->second.name.empty())
      return d->second.name;
    const auto kb = model_->keyboards.find(keyboardId);
    if (kb != model_->keyboards.end() && !kb->second.name.empty())
      return kb->second.name;
  }
  return "Keyboard " + std::to_string(static_cast<unsigned long long>(keyboardId));
}

Id CouplerMatrix::primaryDivisionFor(Id keyboardId) const {
  const auto it = primaryDivision_.find(keyboardId);
  return it == primaryDivision_.end() ? 0 : it->second;
}

bool hasDrawnManuals(const OrganModel& m) {
  for (const auto& [id, kb] : m.keyboards) {
    (void)id;
    if (kb.keyImageSetId != 0) return true;
    for (int a = 0; a < 3; ++a)
      if (kb.altKeyImageSetId[a] != 0) return true;
  }
  return false;
}

Id defaultKeyboard(const OrganModel& m, const CouplerMatrix& flow) {
  const std::vector<Id>& keys = flow.inputKeyboards();
  if (keys.empty()) return 0;

  // Compass declared: the widest one wins. Strictly greater, over keyboards
  // sorted ascending, so this is exactly the old fallback.
  bool anyCompass = false;
  for (Id kb : keys) {
    const auto it = m.keyboards.find(kb);
    if (it != m.keyboards.end() && it->second.numKeys > 0) {
      anyCompass = true;
      break;
    }
  }
  if (anyCompass) {
    Id best = keys.front();
    int widest = -1;
    for (Id kb : keys) {
      const auto it = m.keyboards.find(kb);
      const int n = it == m.keyboards.end() ? 0 : it->second.numKeys;
      if (n > widest) {
        widest = n;
        best = kb;
      }
    }
    return best;
  }

  // No compass anywhere: rank each keyboard's division by playable pipework.
  auto divisionOf = [&](Id kb) -> Id {
    const auto it = m.keyboards.find(kb);
    if (it != m.keyboards.end() && it->second.primaryDivisionHint != 0)
      return it->second.primaryDivisionHint;
    return flow.primaryDivisionFor(kb);
  };
  std::unordered_map<Id, size_t> pipes;
  std::unordered_set<Id> enclosed;
  for (const auto& [stopId, stop] : m.stops) {
    (void)stopId;
    size_t n = 0;
    for (const auto& e : stop.ranks) {
      const auto rit = m.ranks.find(e.rankId);
      if (rit == m.ranks.end() || rit->second.pipes.empty()) continue;
      n += rit->second.pipes.size();
      for (const auto& p : rit->second.pipes)
        if (m.pipeEnclosure.count(p.pipeId) != 0) enclosed.insert(stop.divisionId);
    }
    if (n > 0) pipes[stop.divisionId] += n;
  }
  auto codeOf = [&](Id kb) {
    const auto it = m.keyboards.find(kb);
    return it == m.keyboards.end() ? 0 : it->second.assignmentCode;
  };
  // A manual over the pedal, an unenclosed division over an enclosed one,
  // then the most pipework, then the lowest assignment code. Tuple compare
  // in that order: audibility first, musical obviousness second.
  Id best = 0;
  std::tuple<int, int, int, long long, int> bestKey{-1, -1, -1, -1, 0};
  for (Id kb : keys) {
    const Id div = divisionOf(kb);
    const auto pit = pipes.find(div);
    const int code = codeOf(kb);
    const auto key = std::make_tuple(
        pit == pipes.end() ? 0 : 1,                              // sounds at all
        (code >= 2 && code <= 5) ? 1 : 0,                        // a manual
        (enclosed.count(div) == 0) ? 1 : 0,                      // unenclosed
        pit == pipes.end() ? 0LL : static_cast<long long>(pit->second),
        -code);
    if (best == 0 || key > bestKey) {
      best = kb;
      bestKey = key;
    }
  }
  return best;
}

bool CouplerMatrix::conditionMet(const KeyAction& a,
                                 const std::unordered_set<Id>& engaged) const {
  if (a.conditionSwitchId == 0) return true; // ungated: the manual's own keys
  const bool on = engaged.count(a.conditionSwitchId) != 0;
  // A unison-off is written as an edge that is live while its switch is NOT
  // engaged, so the sense is per-edge and cannot be assumed.
  return on == a.conditionWhenEngaged;
}

namespace {
template <typename T>
bool insertUnique(std::vector<T>& v, const T& item) {
  // Linear, because these hold a handful of entries at most and a hash set
  // would allocate on the audio thread to hold them.
  for (const T& x : v)
    if (x == item) return false;
  v.push_back(item);
  return true;
}
} // namespace

void CouplerMatrix::expandInto(int sourceKeyboardId, int midiNote,
                               float velocity,
                               const std::unordered_set<Id>& engagedSwitches,
                               KeyFlowScratch& scratch,
                               std::vector<ExpandedNote>& out) const {
  const Id source = static_cast<Id>(sourceKeyboardId);
  if (!edges_.empty() && needsFallback_.count(source) == 0) {
    walk(sourceKeyboardId, midiNote, velocity, engagedSwitches, scratch, out);
    return;
  }

  // This keyboard's key flow does not reach pipework at all, so sound every
  // division at unison rather than nothing. See the header.
  if (model_ != nullptr)
    for (const auto& [divisionId, division] : model_->divisions) {
      (void)division;
      out.push_back({static_cast<int>(divisionId), midiNote, velocity});
    }
  if (out.empty()) out.push_back({0, midiNote, velocity});
}

void CouplerMatrix::walk(int sourceKeyboardId, int midiNote, float velocity,
                         const std::unordered_set<Id>& engagedSwitches,
                         KeyFlowScratch& scratch,
                         std::vector<ExpandedNote>& out) const {
  scratch.clear();
  const std::pair<Id, int> origin{static_cast<Id>(sourceKeyboardId), midiNote};
  scratch.frontier.push_back(origin);
  scratch.seen.push_back(origin);

  // Bounded rather than trusted: a file with a coupler cycle must not spin the
  // audio thread. A real organ is three or four hops deep.
  constexpr int kMaxDepth = 16;
  for (int depth = 0; depth < kMaxDepth && !scratch.frontier.empty(); ++depth) {
    scratch.next.clear();
    for (const auto& step : scratch.frontier) {
      // A keyboard sounds the division it is associated with, whether or not a
      // KeyAction says so. On a full-size organ nothing does: the manuals are
      // wired to their pipework through the switch graph, and
      // Hint_PrimaryAssociatedDivisionID is the only thing that names the link
      // without walking it.
      const auto divIt = divisionOfKeyboard_.find(step.first);
      if (divIt != divisionOfKeyboard_.end()) {
        const int div = static_cast<int>(divIt->second);
        if (insertUnique(scratch.emitted, std::pair<int, int>{div, step.second}))
          out.push_back({div, step.second, velocity});
      }

      const auto it = edges_.find(step.first);
      if (it == edges_.end()) continue;
      for (const KeyAction* a : it->second) {
        if (!a->carries(step.second)) continue;
        if (!conditionMet(*a, engagedSwitches)) continue;
        const int note = step.second + a->midiIncrement;
        // Coupled off the end of the compass: a 16' coupler at the bottom of
        // the manual has nowhere to go, and a real organ simply does not
        // sound there.
        if (note < 0 || note > 127) continue;

        if (a->destIsKeyboard) {
          const Id kb = static_cast<Id>(a->destKeyboard);
          // Keyed on keyboard AND note: the same keyboard reached at two
          // pitches through a 16' and an 8' coupler must sound both.
          if (kb != 0 && insertUnique(scratch.seen, std::pair<Id, int>{kb, note}))
            scratch.next.push_back({kb, note});
        } else if (a->destDivision != 0) {
          if (insertUnique(scratch.emitted,
                           std::pair<int, int>{a->destDivision, note}))
            out.push_back({a->destDivision, note, velocity});
        }
      }
    }
    scratch.frontier.swap(scratch.next);
  }
}

std::vector<ExpandedNote> CouplerMatrix::expand(
    int sourceKeyboardId, int midiNote, float velocity,
    const std::unordered_set<Id>& engagedSwitches) const {
  KeyFlowScratch scratch;
  std::vector<ExpandedNote> out;
  expandInto(sourceKeyboardId, midiNote, velocity, engagedSwitches, scratch, out);
  return out;
}

void CouplerMatrix::setMasterCoupler(int t, int f, int k, bool e) {
  master_[t][f][k] = e;
}

std::vector<ResolvedPipe> resolvePipes(const OrganModel& model, int divisionId,
                                       int midiNote,
                                       const std::unordered_set<Id>& engagedStops) {
  std::vector<ResolvedPipe> out;
  for (const auto& [stopId, stop] : model.stops) {
    if (stop.divisionId != divisionId) continue;
    if (engagedStops.count(stopId) == 0) continue;
    for (const StopRankEntry& entry : stop.ranks) {
      if (midiNote < entry.firstMappedDivisionNote ||
          midiNote >= entry.firstMappedDivisionNote + entry.numMappedNotes)
        continue;
      const int pipeNote = midiNote + entry.midiIncrement;
      auto rankIt = model.ranks.find(entry.rankId);
      if (rankIt == model.ranks.end()) continue;
      for (const Pipe& pipe : rankIt->second.pipes) {
        if (pipe.midiNote != pipeNote) continue;
        ResolvedPipe rp;
        rp.stopId = stopId;
        rp.rankId = entry.rankId;
        rp.pipeId = pipe.pipeId;
        rp.pipeMidiNote = pipeNote;
        out.push_back(rp);
      }
    }
  }
  return out;
}

// ---------------------------------------------------- M2.4 continuous controls

void ContinuousControlBank::reset(const OrganModel& model) {
  model_ = &model;
  values_.clear();
  values_.reserve(model.continuousControls.size());
  for (const auto& [id, c] : model.continuousControls)
    values_[id] = clampToRange(c, c.defaultValue);
  propagate();
}

int ContinuousControlBank::clampToRange(const ContinuousControl& c, int v) const {
  // The loader has already repaired reversed ranges, but this must be safe on
  // a hand-built model too.
  const int lo = std::min(c.minValue, c.maxValue);
  const int hi = std::max(c.minValue, c.maxValue);
  return v < lo ? lo : (v > hi ? hi : v);
}

void ContinuousControlBank::setValue(Id controlId, int value) {
  if (model_ == nullptr) return;
  const auto it = model_->continuousControls.find(controlId);
  if (it == model_->continuousControls.end()) return;
  values_[controlId] = clampToRange(it->second, value);
}

int ContinuousControlBank::value(Id controlId) const {
  const auto it = values_.find(controlId);
  return it == values_.end() ? 0 : it->second;
}

double ContinuousControlBank::normalised(Id controlId) const {
  if (model_ == nullptr) return 0.0;
  const auto cit = model_->continuousControls.find(controlId);
  if (cit == model_->continuousControls.end()) return 0.0;
  const ContinuousControl& c = cit->second;
  const int lo = std::min(c.minValue, c.maxValue);
  const int hi = std::max(c.minValue, c.maxValue);
  if (hi <= lo) return 0.0;
  const double t = static_cast<double>(value(controlId) - lo) /
                   static_cast<double>(hi - lo);
  return c.inverted ? 1.0 - t : t;
}

void ContinuousControlBank::propagate(Id pinned) {
  if (model_ == nullptr || model_->controlLinkages.empty()) return;

  // One pass per linkage is enough to carry a value along the longest possible
  // acyclic chain; stop early once a pass changes nothing.
  const size_t maxPasses = model_->controlLinkages.size();
  for (size_t pass = 0; pass < maxPasses; ++pass) {
    bool changed = false;
    for (const auto& l : model_->controlLinkages) {
      if (l.sourceControlId == 0 || l.destControlId == 0) continue;
      const auto dit = model_->continuousControls.find(l.destControlId);
      if (dit == model_->continuousControls.end()) continue;
      if (l.destControlId == pinned) continue; // the player's own move stands
      const auto sit = values_.find(l.sourceControlId);
      if (sit == values_.end()) continue;

      const double scaled = sit->second * l.scale + l.offset;
      const int next = clampToRange(
          dit->second, static_cast<int>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5));
      int& slot = values_[l.destControlId];
      if (slot != next) {
        slot = next;
        changed = true;
      }
    }
    if (!changed) break;
  }
}

double ContinuousControlBank::shutterFor(const Enclosure& e) const {
  if (e.continuousControlId == 0) return 1.0;
  if (model_ == nullptr ||
      model_->continuousControls.count(e.continuousControlId) == 0)
    return 1.0;
  return normalised(e.continuousControlId);
}

} // namespace mp
