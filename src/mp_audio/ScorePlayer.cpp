#include "ScorePlayer.h"

#include <algorithm>
#include <cmath>

namespace mp {

void ScorePlayer::prepare(double sampleRate) {
  if (sampleRate > 0.0) sampleRate_ = sampleRate;
}

void ScorePlayer::orderEvents(std::vector<Event>& events) {
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    if (a.seconds != b.seconds) return a.seconds < b.seconds;
    return !a.on && b.on;
  });
}

void ScorePlayer::play(std::vector<Event> events, double fromSeconds) {
  const double length = events.empty() ? 0.0 : events.back().seconds;
  {
    const juce::SpinLock::ScopedLockType l(lock_);
    events_.swap(events);
    startAt_ = std::max(0.0, fromSeconds);
    restart_ = true;
    stopRequested_ = false;
  }
  // The list replaced is freed here, outside the lock.
  length_.store(length, std::memory_order_release);
  position_.store(std::max(0.0, fromSeconds), std::memory_order_release);
  playing_.store(true, std::memory_order_release);
  active_.store(true, std::memory_order_release);
}

void ScorePlayer::stop() {
  {
    const juce::SpinLock::ScopedLockType l(lock_);
    stopRequested_ = true;
    restart_ = false;
  }
  playing_.store(false, std::memory_order_release);
}

void ScorePlayer::releaseHeld(juce::MidiBuffer& midi) {
  if (heldCount_ == 0) return;
  for (size_t i = 0; i < held_.size(); ++i)
    if (held_[i] != 0) {
      midi.addEvent(juce::MidiMessage::noteOff(static_cast<int>(i / 128) + 1, static_cast<int>(i % 128)), 0);
      held_[i] = 0;
    }
  heldCount_ = 0;
}

void ScorePlayer::process(juce::MidiBuffer& midi, int numSamples) {
  if (!active_.load(std::memory_order_acquire) || numSamples <= 0) return;
  const juce::SpinLock::ScopedTryLockType tl(lock_);
  if (!tl.isLocked()) return;

  if (stopRequested_) {
    releaseHeld(midi);
    stopRequested_ = false;
    active_.store(false, std::memory_order_release);
    return;
  }
  if (restart_) {
    // From the top or from another place: what the last start held goes up
    // first, as note-offs at the start of this block.
    releaseHeld(midi);
    restart_ = false;
    pos_ = std::llround(startAt_ * sampleRate_);
    index_ = static_cast<size_t>(
        std::lower_bound(events_.begin(), events_.end(), startAt_,
                         [](const Event& e, double t) { return e.seconds < t; }) -
        events_.begin());
  }

  const int64_t blockEnd = pos_ + numSamples;
  while (index_ < events_.size()) {
    const Event& e = events_[index_];
    // Rounded: 0.02 s at 48 kHz is 959.99... in floating point, and sample 960.
    const int64_t at = std::llround(e.seconds * sampleRate_);
    if (at >= blockEnd) break;
    const int offset = static_cast<int>(std::clamp<int64_t>(at - pos_, 0, numSamples - 1));
    const int ch = std::clamp<int>(e.channel, 1, 16);
    auto& count = held_[static_cast<size_t>((ch - 1) * 128 + (e.note & 127))];
    if (e.on) {
      if (count == 0) {
        midi.addEvent(juce::MidiMessage::noteOn(ch, e.note & 127, static_cast<juce::uint8>(100)), offset);
        ++heldCount_;
      }
      if (count < 255) ++count;
    } else if (count > 0) {
      if (--count == 0) {
        midi.addEvent(juce::MidiMessage::noteOff(ch, e.note & 127), offset);
        --heldCount_;
      }
    }
    ++index_;
  }
  pos_ = blockEnd;
  position_.store(static_cast<double>(pos_) / sampleRate_, std::memory_order_release);

  if (index_ >= events_.size()) {
    // The end of the score: a final chord that the file never lets go of is
    // let go here, since an organ pipe does not decay.
    releaseHeld(midi);
    playing_.store(false, std::memory_order_release);
    active_.store(false, std::memory_order_release);
  }
}

} // namespace mp
