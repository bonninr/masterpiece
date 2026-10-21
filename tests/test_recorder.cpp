// The MIDI recorder (#427). A take plays back on the keyboard it was played
// on: the console a message came from is part of what says which manual it
// plays, and a take that lost its console played its notes on whatever the
// channel means in the organ's own assignment -- the pedal, whose code there
// is 1.
//
// The recorder is JUCE-backed, so this test runs in the full build.
#include "MpTest.h"

#if defined(MP_TEST_HAS_AUDIO)

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/MidiRecorder.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;
using mp::Id;

juce::File fixture() {
  return juce::File(juce::String(MP_TEST_FIXTURES_DIR) +
                     "/recorder-keyboards.Organ_Hauptwerk_xml");
}

// One block through the processor: the device queue is drained and the engine
// sees what the console sent.
void render(mp::MasterpieceProcessor& proc, int samples = 256) {
  juce::AudioBuffer<float> buffer(2, samples);
  juce::MidiBuffer midi;
  proc.processBlock(buffer, midi);
}

// The console the player has named for their MIDI keyboard.
constexpr int kConsole = 2;

class RecorderPlaysOnItsOwnKeyboardTest final : public mp::test::Test {
public:
  RecorderPlaysOnItsOwnKeyboardTest()
      : Test("functional.audio.recorder-plays-on-the-keyboard-it-was-played-on",
             Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    // The test sets its own routing: what the player has mapped on this
    // machine is not part of it.
    proc.clearChannelAssignments();
    proc.midiMap().removeBindings([](const mp::MidiBinding&) { return true; });

    // The organ's own assignment: the code 1 is the pedal, the code 2 the
    // first manual.
    const Id pedal = proc.keyboardForChannel(1, 0);
    const Id manual = proc.keyboardForChannel(2, 0);
    MP_CHECK(pedal != 0 && manual != 0 && pedal != manual,
             "the organ has a manual and a pedal");

    // The player's console: this MIDI keyboard plays Manual I, and that
    // keyboard alone -- the console is named, as it is in Settings > MIDI.
    proc.setKeyboardForChannel(1, manual, kConsole, true);
    MP_CHECK(proc.keyboardForChannel(1, kConsole) == manual,
             "the console plays the manual");
    MP_CHECK(proc.keyboardForChannel(1, 0) == pedal,
             "while the channel on its own is the pedal: a take that lost its "
             "console would play there");

    // Record the note, played on the console.
    proc.recorder().startRecording();
    proc.pushMidi(kConsole, juce::MidiMessage::noteOn(1, 48, 100.0f));
    render(proc);
    proc.recorder().stopRecording();
    MP_CHECK(proc.recorder().eventCount() == 1, "the note was recorded");

    // Play the take back.
    proc.recorder().startPlayback();
    render(proc);
    MP_CHECK(proc.keyDownOnKeyboard(manual, 48),
             "the take plays on the manual it was played on");
    MP_CHECK(!proc.keyDownOnKeyboard(pedal, 48), "and not on the pedalboard");
  }
};
static RecorderPlaysOnItsOwnKeyboardTest g_recorderKeyboard;

// The recorder's buttons on a console (5.1): a learned controller raises a
// request on the audio thread and nothing more; the message thread carries it
// out. A second Record with a take that was never saved keeps it in the
// folder first, and a new take begins. Play plays and stops.
class RecorderMidiButtonsTest final : public mp::test::Test {
public:
  RecorderMidiButtonsTest() : Test("functional.audio.recorder-midi-buttons", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::MidiMap::isConsoleTarget(mp::MidiTargetKind::RecorderRecord) &&
                 mp::MidiMap::isConsoleTarget(mp::MidiTargetKind::RecorderPlay),
             "kept for every organ");

    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    proc.midiMap().removeBindings([](const mp::MidiBinding&) { return true; });
    auto button = [&](int cc, mp::MidiTargetKind kind) {
      mp::MidiBinding b;
      b.source.kind = mp::MidiSourceKind::ControlChange;
      b.source.channel = 16;
      b.source.number = cc;
      b.targetKind = kind;
      b.trigger = mp::MidiTrigger::Momentary;
      b.latching = false;
      proc.midiMap().setBindingsFor(kind, 0, {b});
    };
    button(20, mp::MidiTargetKind::RecorderRecord);
    button(21, mp::MidiTargetKind::RecorderPlay);
    const std::string text = proc.midiMap().toText();
    MP_CHECK(text.find("recorder-record") != std::string::npos && text.find("recorder-play") != std::string::npos,
             "the map file names them recorder-record and recorder-play");
    mp::MidiMap reread;
    MP_CHECK(reread.fromText(text) && reread.bindingsFor(mp::MidiTargetKind::RecorderRecord, 0).size() == 1,
             "and reads them back");
    auto press = [&](int cc) {
      proc.pushMidi(kConsole, juce::MidiMessage::controllerEvent(16, cc, 127));
      render(proc);
      proc.pushMidi(kConsole, juce::MidiMessage::controllerEvent(16, cc, 0));
      render(proc);
    };
    const juce::File folder =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-recorder-buttons");
    folder.deleteRecursively();

