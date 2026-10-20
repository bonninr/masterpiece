// The question the Record button asks before a new take replaces one that
// was never saved (#428).
//
// The rule lives in the recorder, so it can be checked here without a window:
// what the button does is decided by hasUnsavedTake(), and the two answers
// are the two ways of starting a recording.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/MidiRecorder.h"

#include <cmath>
#include <vector>

using mp::test::Category;

namespace {

constexpr double kSampleRate = 48000.0;

// A recorder with one note in it: a take that was played and stopped.
mp::MidiRecorder recordedTake() {
  mp::MidiRecorder rec;
  rec.prepare(kSampleRate);
  rec.startRecording();
  juce::MidiBuffer b;
  b.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 100);
  rec.process(b, 480);
  rec.stopRecording();
  return rec;
}

// The notes the recorder plays back, in the order they come.
std::vector<int> playedNotes(mp::MidiRecorder& rec, int blocks) {
  std::vector<int> notes;
  rec.startPlayback();
  juce::MidiBuffer b;
  for (int i = 0; i < blocks; ++i) {
    b.clear();
    rec.process(b, 256);
    for (const auto meta : b)
      if (meta.getMessage().isNoteOn()) notes.push_back(meta.getMessage().getNoteNumber());
  }
  rec.stopPlayback();
  return notes;
}

} // namespace

// The question comes when a take is there that has never been written to a
// file, and nowhere else.
class RecordAsksBeforeReplaceTest final : public mp::test::Test {
public:
  RecordAsksBeforeReplaceTest()
      : Test("functional.midi.record-asks-before-replace", Category::Functional) {}
  void run() override {
    mp::MidiRecorder rec;
    rec.prepare(kSampleRate);
    MP_CHECK(!rec.hasUnsavedTake(), "a recorder with nothing in it asks nothing");

    rec.startRecording();
    juce::MidiBuffer b;
    b.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 100);
    rec.process(b, 480);
    rec.stopRecording();
    MP_CHECK(rec.hasUnsavedTake(), "a take that was never saved is asked about");

    // The file holds it, so a new recording may replace it without asking.
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("mp-recorder-replace.mid");
    MP_CHECK(rec.saveToFile(file), "the take is saved");
    MP_CHECK(!rec.hasUnsavedTake(), "a saved take is replaced without a question");
    file.deleteFile();

    // Replace: the old take is thrown away and the clock starts again.
    rec.startRecording();
    MP_CHECK(rec.eventCount() == 0, "the old take is gone");
    MP_CHECK(rec.lengthSeconds() == 0.0, "and the clock starts again");
    b.clear();
    b.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 0);
    rec.process(b, 480);
    rec.stopRecording();
    MP_CHECK(rec.eventCount() == 1, "the new take holds only what was played after");
    MP_CHECK(rec.lengthSeconds() < 0.001, "and it starts at the beginning");
    MP_CHECK(rec.hasUnsavedTake(), "and it is unsaved again");
  }
};

// Append: the take stays and the recording carries on from its end.
class RecordAppendsTest final : public mp::test::Test {
public:
  RecordAppendsTest() : Test("functional.midi.record-appends", Category::Functional) {}
  void run() override {
    mp::MidiRecorder rec = recordedTake();
    MP_CHECK(rec.eventCount() == 1, "the first take holds one event");
    const double firstLength = rec.lengthSeconds();

    rec.startRecording(mp::MidiRecorder::Take::Append);
    juce::MidiBuffer b;
    b.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 0);
    rec.process(b, 480);
    rec.stopRecording();

    MP_CHECK(rec.eventCount() == 2, "the take keeps what was recorded and gains the new event");
    // The first take ran one block of 480 samples, so the new event lands at
    // the end of it: 480 samples at 48 kHz.
    MP_CHECK(std::abs(rec.lengthSeconds() - 480.0 / kSampleRate) < 1e-6,
             "the new event lands at the end of the first take, got " +
                 std::to_string(rec.lengthSeconds()));
    MP_CHECK(rec.lengthSeconds() > firstLength, "which is after the first event");

    const auto notes = playedNotes(rec, 4);
    MP_CHECK(notes.size() == 2 && notes[0] == 60 && notes[1] == 64,
             "and the take plays back as one performance, in order");
  }
};

// A take that was saved and then recorded on is unsaved again: the file holds
// the older take, not this one.
class AppendAfterSaveTest final : public mp::test::Test {
public:
  AppendAfterSaveTest() : Test("functional.midi.append-after-save", Category::Functional) {}
  void run() override {
    mp::MidiRecorder rec = recordedTake();
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("mp-recorder-append.mid");
    MP_CHECK(rec.saveToFile(file), "the take is saved");
    MP_CHECK(!rec.hasUnsavedTake(), "and needs no question");

    rec.startRecording(mp::MidiRecorder::Take::Append);
    juce::MidiBuffer b;
    b.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 0);
    rec.process(b, 480);
    rec.stopRecording();
    MP_CHECK(rec.hasUnsavedTake(), "the take is unsaved again once it has grown");
    file.deleteFile();
  }
};

// A take loaded from a file is on disk, and recording on from it carries on
// after what the file holds.
class AppendAfterLoadTest final : public mp::test::Test {
public:
  AppendAfterLoadTest() : Test("functional.midi.append-after-load", Category::Functional) {}
  void run() override {
    mp::MidiRecorder rec = recordedTake();
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("mp-recorder-load.mid");
    MP_CHECK(rec.saveToFile(file), "the take is saved");

    mp::MidiRecorder back;
    back.prepare(kSampleRate);
    MP_CHECK(back.loadFromFile(file), "and loads back into the recorder");
    MP_CHECK(!back.hasUnsavedTake(), "a loaded take needs no question");
    const double loadedLength = back.lengthSeconds();

    back.startRecording(mp::MidiRecorder::Take::Append);
    juce::MidiBuffer b;
    b.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 0);
    back.process(b, 480);
    back.stopRecording();
    MP_CHECK(back.eventCount() == 2, "the loaded take gains the new event");
    MP_CHECK(back.lengthSeconds() > loadedLength,
             "which lands after the event the file holds");
    file.deleteFile();
  }
};

static RecordAsksBeforeReplaceTest g_recordAsksBeforeReplace;
static RecordAppendsTest g_recordAppends;
static AppendAfterSaveTest g_appendAfterSave;
static AppendAfterLoadTest g_appendAfterLoad;

#endif // MP_TEST_HAS_AUDIO
