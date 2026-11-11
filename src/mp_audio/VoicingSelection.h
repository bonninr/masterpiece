// What a voicing change reaches (#848): one rank or several -- a stop's, a
// division's, a windchest's -- and in each of them the whole rank, one pipe,
// or the pipes of one side of the chest.
//
// A voicer works on what sounds together. A stop of three ranks is evened
// out once, not three times; a C-side chest that speaks louder than the C#
// side is brought down as one. The sliders show the first pipe or rank
// reached, and a value set on them is given to every one, as GrandOrgue's
// organ settings do with several chosen; a step from the console moves each
// by the step, so a group keeps its shape.
//
// JUCE-free and header-only, like VoicingSet.h.
#pragma once
#include "../mp_core/OrganModel.h"
#include "../mp_core/Perspectives.h"
#include "VoicingSet.h"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mp {

enum class VoicingScope {
  Rank,       // each rank's own trim
  Pipe,       // the pipe at one note of each rank
  CSide,      // every pipe at C, D, E, F#, G#, A#
  CSharpSide  // every pipe at C#, D#, F, G, A, B
};

struct VoicingTarget {
  std::vector<Id> ranks;
  VoicingScope scope = VoicingScope::Rank;
  int note = 60;  // for VoicingScope::Pipe
  bool operator==(const VoicingTarget& o) const {
    return ranks == o.ranks && scope == o.scope && (scope != VoicingScope::Pipe || note == o.note);
  }
};

// The two sides of a chest: a pipe organ's chest is often laid out with the C
// pipes on one side and the C# pipes on the other, a whole tone apart.
inline bool onCSide(int midiNote) { return midiNote % 2 == 0; }

// The pipes a target reaches, each with its rank, ranks in the target's
// order and pipes by note. Empty for VoicingScope::Rank.
inline std::vector<std::pair<Id, Id>> pipesOf(const OrganModel& model, const VoicingTarget& t) {
  std::vector<std::pair<Id, Id>> out;
  if (t.scope == VoicingScope::Rank) return out;
  for (const Id rankId : t.ranks) {
    const auto it = model.ranks.find(rankId);
    if (it == model.ranks.end()) continue;
    std::vector<std::pair<int, Id>> byNote;
    for (const auto& pipe : it->second.pipes) {
      const bool in = t.scope == VoicingScope::Pipe    ? pipe.midiNote == t.note
                      : t.scope == VoicingScope::CSide ? onCSide(pipe.midiNote)
                                                       : !onCSide(pipe.midiNote);
      if (in) byNote.push_back({pipe.midiNote, pipe.pipeId});
    }
    std::sort(byNote.begin(), byNote.end());
    for (const auto& [note, pipeId] : byNote) out.push_back({rankId, pipeId});
  }
  return out;
}

// What the sliders show: the first rank's trim, or the first pipe's own.
inline PipeVoicing shownVoicing(const VoicingSet& set, const OrganModel& model, const VoicingTarget& t) {
  if (t.scope == VoicingScope::Rank) return t.ranks.empty() ? PipeVoicing{} : set.rank(t.ranks.front());
  const auto pipes = pipesOf(model, t);
  return pipes.empty() ? PipeVoicing{} : set.pipe(pipes.front().second);
}

// Which of a voicing's values setVoicing gives: the one slider moved, so a
// group's other values keep their differences.
enum VoicingValues : unsigned {
  kVoicingLevel = 1,
  kVoicingTuning = 2,
  kVoicingBrightness = 4,
  kVoicingBass = 8,
  kVoicingPan = 16,
  kVoicingWidth = 32,
};

