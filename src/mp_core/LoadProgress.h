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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace mp {

struct LoadProgress {
  enum class Phase {
    Idle,
    OpeningArchive,
    ReadingArchive,
    ExtractingArchive,
    ReadingDefinition,  // parsing the ODF; no useful item count yet
    // Between the definition and the samples the instrument is built, and on
    // a large set that is ten seconds during which the old label said
    // "reading". Named for what is actually happening, because a dialog that
    // claims to be reading a file for ten seconds after it has read it
    // teaches the player to distrust it.
    BuildingWind,       // solving the wind system: the long half of the build
    WiringConsole,      // linkages, couplers, the key-flow walk
    // The organ being left is let go, the samples the pipework wants are
    // listed and looked for on disk and in the cache. On a large set this is
    // seconds, and it was shown as "Wiring the console", which takes
    // milliseconds (#133).
    FindingSamples,
    // The samples read back from the cache of an earlier load. On a large
    // set this is the long part of a cached load, and it was shown as
    // "Finding the samples" with no progress (#591). done/total count samples.
    ReadingCache,
    LoadingSamples,     // the long one: done/total are meaningful here
    // The samples just read are written to the cache for the next load, with
    // the organ not yet playable (unless the player chose to save in silence
    // while it plays). archiveFraction carries how far the write has got.
    SavingCache,
    Preparing,          // audio graph, MIDI map, combinations: after the samples
    BuildingConsole,    // artwork decode, back on the message thread
    Done,
    Cancelled,
    Failed,
  };

  std::atomic<Phase> phase{Phase::Idle};
  // What "Preparing to play" is doing at the moment, for the loading window:
  // after the last sample there are seconds of work on a large set, and a
  // window that says nothing then looks stuck. `stepCount` is the number the
  // step names (the voices being set up), or 0.
  enum class Step { None, LettingGo, Voices, Releases, Audio, Ranks, Combinations };
  std::atomic<Step> step{Step::None};
  std::atomic<int> stepCount{0};
  void beginStep(Step s, int count = 0) {
    stepCount.store(count, std::memory_order_relaxed);
    step.store(s, std::memory_order_release);
  }
  std::atomic<int> done{0};
  std::atomic<int> total{0};
  // Archive indexing counts compressed bytes; extraction counts the files
  // up to the last definition/artwork. Both publish a per-phase fraction.
  std::atomic<double> archiveFraction{-1.0};
  // Set by the UI, read by the loader's workers. Never cleared by the loader:
  // whoever starts a load clears it, so a cancel arriving late cannot leak
  // into the next attempt.
  std::atomic<bool> cancelled{false};

  void beginPhase(Phase p, int itemTotal = 0) {
    // The stage being left is charged the time since it began.
    const int64_t now = nowMs();
    const auto leaving = static_cast<size_t>(phase.load(std::memory_order_acquire));
    const int64_t began = phaseBeganMs.exchange(now, std::memory_order_acq_rel);
    if (began > 0 && leaving < phaseMs.size())
      phaseMs[leaving].fetch_add(now - began, std::memory_order_relaxed);
    archiveFraction.store(-1.0, std::memory_order_relaxed);
    step.store(Step::None, std::memory_order_relaxed);
    stepCount.store(0, std::memory_order_relaxed);
    done.store(0, std::memory_order_relaxed);
    total.store(itemTotal, std::memory_order_relaxed);
    phase.store(p, std::memory_order_release);
  }

  // How long each stage of this load took, for the log: a report of a slow
  // load can then say which stage was slow. Cleared by whoever starts a load,
  // stage by stage.
  static constexpr size_t kPhases = 16;
  std::array<std::atomic<int64_t>, kPhases> phaseMs{};
  std::atomic<int64_t> phaseBeganMs{0};
  int64_t timeIn(Phase p) const {
    const auto i = static_cast<size_t>(p);
    return i < phaseMs.size() ? phaseMs[i].load(std::memory_order_relaxed) : 0;
  }
  static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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

  // The watchdog: what the system says is still free, asked as the load goes
  // on. The ceiling is the program's own reckoning, and a reckoning can be
  // wrong or the rest of the computer can take memory meanwhile; the system's
  // figure cannot. A load that would leave less than `reserveBytes` free
  // stops there, as at the ceiling, so the program is never the one the
  // system ends for want of memory. Set before the load starts and left
  // alone while it runs; it returns -1 when the system gives no figure.
  std::function<int64_t()> freeMemory;
  int64_t reserveBytes = 0;
  // Stopped because the system ran low, the ceiling not yet reached.
  std::atomic<bool> memoryLow{false};
  // Asked at most once in this many bytes charged, and for every charge as
  // large: reading the figure costs a system call.
  static constexpr int64_t kCheckEveryBytes = int64_t{32} * 1024 * 1024;
  int64_t checkEveryBytes = kCheckEveryBytes;
  std::atomic<int64_t> sinceCheck{0};

  void resetBudget(int64_t bytes) {
    budgetBytes.store(bytes, std::memory_order_relaxed);
    usedBytes.store(0, std::memory_order_relaxed);
    sinceCheck.store(0, std::memory_order_relaxed);
    memoryLow.store(false, std::memory_order_relaxed);
    overBudget.store(false, std::memory_order_release);
  }
  // Count `bytes` against the ceiling and against what the system has free.
  // Returns false, and stops the load, once either says no more. A charge
  // made before its bytes are taken (a cache read back whole) needs them free
  // as well as the reserve; one made after (a sample read) is already in the
  // figure, and needing it twice only stops a little early.
  bool charge(int64_t bytes) {
    const int64_t used = usedBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    const int64_t budget = budgetBytes.load(std::memory_order_relaxed);
    if (budget > 0 && used > budget) return stop(false);
    if (freeMemory) {
      const int64_t since = sinceCheck.fetch_add(bytes, std::memory_order_relaxed) + bytes;
      if (bytes >= checkEveryBytes || since >= checkEveryBytes) {
        sinceCheck.store(0, std::memory_order_relaxed);
        const int64_t free = freeMemory();
        if (free >= 0 && free < reserveBytes + bytes) return stop(true);
      }
    }
    return true;
  }
  // The system refused memory outright: the load stops as it does when free
  // memory runs low, keeping what it read.
  void memoryRefused() { stop(true); }

 private:
  bool stop(bool low) {
    if (low) memoryLow.store(true, std::memory_order_relaxed);
    overBudget.store(true, std::memory_order_release);
    cancelled.store(true, std::memory_order_release);
    return false;
  }

 public:

  // 0..1, or -1 when this phase has no countable items. The caller decides
  // whether that means a spinner or a bar; both are honest, a fake bar is not.
  double fraction() const {
    const auto current = phase.load(std::memory_order_acquire);
    if (current == Phase::OpeningArchive) return -1.0;
    if (current == Phase::ReadingArchive || current == Phase::ExtractingArchive ||
        current == Phase::SavingCache)
      return archiveFraction.load(std::memory_order_relaxed);
    const int t = total.load(std::memory_order_relaxed);
    if (t <= 0) return -1.0;
    const int d = done.load(std::memory_order_relaxed);
    const double f = static_cast<double>(d) / static_cast<double>(t);
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
  }

  static const char* phaseName(Phase p) {
    switch (p) {
      case Phase::OpeningArchive:    return "Opening the archive";
      case Phase::ReadingArchive:    return "Reading the archive";
      case Phase::ExtractingArchive: return "Extracting definitions and artwork";
      case Phase::ReadingDefinition: return "Reading the organ definition";
      case Phase::BuildingWind:      return "Building the wind model";
      case Phase::WiringConsole:     return "Wiring the console";
      case Phase::FindingSamples:    return "Finding the samples";
      case Phase::ReadingCache:      return "Loading samples from the cache";
      case Phase::LoadingSamples:    return "Loading samples";
      case Phase::SavingCache:       return "Creating the sample cache";
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