    press(20);
    MP_CHECK(!proc.recorder().isRecording(), "the audio thread only raises the request");
    const int asked = proc.takeRecorderRequest();
    MP_CHECK(asked == 1, "Record is asked for once, on the press");
    MP_CHECK(proc.takeRecorderRequest() == 0, "and taken");
    proc.pressRecorderButton(asked == 1, folder);
    MP_CHECK(proc.recorder().isRecording(), "Record: recording");
    proc.pushMidi(kConsole, juce::MidiMessage::noteOn(1, 48, 100.0f));
    render(proc);
    for (int i = 0; i < 200; ++i) render(proc);  // a take of about a second
    proc.pushMidi(kConsole, juce::MidiMessage::noteOff(1, 48));
    render(proc);
    press(20);
    proc.pressRecorderButton(proc.takeRecorderRequest() == 1, folder);
    MP_CHECK(!proc.recorder().isRecording() && proc.recorder().hasUnsavedTake(),
             "Record again: stopped, with a take never saved");

    press(20);
    const juce::File kept = proc.pressRecorderButton(proc.takeRecorderRequest() == 1, folder);
    MP_CHECK(kept.existsAsFile() && kept.isAChildOf(folder) && kept.hasFileExtension(".mid"),
             "a third press keeps the take in the folder: " + kept.getFullPathName().toStdString());
    MP_CHECK(proc.recorder().isRecording(), "and a new take begins");
    mp::MidiRecorder back;
    back.prepare(48000.0);
    MP_CHECK(back.loadFromFile(kept) && back.eventCount() >= 1, "the kept file holds the note");
    {
      juce::FileInputStream in(kept);
      juce::MidiFile file;
      int buttons = 0;
      if (in.openedOk() && file.readFrom(in))
        for (int t = 0; t < file.getNumTracks(); ++t)
          for (const auto* e : *file.getTrack(t))
            if (e->message.isControllerOfType(20)) ++buttons;
      MP_CHECK(buttons == 0, "and none of the Record button's presses: " + std::to_string(buttons));
    }
    proc.pressRecorderButton(true, folder);  // stop the new, empty take

    // Play, and Play again.
    proc.recorder().clear();
    proc.recorder().loadFromFile(kept);
    press(21);
    proc.pressRecorderButton(proc.takeRecorderRequest() == 1, folder);
    MP_CHECK(proc.recorder().isPlaying(), "Play: playing");
    press(21);
    proc.pressRecorderButton(proc.takeRecorderRequest() == 1, folder);
    MP_CHECK(!proc.recorder().isPlaying(), "Play again: stopped");
    folder.deleteRecursively();
  }
};
static RecorderMidiButtonsTest g_recorderButtons;

