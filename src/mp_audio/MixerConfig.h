// The mixer: which bus each rank speaks through, and what each bus feeds.
//
// **This is the player's configuration, not the organ's.** Worth stating
// plainly, because the file name suggests otherwise: not one of the eight
// sample sets on disk declares a single audio-routing object — no bus, no
// group, no output table. The only audio-output fields that exist anywhere
// are four codes and a level trim on `_General`, plus per-pipe
// `VirtualOutputPos_*` placement. So a mixer is built the way a MIDI map is
// built: by the person at the console, saved per organ, never inside the
// sample set.
//
// That makes the routing model here a contract between the UI, the validator
// and the engine rather than a parser. See AudioGraph.h for the allocation
// primitives it is built on.
//
// Header-only and JUCE-free on purpose: routing is decided at note-on, never
// per sample, and the fast loop must be able to test it without audio.
#pragma once
#include "AudioGraph.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

// One rank's routing, plus the buses and groups it can reach.
struct MixerConfig {
  std::vector<MixerBus> buses;
  std::vector<BusGroup> groups;
  // Only ranks the player has actually routed. Anything absent falls back to
  // the simple default, so a fresh organ is audible before anyone opens the
  // mixer -- the same principle as an unmapped MIDI message still playing.
  std::unordered_map<Id, RankRouting> rankRoutings;

  const MixerBus* bus(BusId id) const {
    for (const auto& b : buses)
      if (b.id == id) return &b;
    return nullptr;
  }

  const BusGroup* group(int groupId) const {
    for (const auto& g : groups)
      if (g.groupId == groupId) return &g;
    return nullptr;
  }

  // What a rank plays through. Falls back to the simple 1:1 perspective
  // mapping rather than to silence: an unrouted rank that cannot be heard is
  // indistinguishable from a broken engine.
  RankRouting routingFor(Id rankId) const {
    const auto it = rankRoutings.find(rankId);
    if (it != rankRoutings.end()) return it->second;
    return simpleRoutingForRank(rankId);
  }

  // A stereo pair on the first two device channels, which is what almost
  // every player actually has. Built rather than assumed so the mixer opens
  // showing something true.
  static MixerConfig stereoDefault() {
    MixerConfig c;
    MixerBus left;
    left.id = BusId{1};
    left.deviceChannels = {0, 1};
    c.buses.push_back(left);
    return c;
  }
};

// --- validation -----------------------------------------------------------
//
// Both queries from the roadmap index. They take a model AND a config,
// because neither can be answered from the organ file alone: what is being
// checked is whether the player's routing covers the organ they loaded.

struct MixerDiagnostics {
  // bus-unrouted-rank: a rank whose primary perspective resolves to no bus, so
  // every pipe in it is silent. The single most expensive mistake a mixer can
  // make, and invisible until someone draws that stop.
  std::vector<Id> unroutedRanks;
  // A bus that nothing reaches a device through: no channels and no sends. Not
  // an error by itself -- a freshly added bus looks like this -- but a rank
  // routed into one is inaudible for a reason that is nowhere on screen.
  std::vector<int> busesWithoutOutput;
  // A rank routed to a bus id the config does not define. Distinct from
  // unrouted: this is a stale reference, usually a bus deleted under a
  // routing, and silently falling back would hide the deletion.
  std::vector<Id> ranksRoutedToMissingBus;

  bool clean() const {
    return unroutedRanks.empty() && busesWithoutOutput.empty() &&
           ranksRoutedToMissingBus.empty();
  }
};

inline MixerDiagnostics validateMixer(const OrganModel& model,
                                      const MixerConfig& config) {
  MixerDiagnostics d;

  for (const auto& b : config.buses)
    if (b.deviceChannels.empty()) d.busesWithoutOutput.push_back(b.id.value);

  // Sorted so two runs over the same organ report in the same order: an
  // unordered_map's order is not stable across builds, and a diagnostic that
  // reshuffles cannot be diffed.
  std::vector<Id> rankIds;
  rankIds.reserve(model.ranks.size());
  for (const auto& [id, rank] : model.ranks) rankIds.push_back(id);
  std::sort(rankIds.begin(), rankIds.end());

  for (Id rankId : rankIds) {
    const RankRouting routing = config.routingFor(rankId);
    const auto& primary = routing.perspectives[0];

    BusId dest{0};
    if (std::holds_alternative<BusId>(primary.dest)) {
      dest = std::get<BusId>(primary.dest);
    } else {
      const BusGroup* g = config.group(std::get<int>(primary.dest));
      if (g == nullptr || g->members.empty()) {
        d.unroutedRanks.push_back(rankId);
        continue;
      }
      // Any member will do for "is this reachable at all"; which one a given
      // key lands on is allocateBus's business.
      dest = g->members.front();
    }

    if (dest.value == 0) {
      d.unroutedRanks.push_back(rankId);
    } else if (config.bus(dest) == nullptr) {
      d.ranksRoutedToMissingBus.push_back(rankId);
    }
  }
  return d;
}

} // namespace mp
