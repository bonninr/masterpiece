// Rank/pipe -> mixer routing model (engine side of docs/screens/06).
// JUCE-free on purpose: routing decisions happen at note-on (never per
// sample) and must be deterministic + unit-testable without audio.
// Full engine (mix buses, sends, IR) lands in M4; this header is the
// contract the UI (06), validator, and engine all share. (ADR-010 scale:
// 10-50k pipes routed through up to 1024 primary buses.)
#pragma once
#include "../mp_core/OrganModel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mp {

// Strong bus id (NOT a plain int alias): group ids are bare ints, so
// std::variant<BusId, int> needs distinct types to compile.
struct BusId {
  int value = 0; // 1-based like HW (0001..1024); 0 = none (silent rank)
  constexpr BusId() = default;
  constexpr explicit BusId(int v) : value(v) {}
  constexpr bool operator==(const BusId& o) const { return value == o.value; }
  constexpr bool operator!=(const BusId& o) const { return value != o.value; }
};
inline constexpr int kNumPerspectives = 4;

struct MixerBus {
  BusId id{};
  float levelDb = 0.0f;
  // Device channels, counted from 0. A stereo pair takes them two at a time,
  // left then right: the first two are the pair, each further two a copy of
  // it. A mono pair plays left and right summed on each channel it names:
  // the first is its own, the rest copies. Empty = no direct output.
  std::vector<int> deviceChannels;
  bool mono = false;
  // The player's name for the output ("Main", "Gallery"); empty shows its
  // number.
  std::string name;
  // Other outputs this one also plays into, by id: four speaker outputs
  // feeding the main mix as well. The signal is taken after this output's
  // fader and its room, and joins the target before the target's room, and
  // through the target's fader (#1188). sendDb holds each send's level, in step with sends; a missing
  // entry is 0 dB.
  std::vector<int> sends;
  std::vector<float> sendDb;
  float sendLevelDb(int target) const {
    for (size_t i = 0; i < sends.size(); ++i)
      if (sends[i] == target) return i < sendDb.size() ? sendDb[i] : 0.0f;
    return 0.0f;
  }
};

// The salt is the rank's stop family (rankFamilies below): the ranks one stop
// plays -- its perspectives, its tremmed copies, a mixture's ranks -- share
// it, so one key of one stop sounds from one member. Another stop starts one
// member further on.
enum class AllocationAlgorithm {
  StaticChromatic,    // (key + rankSalt + offset) % n
  StaticOctaveCycled, // ((key%12) + octave + rankSalt + offset) % n: within
                      // an octave the notes go round the members, each
                      // octave and each stop starting one further on
  StaticByRank,       // (rankSalt + offset) % n, every note of a stop on one
                      // member; the salt is the stop family's place among
                      // the group's families, so they take the members in turn
};

// The method's word in the settings file. An unknown or missing word is
// Across the octave, the default; so is "busy", the Least busy of the first
// previews, gone because a note has to sound from the same speaker every
// time it is played (#1023).
inline const char* allocationToken(AllocationAlgorithm a) {
  switch (a) {
    case AllocationAlgorithm::StaticChromatic: return "chromatic";
    case AllocationAlgorithm::StaticByRank: return "rank";
    case AllocationAlgorithm::StaticOctaveCycled: break;
  }
  return "octave";
}
inline AllocationAlgorithm allocationFromToken(const std::string& word) {
  if (word == "chromatic") return AllocationAlgorithm::StaticChromatic;
  if (word == "rank") return AllocationAlgorithm::StaticByRank;
  return AllocationAlgorithm::StaticOctaveCycled;
}

// A speaker group never processes audio itself: at note-on, one member bus
// is picked for the pipe and the voice plays through it.
struct BusGroup {
  int groupId = 0;
  std::vector<BusId> members; // primary bus ids, in user order
  AllocationAlgorithm algorithm = AllocationAlgorithm::StaticOctaveCycled;
};

