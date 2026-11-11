// Live levels: the faders a player moves while playing. One per rank, one per
// division, one per perspective or windchest group and one per output pair,
// set from MIDI knobs and pedals, the Mixer tab or the control API, and heard
// at once on the pipes already sounding.
//
// They are not the voicing. The voicing is the organ as the player wants it
// to stand, read when a note starts and saved with the organ; these are a
// performance's balance, followed by every voice block by block, and every
// fader starts at full with each organ.
//
// The law is the MIDI master volume's: 127 leaves the level as voiced, 0 is
// silence, and the steps between are even in decibels over the lower 40.
// Above 127, up to 203, the Mixer tab and the control API raise a level by up
// to 24 dB in steps of about the same size, as far as the master volume goes
// (#971). MIDI sends 127 at most, so a knob or a pedal at its top leaves the
// level as voiced.
//
// Costs nothing while every fader is at full: advance() says so, the engine
// is then handed no table, and no voice multiplies by anything. With one
// fader moved, each voice takes one multiply a block.
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
  // The level as voiced, and the most MIDI can send.
  static constexpr int kFull = 127;
  // The loudest a level goes: 24 dB over the voicing, 76 steps above full.
  static constexpr int kTop = 203;
  static constexpr float kBoostDb = 24.0f;
  // Output pairs: a fixed table, so the mixer can gain or lose pairs while
  // the audio runs without the table moving under it.
  static constexpr int kMaxPairs = 64;

  static float gainOf(int value) {
    if (value <= 0) return 0.0f;
    if (value == kFull) return 1.0f;
    return std::pow(10.0f, dbOf(value) / 20.0f);
  }
  // The level in dB; -inf is not representable, so 0 reads as -100. Full is
  // exactly 0 dB: the law below gives -0 there, which prints as "-0.0".
  static float dbOf(int value) {
    if (value <= 0) return -100.0f;
    if (value == kFull) return 0.0f;
    if (value > kFull)
      return kBoostDb * static_cast<float>(std::min(value, kTop) - kFull) / static_cast<float>(kTop - kFull);
    return -40.0f * (1.0f - static_cast<float>(value) / static_cast<float>(kFull));
  }
  // The step nearest a level in dB, for a level typed in: -40 dB and below is
  // the lowest step above silence, +24 dB and above the top.
  static int valueOf(float db) {
    if (db >= 0.0f)
      return kFull + static_cast<int>(std::lround(std::min(db, kBoostDb) / kBoostDb * (kTop - kFull)));
    return std::max(1, static_cast<int>(std::lround(kFull * (1.0f + std::max(db, -40.0f) / 40.0f))));
  }

  LiveLevels() { configure({}, 0); }

  // Sizes the rank, division and group tables and puts every fader at full.
  // Only while no block is being rendered: at an organ load. `rankDivision`
  // gives each rank's division index, or -1 for a rank in none; `rankGroup`
  // its perspective's or windchest group's index the same way, and may be
  // empty for an organ with no groups.
  void configure(std::vector<int> rankDivision, int divisions, std::vector<int> rankGroup = {}, int groups = 0) {
    rankDivision_ = std::move(rankDivision);
    for (int& d : rankDivision_)
      if (d >= divisions) d = -1;
    const size_t ranks = rankDivision_.size();
    rankGroup_ = std::move(rankGroup);
    rankGroup_.resize(ranks, -1);
    for (int& g : rankGroup_)
      if (g >= groups) g = -1;
    const auto n = static_cast<size_t>(std::max(0, divisions));
    const auto ng = static_cast<size_t>(std::max(0, groups));
    rankSet_ = makeFaders(ranks);
    divisionSet_ = makeFaders(n);
    groupSet_ = makeFaders(ng);
    pairSet_ = makeFaders(kMaxPairs);
    rankSw_ = makeSwitches(ranks);
    divisionSw_ = makeSwitches(n);
    groupSw_ = makeSwitches(ng);
    pairSw_ = makeSwitches(kMaxPairs);
    rankOpen_.assign(ranks, 1.0f);
    rankNow_.assign(ranks, 1.0f);
    divisionNow_.assign(n, 1.0f);
    groupNow_.assign(ng, 1.0f);
    pairNow_.assign(kMaxPairs, 1.0f);
    rankGain_.assign(ranks, 1.0f);
    active_ = false;
    moving_ = false;
    dirty_.store(false, std::memory_order_relaxed);
  }

  // Divisions for ranks found in none at configure(): a set that plays its
  // pipes through pallets names its stops' ranks only once the load knows
  // which pipes it ships (#1054). A rank with a division keeps it, and the
  // faders keep their levels. Called at load, while the audio is held.
  void addRankDivisions(const std::vector<int>& rankDivision) {
    for (size_t i = 0; i < rankDivision_.size() && i < rankDivision.size(); ++i)
      if (rankDivision_[i] < 0 && rankDivision[i] >= 0 && rankDivision[i] < divisionCount())
        rankDivision_[i] = rankDivision[i];
    dirty_.store(true, std::memory_order_release);
  }

  int rankCount() const { return static_cast<int>(rankDivision_.size()); }
  // A rank's division index, or -1 for a rank in none or out of range.
  int rankDivision(int i) const { return i >= 0 && i < rankCount() ? rankDivision_[static_cast<size_t>(i)] : -1; }
  int divisionCount() const { return static_cast<int>(divisionNow_.size()); }
  // A rank's group index, or -1 for a rank in none or out of range.
  int rankGroup(int i) const { return i >= 0 && i < rankCount() ? rankGroup_[static_cast<size_t>(i)] : -1; }
  int groupCount() const { return static_cast<int>(groupNow_.size()); }

  // Any thread. An index out of range is ignored; a value is held to 0..kTop.
  void setRank(int i, int value) { set(rankSet_.get(), rankCount(), i, value); }
  void setDivision(int i, int value) { set(divisionSet_.get(), divisionCount(), i, value); }
  void setGroup(int i, int value) { set(groupSet_.get(), groupCount(), i, value); }
  void setPair(int i, int value) { set(pairSet_.get(), kMaxPairs, i, value); }
  int rank(int i) const { return get(rankSet_.get(), rankCount(), i); }
  int division(int i) const { return get(divisionSet_.get(), divisionCount(), i); }
  int group(int i) const { return get(groupSet_.get(), groupCount(), i); }
  int pair(int i) const { return get(pairSet_.get(), kMaxPairs, i); }

  // Mute and solo, a mixing desk's two buttons on every strip. They leave the
  // fader where it is. A muted strip is silent. A solo on a rank, a division
  // or a group leaves only the ranks under a soloed strip sounding; a solo on
  // a pair leaves only the soloed pairs. Mute wins over solo. Noises are on
  // no strip and stay as they are. Any thread.
  enum class Strip { Rank, Division, Group, Pair };
  void setMuted(Strip s, int i, bool on) { setSwitch(s, i, kMute, on); }
  void setSoloed(Strip s, int i, bool on) { setSwitch(s, i, kSolo, on); }
  bool muted(Strip s, int i) const { return (switches(s, i) & kMute) != 0; }
  bool soloed(Strip s, int i) const { return (switches(s, i) & kSolo) != 0; }
  int count(Strip s) const {
    switch (s) {
      case Strip::Rank: return rankCount();
      case Strip::Division: return divisionCount();
      case Strip::Group: return groupCount();
      case Strip::Pair: return kMaxPairs;
    }
    return 0;
  }

  // Audio thread, once a block before the voices render: each gain moves
  // toward its fader, smoothly enough that a knob turned fast does not click.
  // Returns whether any level is away from full, below or above; while none
  // is and none has been touched since, this returns at once.
  bool advance(int numFrames, double sampleRate) {
    const bool touched = dirty_.exchange(false, std::memory_order_acq_rel);
    if (!touched && !moving_) return active_;
    const float step = sampleRate > 0.0
                           ? 1.0f - static_cast<float>(std::exp(-numFrames / (kSmoothSeconds * sampleRate)))
                           : 1.0f;
    bool moving = false, away = false;
    const auto glide = [&](float target, float& now) {
      float g = now + (target - now) * step;
      if (std::fabs(target - g) < 1.0e-4f) g = target;
      else moving = true;
      now = g;
      if (g != 1.0f) away = true;
    };
    const auto follow = [&](const std::atomic<int>* set, std::vector<float>& now) {
      for (size_t i = 0; i < now.size(); ++i) glide(gainOf(set[i].load(std::memory_order_relaxed)), now[i]);
    };
    follow(rankSet_.get(), rankNow_);
    follow(divisionSet_.get(), divisionNow_);
    follow(groupSet_.get(), groupNow_);
    // A pair's own buttons act on its own gain.
    const bool pairSolo = anySolo(pairSw_.get(), kMaxPairs);
    for (size_t i = 0; i < pairNow_.size(); ++i) {
      const int sw = pairSw_[i].load(std::memory_order_relaxed);
      const bool open = (sw & kMute) == 0 && (!pairSolo || (sw & kSolo) != 0);
      glide(open ? gainOf(pairSet_[i].load(std::memory_order_relaxed)) : 0.0f, pairNow_[i]);
    }
    // A rank's are its own, its division's and its group's, together.
    const bool rankSolo = anySolo(rankSw_.get(), rankCount()) || anySolo(divisionSw_.get(), divisionCount()) ||
                          anySolo(groupSw_.get(), groupCount());
    for (size_t i = 0; i < rankGain_.size(); ++i) {
      const int d = rankDivision_[i];
      const int g = rankGroup_[i];
      const int sw = rankSw_[i].load(std::memory_order_relaxed) |
                     (d >= 0 ? divisionSw_[static_cast<size_t>(d)].load(std::memory_order_relaxed) : 0) |
                     (g >= 0 ? groupSw_[static_cast<size_t>(g)].load(std::memory_order_relaxed) : 0);
      glide((sw & kMute) == 0 && (!rankSolo || (sw & kSolo) != 0) ? 1.0f : 0.0f, rankOpen_[i]);
      rankGain_[i] = rankNow_[i] * rankOpen_[i] * (d >= 0 ? divisionNow_[static_cast<size_t>(d)] : 1.0f) *
                     (g >= 0 ? groupNow_[static_cast<size_t>(g)] : 1.0f);
    }
    moving_ = moving;
    active_ = away;
    return active_;
  }

  // Read by the voices during the block: a rank's own fader times its
  // division's and its group's, and a pair's. -1 or out of range is full.
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
    faders[i].store(std::clamp(value, 0, kTop), std::memory_order_relaxed);
    dirty_.store(true, std::memory_order_release);
  }
  static int get(const std::atomic<int>* faders, int count, int i) {
    return i >= 0 && i < count ? faders[i].load(std::memory_order_relaxed) : kFull;
  }

  static constexpr int kMute = 1, kSolo = 2;
  static std::unique_ptr<std::atomic<int>[]> makeSwitches(size_t n) {
    std::unique_ptr<std::atomic<int>[]> f(new std::atomic<int>[std::max<size_t>(n, 1)]);
    for (size_t i = 0; i < std::max<size_t>(n, 1); ++i) f[i].store(0, std::memory_order_relaxed);
    return f;
  }
  std::atomic<int>* switchTable(Strip s) const {
    switch (s) {
      case Strip::Rank: return rankSw_.get();
      case Strip::Division: return divisionSw_.get();
      case Strip::Group: return groupSw_.get();
      case Strip::Pair: return pairSw_.get();
    }
    return nullptr;
  }
  int switches(Strip s, int i) const {
    return i >= 0 && i < count(s) ? switchTable(s)[i].load(std::memory_order_relaxed) : 0;
  }
  void setSwitch(Strip s, int i, int bit, bool on) {
    if (i < 0 || i >= count(s)) return;
    if (on) switchTable(s)[i].fetch_or(bit, std::memory_order_relaxed);
    else switchTable(s)[i].fetch_and(~bit, std::memory_order_relaxed);
    dirty_.store(true, std::memory_order_release);
  }
  static bool anySolo(const std::atomic<int>* sw, int n) {
    for (int i = 0; i < n; ++i)
      if ((sw[i].load(std::memory_order_relaxed) & kSolo) != 0) return true;
    return false;
  }

  std::vector<int> rankDivision_, rankGroup_;
  std::unique_ptr<std::atomic<int>[]> rankSet_, divisionSet_, groupSet_, pairSet_;
  std::unique_ptr<std::atomic<int>[]> rankSw_, divisionSw_, groupSw_, pairSw_;
  // Audio thread only. rankOpen_ is each rank's mute and solo, 0 to 1.
  std::vector<float> rankNow_, divisionNow_, groupNow_, pairNow_, rankGain_, rankOpen_;
  bool active_ = false;
  bool moving_ = false;
  std::atomic<bool> dirty_{false};
};

}  // namespace mp
