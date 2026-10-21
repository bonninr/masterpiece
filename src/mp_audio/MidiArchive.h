#pragma once

// Everything played, kept (E2). A second recorder beside the Recorder
// window's, running all the time while the player has it on: a take begins
// with the first note, ends after a silence, and is written to a dated file in
// the archive folder, which is kept under a size and an age.
//
// The audio thread only appends, under a lock it never waits for; the message
// thread takes a finished take by swapping the list out, writes it and prunes
// the folder.

#include "MidiRecorder.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace mp {

class MidiArchive {
public:
  // A take longer than this is saved and a new one begins, so memory stays
  // bounded.
  static constexpr double kMaxTakeSeconds = 2.0 * 60.0 * 60.0;

  void prepare(double sampleRate);

  // Audio thread. A message from a console, or 0 for none; before the first
  // note-on of a take only a note-on is kept.
  void capture(const juce::MidiMessage& message, int deviceId);
  // Audio thread, after the block's captures.
  void advance(int numSamples) { pos_.store(pos_.load(std::memory_order_relaxed) + numSamples, std::memory_order_release); }

  // Message thread. The take in progress, when it has ended -- `silenceSeconds`
  // with no message, longer than kMaxTakeSeconds, or `now` -- with times in
  // seconds from its first event and how long ago that first event was.
  bool takeEnded(double silenceSeconds, bool now, std::vector<MidiRecorder::TakeEvent>& out, double& startedAgo);
  bool hasTake() const { return count_.load(std::memory_order_acquire) != 0; }

  // Message thread. Writes `events` to <folder>/<yyyy-mm-dd>/<hh-mm-ss> <organ>.mid,
  // dated `started`, and lists it in the folder's index. Returns the file, or
  // none when it could not be written.
  static juce::File save(const juce::File& folder, const std::vector<MidiRecorder::TakeEvent>& events,
                         const juce::String& organ, juce::Time started, const MidiRecorder::DeviceName& nameOf);

  // Deletes the oldest files the index lists until those left are younger
  // than `maxDays` and together under `maxBytes`. Only listed `.mid` files
  // inside `folder` are ever deleted, and never the newest; a day's folder
  // left empty goes with its last file. Returns how many were deleted.
  static int prune(const juce::File& folder, int64_t maxBytes, int maxDays, juce::Time now);

  static constexpr const char* kIndexName = "archive-index.txt";

private:
  struct Event {
    int64_t samplePos = 0;
    juce::MidiMessage message;
    int deviceId = 0;
  };
  juce::SpinLock lock_;
  std::vector<Event> events_, spare_;
  double sampleRate_ = 48000.0;
  std::atomic<int64_t> pos_{0};    // the block being captured
  std::atomic<int64_t> first_{0};  // the take's first event
  std::atomic<int64_t> last_{0};   // and its latest
  std::atomic<size_t> count_{0};
};

}  // namespace mp
