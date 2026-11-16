// Rank/pipe -> mixer routing model (engine side of docs/screens/06).
// JUCE-free on purpose: routing decisions happen at note-on (never per
// sample) and must be deterministic + unit-testable without audio.
// Full engine (mix buses, sends, IR) lands in M4; this header is the
// contract the UI (06), validator, and engine all share. (ADR-010 scale:
// 10-50k pipes routed through up to 1024 primary buses.)
#pragma once
#include "../mp_core/OrganModel.h"

#include <array>
#include <cstdint>
#include <string>
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
};

enum class AllocationAlgorithm {
  StaticChromatic,    // (key + rankSalt + offset) % n
  StaticOctaveCycled, // ((key%12) + octave + rankSalt + offset) % n: within
                      // an octave the notes go round the members, each
                      // octave and each rank starting one further on
  LeastBusy,          // the member with the fewest pipes sounding at note-on
  StaticByRank,       // (rankSalt + offset) % n, every note of a rank on one
                      // member; the salt is the rank's place among the
                      // group's ranks, so they take the members in turn
};

// The method's word in the settings file. An unknown or missing word is
// Across the octave, the default.
inline const char* allocationToken(AllocationAlgorithm a) {
  switch (a) {
    case AllocationAlgorithm::StaticChromatic: return "chromatic";
    case AllocationAlgorithm::LeastBusy: return "busy";
    case AllocationAlgorithm::StaticByRank: return "rank";
    case AllocationAlgorithm::StaticOctaveCycled: break;
  }
  return "octave";
}
inline AllocationAlgorithm allocationFromToken(const std::string& word) {
  if (word == "chromatic") return AllocationAlgorithm::StaticChromatic;
  if (word == "busy") return AllocationAlgorithm::LeastBusy;
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
    case AllocationAlgorithm::LeastBusy:
      // Without the counts, where the octave would put it: see
      // allocateLeastBusy for the method itself.
      return allocateBus(group, midiKey, rankSalt, AllocationAlgorithm::StaticOctaveCycled, noteOffset);
    case AllocationAlgorithm::StaticByRank:
      return mod(static_cast<long long>(rankSalt) + noteOffset);
  }
  return group.members.front();
}

// The member with the fewest pipes sounding. `busy(BusId)` answers how many
// sound through a bus. A tie goes to the first member at or after the one
// the octave would pick, so an idle group spreads as Across the octave does.
template <typename Busy>
BusId allocateLeastBusy(const BusGroup& group, int midiKey, int rankSalt, int noteOffset, Busy&& busy) {
  const auto n = static_cast<int>(group.members.size());
  if (n == 0) return BusId{0};
  const BusId start =
      allocateBus(group, midiKey, rankSalt, AllocationAlgorithm::StaticOctaveCycled, noteOffset);
  int first = 0;
  while (first < n && group.members[static_cast<size_t>(first)] != start) ++first;
  BusId best = start;
  int fewest = busy(start);
  for (int k = 1; k < n && fewest > 0; ++k) {
    const BusId m = group.members[static_cast<size_t>((first + k) % n)];
    if (const int c = busy(m); c < fewest) {
      fewest = c;
      best = m;
    }
  }
  return best;
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
