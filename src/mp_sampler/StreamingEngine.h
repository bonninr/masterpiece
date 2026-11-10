// Which attack and which release sample a note plays (the selection
// matrices), and the render-thread settings. The voices themselves, their
// rings and the streamer are in VoiceEngine.
#pragma once
#include "../mp_core/OrganModel.h"
#include <cstdint>

namespace mp {

struct ParallelConfig {
  int renderThreads = 0; // 0 = auto (hardware_concurrency - 1, audio thread excluded)
  int minVoicesPerThread = 32; // below renderThreads*this total voices, render inline
};

// M2.1 selection matrices (HW ceiling semantics). A note sounds when velocity
// <= velHigh, timeSinceCloseMs >= minTimeSinceCloseMs and ctsValue <= ctsHigh.
// Of the rows that fit, the lowest control ceiling, then file order. Returns
// the attack index or -1 when nothing matches (layer stays silent for this
// note).
struct NoteStrike {
  int velocity = 64; // key velocity 0..127
  int64_t timeSinceCloseMs = INT64_MAX; // since this pipe last closed
  int ctsValue = 127; // attack continuous-control value 0..127
};

int selectAttack(const PipeLayer& layer, const NoteStrike& strike);

struct NoteRelease {
  int attackIndex = 0; // index played at note-on
  int attackId = 0; // UniqueID played at note-on (coupling key)
  int attackVelocity = 64; // velocity of the played attack
  int attackCts = 127; // cts value of the played attack
  int velocity = 64; // key-off velocity 0..127
  int64_t holdTimeMs = 0; // key hold duration
  int ctsValue = 127; // release continuous-control value 0..127
};

// Release choice: candidates must match velocity/hold/cts ceilings AND the
// attack context; releases naming this attackId via
// ReleaseSelCriteria_PreferThisRelForAttackID win; among the rest, the
// tightest hold-time ceiling that fits, then the lowest control ceiling (ties:
// file order).
int selectRelease(const PipeLayer& layer, const NoteRelease& rel);

} // namespace mp