// The values named in `which`, taken from `v`, for every rank or pipe
// reached. A pipe of its own takes only a level and a tuning.
inline void setVoicing(VoicingSet& set, const OrganModel& model, const VoicingTarget& t, const PipeVoicing& v,
                       unsigned which) {
  const auto give = [&](PipeVoicing& pv, bool rank) {
    if (which & kVoicingLevel) pv.gainDb = v.gainDb;
    if (which & kVoicingTuning) pv.tuningCents = v.tuningCents;
    if (!rank) return;
    if (which & kVoicingBrightness) pv.brightnessDb = v.brightnessDb;
    if (which & kVoicingBass) pv.bassDb = v.bassDb;
    if (which & kVoicingPan) pv.balance = v.balance;
    if (which & kVoicingWidth) pv.width = v.width;
  };
  if (t.scope == VoicingScope::Rank) {
    for (const Id rankId : t.ranks) {
      PipeVoicing pv = set.rank(rankId);
      give(pv, true);
      set.setRank(rankId, pv);
    }
    return;
  }
  for (const auto& [rankId, pipeId] : pipesOf(model, t)) {
    PipeVoicing pv = set.pipe(pipeId);
    give(pv, false);
    set.setPipe(pipeId, pv);
  }
}

// A step on every rank or pipe reached, each from where it stands, held
// within +/-limit and kept to tenths. False when the target reaches nothing.
inline bool stepVoicing(VoicingSet& set, const OrganModel& model, const VoicingTarget& t, VoicingField field,
                        float by, float limit) {
  const auto step = [&](PipeVoicing& pv) {
    const float v = std::round((voicingValue(pv, field) + by) * 10.0f) / 10.0f;
    setVoicingValue(pv, field, std::clamp(v, -limit, limit));
  };
  if (t.scope == VoicingScope::Rank) {
    for (const Id rankId : t.ranks) {
      PipeVoicing pv = set.rank(rankId);
      step(pv);
      set.setRank(rankId, pv);
    }
    return !t.ranks.empty();
  }
  const auto pipes = pipesOf(model, t);
  for (const auto& [rankId, pipeId] : pipes) {
    PipeVoicing pv = set.pipe(pipeId);
    step(pv);
    set.setPipe(pipeId, pv);
  }
  return !pipes.empty();
}

// --- the groups a voicer chooses from ---------------------------------------

// A stop's ranks, in the order the stop names them, each once; then those it
// reaches through its pallets (#1055).
inline std::vector<Id> ranksOfStop(const OrganModel& model, Id stopId) {
  std::vector<Id> out;
  const auto it = model.stops.find(stopId);
  if (it == model.stops.end()) return out;
  const auto add = [&](Id rankId) {
    if (model.ranks.count(rankId) != 0 && std::find(out.begin(), out.end(), rankId) == out.end())
      out.push_back(rankId);
  };
  for (const auto& e : it->second.ranks) add(e.rankId);
  for (const Id rankId : it->second.palletRanks) add(rankId);
  return out;
}

// A division's ranks: those of every stop it holds, by id.
inline std::vector<Id> ranksOfDivision(const OrganModel& model, Id divisionId) {
  std::vector<Id> out;
  for (const auto& [stopId, stop] : model.stops)
    if (stop.divisionId == divisionId)
      for (const Id rankId : ranksOfStop(model, stopId))
        if (std::find(out.begin(), out.end(), rankId) == out.end()) out.push_back(rankId);
  std::sort(out.begin(), out.end());
  return out;
}

// The windchests and their ranks: a GrandOrgue set's windchest groups, or
// else the wind compartments the ranks' pipes draw from, by name. Empty when
// there are fewer than two, since one chest chooses nothing.
inline std::map<std::string, std::vector<Id>> windchestRanks(const OrganModel& model) {
  auto groups = windchestsOf(model);
  if (!groups.empty()) return groups;
  for (const auto& [rankId, rank] : model.ranks) {
    std::unordered_set<Id> seen;
    for (const auto& pipe : rank.pipes) {
      const Id c = pipe.windSourceCompartmentId;
      if (c == 0 || !seen.insert(c).second) continue;
      const auto w = model.wind.find(c);
      if (w == model.wind.end()) continue;
      groups[w->second.name.empty() ? std::to_string(c) : w->second.name].push_back(rankId);
    }
  }
  if (groups.size() < 2) groups.clear();
  // Two compartments of one name are one chest.
  for (auto& [name, ids] : groups) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }
  return groups;
}

}  // namespace mp
