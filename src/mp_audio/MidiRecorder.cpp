#include "MidiRecorder.h"

#include <algorithm>

namespace mp {

void MidiRecorder::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void MidiRecorder::startRecording() {
  events_.clear();
  // Reserve enough that a few minutes of playing never allocates on the audio
  // thread. A busy organist produces perhaps 50 events a second.
  events_.reserve(16384);
  writePos_ = 0;
  state_ = State::Recording;
}

void MidiRecorder::stopRecording() {
  if (state_ == State::Recording) state_ = State::Idle;
}

void MidiRecorder::startPlayback() {
  if (events_.empty()) return;
  playPos_ = 0;
  playIndex_ = 0;
  state_ = State::Playing;
}

void MidiRecorder::stopPlayback() {
  if (state_ == State::Playing) state_ = State::Idle;
}

void MidiRecorder::clear() {
  events_.clear();
  state_ = State::Idle;
  writePos_ = playPos_ = 0;
  playIndex_ = 0;
}

double MidiRecorder::lengthSeconds() const {
  if (events_.empty()) return 0.0;
  return static_cast<double>(events_.back().samplePos) / sampleRate_;
}

double MidiRecorder::positionSeconds() const {
  const int64_t pos = state_ == State::Playing ? playPos_ : writePos_;
  return static_cast<double>(pos) / sampleRate_;
}

void MidiRecorder::captureLive(const juce::MidiMessage& message) {
  if (state_ != State::Recording) return;
  // In time order: after everything at or before the start of this block.
  const auto at = std::upper_bound(
      events_.begin(), events_.end(), blockStart_,
      [](int64_t pos, const Event& e) { return pos < e.samplePos; });
  events_.insert(at, Event{blockStart_, message});
}

void MidiRecorder::process(juce::MidiBuffer& midi, int numSamples) {
  if (numSamples <= 0) return;

  if (state_ == State::Recording) {
    blockStart_ = writePos_;
    for (const auto meta : midi) {
      // events_ may grow here, which allocates. Recording is a deliberate act
      // and the reserve above covers a long session; a reallocation costs one
      // late block rather than a fault, which is the right trade for a feature
      // the player switches on knowingly.
      events_.push_back({writePos_ + meta.samplePosition, meta.getMessage()});
    }
    writePos_ += numSamples;
    return;
  }

  if (state_ == State::Playing) {
    const int64_t blockEnd = playPos_ + numSamples;
    while (playIndex_ < events_.size() &&
           events_[playIndex_].samplePos < blockEnd) {
      const auto& e = events_[playIndex_];
      const int offset =
          static_cast<int>(juce::jlimit<int64_t>(0, numSamples - 1,
                                                 e.samplePos - playPos_));
      midi.addEvent(e.message, offset);
      ++playIndex_;
    }
    playPos_ = blockEnd;

    // Stop at the end rather than looping: a recording that silently repeats
    // is confusing, and looping is a transport feature, not a default.
    if (playIndex_ >= events_.size()) state_ = State::Idle;
  }
}

bool MidiRecorder::saveToFile(const juce::File& file) const {
  if (events_.empty()) return false;

  juce::MidiMessageSequence seq;
  for (const auto& e : events_) {
    juce::MidiMessage m = e.message;
    m.setTimeStamp(static_cast<double>(e.samplePos) / sampleRate_);
    seq.addEvent(m);
  }
  seq.updateMatchedPairs();

  juce::MidiFile mf;
  // Ticks per quarter note; with a 1 second = 1 quarter mapping below this
  // gives millisecond resolution, which is finer than any console sends.
  const short ticksPerQuarter = 960;
  mf.setTicksPerQuarterNote(ticksPerQuarter);

  juce::MidiMessageSequence ticked;
  for (int i = 0; i < seq.getNumEvents(); ++i) {
    juce::MidiMessage m = seq.getEventPointer(i)->message;
    m.setTimeStamp(m.getTimeStamp() * ticksPerQuarter);
    ticked.addEvent(m);
  }
  mf.addTrack(ticked);

  file.getParentDirectory().createDirectory();
  file.deleteFile();
  std::unique_ptr<juce::FileOutputStream> out(file.createOutputStream());
  if (out == nullptr) return false;
  return mf.writeTo(*out);
}

bool MidiRecorder::loadFromFile(const juce::File& file) {
  juce::FileInputStream in(file);
  if (!in.openedOk()) return false;

  juce::MidiFile mf;
  if (!mf.readFrom(in)) return false;
  mf.convertTimestampTicksToSeconds();

  clear();
  for (int t = 0; t < mf.getNumTracks(); ++t) {
    const auto* track = mf.getTrack(t);
    for (int i = 0; i < track->getNumEvents(); ++i) {
      const auto& m = track->getEventPointer(i)->message;
      if (m.isMetaEvent()) continue; // tempo and names carry no performance
      events_.push_back(
          {static_cast<int64_t>(m.getTimeStamp() * sampleRate_), m});
    }
  }
  std::stable_sort(events_.begin(), events_.end(),
                   [](const Event& a, const Event& b) {
                     return a.samplePos < b.samplePos;
                   });
  return !events_.empty();
}

} // namespace mp
