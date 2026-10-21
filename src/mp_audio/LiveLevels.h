// Live levels: the faders a player moves while playing. One per rank, one per
// division and one per output pair, set from MIDI knobs and pedals, the
// Mixer tab or the control API, and heard at once on the pipes already
// sounding.
//
// They are not the voicing. The voicing is the organ as the player wants it
// to stand, read when a note starts and saved with the organ; these are a
// performance's balance, followed by every voice block by block, and every
// fader starts at full with each organ.
//
// The law is the MIDI master volume's: 127 leaves the level as voiced, 0 is
// silence, and the steps between are even in decibels over the lower 40.
//
// Costs nothing while every fader is at full: advance() says so, the engine
// is then handed no table, and no voice multiplies by anything. With one
// fader down, each voice takes one multiply a block.
//
// JUCE-free, so the fast tests can drive it without audio.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace mp {

class LiveLevels {
public:
  static constexpr int kFull = 127;
  // Output pairs: a fixed table, so the mixer can gain or lose pairs while
  // the audio runs without the table moving under it.
  static constexpr int kMaxPairs = 64;

  static float gainOf(int value) {
    if (value <= 0) return 0.0f;
    if (value >= kFull) return 1.0f;
    return std::pow(10.0f, dbOf(value) / 20.0f);
  }
  // The level in dB; -inf is not representable, so 0 reads as -100. Full is
  // exactly 0 dB: the law below gives -0 there, which prints as "-0.0".
  static float dbOf(int value) {
    if (value <= 0) return -100.0f;
    if (value >= kFull) return 0.0f;
    return -40.0f * (1.0f - static_cast<float>(std::min(value, kFull)) / static_cast<float>(kFull));
  }

  LiveLevels() { configure({}, 0); }

  // Sizes the rank and division tables and puts every fader at full. Only
  // while no block is being rendered: at an organ load. `rankDivision` gives
  // each rank's division index, or -1 for a rank in none.
  void configure(std::vector<int> rankDivision, int divisions) {
    rankDivision_ = std::move(rankDivision);
    for (int& d : rankDivision_)
      if (d >= divisions) d = -1;
    const size_t ranks = rankDivision_.size();
    const auto n = static_cast<size_t>(std::max(0, divisions));
    rankSet_ = makeFaders(ranks);
    divisionSet_ = makeFaders(n);
    pairSet_ = makeFaders(kMaxPairs);
    rankNow_.assign(ranks, 1.0f);
    divisionNow_.assign(n, 1.0f);
    pairNow_.assign(kMaxPairs, 1.0f);
    rankGain_.assign(ranks, 1.0f);
    active_ = false;
    moving_ = false;
    dirty_.store(false, std::memory_order_relaxed);
  }

  int rankCount() const { return static_cast<int>(rankDivision_.size()); }
  int divisionCount() const { return static_cast<int>(divisionNow_.size()); }

  // Any thread. An index out of range is ignored; a value is held to 0..127.
  void setRank(int i, int value) { set(rankSet_.get(), rankCount(), i, value); }
  void setDivision(int i, int value) { set(divisionSet_.get(), divisionCount(), i, value); }
  void setPair(int i, int value) { set(pairSet_.get(), kMaxPairs, i, value); }
  int rank(int i) const { return get(rankSet_.get(), rankCount(), i); }
  int division(int i) const { return get(divisionSet_.get(), divisionCount(), i); }
  int pair(int i) const { return get(pairSet_.get(), kMaxPairs, i); }

  // Audio thread, once a block before the voices render: each gain moves
  // toward its fader, smoothly enough that a knob turned fast does not click.
  // Returns whether any level is below full; while none is and none has been
  // touched since, this returns at once.
  bool advance(int numFrames, double sampleRate) {
    const bool touched = dirty_.exchange(false, std::memory_order_acq_rel);
    if (!touched && !moving_) return active_;
    const float step = sampleRate > 0.0
                           ? 1.0f - static_cast<float>(std::exp(-numFrames / (kSmoothSeconds * sampleRate)))
                           : 1.0f;
    bool moving = false, below = false;
    const auto follow = [&](const std::atomic<int>* set, std::vector<float>& now) {
      for (size_t i = 0; i < now.size(); ++i) {
        const float target = gainOf(set[i].load(std::memory_order_relaxed));
        float g = now[i] + (target - now[i]) * step;
        if (std::fabs(target - g) < 1.0e-4f) g = target;
        else moving = true;
        now[i] = g;
        if (g != 1.0f) below = true;
      }
    };
    follow(rankSet_.get(), rankNow_);
    follow(divisionSet_.get(), divisionNow_);
    follow(pairSet_.get(), pairNow_);
    for (size_t i = 0; i < rankGain_.size(); ++i) {
      const int d = rankDivision_[i];
      rankGain_[i] = rankNow_[i] * (d >= 0 ? divisionNow_[static_cast<size_t>(d)] : 1.0f);
    }
    moving_ = moving;
    active_ = below;
    return active_;
  }

  // Read by the voices during the block: a rank's own fader times its
  // division's, and a pair's. -1 or out of range is full.
  float rankGain(int i) const {
    return i >= 0 && static_cast<size_t>(i) < rankGain_.size() ? rankGain_[static_cast<size_t>(i)] : 1.0f;
  }
  float pairGain(int i) const {
    return i >= 0 && i < kMaxPairs ? pairNow_[static_cast<size_t>(i)] : 1.0f;
  }
  // The same tables whole, for the voice engine.
  const float* rankGains() const { return rankGain_.data(); }
  const float* pairGains() const { return pairNow_.data(); }

private:
  // How fast a gain follows its fader: a knob swept end to end settles in a
  // tenth of a second, and each block's step stays small.
  static constexpr double kSmoothSeconds = 0.03;

  static std::unique_ptr<std::atomic<int>[]> makeFaders(size_t n) {
    std::unique_ptr<std::atomic<int>[]> f(new std::atomic<int>[std::max<size_t>(n, 1)]);
    for (size_t i = 0; i < std::max<size_t>(n, 1); ++i) f[i].store(kFull, std::memory_order_relaxed);
    return f;
  }
  void set(std::atomic<int>* faders, int count, int i, int value) {
    if (i < 0 || i >= count) return;
    faders[i].store(std::clamp(value, 0, kFull), std::memory_order_relaxed);
    dirty_.store(true, std::memory_order_release);
  }
  static int get(const std::atomic<int>* faders, int count, int i) {
    return i >= 0 && i < count ? faders[i].load(std::memory_order_relaxed) : kFull;
  }

  std::vector<int> rankDivision_;
  std::unique_ptr<std::atomic<int>[]> rankSet_, divisionSet_, pairSet_;
  // Audio thread only.
  std::vector<float> rankNow_, divisionNow_, pairNow_, rankGain_;
  bool active_ = false;
  bool moving_ = false;
  std::atomic<bool> dirty_{false};
};

}  // namespace mp
