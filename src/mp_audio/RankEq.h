// EQ per rank and per division: a bass shelf and a brightness shelf, set on
// the Voicing tab and saved with the voicing; and where the rank stands, its
// pan and its stereo width, applied to the same groups after the shelves.
// A rank's and its division's add: the shelves and the pans add (the pan
// held to -1..+1), the widths multiply.
//
// A filter per voice would cost a full organ thousands of filters. Instead,
// the ranks that have an EQ play into buses of their own, one per EQ group
// and per enclosure bus they would otherwise have played into, and each such
// bus is filtered once per block and added into that enclosure bus before
// its shades act. So a rank with an EQ is still behind its swell box, and
// still goes to its output pair: the pairs are rendered one at a time, and
// each pair keeps its own filter memory.
//
// An EQ group is the thing whose EQ it is: a rank with an EQ of its own (its
// division's added to it), or a division whose ranks have none of their own.
// So moving a slider changes the filters of a group that exists, and the
// pipes already sounding hear it at once; a rank given its first EQ is heard
// with it from its next note.
//
// Costs nothing while no rank and no division has an EQ: no group exists,
// no bus is added, and the voices play into the enclosure buses exactly as
// before. A group whose EQ is taken away stays until the last of its pipes
// has stopped, and then goes.
//
// JUCE-free: the tests drive it without audio.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mp {

struct ShelfEq {
  float bassDb = 0.0f;    // a low shelf at kBassHz
  float brightDb = 0.0f;  // a high shelf at kBrightHz
  float pan = 0.0f;       // -1 left, +1 right
  float width = 1.0f;     // 0 mono, 1 as recorded, 2 twice as wide
  bool flat() const { return bassDb == 0.0f && brightDb == 0.0f && pan == 0.0f && width == 1.0f; }
  bool operator==(const ShelfEq& o) const {
    return bassDb == o.bassDb && brightDb == o.brightDb && pan == o.pan && width == o.width;
  }
  bool operator!=(const ShelfEq& o) const { return !(*this == o); }
};

class RankEq {
public:
  static constexpr int kMaxGroups = 16;    // buses at once
  static constexpr int kMaxPairs = 8;      // output pairs, each its own filter memory
  static constexpr int kMaxChannels = 8;   // channels of a bus filtered
  static constexpr double kBassHz = 200.0;
  static constexpr double kBrightHz = 3000.0;
  static constexpr float kMaxDb = 12.0f;
  static constexpr float kMaxWidth = 2.0f;

  // The organ's ranks, each with its division's index or -1, how many
  // divisions, how many enclosure buses there are, and the rate. Clears
  // every EQ and every group. Only while no block is rendered.
  void configure(std::vector<int> rankDivision, int divisions, int baseBuses, double sampleRate) {
    rankDivision_ = std::move(rankDivision);
    for (int& d : rankDivision_)
      if (d >= divisions) d = -1;
    ranks_.assign(rankDivision_.size(), {});
    divisions_.assign(static_cast<size_t>(std::max(0, divisions)), {});
    ownerOfRank_.assign(rankDivision_.size(), -1);
    baseBuses_ = std::max(1, baseBuses);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (auto& g : groups_) g = Group{};
    highWater_ = 0;
    wanted_ = false;
  }

  int rankCount() const { return static_cast<int>(ranks_.size()); }
  int divisionCount() const { return static_cast<int>(divisions_.size()); }
  int baseBuses() const { return baseBuses_; }

  // Every rank's and every division's EQ, in the orders configure() was
  // given; shorter lists leave the rest flat. Only while no block is
  // rendered: under the audio lock.
  void setParams(const std::vector<ShelfEq>& ranks, const std::vector<ShelfEq>& divisions) {
    for (size_t i = 0; i < ranks_.size(); ++i) ranks_[i] = i < ranks.size() ? clamp(ranks[i]) : ShelfEq{};
    for (size_t i = 0; i < divisions_.size(); ++i)
      divisions_[i] = i < divisions.size() ? clamp(divisions[i]) : ShelfEq{};
    wanted_ = false;
    for (size_t r = 0; r < ranks_.size(); ++r) {
      ownerOfRank_[r] = ownerFor(static_cast<int>(r));
      if (ownerOfRank_[r] >= 0) wanted_ = true;
    }
    // Groups that exist follow their owner's EQ now, so a held note hears it.
    for (auto& g : groups_)
      if (g.used) design(g, paramsOf(g.owner));
  }

  ShelfEq rank(int i) const { return i >= 0 && i < rankCount() ? ranks_[static_cast<size_t>(i)] : ShelfEq{}; }
  ShelfEq division(int i) const {
    return i >= 0 && i < divisionCount() ? divisions_[static_cast<size_t>(i)] : ShelfEq{};
  }

