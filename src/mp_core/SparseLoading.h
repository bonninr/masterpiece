// Sparse loading: how many notes of a rank are loaded, and what the ones
// that are not play instead.
//
// The whole feature is a change to the MODEL before the samples are read.
// Sample loading is driven by the pipes' layers (SampleLibrary::loadAll
// builds its list from rank.pipes[].layers[].attacks/releases), so a pipe
// that is not kept takes a copy of a kept pipe's layers, and the samples it
// would have played are then referenced by nobody and are not loaded at all.
//
// The pipe keeps its own id, its own note, its own footage and its own
// tuning: wind chests, tremulants, enclosures and the live levels are all
// looked up by pipe id, and those ids do not move.
//
// A borrowed pipe plays a neighbour's samples retuned to its own pitch. The
// retuning is the dangerous part, and it is in the audio path, not here:
// see MasterpieceProcessor::playbackRatioFor.
//
// JUCE-free, so the fast test loop can run it.
#pragma once
#include "OrganModel.h"

#include <functional>
#include <unordered_set>
#include <vector>

namespace mp {

// What a sparse load does, worked out before the model is changed.
struct SparsePlan {
  struct Rank {
    Id rankId = 0;
    // By the rank's own pipe order: the pipe plays its own samples.
    std::vector<bool> keepsOwn;
    // By the rank's own pipe order: the pipe it borrows from, -1 for none.
    std::vector<int> neighbour;
  };
  std::vector<Rank> ranks;
  int ranksConsidered = 0;
  int ranksLeftAlone = 0;
  int pipes = 0;
  int kept = 0;
  int borrowed = 0;
};

// The samples a sparse load reads: the kept pipes' own, and the neighbours'
// the borrowed pipes play.
struct SparseSamples {
  std::unordered_set<Id> attacks;
  std::unordered_set<Id> releases;
};

// Works out the plan for a model. The model is not changed.
//
// `every` is 1 for every note, 2 for every second note, 3 for every third.
// Anything below 2 leaves the model alone.
//
// `protectedRanks` are the ranks left untouched whatever `every` says: the
// sampled tremulant ranks, which the caller names.
//
// `samplePresent` says whether a sample can be read: on disk, in the organ's
// archive, or already held. A pipe whose neighbour's samples cannot be read
// keeps its own, so a demo's missing pipes stay missing.
SparsePlan planSparseLoading(const OrganModel& model, int every,
                             const std::unordered_set<Id>& protectedRanks,
                             const std::function<bool(Id)>& samplePresent);

// Puts the plan into the model: a borrowed pipe takes a copy of its
// neighbour's layers -- attacks, releases, loop points, gains and
// crossfades, which are properties of the samples and travel with them --
// and records the neighbour in Pipe::borrowedFromPipeId. Returns how many
// pipes were borrowed.
int applySparsePlan(OrganModel& model, const SparsePlan& plan);

// The samples the plan reads: the kept pipes' own, and the neighbours' the
// borrowed pipes play. Asked without changing the model, for the memory
// estimate.
SparseSamples sparseSampleIds(const OrganModel& model, const SparsePlan& plan);

}  // namespace mp