// The take saved for a sequencer (5.2): a track per keyboard, the console's
// own messages left out, every note-on with its note-off, the stops as
// markers, and times from the first note exact to the tick. The plain save
// of the same take is the recorder's own and keeps everything.
class RecorderSequencerFileTest final : public mp::test::Test {
public:
  RecorderSequencerFileTest() : Test("functional.audio.recorder-sequencer-file", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(juce::File(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml"), 0, true).ok,
             "the organ loads");
    proc.clearChannelAssignments();
    proc.midiMap().removeBindings([](const mp::MidiBinding&) { return true; });
    proc.setKeyboardForChannel(1, 701, kConsole, true);
    MP_CHECK(proc.keyboardForChannel(5, kConsole) == 0, "the channel 5 plays no keyboard");
    mp::MidiBinding stop;
    stop.source.kind = mp::MidiSourceKind::ControlChange;
    stop.source.channel = 16;
    stop.source.number = 30;
    stop.targetKind = mp::MidiTargetKind::Switch;
    stop.targetId = 1101;
    stop.trigger = mp::MidiTrigger::EngageOnly;
    proc.midiMap().setBindingsFor(mp::MidiTargetKind::Switch, 1101, {stop});
    mp::MidiBinding piston = stop;
    piston.source.kind = mp::MidiSourceKind::ProgramChange;
    piston.source.number = 3;
    piston.targetKind = mp::MidiTargetKind::RecorderPlay;
    piston.targetId = 0;
    piston.trigger = mp::MidiTrigger::Momentary;
    proc.midiMap().setBindingsFor(mp::MidiTargetKind::RecorderPlay, 0, {piston});

    // The take, a block at a time: 300 blocks of 256 at 48 kHz is 1.6 s,
    // 3072 ticks.
    auto at = [&](std::initializer_list<juce::MidiMessage> messages) {
      for (const auto& m : messages) proc.pushMidi(kConsole, m);
      render(proc);
      for (int i = 1; i < 300; ++i) render(proc);
    };
    const juce::uint8 sysex[] = {0x7d, 0x01, 0x02};
    proc.recorder().startRecording();
    at({juce::MidiMessage::createSysExMessage(sysex, 3), juce::MidiMessage::midiClock()});
    at({juce::MidiMessage::controllerEvent(16, 30, 127), juce::MidiMessage::noteOn(1, 60, 100.0f),
        juce::MidiMessage::noteOn(5, 40, 90.0f),
        juce::MidiMessage(0xfe), juce::MidiMessage::programChange(16, 3)});
    at({juce::MidiMessage::noteOn(1, 60, 100.0f), juce::MidiMessage::noteOff(1, 72)});
    at({juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(0)), juce::MidiMessage::noteOn(1, 64, 80.0f)});
    proc.recorder().stopRecording();
    const auto take = proc.recorder().takeEvents();
    MP_CHECK(take.size() >= 8, "the take holds the messages: " + std::to_string(take.size()));

    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-recorder-seq");
    dir.deleteRecursively();
    const juce::File seq = dir.getChildFile("take-seq.mid"), plain = dir.getChildFile("take.mid");
    MP_CHECK(proc.saveTakeForSequencer(seq, true), "saved for a sequencer");
    MP_CHECK(proc.recorder().saveToFile(plain), "and saved plain");
    juce::MidiFile file;
    {
      juce::FileInputStream in(seq);
      MP_CHECK(in.openedOk() && file.readFrom(in), "the file reads back");
    }
    MP_CHECK(file.getTimeFormat() == 960, "960 ticks a quarter");
    MP_CHECK(file.getNumTracks() == 3, "the first track, Manual I and Other: " + std::to_string(file.getNumTracks()));
    auto trackName = [&](int t) {
      for (const auto* e : *file.getTrack(t))
        if (e->message.isTrackNameEvent()) return e->message.getTextFromTextMetaEvent();
      return juce::String();
    };
    MP_CHECK(trackName(1) == "Manual I" && trackName(2) == "Other",
             "the tracks are named: " + trackName(1).toStdString() + ", " + trackName(2).toStdString());

    int dropped = 0, ons = 0, offs = 0, unmatched = 0, tempo = 0;
    juce::StringArray markers;
    double first = 1e9;
    std::vector<std::pair<int, double>> manual;  // note, tick of each note-on on Manual I
    std::vector<std::pair<int, double>> manualOff;
    for (int t = 0; t < file.getNumTracks(); ++t)
      for (const auto* e : *file.getTrack(t)) {
        const auto& m = e->message;
        if (m.isSysEx() || m.isMidiClock() || m.isActiveSense() || m.isProgramChange() || m.isController()) ++dropped;
        if (m.isTempoMetaEvent()) ++tempo;
        if (m.isTextMetaEvent() && m.getMetaEventType() == 6) markers.add(m.getTextFromTextMetaEvent());
        if (m.isNoteOn()) {
          ++ons;
          if (e->noteOffObject == nullptr) ++unmatched;
          if (t == 1) manual.emplace_back(m.getNoteNumber(), m.getTimeStamp());
        }
        if (m.isNoteOff()) {
          ++offs;
          if (t == 1) manualOff.emplace_back(m.getNoteNumber(), m.getTimeStamp());
        }
        if (!m.isMetaEvent()) first = std::min(first, m.getTimeStamp());
      }
    MP_CHECK(dropped == 0, "no console messages left: " + std::to_string(dropped));
    MP_CHECK(tempo == 1, "one tempo");
    MP_CHECK(ons == 4 && offs == 4 && unmatched == 0,
             "every note-on has its note-off: " + std::to_string(ons) + "/" + std::to_string(offs) + "/" +
                 std::to_string(unmatched));
    MP_CHECK(markers.size() == 1 && markers[0].startsWith("Principal 8"),
             "the stop is a marker with its name: " + markers.joinIntoString("|").toStdString());
    MP_CHECK(first == 0.0, "the take starts at 0: " + std::to_string(first));
    // Manual I: 60 at 0, 60 again at 3072 (the first let go there), 64 at
    // 6144, held to the end; the stray note-off for 72 is gone.
    std::sort(manual.begin(), manual.end(), [](auto a, auto b) { return a.second < b.second; });
    MP_CHECK(manual.size() == 3 && manual[0] == std::make_pair(60, 0.0) &&
                 manual[1] == std::make_pair(60, 3072.0) && manual[2] == std::make_pair(64, 6144.0),
             "the notes on Manual I, exact to the tick");
    bool stray = false;
    for (const auto& [note, tick] : manualOff) stray = stray || note == 72;
    MP_CHECK(!stray && manualOff.size() == 3, "the note-off for a key never down is left out");
    bool heldToEnd = false;
    for (const auto& [note, tick] : manualOff) heldToEnd = heldToEnd || (note == 64 && tick == 9216.0);
    MP_CHECK(heldToEnd, "a key still down is let go where the take ends");

    // The plain save, the recorder's own: the notes and the stop's controller
    // as they were played, the ones the other file pairs or turns into a marker.
    mp::MidiRecorder back;
    back.prepare(48000.0);
    MP_CHECK(back.loadFromFile(plain) && back.eventCount() >= 7,
             "the plain save keeps the take as played: " + std::to_string(back.eventCount()) + " of " +
                 std::to_string(proc.recorder().eventCount()));
    MP_CHECK(proc.saveTakeForSequencer(seq, false), "saved without markers");
    {
      juce::FileInputStream in(seq);
      juce::MidiFile again;
      int marks = 0;
      if (in.openedOk() && again.readFrom(in))
        for (const auto* e : *again.getTrack(0))
          if (e->message.isTextMetaEvent() && e->message.getMetaEventType() == 6) ++marks;
      MP_CHECK(marks == 0, "and then it has none");
    }
    dir.deleteRecursively();
  }
};
static RecorderSequencerFileTest g_recorderSequencer;