  // Whether any rank has an EQ: only then does a starting note need busFor().
  bool wanted() const { return wanted_; }
  // How many group buses are in use, from the first: the buses past the
  // enclosure buses that a block renders. 0 while nothing has an EQ.
  int groupBuses() const { return highWater_; }
  int busCount() const { return baseBuses_ + highWater_; }
  // The enclosure bus a group bus adds into.
  int parentOf(int group) const { return groups_[static_cast<size_t>(group)].parent; }

  // Audio thread, as a note starts: the bus a pipe of this rank plays into,
  // given the enclosure bus it would otherwise play into.
  int busFor(int rank, int parentBus) {
    if (rank < 0 || rank >= rankCount()) return parentBus;
    const int owner = ownerOfRank_[static_cast<size_t>(rank)];
    if (owner < 0) return parentBus;
    int free = -1;
    for (int i = 0; i < kMaxGroups; ++i) {
      const Group& g = groups_[static_cast<size_t>(i)];
      if (g.used && g.owner == owner && g.parent == parentBus) return baseBuses_ + i;
      if (!g.used && free < 0) free = i;
    }
    if (free < 0) return parentBus;  // every bus taken: this pipe plays without its EQ
    Group& g = groups_[static_cast<size_t>(free)];
    g = Group{};
    g.used = true;
    g.owner = owner;
    g.parent = parentBus;
    design(g, paramsOf(owner));
    highWater_ = std::max(highWater_, free + 1);
    return baseBuses_ + free;
  }

  // Audio thread, once a block before rendering: a group no longer wanted
  // goes once `voicesOn(bus)` says none of its pipes sounds. Nothing to do
  // while no group exists.
  template <class VoicesOn>
  void beginBlock(VoicesOn voicesOn) {
    if (highWater_ == 0) return;
    for (int i = 0; i < highWater_; ++i) {
      Group& g = groups_[static_cast<size_t>(i)];
      if (g.used && !ownerWanted(g.owner) && voicesOn(baseBuses_ + i) == 0) g.used = false;
    }
    while (highWater_ > 0 && !groups_[static_cast<size_t>(highWater_ - 1)].used) --highWater_;
  }

