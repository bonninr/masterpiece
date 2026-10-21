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

#include <string>

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

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
