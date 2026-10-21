// MIDI recorder and player.
//
// Records everything that reaches the engine — notes AND the stop changes,
// couplers and shoe movements that arrive as MIDI — so a recording plays back
// as the same performance rather than as notes on whatever registration
// happens to be drawn at the time.
//
// Timestamps are in samples, converted to seconds on save. Sample time is what
// the audio callback actually has; wall-clock would drift against it.
#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

#include <bitset>
#include <vector>

namespace mp {

class MidiRecorder {
public:
  enum class State { Idle, Recording, Playing };
  // What a new recording does with the take that is already there.
  enum class Take { Replace, Append };

  void prepare(double sampleRate);

  // Starts a recording. Replace throws the take that is there away; Append
  // keeps it and carries on from the end of it.
  void startRecording(Take take = Take::Replace);
  void stopRecording();
  void startPlayback();
  void stopPlayback();
  void clear();

  State state() const { return state_; }
  bool isRecording() const { return state_ == State::Recording; }
  bool isPlaying() const { return state_ == State::Playing; }
  bool empty() const { return events_.empty(); }
  // True when a take is there that has never been written to a file. A new
  // recording over it would throw it away, so the player is asked first.
  bool hasUnsavedTake() const { return !events_.empty() && !saved_; }
  int eventCount() const { return static_cast<int>(events_.size()); }
  double lengthSeconds() const;
  double positionSeconds() const;

  // Called once per block, before the engine consumes the buffer.
  // While recording, everything in `midi` is captured. While playing, the
  // recorded events for this block are merged INTO `midi`, so playback drives
  // exactly the same path a live console does.
  void process(juce::MidiBuffer& midi, int numSamples);
  // A message from a hardware console. Those do not come in the host's buffer
  // but through the processor's own device queue, read after process(), so
  // they are handed over here -- or recording a real console captured nothing
  // but the on-screen keys. They arrived between blocks, so they are placed
  // at the start of the block they were read in.
  void captureLive(const juce::MidiMessage& message);

  bool saveToFile(const juce::File& file);
  bool loadFromFile(const juce::File& file);

private:
  struct Event {
    int64_t samplePos = 0;
    juce::MidiMessage message;
  };

  std::vector<Event> events_;
  State state_ = State::Idle;
  // The take is on disk: it was written to a file, or loaded from one. A new
  // recording may replace it without asking, because the file still holds it.
  bool saved_ = false;
  double sampleRate_ = 48000.0;
  int64_t writePos_ = 0;   // recording head
  int64_t blockStart_ = 0; // where the block being recorded began
  int64_t playPos_ = 0;    // playback head
  size_t playIndex_ = 0;   // next event to emit
  // The keys playback has put down and not yet let go, by channel and note.
  // When playback ends with any of them still down -- stopped part-way, or a
  // file that ends on a held chord -- they are let go in the next block: an
  // organ pipe has no decay, and the chord would sound until Panic (#325).
  // Only playback's own keys: what the player is holding stays held.
  std::bitset<16 * 128> held_;
  // Playback was started again from the top while it was running.
  bool restarted_ = false;
  void releaseHeld(juce::MidiBuffer& midi);
};

} // namespace mp
