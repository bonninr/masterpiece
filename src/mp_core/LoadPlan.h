// What a load reads, from the player's choices in Organ settings, Stops: the
// perspectives, stops and single ranks left out. The load and the memory
// figure on that tab both work it out here, so the figure is what the load
// does (#574).
//
// Leaving out every perspective keeps them all, and when the single ranks left
// out cover every rank the rest leaves, the single ranks give way. Stops left
// out stay out even when they take the last ranks: the load then reads no
// rank (readsNothing).
//
// JUCE-free, so the fast tests can check it.
#pragma once

#include "OrganModel.h"
#include "Perspectives.h"

#include <set>
#include <string>
#include <unordered_set>
#include <utility>

namespace mp {

struct LoadPlan {
  // The ranks the load reads; empty reads every rank, unless readsNothing.
  std::unordered_set<Id> onlyRanks;
  // The stops left out took every rank: the load reads none.
  bool readsNothing = false;
  // The choices that took effect: those naming nothing in this organ, or
  // given way, are not here.
  std::set<std::string> perspectivesLeftOut;
  std::set<Id> ranksLeftOut;
  // Stops left out, and stops none of whose ranks come.
  std::unordered_set<Id> unloadedStops;
};

inline LoadPlan planLoad(const OrganModel& model, const std::set<std::string>& perspectivesOut,
                         const std::set<Id>& stopsOut, const std::set<Id>& ranksOut) {
  const auto groups = perspectivesOf(model);
  auto plan = [&](bool withRanks, bool withStops) {
    LoadPlan p;
    // Ranks out whatever stop plays them: a perspective's, and single ones.
    // Only names this organ has count, so a choice saved for a set that has
    // since been renamed does not silence it.
    std::unordered_set<Id> out;
    for (const auto& [name, ranks] : groups)
      if (perspectivesOut.count(name) != 0) {
        p.perspectivesLeftOut.insert(name);
        out.insert(ranks.begin(), ranks.end());
      }
    if (p.perspectivesLeftOut.size() == groups.size()) {
      p.perspectivesLeftOut.clear();
      out.clear();
    }
    if (withRanks) {
      for (Id rankId : ranksOut)
        if (model.ranks.count(rankId) != 0) p.ranksLeftOut.insert(rankId);
      bool every = true;
      for (const auto& [rankId, rank] : model.ranks) {
        (void)rank;
        if (out.count(rankId) == 0 && p.ranksLeftOut.count(rankId) == 0) every = false;
      }
      if (every) p.ranksLeftOut.clear();
      out.insert(p.ranksLeftOut.begin(), p.ranksLeftOut.end());
    }
    const bool stops = withStops && !stopsOut.empty();
    if (!stops) {
      if (!out.empty())
        for (const auto& [rankId, rank] : model.ranks) {
          (void)rank;
          if (out.count(rankId) == 0) p.onlyRanks.insert(rankId);
        }
    } else {
      // A stop's rank and the alternate its tremulant swaps in go together:
      // the tremulant's recordings are as much that stop's as the rank
      // itself (#165). A rank a stop that stays plays comes anyway.
      std::unordered_set<Id> wanted;
      for (const auto& [stopId, stop] : model.stops)
        if (stopsOut.count(stopId) == 0)
          for (const auto& e : stop.ranks) {
            wanted.insert(e.rankId);
            if (e.alternateRankId != 0) wanted.insert(e.alternateRankId);
          }
      std::unordered_set<Id> leftOut;
      for (Id stopId : stopsOut) {
        const auto it = model.stops.find(stopId);
        if (it == model.stops.end()) continue;
        p.unloadedStops.insert(stopId);
        for (const auto& e : it->second.ranks)
          for (const Id rankId : {e.rankId, e.alternateRankId})
            if (rankId != 0 && wanted.count(rankId) == 0) leftOut.insert(rankId);
      }
      for (const auto& [rankId, rank] : model.ranks) {
        (void)rank;
        if (leftOut.count(rankId) == 0 && out.count(rankId) == 0) p.onlyRanks.insert(rankId);
      }
    }
    // A stop none of whose ranks came is as good as left out: the console
    // dims it.
    if (!out.empty() || (stops && p.onlyRanks.empty()))
      for (const auto& [stopId, stop] : model.stops) {
        bool any = false;
        for (const auto& e : stop.ranks)
          if (p.onlyRanks.count(e.rankId) != 0) any = true;
        if (!stop.ranks.empty() && !any) p.unloadedStops.insert(stopId);
      }
    // Nothing left to read, though something was left out.
    const bool nothing = p.onlyRanks.empty() && (!out.empty() || stops);
    return std::make_pair(p, nothing);
  };
  auto chosen = plan(true, true);
  if (chosen.second) chosen = plan(false, true);
  chosen.first.readsNothing = chosen.second;
  return chosen.first;
}

}  // namespace mp
