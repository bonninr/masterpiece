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

#include <vector>

namespace mp {

class MidiRecorder {
public:
  enum class State { Idle, Recording, Playing };

  void prepare(double sampleRate);

  void startRecording();
  void stopRecording();
  void startPlayback();
  void stopPlayback();
  void clear();

  State state() const { return state_; }
  bool isRecording() const { return state_ == State::Recording; }
  bool isPlaying() const { return state_ == State::Playing; }
  bool empty() const { return events_.empty(); }
  int eventCount() const { return static_cast<int>(events_.size()); }
  double lengthSeconds() const;
  double positionSeconds() const;

  // Called once per block, before the engine consumes the buffer.
  // While recording, everything in `midi` is captured. While playing, the
  // recorded events for this block are merged INTO `midi`, so playback drives
  // exactly the same path a live console does.
  void process(juce::MidiBuffer& midi, int numSamples);

  bool saveToFile(const juce::File& file) const;
  bool loadFromFile(const juce::File& file);

private:
  struct Event {
    int64_t samplePos = 0;
    juce::MidiMessage message;
  };

  std::vector<Event> events_;
  State state_ = State::Idle;
  double sampleRate_ = 48000.0;
  int64_t writePos_ = 0;   // recording head
  int64_t playPos_ = 0;    // playback head
  size_t playIndex_ = 0;   // next event to emit
};

} // namespace mp
