// Plays a score from the score panel through the organ.
//
// The panel turns each staff's notes into events on the channel of the
// manual the player gave that staff, and hands them over sorted. Each block,
// the events that fall in it are merged into the block's MIDI, at the same
// point as the recorder's playback, so a score drives exactly the path a live
// console does: couplers, the drawn keys and Panic all see it.
//
// Idle, process() is one atomic load. The message thread swaps the event list
// under a spin lock the audio thread only ever tries: a block that finds it
// held plays nothing new and carries on in the next.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace mp {

class ScorePlayer {
public:
  struct Event {
    double seconds = 0.0;
    uint8_t channel = 1;  // 1..16
    uint8_t note = 60;
    bool on = true;
  };

  void prepare(double sampleRate);

  // Message thread. `events` sorted by time, note-offs before note-ons at the
  // same moment (orderEvents() does it). Starts at `fromSeconds`.
  void play(std::vector<Event> events, double fromSeconds);
  // Stops; the keys it holds are let go in the next block.
  void stop();
  bool isPlaying() const { return playing_.load(std::memory_order_acquire); }
  // Where the playing is, in seconds of the score.
  double positionSeconds() const { return position_.load(std::memory_order_acquire); }
  // Length of the list handed to play(), in seconds.
  double lengthSeconds() const { return length_.load(std::memory_order_acquire); }

  // Sorts as play() wants: by time, note-offs first at the same moment, so a
  // repeated note sounds again.
  static void orderEvents(std::vector<Event>& events);

  // Audio thread, once per block, after the recorder.
  void process(juce::MidiBuffer& midi, int numSamples);

private:
  void releaseHeld(juce::MidiBuffer& midi);

  juce::SpinLock lock_;
  // Guarded by lock_.
  std::vector<Event> events_;
  double startAt_ = 0.0;
  bool restart_ = false;
  bool stopRequested_ = false;

  // Audio thread only.
  double sampleRate_ = 48000.0;
  int64_t pos_ = 0;
  size_t index_ = 0;
  // How many of the playing's notes hold each key, by channel and note: two
  // staves given the same manual share keys, and a key goes up only when the
  // last of them lets go.
  std::array<uint8_t, 16 * 128> held_{};
  int heldCount_ = 0;

  std::atomic<bool> active_{false};   // the audio thread has work: playing or releasing
  std::atomic<bool> playing_{false};  // what the panel shows
  std::atomic<double> position_{0.0};
  std::atomic<double> length_{0.0};
};

} // namespace mp
