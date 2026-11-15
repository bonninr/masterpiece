// Captures the organ's output to a WAV file.
//
// Sits after the master fader and BEFORE the metronome: the metronome is a
// monitoring aid, and a click track in the recording is not the performance.
//
// The audio thread must never touch a disk, so it hands blocks to JUCE's
// ThreadedWriter, which owns a FIFO and a background thread and does the
// writing there. What the audio thread pays is a memcpy into that FIFO.
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace mp {

class AudioRecorder {
public:
  AudioRecorder();
  ~AudioRecorder();

  // Message thread. Returns false if the file could not be opened for
  // writing, which is the only failure a player can do anything about.
  bool start(const juce::File& file, double sampleRate, int channels);
  void stop();

  bool isRecording() const {
    return recording_.load(std::memory_order_relaxed);
  }
  // Where it is being written, so the panel can name it.
  juce::File file() const { return file_; }
  double secondsRecorded() const;
  // Frames handed over that the file never got, because the writer fell
  // further behind than its room (#1128). Zero in a take that is whole.
  int64_t framesLost() const { return framesLost_.load(std::memory_order_relaxed); }
  double secondsLost() const {
    return sampleRate_ > 0.0 ? static_cast<double>(framesLost()) / sampleRate_ : 0.0;
  }
  // The thread the file is written on. A test holds it up to stand in for a
  // slow disk.
  juce::TimeSliceThread& writerThread() { return backgroundThread_; }

  // Audio thread. Does nothing unless recording.
  void write(const juce::AudioBuffer<float>& buffer);

private:
  // The writer is swapped under a lock that the audio thread also takes. It
  // is contended only when recording starts or stops — never while running —
  // which is the trade JUCE's own recorder makes, and the alternative is a
  // hand-rolled FIFO that would have the same shape with more ways to be
  // wrong.
  juce::CriticalSection writerLock_;
  juce::TimeSliceThread backgroundThread_{"mp audio recorder"};
  std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer_;
  juce::AudioFormatWriter::ThreadedWriter* activeWriter_ = nullptr;

  std::atomic<bool> recording_{false};
  std::atomic<int64_t> framesWritten_{0};
  std::atomic<int64_t> framesLost_{0};
  double sampleRate_ = 0.0;
  juce::File file_;
};

} // namespace mp
