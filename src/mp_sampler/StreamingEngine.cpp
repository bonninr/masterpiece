#include "StreamingEngine.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace mp {

struct SampleHandle::Impl { std::string path; };

bool SampleHandle::open(const std::string& wavPath, std::string& error) {
  // M1: path validation only. M2 wires real readers here:
  //   .wav -> JUCE MemoryMappedAudioFormatReader (direct mmap playback)
  //   .wv  -> libwavpack decode via background pool into voice buffers (ADR-011)
  // (Decoders live behind this handle; core stays linkable without audio device.)
  if (wavPath.empty()) { error = "empty sample path"; return false; }
  auto hasSuffix = [&](const char* s) {
    const std::string suf(s);
    if (wavPath.size() < suf.size()) return false;
    return std::equal(suf.rbegin(), suf.rend(), wavPath.rbegin(),
                      [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                      });
  };
  if (hasSuffix(".hbw") || hasSuffix(".hbx")) {
    error = "encrypted sample (.hbw/.hbx): only the program it was encrypted "
            "for can read it";
    return false;
  }
  if (!(hasSuffix(".wav") || hasSuffix(".wv"))) {
    error = "unsupported sample format: .wav and .wv only";
    return false;
  }
  impl = std::make_shared<Impl>();
  impl->path = wavPath;
  numChannels = 2; sampleRate = 48000.0; numFrames = 48000 * 4;
  return true;
}

int SampleHandle::readHead(float* const* dest, int numCh, int64_t startFrame, int numFramesWanted) const {
  (void)dest; (void)numCh; (void)startFrame; (void)numFramesWanted;
  return 0; // M2: real mmap read
}

int selectAttack(const PipeLayer& layer, const NoteStrike& strike) {
  for (size_t i = 0; i < layer.attacks.size(); ++i) {
    const AttackSample& a = layer.attacks[i];
    if (strike.velocity > a.velHigh) continue;
    if (strike.timeSinceCloseMs < static_cast<int64_t>(a.minTimeSinceCloseMs)) continue;
    if (strike.ctsValue > a.ctsHigh) continue;
    return static_cast<int>(i);
  }
  return -1;
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
  // short one was never heard. GrandOrgue sorts them the same way. Ties keep
  // file order.
  for (int pass = 0; pass < 2; ++pass) {
    int best = -1;
    for (size_t i = 0; i < layer.releases.size(); ++i) {
      const ReleaseSample& r = layer.releases[i];
      const bool linked =
          r.preferLinkedAttackId != 0 && r.preferLinkedAttackId == rel.attackId;
      if ((pass == 0) != linked) continue;
      if (rel.velocity > r.velHigh) continue;
      if (rel.holdTimeMs > r.holdTimeMsHigh) continue;
      if (rel.ctsValue > r.ctsHigh) continue;
      if (rel.attackVelocity > r.attackVelHigh) continue;
      if (rel.attackCts > r.attackCtsHigh) continue;
      if (best < 0 || r.holdTimeMsHigh < layer.releases[static_cast<size_t>(best)].holdTimeMsHigh)
        best = static_cast<int>(i);
    }
    if (best >= 0) return best;
  }
  return -1;
}

} // namespace mp