// The consoles in the file (E2): a track per console, named with its Device
// Name event, read back to the console of that name. Two consoles on the same
// channel each play their own keyboard again from the file.
class RecorderDevicesInTheFileTest final : public mp::test::Test {
public:
  RecorderDevicesInTheFileTest()
      : Test("functional.audio.recorder-devices-in-the-file", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    proc.clearChannelAssignments();
    proc.midiMap().removeBindings([](const mp::MidiBinding&) { return true; });
    const Id pedal = proc.keyboardForChannel(1, 0);
    const Id manual = proc.keyboardForChannel(2, 0);
    const int a = proc.midiMap().devices().claim("Console A");
    const int b = proc.midiMap().devices().claim("Console B");
    MP_CHECK(a != 0 && b != 0 && a != b, "two consoles");
    proc.setKeyboardForChannel(1, manual, a, true);
    proc.setKeyboardForChannel(1, pedal, b, true);
    MP_CHECK(proc.keyboardForChannel(1, a) == manual && proc.keyboardForChannel(1, b) == pedal,
             "the channel 1 of each plays its own keyboard");

    proc.recorder().startRecording();
    proc.pushMidi(a, juce::MidiMessage::noteOn(1, 48, 100.0f));
    proc.pushMidi(b, juce::MidiMessage::noteOn(1, 50, 100.0f));
    render(proc);
    proc.recorder().stopRecording();
    const juce::File file =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-recorder-devices.mid");
    MP_CHECK(proc.saveTake(file), "the take is saved");
    {
      juce::FileInputStream in(file);
      juce::MidiFile mf;
      juce::StringArray names;
      if (in.openedOk() && mf.readFrom(in))
        for (int t = 0; t < mf.getNumTracks(); ++t)
          for (const auto* e : *mf.getTrack(t))
            if (e->message.isMetaEvent() && e->message.getMetaEventType() == 9)
              names.add(e->message.getTextFromTextMetaEvent());
      MP_CHECK(mf.getNumTracks() == 3 && names.size() == 2 && names.contains("Console A") && names.contains("Console B"),
               "the tempo track and one per console, named: " + names.joinIntoString("|").toStdString());
    }

    proc.recorder().clear();
    MP_CHECK(proc.loadTake(file), "and read back");
    int fromA = 0, fromB = 0;
    for (const auto& e : proc.recorder().takeEvents()) {
      if (e.message.isNoteOn() && e.message.getNoteNumber() == 48 && e.deviceId == a) ++fromA;
      if (e.message.isNoteOn() && e.message.getNoteNumber() == 50 && e.deviceId == b) ++fromB;
    }
    MP_CHECK(fromA == 1 && fromB == 1, "each note with the console it was played on");
    proc.recorder().startPlayback();
    render(proc);
    MP_CHECK(proc.keyDownOnKeyboard(manual, 48) && proc.keyDownOnKeyboard(pedal, 50),
             "played back, each on its own keyboard");
    MP_CHECK(!proc.keyDownOnKeyboard(pedal, 48) && !proc.keyDownOnKeyboard(manual, 50), "and on no other");
    file.deleteFile();
  }
};
static RecorderDevicesInTheFileTest g_recorderDevices;

