#include "StreamingEngine.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace mp {

int selectAttack(const PipeLayer& layer, const NoteStrike& strike) {
  // Rows chosen by a control are bands by ceiling, so the lowest ceiling the
  // value fits wins, wherever the row sits in the file: St. Maximin lists the
  // two tunings of a pipe in either order. A layer with no control asks at 127,
  // only the top rows fit, and the first of them is taken.
  int best = -1;
  for (size_t i = 0; i < layer.attacks.size(); ++i) {
    const AttackSample& a = layer.attacks[i];
    if (strike.velocity > a.velHigh) continue;
    if (strike.timeSinceCloseMs < static_cast<int64_t>(a.minTimeSinceCloseMs)) continue;
    if (strike.ctsValue > a.ctsHigh) continue;
    if (best < 0 || a.ctsHigh < layer.attacks[static_cast<size_t>(best)].ctsHigh)
      best = static_cast<int>(i);
  }
  return best;
}

int selectRelease(const PipeLayer& layer, const NoteRelease& rel) {
  // Two passes: prefer-linked first, then the rest. Both apply the full
  // ceiling match (velocity, hold time, cts, attack context).
  //
  // Of the releases that match, the one with the TIGHTEST hold-time ceiling:
  // a set lists its releases in any order, and a short note matches the
  // medium and the long release as well as the short one written for it.
  // Nancy lists hers medium (602 ms), short (284 ms), long, and taking the
  // first match gave every note under 602 ms the medium release, so the
  // short one was never heard. GrandOrgue sorts them the same way. Between
  // equal hold times the lowest control ceiling wins, as for an attack. Ties
  // keep file order.
  for (int pass = 0; pass < 2; ++pass) {
    int best = -1;
    for (size_t i = 0; i < layer.releases.size(); ++i) {
      const ReleaseSample& r = layer.releases[i];
      const bool linked =
          r.preferLinkedAttackId != 0 && r.preferLinkedAttackId == rel.attackId;
      if ((pass == 0) != linked) continue;
      if (r.empty) continue;
      if (rel.velocity > r.velHigh) continue;
      if (rel.holdTimeMs > r.holdTimeMsHigh) continue;
      if (rel.ctsValue > r.ctsHigh) continue;
      if (rel.attackVelocity > r.attackVelHigh) continue;
      if (rel.attackCts > r.attackCtsHigh) continue;
      if (best < 0) {
        best = static_cast<int>(i);
        continue;
      }
      const ReleaseSample& b = layer.releases[static_cast<size_t>(best)];
      if (r.holdTimeMsHigh < b.holdTimeMsHigh ||
          (r.holdTimeMsHigh == b.holdTimeMsHigh && r.ctsHigh < b.ctsHigh))
        best = static_cast<int>(i);
    }
    if (best >= 0) return best;
  }
  return -1;
}

} // namespace mp
