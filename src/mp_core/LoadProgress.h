// How a long load reports itself, and how it is told to stop.
//
// Loading an organ is the longest thing this program does — minutes on a large
// set from a slow disk — and for all of that time the only honest UI is one
// that says where it has got to and lets the player change their mind.
//
// Deliberately tiny and JUCE-free: it is read by a UI timer on the message
// thread and written by the loader's worker threads, so everything in it is
// atomic and nothing in it allocates. A progress channel that needed a lock
// would put the message thread behind the very work it is reporting on.
#pragma once
#include <atomic>

namespace mp {

struct LoadProgress {
  enum class Phase {
    Idle,
    ReadingDefinition,  // parsing the ODF; no useful item count yet
    LoadingSamples,     // the long one: done/total are meaningful here
    BuildingConsole,    // artwork decode, back on the message thread
    Done,
    Cancelled,
    Failed,
  };

  std::atomic<Phase> phase{Phase::Idle};
  std::atomic<int> done{0};
  std::atomic<int> total{0};
  // Set by the UI, read by the loader's workers. Never cleared by the loader:
  // whoever starts a load clears it, so a cancel arriving late cannot leak
  // into the next attempt.
  std::atomic<bool> cancelled{false};

  void beginPhase(Phase p, int itemTotal = 0) {
    done.store(0, std::memory_order_relaxed);
    total.store(itemTotal, std::memory_order_relaxed);
    phase.store(p, std::memory_order_release);
  }

  bool isCancelled() const { return cancelled.load(std::memory_order_acquire); }

  // 0..1, or -1 when this phase has no countable items. The caller decides
  // whether that means a spinner or a bar; both are honest, a fake bar is not.
  double fraction() const {
    const int t = total.load(std::memory_order_relaxed);
    if (t <= 0) return -1.0;
    const int d = done.load(std::memory_order_relaxed);
    const double f = static_cast<double>(d) / static_cast<double>(t);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
  }

  static const char* phaseName(Phase p) {
    switch (p) {
      case Phase::ReadingDefinition: return "Reading the organ definition";
      case Phase::LoadingSamples:    return "Loading samples";
      case Phase::BuildingConsole:   return "Building the console";
      case Phase::Done:              return "Ready";
      case Phase::Cancelled:         return "Cancelled";
      case Phase::Failed:            return "Failed";
      case Phase::Idle:              break;
    }
    return "";
  }
};

}  // namespace mp