// Everything played, kept (E2): a take begins with the first note, ends after
// the silence, and is written to a dated file the folder's index lists; the
// take in progress is written when asked, as on quitting.
class MidiArchiveTest final : public mp::test::Test {
public:
  MidiArchiveTest() : Test("functional.audio.midi-archive", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    proc.midiMap().removeBindings([](const mp::MidiBinding&) { return true; });
    const juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-archive");
    folder.deleteRecursively();
    proc.setArchiveFolderForTests(folder);
    mp::MasterpieceProcessor::ArchiveSettings on;
    on.on = true;
    on.silenceSeconds = 10;
    proc.setArchiveSettings(on, false);
    auto seconds = [&](double s) {
      for (int i = 0; i < static_cast<int>(s * 48000.0 / 256.0); ++i) render(proc);
    };

    proc.pushMidi(kConsole, juce::MidiMessage::controllerEvent(1, 7, 100));
    render(proc);
    MP_CHECK(proc.serviceArchive(true) == juce::File(), "a controller alone begins no take");
    seconds(1.0);
    proc.pushMidi(kConsole, juce::MidiMessage::noteOn(1, 48, 100.0f));
    render(proc);
    seconds(0.5);
    proc.pushMidi(kConsole, juce::MidiMessage::noteOff(1, 48));
    render(proc);
    seconds(5.0);
    MP_CHECK(proc.serviceArchive() == juce::File(), "after 5 s of silence the take goes on");
    seconds(5.5);
    const juce::File file = proc.serviceArchive();
    MP_CHECK(file.existsAsFile() && file.isAChildOf(folder) && file.hasFileExtension(".mid"),
             "after 10 s it is written: " + file.getFullPathName().toStdString());
    MP_CHECK(file.getParentDirectory().getFileName() == juce::Time::getCurrentTime().formatted("%Y-%m-%d"),
             "in the day's folder");
    MP_CHECK(folder.getChildFile(mp::MidiArchive::kIndexName).loadFileAsString().contains(file.getFileName()),
             "and listed in the index");
    mp::MidiRecorder back;
    back.prepare(48000.0);
    MP_CHECK(back.loadFromFile(file) && back.eventCount() == 2, "the note and its release");
    const auto events = back.takeEvents();
    MP_CHECK(!events.empty() && events.front().seconds == 0.0 && std::abs(events.back().seconds - 0.5) < 0.01,
             "from 0, the release half a second on");
    MP_CHECK(proc.serviceArchive() == juce::File(), "and nothing more until a note");

    proc.pushMidi(kConsole, juce::MidiMessage::noteOn(1, 50, 100.0f));
    render(proc);
    const juce::File quit = proc.serviceArchive(true);
    MP_CHECK(quit.existsAsFile() && quit != file, "the take in progress is written when asked, as on quitting");
    proc.setArchiveSettings({}, false);
    folder.deleteRecursively();
  }
};
static MidiArchiveTest g_midiArchive;

