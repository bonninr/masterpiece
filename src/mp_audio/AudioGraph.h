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
inline constexpr int kMaxPrimaryBuses = 1024;
inline constexpr int kMaxBusGroups = 1024;
inline constexpr int kNumPerspectives = 4;

enum class BusKind { Primary, Intermediate, Master };

struct MixerBus {
  BusId id{};
  BusKind kind = BusKind::Primary;
  float levelDb = 0.0f;
  std::vector<int> deviceChannels; // empty = no direct output (mix-send only)
  // M4: sends to mix buses {busId, levelDb}, per-bus IR {file, wet%, perOrgan}.
};

// A group never processes audio itself: at note-on, one member bus is picked
// for the pipe (and its perspectives) and the voice plays through it.
struct BusGroup {
  int groupId = 0;
  std::vector<BusId> members; // primary bus ids, in user order
};

enum class AllocationAlgorithm {
  StaticChromatic,    // (key + rankSalt + offset) % n — v1 default
  StaticOctaveCycled, // ((key%12) + octave + rankSalt + offset) % n;
                      // v1 approximation of the HW default
                      // ('Static: cyclic within octave, octaves cycled,
                      // ranks cycled'); refined against a licensed install at M4
  DynamicRoundRobin,  // M4: least-recently-used member bus in group
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
    case AllocationAlgorithm::DynamicRoundRobin:
      // M4: needs mutable LRU state; v1 falls back to static (documented).
      return mod(static_cast<long long>(midiKey) + rankSalt + noteOffset);
  }
  return group.members.front();
}

// Resolve all four perspective destinations for one pipe at note-on.
// Direct bus dests pass through; group dests allocate. rankSalt should be
// stable per rank (rankId is the natural salt — keeps chords spread).
inline std::array<BusId, kNumPerspectives> voiceRoutes(
    const RankRouting& routing, const BusGroup& (*findGroup)(int),
    int midiKey) {
  std::array<BusId, kNumPerspectives> out{};
  for (int p = 0; p < kNumPerspectives; ++p) {
    const auto& pr = routing.perspectives[p];
    if (std::holds_alternative<BusId>(pr.dest)) {
      out[static_cast<size_t>(p)] = std::get<BusId>(pr.dest);
    } else {
      const BusGroup& g = findGroup(std::get<int>(pr.dest));
      out[static_cast<size_t>(p)] = allocateBus(
          g, midiKey, routing.rankId, pr.algorithm, pr.noteOffset);
    }
  }
  return out;
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
