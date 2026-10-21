#include "MidiRecorder.h"

#include <algorithm>

namespace mp {

MidiRecorder::MidiRecorder() {
  // The keys playback holds down: a handful at a time, and the room for them
  // is there from the start, so the audio thread never has to look for it.
  held_.reserve(128);
}

void MidiRecorder::prepare(double sampleRate) {
  sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void MidiRecorder::startRecording(Take take) {
  if (take == Take::Replace) {
    events_.clear();
    // Reserve enough that a few minutes of playing never allocates on the audio
    // thread. A busy organist produces perhaps 50 events a second.
    events_.reserve(16384);
    writePos_ = 0;
  } else {
    // The same room after the take that is kept: a take read from a file
    // holds exactly what it needs, and the audio thread must not grow it.
    events_.reserve(events_.size() + 16384);
  }
  // Append keeps the take and the write head where they are: the new events
  // land after the old ones on the same clock, so the take plays back as one
  // performance with no gap in it.
  saved_ = false;
  state_ = State::Recording;
}

void MidiRecorder::stopRecording() {
  if (state_ == State::Recording) state_ = State::Idle;
}

void MidiRecorder::startPlayback() {
  if (events_.empty()) return;
  playPos_ = 0;
  playIndex_ = 0;
  restarted_ = state_ == State::Playing;
  state_ = State::Playing;
}

void MidiRecorder::stopPlayback() {
  if (state_ == State::Playing) state_ = State::Idle;
}

void MidiRecorder::clear() {
  events_.clear();
  state_ = State::Idle;
  saved_ = false;
  writePos_ = playPos_ = 0;
  playIndex_ = 0;
  held_.clear();
}

double MidiRecorder::lengthSeconds() const {
  if (events_.empty()) return 0.0;
  return static_cast<double>(events_.back().samplePos) / sampleRate_;
}

double MidiRecorder::positionSeconds() const {
  const int64_t pos = state_ == State::Playing ? playPos_ : writePos_;
  return static_cast<double>(pos) / sampleRate_;
}

void MidiRecorder::captureLive(const juce::MidiMessage& message, int deviceId) {
  if (state_ != State::Recording) return;
  // In time order: after everything at or before the start of this block.
  const auto at = std::upper_bound(
      events_.begin(), events_.end(), blockStart_,
      [](int64_t pos, const Event& e) { return pos < e.samplePos; });
  events_.insert(at, Event{blockStart_, message, deviceId});
}

void MidiRecorder::releaseHeld(std::vector<Playback>& out) {
  for (const auto& h : held_)
    out.push_back({h.deviceId, juce::MidiMessage::noteOff(h.channel, h.note), 0});
  held_.clear();
}

void MidiRecorder::process(juce::MidiBuffer& midi, int numSamples,
                           std::vector<Playback>& out) {
  out.clear();
  if (numSamples <= 0) return;

  // Playback that has ended, begun again from the top, or given way to a
  // recording lets go of the keys it left down. As note-offs in this block,
  // so each pipe gets its release; `out` is never captured, so they stay out
  // of the take.
  if (restarted_ || state_ != State::Playing) releaseHeld(out);
  restarted_ = false;

  if (state_ == State::Recording) {
    blockStart_ = writePos_;
    for (const auto meta : midi) {
      // events_ may grow here, which allocates. Recording is a deliberate act
      // and the reserve above covers a long session; a reallocation costs one
      // late block rather than a fault, which is the right trade for a feature
      // the player switches on knowingly.
      events_.push_back({writePos_ + meta.samplePosition, meta.getMessage(), 0});
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
      // The console it came from goes with it: it is what says which manual
      // the note plays, and a take that lost it would play on the manual the
      // channel means in the organ's own assignment.
      out.push_back({e.deviceId, e.message, offset});
      if (e.message.isNoteOnOrOff()) {
        const int channel = e.message.getChannel();
        const int note = e.message.getNoteNumber();
        // The key let go: it is no longer held by playback.
        for (size_t i = 0; i < held_.size(); ++i)
          if (held_[i].channel == channel && held_[i].note == note) {
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
          }
        // A key struck again: the one held down is the same key.
        if (e.message.isNoteOn())
          held_.push_back({e.deviceId, channel, note});
      }
      ++playIndex_;
    }
    playPos_ = blockEnd;

    // Stop at the end rather than looping: a recording that silently repeats
    // is confusing, and looping is a transport feature, not a default.
    if (playIndex_ >= events_.size()) state_ = State::Idle;
  }
}

std::vector<MidiRecorder::TakeEvent> MidiRecorder::takeEvents() const {
  std::vector<TakeEvent> out;
  out.reserve(events_.size());
  for (const auto& e : events_)
    out.push_back({static_cast<double>(e.samplePos) / sampleRate_, e.message, e.deviceId});
  return out;
}

bool MidiRecorder::writeTake(const juce::File& file, const std::vector<TakeEvent>& events,
                             const DeviceName& nameOf) {
  if (events.empty()) return false;
  juce::MidiFile mf;
  // Ticks per quarter note; with a 1 second = 1 quarter mapping below this
  // gives millisecond resolution, which is finer than any console sends.
  const short ticksPerQuarter = 960;
  mf.setTicksPerQuarterNote(ticksPerQuarter);

  // A quarter lasts a second, said in the file: with no tempo event every
  // reader takes 120 bpm, and a take played back at half its length.
  juce::MidiMessageSequence tempo;
  tempo.addEvent(juce::MidiMessage::tempoMetaEvent(1000000), 0.0);
  mf.addTrack(tempo);

  // A track per console, the one of no console first.
  std::vector<int> devices;
  for (const auto& e : events)
    if (std::find(devices.begin(), devices.end(), e.deviceId) == devices.end()) devices.push_back(e.deviceId);
  std::sort(devices.begin(), devices.end());
  for (const int device : devices) {
    juce::MidiMessageSequence track;
    const std::string name = device != 0 && nameOf ? nameOf(device) : std::string();
    if (!name.empty()) track.addEvent(juce::MidiMessage::textMetaEvent(9, juce::String::fromUTF8(name.c_str())), 0.0);
    for (const auto& e : events)
      if (e.deviceId == device) {
        juce::MidiMessage m = e.message;
        m.setTimeStamp(e.seconds * ticksPerQuarter);
        track.addEvent(m);
      }
    track.updateMatchedPairs();
    mf.addTrack(track);
  }

  file.getParentDirectory().createDirectory();
  file.deleteFile();
  std::unique_ptr<juce::FileOutputStream> out(file.createOutputStream());
  return out != nullptr && mf.writeTo(*out);
}

bool MidiRecorder::saveToFile(const juce::File& file, const DeviceName& nameOf) {
  if (!writeTake(file, takeEvents(), nameOf)) return false;
  // The take is on disk now, so a new recording may replace it without asking.
  saved_ = true;
  return true;
}

bool MidiRecorder::loadFromFile(const juce::File& file, const DeviceId& idOf) {
  juce::FileInputStream in(file);
  if (!in.openedOk()) return false;

  juce::MidiFile mf;
  if (!mf.readFrom(in)) return false;
  mf.convertTimestampTicksToSeconds();

  clear();
  for (int t = 0; t < mf.getNumTracks(); ++t) {
    const auto* track = mf.getTrack(t);
    // The console a track was played on, by the name it opens with; a track
    // with no name, or a console not known here, plays by the organ's own
    // assignment of its channels.
    int device = 0;
    for (int i = 0; i < track->getNumEvents() && device == 0; ++i) {
      const auto& m = track->getEventPointer(i)->message;
      if (m.isMetaEvent() && m.getMetaEventType() == 9 && idOf)
        device = idOf(m.getTextFromTextMetaEvent().toStdString());
    }
    for (int i = 0; i < track->getNumEvents(); ++i) {
      const auto& m = track->getEventPointer(i)->message;
      if (m.isMetaEvent()) continue; // tempo and names carry no performance
      events_.push_back(
          {static_cast<int64_t>(m.getTimeStamp() * sampleRate_), m, device});
    }
  }
  std::stable_sort(events_.begin(), events_.end(),
                   [](const Event& a, const Event& b) {
                     return a.samplePos < b.samplePos;
                   });
  // The take came from a file, so it is on disk: a new recording may replace
  // it without asking.
  saved_ = !events_.empty();
  // The write head sits at the end of the take, so that recording on from here
  // carries on after what the file holds.
  writePos_ = events_.empty() ? 0 : events_.back().samplePos + 1;
  return !events_.empty();
}

} // namespace mp
