#include "DiskProbe.h"
#include "VoiceEngine.h"
#include "../mp_core/Utf8Path.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

namespace mp {

namespace {
// The share of the disk's time streaming may count on: the rest is for the
// moments many voices come due together, and for everything else using it.
constexpr double kDiskShare = 0.5;

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

// Seconds of the disk's time per second of one voice's audio, for the
// frames themselves.
double transferShare(const TailReadCost& cost, double sampleRate) {
  return cost.framesPerMs > 0.0 ? sampleRate / (cost.framesPerMs * 1000.0) : 0.0;
}
}  // namespace

TailReadCost measureTailReads(const std::vector<TailToTime>& tails, int maxFiles) {
  TailReadCost cost;
  // A short read, for the fixed part, and half a second, for the rest.
  constexpr int kShort = 1024;
  constexpr int kLong = 24000;
  std::vector<const TailToTime*> usable;
  for (const auto& t : tails)
    if (t.tail != nullptr && t.tail->read && t.tail->totalFrames - t.resident >= 4 * kLong)
      usable.push_back(&t);
  if (usable.empty() || maxFiles <= 0) return cost;
  // Spread over the organ: neighbours in the list are often neighbours on
  // the disk, and a read of the next file along costs no seek.
  const size_t n = std::min(usable.size(), static_cast<size_t>(maxFiles));
  std::vector<const TailToTime*> pick;
  pick.reserve(n);
  for (size_t i = 0; i < n; ++i) pick.push_back(usable[i * usable.size() / n]);
  int widest = 1;
  for (const auto* t : pick) widest = std::max(widest, t->channels);
  std::vector<float> dest(static_cast<size_t>(kLong) * static_cast<size_t>(widest));

  // Each read at its own place in the tail, a third of it from the others,
  // so no read finds the frames an earlier one brought in.
  const auto timed = [&dest](const TailToTime& t, double where, int frames) {
    const int64_t room = t.tail->totalFrames - t.resident - frames;
    const int64_t at = t.resident + static_cast<int64_t>(where * static_cast<double>(room));
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t got = t.tail->read(at, frames, dest.data(), std::max(1, t.channels));
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return got == frames ? ms : -1.0;
  };
  std::vector<double> opens, shorts, longs;
  for (const auto* t : pick)
    if (const double ms = timed(*t, 0.1, kShort); ms >= 0.0) opens.push_back(ms);
  for (const auto* t : pick)
    if (const double ms = timed(*t, 0.45, kShort); ms >= 0.0) shorts.push_back(ms);
  for (const auto* t : pick)
    if (const double ms = timed(*t, 0.8, kLong); ms >= 0.0) longs.push_back(ms);
  if (shorts.empty() || longs.empty()) return cost;

  const double s = median(shorts);
  const double l = median(longs);
  cost.files = static_cast<int>(std::min(shorts.size(), longs.size()));
  cost.openMs = median(opens);
  cost.framesPerMs = l > s ? static_cast<double>(kLong - kShort) / (l - s) : 0.0;
  cost.latencyMs =
      std::max(0.0, cost.framesPerMs > 0.0 ? s - kShort / cost.framesPerMs : s);
  return cost;
}

double ringSecondsFor(const TailReadCost& cost, double sampleRate, int voices,
                      double minSeconds, double maxSeconds) {
  if (cost.files <= 0 || voices <= 0 || sampleRate <= 0.0) return maxSeconds;
  // A voice costs the disk two reads per ring length, each the fixed part
  // and half a ring of frames: 2L/R + transfer, per second, per voice.
  const double spare = kDiskShare - voices * transferShare(cost, sampleRate);
  if (spare <= 0.0) return maxSeconds;
  const double ring = 2.0 * voices * (cost.latencyMs / 1000.0) / spare;
  // To the next twentieth of a second.
  return std::clamp(std::ceil(ring * 20.0) / 20.0, minSeconds, maxSeconds);
}

} // namespace mp