// The archive's limits: the oldest listed files go first, for age and then
// for size; a file the index does not list, a listed file that is not a
// `.mid`, one outside the folder and the newest are never deleted.
class MidiArchiveLimitsTest final : public mp::test::Test {
public:
  MidiArchiveLimitsTest() : Test("functional.audio.midi-archive-limits", Category::Functional) {}
  void run() override {
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File folder = temp.getChildFile("mp-archive-limits");
    const juce::File outside = temp.getChildFile("mp-archive-outside.mid");
    folder.deleteRecursively();
    folder.createDirectory();
    const juce::Time now = juce::Time::getCurrentTime();
    const juce::int64 day = 24LL * 60 * 60 * 1000;
    juce::String index;
    auto file = [&](const juce::String& rel, int ageDays, bool listed) {
      const juce::File f = rel.startsWith("..") ? outside : folder.getChildFile(rel);
      f.getParentDirectory().createDirectory();
      juce::MemoryBlock bytes(1000, true);
      f.replaceWithData(bytes.getData(), bytes.getSize());
      if (listed) index << juce::String(now.toMilliseconds() - ageDays * day) << "\t" << rel << "\n";
      return f;
    };
    const auto a = file("2026-01-01/a.mid", 100, true);
    const auto mine = file("2026-01-01/mine.mid", 300, false);
    const auto b = file("2026-06-01/b.mid", 50, true);
    const auto c = file("2026-09-01/c.mid", 20, true);
    const auto d = file("2026-10-01/d.mid", 1, true);
    const auto notes = file("notes.txt", 200, true);
    file("../mp-archive-outside.mid", 300, true);
    folder.getChildFile(mp::MidiArchive::kIndexName).replaceWithText(index);

    MP_CHECK(mp::MidiArchive::prune(folder, 1000000000, 90, now) == 1, "the age: one file older than 90 days");
    MP_CHECK(!a.existsAsFile() && mine.existsAsFile() && a.getParentDirectory().isDirectory(),
             "the old listed file goes; the unlisted one beside it and its folder stay");
    MP_CHECK(notes.existsAsFile() && outside.existsAsFile(), "a listed file that is not a take, and one outside, stay");
    MP_CHECK(mp::MidiArchive::prune(folder, 2500, 90, now) == 1, "the size: the oldest goes until under 2500 bytes");
    MP_CHECK(!b.existsAsFile() && !b.getParentDirectory().exists() && c.existsAsFile() && d.existsAsFile(),
             "b went, and its emptied day folder with it");
    MP_CHECK(mp::MidiArchive::prune(folder, 10, 90, now) == 1 && !c.existsAsFile() && d.existsAsFile(),
             "the newest is never deleted");
    const auto left = folder.getChildFile(mp::MidiArchive::kIndexName).loadFileAsString();
    MP_CHECK(left.contains("d.mid") && !left.contains("c.mid") && !left.contains("a.mid"),
             "the index lists what is left");
    folder.deleteRecursively();
    outside.deleteFile();
  }
};
static MidiArchiveLimitsTest g_midiArchiveLimits;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
