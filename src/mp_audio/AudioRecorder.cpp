#include "AudioRecorder.h"

namespace mp {

namespace {
// The writer's room, in seconds of audio. A disk or a scheduler that holds
// the writer up for longer than this loses audio from the file (#1128); the
// old room of 32768 frames was 0.68 s at 48 kHz. Eight seconds of stereo
// float at 48 kHz is 3 MB.
constexpr double kRoomSeconds = 8.0;
}  // namespace

AudioRecorder::AudioRecorder() {
  // Above the program's other background work, so the render threads at
  // full load leave it its turn. Where the system refuses the priority, the
  // default one still writes.
  if (!backgroundThread_.startThread(juce::Thread::Priority::high)) backgroundThread_.startThread();
}

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
  framesLost_.store(0, std::memory_order_relaxed);

  auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
      writer, backgroundThread_, juce::jmax(32768, static_cast<int>(sampleRate * kRoomSeconds)));
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
  const bool had = writer_ != nullptr;
  writer_.reset();
  if (had && framesLost() > 0)
    juce::Logger::writeToLog("recording: " + juce::String(secondsLost(), 2) +
                             " s missing from " + file_.getFileName() +
                             ": the disk took longer than " + juce::String(kRoomSeconds, 0) + " s to write");
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
  // A full FIFO takes nothing from the block: count it, so the gap in the
  // file is known.
  if (!activeWriter_->write(buffer.getArrayOfReadPointers(),
                            buffer.getNumSamples()))
    framesLost_.fetch_add(buffer.getNumSamples(), std::memory_order_relaxed);
  framesWritten_.fetch_add(buffer.getNumSamples(), std::memory_order_relaxed);
}

} // namespace mp