  // Audio thread: a group's bus filtered in place, with the filter memory of
  // this output pair.
  void process(int group, int pair, float* const* planes, int numChannels, int numFrames) {
    Group& g = groups_[static_cast<size_t>(group)];
    if (!g.used || g.bypass) return;
    pair = std::clamp(pair, 0, kMaxPairs - 1);
    const int channels = std::min(numChannels, kMaxChannels);
    for (int c = 0; c < channels; ++c) {
      float* x = planes[c];
      for (int s = 0; s < 2; ++s) {
        const Biquad& f = g.filter[static_cast<size_t>(s)];
        if (f.unity) continue;
        float* z = g.state[static_cast<size_t>(pair)][static_cast<size_t>(c)][static_cast<size_t>(s)].data();
        float z1 = z[0], z2 = z[1];
        for (int i = 0; i < numFrames; ++i) {
          const float in = x[i];
          const float out = f.b0 * in + z1;
          z1 = f.b1 * in - f.a1 * out + z2;
          z2 = f.b2 * in - f.a2 * out;
          x[i] = out;
        }
        z[0] = z1;
        z[1] = z2;
      }
    }
    // Where it stands: the width by mid and side, then the pan, on the first
    // two channels. At a width of 100% and the pan in the middle nothing is
    // done, so the samples are the shelves' own.
    if (channels < 2 || (g.width == 1.0f && g.pan == 0.0f)) return;
    float* left = planes[0];
    float* right = planes[1];
    if (g.width != 1.0f) {
      const float w = g.width;
      for (int i = 0; i < numFrames; ++i) {
        const float mid = (left[i] + right[i]) * 0.5f;
        const float side = (left[i] - right[i]) * 0.5f * w;
        left[i] = mid + side;
        right[i] = mid - side;
      }
    }
    if (g.pan != 0.0f)
      for (int i = 0; i < numFrames; ++i) {
        left[i] *= g.gainLeft;
        right[i] *= g.gainRight;
      }
  }

private:
  struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    bool unity = true;
  };
  struct Group {
    bool used = false;
    int owner = -1;   // a rank's index, or rankCount() + a division's
    int parent = 0;   // the enclosure bus it adds into
    bool bypass = true;
    std::array<Biquad, 2> filter{};
    float pan = 0.0f, width = 1.0f;
    float gainLeft = 1.0f, gainRight = 1.0f;
    // Transposed direct form II memory: [pair][channel][filter][2].
    std::array<std::array<std::array<std::array<float, 2>, 2>, kMaxChannels>, kMaxPairs> state{};
  };

  static ShelfEq clamp(ShelfEq e) {
    e.bassDb = std::clamp(e.bassDb, -kMaxDb, kMaxDb);
    e.brightDb = std::clamp(e.brightDb, -kMaxDb, kMaxDb);
    e.pan = std::clamp(e.pan, -1.0f, 1.0f);
    e.width = std::clamp(e.width, 0.0f, kMaxWidth);
    return e;
  }
  int ownerFor(int r) const {
    if (!ranks_[static_cast<size_t>(r)].flat()) return r;
    const int d = rankDivision_[static_cast<size_t>(r)];
    if (d >= 0 && !divisions_[static_cast<size_t>(d)].flat()) return rankCount() + d;
    return -1;
  }
  bool ownerWanted(int owner) const {
    if (owner < 0) return false;
    if (owner < rankCount()) return !ranks_[static_cast<size_t>(owner)].flat();
    const int d = owner - rankCount();
    return d < divisionCount() && !divisions_[static_cast<size_t>(d)].flat();
  }
  // What a group sounds like: a rank's own EQ plus its division's, or the
  // division's alone.
  ShelfEq paramsOf(int owner) const {
    ShelfEq e;
    if (owner < 0) return e;
    if (owner < rankCount()) {
      e = ranks_[static_cast<size_t>(owner)];
      const int d = rankDivision_[static_cast<size_t>(owner)];
      if (d >= 0) {
        const ShelfEq& div = divisions_[static_cast<size_t>(d)];
        e.bassDb += div.bassDb;
        e.brightDb += div.brightDb;
        e.pan = std::clamp(e.pan + div.pan, -1.0f, 1.0f);
        e.width = std::clamp(e.width * div.width, 0.0f, kMaxWidth);
      }
      return e;
    }
    const int d = owner - rankCount();
    return d < divisionCount() ? divisions_[static_cast<size_t>(d)] : e;
  }

  // The two shelves, as the Audio EQ Cookbook gives them, with a slope of 1.
  void design(Group& g, const ShelfEq& e) const {
    g.filter[0] = shelf(false, kBassHz, e.bassDb);
    g.filter[1] = shelf(true, kBrightHz, e.brightDb);
    g.pan = e.pan;
    g.width = e.width;
    // A constant-power law, cos and sin of (pan + 1) pi/4, scaled so the
    // middle is unity and neither side is lifted: hard left is the left
    // channel as it was and the right one silent.
    const double theta = (static_cast<double>(e.pan) + 1.0) * 3.14159265358979323846 / 4.0;
    g.gainLeft = static_cast<float>(std::min(1.0, std::sqrt(2.0) * std::cos(theta)));
    g.gainRight = static_cast<float>(std::min(1.0, std::sqrt(2.0) * std::sin(theta)));
    if (e.pan <= -1.0f) g.gainRight = 0.0f;
    if (e.pan >= 1.0f) g.gainLeft = 0.0f;
    g.bypass = g.filter[0].unity && g.filter[1].unity && g.pan == 0.0f && g.width == 1.0f;
  }
  Biquad shelf(bool high, double hz, float db) const {
    Biquad f;
    if (db == 0.0f) return f;
    const double A = std::pow(10.0, db / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * std::min(hz, sampleRate_ * 0.45) / sampleRate_;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt(2.0);  // S = 1
    const double k = 2.0 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (high) {
      b0 = A * ((A + 1) + (A - 1) * cw + k);
      b1 = -2 * A * ((A - 1) + (A + 1) * cw);
      b2 = A * ((A + 1) + (A - 1) * cw - k);
      a0 = (A + 1) - (A - 1) * cw + k;
      a1 = 2 * ((A - 1) - (A + 1) * cw);
      a2 = (A + 1) - (A - 1) * cw - k;
    } else {
      b0 = A * ((A + 1) - (A - 1) * cw + k);
      b1 = 2 * A * ((A - 1) - (A + 1) * cw);
      b2 = A * ((A + 1) - (A - 1) * cw - k);
      a0 = (A + 1) + (A - 1) * cw + k;
      a1 = -2 * ((A - 1) + (A + 1) * cw);
      a2 = (A + 1) + (A - 1) * cw - k;
    }
    f.b0 = static_cast<float>(b0 / a0);
    f.b1 = static_cast<float>(b1 / a0);
    f.b2 = static_cast<float>(b2 / a0);
    f.a1 = static_cast<float>(a1 / a0);
    f.a2 = static_cast<float>(a2 / a0);
    f.unity = false;
    return f;
  }

  std::vector<int> rankDivision_;
  std::vector<ShelfEq> ranks_, divisions_;
  std::vector<int> ownerOfRank_;
  int baseBuses_ = 1;
  double sampleRate_ = 48000.0;
  std::array<Group, kMaxGroups> groups_{};
  int highWater_ = 0;
  bool wanted_ = false;
};

}  // namespace mp
