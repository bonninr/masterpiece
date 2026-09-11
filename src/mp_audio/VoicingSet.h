// Voicing: the player's adjustments to individual pipes and ranks.
//
// A real organ is voiced by a person with a knife and a tuning cone, one pipe
// at a time, and a sample set arrives already voiced by whoever recorded it.
// What this offers is the same operation in software: make that one shrill
// Mixture rank quieter, pull a single sour pipe back into tune.
//
// **Two levels, and they ADD.** A rank-level adjustment is the coarse one —
// "this whole Mixture is 3 dB too loud" — and a pipe-level one is the
// touch-up for the single note that is wrong. Adding them rather than letting
// the pipe override the rank means retuning one pipe does not silently
// discard the rank trim it sits under.
//
// Deliberately JUCE-free and header-only: voicing is read at note-on, never
// per sample, and the fast loop must be able to test it without audio.
#pragma once
#include "../mp_core/OrganModel.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace mp {

struct PipeVoicing {
  float gainDb = 0.0f;
  float tuningCents = 0.0f;
  // Declared because the UI and the file format need somewhere to put them,
  // and NOT applied to audio yet: both need per-voice filtering, which is a
  // cost that has to be designed against the polyphony budget rather than
  // bolted on. A value here is stored and shown, never silently heard.
  float brightnessDb = 0.0f;
  float balance = 0.0f;  // -1 left, +1 right

  bool isNeutral() const {
    return gainDb == 0.0f && tuningCents == 0.0f && brightnessDb == 0.0f &&
           balance == 0.0f;
  }
  void add(const PipeVoicing& o) {
    gainDb += o.gainDb;
    tuningCents += o.tuningCents;
    brightnessDb += o.brightnessDb;
    balance += o.balance;
  }
};

class VoicingSet {
 public:
  void setRank(Id rankId, const PipeVoicing& v) { store(ranks_, rankId, v); }
  void setPipe(Id pipeId, const PipeVoicing& v) { store(pipes_, pipeId, v); }

  PipeVoicing rank(Id rankId) const { return get(ranks_, rankId); }
  PipeVoicing pipe(Id pipeId) const { return get(pipes_, pipeId); }

  // What actually reaches a voice: the rank trim plus this pipe's own.
  PipeVoicing effective(Id rankId, Id pipeId) const {
    PipeVoicing v = get(ranks_, rankId);
    v.add(get(pipes_, pipeId));
    return v;
  }

  // Cheap enough to ask per note-on, which is the point: an organ nobody has
  // voiced must not pay two hash lookups for every pipe of every chord.
  bool empty() const { return ranks_.empty() && pipes_.empty(); }

  void clear() {
    ranks_.clear();
    pipes_.clear();
  }

  const std::unordered_map<Id, PipeVoicing>& ranks() const { return ranks_; }
  const std::unordered_map<Id, PipeVoicing>& pipes() const { return pipes_; }

  // Ids in a stable order, so a saved file diffs against its predecessor
  // rather than reshuffling on every write.
  std::vector<Id> rankIds() const { return sortedKeys(ranks_); }
  std::vector<Id> pipeIds() const { return sortedKeys(pipes_); }

 private:
  using Map = std::unordered_map<Id, PipeVoicing>;

  // A neutral entry is erased rather than stored. Otherwise "set it, then put
  // it back" leaves a row behind, and a file full of zeroes is indis-
  // tinguishable from a file full of forgotten experiments.
  static void store(Map& m, Id id, const PipeVoicing& v) {
    if (id == 0) return;
    if (v.isNeutral()) m.erase(id);
    else m[id] = v;
  }
  static PipeVoicing get(const Map& m, Id id) {
    const auto it = m.find(id);
    return it == m.end() ? PipeVoicing{} : it->second;
  }
  static std::vector<Id> sortedKeys(const Map& m) {
    std::vector<Id> out;
    out.reserve(m.size());
    for (const auto& [id, v] : m) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
  }

  Map ranks_;
  Map pipes_;
};

// Cents to a frequency ratio. A pipe pulled 10 cents flat plays at 0.9942 of
// its pitch; this is the whole of what tuningCents does.
inline double centsRatio(double cents) {
  return cents == 0.0 ? 1.0 : std::pow(2.0, cents / 1200.0);
}

// Two complete sets plus which one is live. A/B is how voicing is actually
// done: make a change, listen to it against what was there before, keep the
// one that is right. Without it the comparison is from memory, and memory
// flatters whichever was heard last.
struct VoicingAB {
  VoicingSet a;
  VoicingSet b;
  bool usingB = false;

  VoicingSet& live() { return usingB ? b : a; }
  const VoicingSet& live() const { return usingB ? b : a; }
  void swap() { usingB = !usingB; }
  // Start the other slot from this one, which is what "try something" means:
  // an A/B against an empty B is a comparison with silence, not with an idea.
  void copyToOther() {
    if (usingB) a = b;
    else b = a;
  }
};

}  // namespace mp
