// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

namespace mp {

// The organ's sound for a live spectrum: the audio thread copies the first
// pair, folded to mono, into a ring while a window shows the spectrum, and
// the window reads the newest samples and analyses them on its own thread.
// With no window open the audio thread does one atomic load and returns.
class SpectrumTap {
public:
  static constexpr int kRing = 8192;
  static constexpr int kFft = 4096;  // 11.7 Hz per bin at 48 kHz

  // A window that shows the spectrum opens the tap and closes it when it
  // goes; several may be open at once.
  void open() { viewers_.fetch_add(1, std::memory_order_acq_rel); }
  void close() { viewers_.fetch_sub(1, std::memory_order_acq_rel); }
  bool isOpen() const { return viewers_.load(std::memory_order_acquire) > 0; }

  // Audio thread.
  void push(const float* const* channels, int numChannels, int numSamples) {
    if (!isOpen() || numChannels <= 0) return;
    int w = write_.load(std::memory_order_relaxed);
    const float* l = channels[0];
    const float* r = numChannels > 1 ? channels[1] : channels[0];
    for (int i = 0; i < numSamples; ++i) {
      ring_[static_cast<size_t>(w)].store(0.5f * (l[i] + r[i]), std::memory_order_relaxed);
      w = (w + 1) & (kRing - 1);
    }
    write_.store(w, std::memory_order_release);
    pushed_.fetch_add(numSamples, std::memory_order_relaxed);
  }
  long long samplesPushed() const { return pushed_.load(std::memory_order_relaxed); }

  // Any thread: the level of each FFT bin in dBFS (a full-scale sine reads
  // 0 dB), from the newest kFft samples under a Hann window. kFft / 2 + 1
  // values, bin k at k * sampleRate / kFft Hz.
  std::vector<float> spectrumDb() const;

  // The analysis itself, for a block of kFft samples.
  static std::vector<float> analyse(const float* samples);

private:
  std::atomic<int> viewers_{0};
  std::atomic<int> write_{0};
  std::atomic<long long> pushed_{0};
  std::array<std::atomic<float>, kRing> ring_{};
};

}  // namespace mp