struct PerspectiveRoute {
  // Destination: a primary bus directly, or a group to allocate from.
  std::variant<BusId, int> dest = BusId{0}; // int = groupId
  AllocationAlgorithm algorithm = AllocationAlgorithm::StaticOctaveCycled;
  int noteOffset = 0;
};

struct RankRouting {
  Id rankId = 0;
  std::array<PerspectiveRoute, kNumPerspectives> perspectives{};
};

// Pick one member bus for (key, rank) at note-on. Returns 0 when the group
// is empty (rank inaudible — validator flags this, engine stays silent).
inline BusId allocateBus(const BusGroup& group, int midiKey, int rankSalt,
                         AllocationAlgorithm algo, int noteOffset) {
  const auto n = static_cast<int>(group.members.size());
  if (n == 0) return BusId{0};
  auto mod = [&](long long v) {
    long long r = v % n;
    return group.members[static_cast<size_t>(r < 0 ? r + n : r)];
  };
  switch (algo) {
    case AllocationAlgorithm::StaticChromatic:
      return mod(static_cast<long long>(midiKey) + rankSalt + noteOffset);
    case AllocationAlgorithm::StaticOctaveCycled: {
      const int pc = ((midiKey % 12) + 12) % 12;
      const int octave = midiKey / 12;
      return mod(static_cast<long long>(pc) + octave + rankSalt + noteOffset);
    }
    case AllocationAlgorithm::StaticByRank:
      return mod(static_cast<long long>(rankSalt) + noteOffset);
  }
  return group.members.front();
}

// Each rank's stop family, numbered 0.. in the order of each family's lowest
// rank id. Ranks drawn by one stop are one family, and a rank two stops share
// joins their families, so a pipe's perspectives, its tremmed copy and a
// mixture's ranks speak from one member of a speaker group. A rank no stop
// names is a family of its own.
inline std::unordered_map<Id, int> rankFamilies(const OrganModel& model) {
  std::unordered_map<Id, Id> parent;
  for (const auto& [rankId, rank] : model.ranks) parent[rankId] = rankId;
  const auto find = [&parent](Id r) {
    Id root = r;
    while (parent[root] != root) root = parent[root];
    while (parent[r] != root) {
      const Id next = parent[r];
      parent[r] = root;
      r = next;
    }
    return root;
  };
  const auto join = [&](Id a, Id b) {
    if (parent.count(a) == 0 || parent.count(b) == 0) return;
    const Id ra = find(a), rb = find(b);
    if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
  };
  for (const auto& [stopId, stop] : model.stops) {
    (void)stopId;
    Id first = 0;
    const auto add = [&](Id r) {
      if (r == 0 || parent.count(r) == 0) return;
      if (first == 0) first = r;
      else join(first, r);
    };
    for (const auto& e : stop.ranks) {
      add(e.rankId);
      add(e.alternateRankId);
    }
    for (Id r : stop.palletRanks) add(r);
  }
  std::vector<Id> ids;
  ids.reserve(parent.size());
  for (const auto& [id, p] : parent) ids.push_back(id);
  std::sort(ids.begin(), ids.end());
  std::unordered_map<Id, int> numberOfRoot, family;
  for (Id id : ids) {
    const Id root = find(id);
    const auto it = numberOfRoot.emplace(root, static_cast<int>(numberOfRoot.size())).first;
    family[id] = it->second;
  }
  return family;
}

// Simple-routing defaults (09/10 screens, HW9 simple model): perspectives
// 1-4 straight to primary buses 1-4; default group 0005 = buses 5-8 carries
// perspective 1 when the user picks grouped output.
inline RankRouting simpleRoutingForRank(Id rankId) {
  RankRouting r;
  r.rankId = rankId;
  for (int p = 0; p < kNumPerspectives; ++p)
    r.perspectives[static_cast<size_t>(p)].dest = BusId{p + 1};
  return r;
}

} // namespace mp
