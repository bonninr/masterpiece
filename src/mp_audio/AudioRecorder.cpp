#include "AudioRecorder.h"

namespace mp {

AudioRecorder::AudioRecorder() { backgroundThread_.startThread(); }

AudioRecorder::~AudioRecorder() {
  stop();
  backgroundThread_.stopThread(2000);
}

bool AudioRecorder::start(const juce::File& file, double sampleRate,
                          int channels) {
  stop();
  if (sampleRate <= 0.0) return false;

  file.deleteFile();
  auto stream = std::unique_ptr<juce::FileOutputStream>(
      file.createOutputStream());
  if (stream == nullptr || !stream->openedOk()) return false;

  // 24-bit, because that is what the sample sets themselves are recorded at
  // and there is no reason for the capture to be the narrowest link.
  juce::WavAudioFormat wav;
  auto* writer = wav.createWriterFor(stream.get(), sampleRate,
                                     static_cast<unsigned>(juce::jlimit(1, 2, channels)),
                                     24, {}, 0);
  if (writer == nullptr) return false;
  stream.release(); // the writer owns it now

  sampleRate_ = sampleRate;
  file_ = file;
  framesWritten_.store(0, std::memory_order_relaxed);

  auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
      writer, backgroundThread_, 32768);
  {
    const juce::ScopedLock sl(writerLock_);
    writer_ = std::move(threaded);
    activeWriter_ = writer_.get();
  }
  recording_.store(true, std::memory_order_release);
  return true;
}

void AudioRecorder::stop() {
  // Clear the pointer the audio thread reads before destroying anything it
  // could still be inside.
  {
    const juce::ScopedLock sl(writerLock_);
    activeWriter_ = nullptr;
  }
  recording_.store(false, std::memory_order_release);
  // Destroying the ThreadedWriter flushes whatever is still in its FIFO and
  // finalises the WAV header, so the file is playable even if the player
  // never pressed stop before quitting.
  writer_.reset();
}

double AudioRecorder::secondsRecorded() const {
  if (sampleRate_ <= 0.0) return 0.0;
  return static_cast<double>(framesWritten_.load(std::memory_order_relaxed)) /
         sampleRate_;
}

void AudioRecorder::write(const juce::AudioBuffer<float>& buffer) {
  if (!recording_.load(std::memory_order_acquire)) return;

  const juce::ScopedLock sl(writerLock_);
  if (activeWriter_ == nullptr) return;
  activeWriter_->write(buffer.getArrayOfReadPointers(),
                       buffer.getNumSamples());
  framesWritten_.fetch_add(buffer.getNumSamples(), std::memory_order_relaxed);
}

} // namespace mp
