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
    // Between the definition and the samples the instrument is built, and on
    // a large set that is ten seconds during which the old label said
    // "reading". Named for what is actually happening, because a dialog that
    // claims to be reading a file for ten seconds after it has read it
    // teaches the player to distrust it.
    BuildingWind,       // solving the wind system: the long half of the build
    WiringConsole,      // linkages, couplers, the key-flow walk
    LoadingSamples,     // the long one: done/total are meaningful here
    Preparing,          // audio graph, MIDI map, combinations: after the samples
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

  // A memory ceiling for the samples of this load, in bytes; 0 is none. The
  // loader charges each sample as it lands, and the first one that takes the
  // total past the ceiling stops the load exactly as Cancel would, with
  // overBudget saying why. Stopping cleanly at the limit is the point: a
  // load that runs the machine out of memory takes the program down with
  // it, and the player loses more than the organ.
  std::atomic<int64_t> budgetBytes{0};
  std::atomic<int64_t> usedBytes{0};
  std::atomic<bool> overBudget{false};

  void resetBudget(int64_t bytes) {
    budgetBytes.store(bytes, std::memory_order_relaxed);
    usedBytes.store(0, std::memory_order_relaxed);
    overBudget.store(false, std::memory_order_release);
  }
  // Count `bytes` against the ceiling. Returns false, and stops the load, once
  // the total is past it.
  bool charge(int64_t bytes) {
    const int64_t used = usedBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    const int64_t budget = budgetBytes.load(std::memory_order_relaxed);
    if (budget <= 0 || used <= budget) return true;
    overBudget.store(true, std::memory_order_release);
    cancelled.store(true, std::memory_order_release);
    return false;
  }

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
      case Phase::BuildingWind:      return "Building the wind model";
      case Phase::WiringConsole:     return "Wiring the console";
      case Phase::LoadingSamples:    return "Loading samples";
      case Phase::Preparing:         return "Preparing to play";
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
