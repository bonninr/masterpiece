#include "SparseLoading.h"

#include <algorithm>
#include <cstdlib>

namespace mp {
namespace {

// The sample ids a pipe's layers name.
void sampleIdsOf(const Pipe& pipe, std::unordered_set<Id>& out) {
  for (const auto& layer : pipe.layers) {
    for (const auto& a : layer.attacks)
      if (a.sample.sampleId != 0) out.insert(a.sample.sampleId);
    for (const auto& r : layer.releases)
      if (r.sample.sampleId != 0) out.insert(r.sample.sampleId);
  }
}

// Whether the pipe has an attack of its own.
bool hasOwnAttack(const Pipe& pipe) {
  for (const auto& layer : pipe.layers)
    for (const auto& a : layer.attacks)
      if (a.sample.sampleId != 0) return true;
  return false;
}

// Whether the pipe has an attack whose sample can be read.
bool hasPresentAttack(const Pipe& pipe, const std::function<bool(Id)>& present) {
  for (const auto& layer : pipe.layers)
    for (const auto& a : layer.attacks)
      if (a.sample.sampleId != 0 && present(a.sample.sampleId)) return true;
  return false;
}

}  // namespace

SparsePlan planSparseLoading(const OrganModel& model, int every,
                             const std::unordered_set<Id>& protectedRanks,
                             const std::function<bool(Id)>& samplePresent) {
  SparsePlan plan;
  // One means every note: the model is left as it is, byte for byte.
  if (every < 2) return plan;

  // The ranks in id order, so the same organ always gives the same answer.
  std::vector<Id> rankIds;
  rankIds.reserve(model.ranks.size());
  for (const auto& [id, rank] : model.ranks) {
    (void)rank;
    rankIds.push_back(id);
  }
  std::sort(rankIds.begin(), rankIds.end());

  for (Id rankId : rankIds) {
    const auto it = model.ranks.find(rankId);
    if (it == model.ranks.end()) continue;
    const Rank& rank = it->second;
    // A rank left alone: a noise, a percussive rank, a sampled tremulant the
    // caller named, or one too small to thin out.
    //
    // The tremulant ranks are the caller's to name, and NOT the model's own
    // tremulant list: that lists every pipe a synthesised tremulant reaches,
    // which is nearly every pipe of most organs. Those pipes are fine to
    // borrow, because the tremulant follows the pipe id, and the id stays.
    if (rank.isNoise || rank.percussive || protectedRanks.count(rankId) != 0 ||
        rank.pipes.size() < 8) {
      ++plan.ranksLeftAlone;
      continue;
    }
    ++plan.ranksConsidered;

    SparsePlan::Rank pr;
    pr.rankId = rankId;
    const size_t n = rank.pipes.size();
    pr.keepsOwn.assign(n, true);
    pr.neighbour.assign(n, -1);
    plan.pipes += static_cast<int>(n);

    // The pipes by the note they sound. Stable, so two pipes on the same note
    // keep the order the organ gave them.
    std::vector<size_t> byNote(n);
    for (size_t i = 0; i < n; ++i) byNote[i] = i;
    std::stable_sort(byNote.begin(), byNote.end(), [&](size_t a, size_t b) {
      return rank.pipes[a].midiNote < rank.pipes[b].midiNote;
    });

    const int lowest = rank.pipes[byNote.front()].midiNote;

    // The pipes that keep their own samples: every `every` notes from the
    // lowest, and the lowest and the highest whatever the rule says.
    std::vector<bool> kept(n, false);
    for (size_t i = 0; i < n; ++i) {
      const int d = rank.pipes[byNote[i]].midiNote - lowest;
      if (d >= 0 && d % every == 0) kept[byNote[i]] = true;
    }
    kept[byNote.front()] = true;
    kept[byNote.back()] = true;

    // The samples the kept pipes name. A pipe whose own samples are among
    // them is read anyway, so borrowing from a neighbour would save nothing
    // and change what the pipe sounds like.
    std::vector<std::unordered_set<Id>> keptSamples(n);
    for (size_t i = 0; i < n; ++i)
      if (kept[i]) sampleIdsOf(rank.pipes[i], keptSamples[i]);

    for (size_t i = 0; i < n; ++i) {
      if (kept[i]) continue;
      const Pipe& pipe = rank.pipes[i];
      // Already borrowed: a model that has been through this once keeps its
      // answer, and asking again gives the same one.
      if (pipe.borrowedFromPipeId != 0) {
        for (size_t j = 0; j < n; ++j)
          if (rank.pipes[j].pipeId == pipe.borrowedFromPipeId) {
            pr.keepsOwn[i] = false;
            pr.neighbour[i] = static_cast<int>(j);
            break;
          }
        continue;
      }
      // The nearest kept pipe, ties to the lower one.
      int neighbour = -1;
      int distance = 0;
      for (size_t j = 0; j < n; ++j) {
        if (!kept[j]) continue;
        const int d = std::abs(rank.pipes[j].midiNote - pipe.midiNote);
        if (neighbour < 0 || d < distance) {
          neighbour = static_cast<int>(j);
          distance = d;
        }
      }
      if (neighbour < 0) continue;
      // Never further than every - 1 semitones.
      if (distance > every - 1) continue;
      // The same footage: a mixture break or a transposed pipe changes it,
      // and borrowing across it would jump an octave or a fifth.
      if (rank.pipes[static_cast<size_t>(neighbour)].basePitch64ftHarmonicNum !=
          pipe.basePitch64ftHarmonicNum)
        continue;
      // The neighbour is not itself borrowed: a pipe plays one neighbour, not
      // a chain of them.
      if (rank.pipes[static_cast<size_t>(neighbour)].borrowedFromPipeId != 0) continue;
      // The neighbour's samples can be read. A demo's missing pipes stay
      // missing.
      if (!hasPresentAttack(rank.pipes[static_cast<size_t>(neighbour)], samplePresent))
        continue;
      // The pipe has a sample of its own. One with none stays as it is: it
      // says nothing, and a neighbour's samples would make it speak.
      if (!hasOwnAttack(pipe)) continue;
      // Its own samples are read anyway, by a pipe that keeps them.
      std::unordered_set<Id> mine;
      sampleIdsOf(pipe, mine);
      bool shared = false;
      for (size_t j = 0; j < n && !shared; ++j) {
        if (!kept[j]) continue;
        for (Id id : mine)
          if (keptSamples[j].count(id) != 0) {
            shared = true;
            break;
          }
      }
      if (shared) continue;
      pr.keepsOwn[i] = false;
      pr.neighbour[i] = neighbour;
      ++plan.borrowed;
    }

    for (size_t i = 0; i < n; ++i)
      if (pr.keepsOwn[i]) ++plan.kept;
    plan.ranks.push_back(std::move(pr));
  }
  return plan;
}

int applySparsePlan(OrganModel& model, const SparsePlan& plan) {
  int borrowed = 0;
  for (const auto& pr : plan.ranks) {
    const auto it = model.ranks.find(pr.rankId);
    if (it == model.ranks.end()) continue;
    Rank& rank = it->second;
    const size_t n = std::min(rank.pipes.size(), pr.neighbour.size());
    for (size_t i = 0; i < n; ++i) {
      const int j = pr.neighbour[i];
      if (j < 0 || static_cast<size_t>(j) >= rank.pipes.size()) continue;
      // A full copy of the layers: attacks, releases, loop points, gains and
      // crossfades are properties of the samples and travel with them.
      rank.pipes[i].layers = rank.pipes[static_cast<size_t>(j)].layers;
      rank.pipes[i].borrowedFromPipeId = rank.pipes[static_cast<size_t>(j)].pipeId;
      ++borrowed;
    }
  }
  return borrowed;
}

SparseSamples sparseSampleIds(const OrganModel& model, const SparsePlan& plan) {
  SparseSamples out;
  for (const auto& pr : plan.ranks) {
    const auto it = model.ranks.find(pr.rankId);
    if (it == model.ranks.end()) continue;
    const Rank& rank = it->second;
    const size_t n = std::min(rank.pipes.size(), pr.keepsOwn.size());
    for (size_t i = 0; i < n; ++i) {
      if (!pr.keepsOwn[i]) continue;
      for (const auto& layer : rank.pipes[i].layers) {
        for (const auto& a : layer.attacks)
          if (a.sample.sampleId != 0) out.attacks.insert(a.sample.sampleId);
        for (const auto& r : layer.releases)
          if (r.sample.sampleId != 0) out.releases.insert(r.sample.sampleId);
      }
    }
  }
  return out;
}

}  // namespace mp
