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

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
