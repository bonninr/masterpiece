// The reverb's wet level from a MIDI knob (Discussion #717): a controller
// learned for it sets the Reverb tab's Wet across its whole range, and the
// mapping is kept in the map file and for every organ.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

class ReverbWetMidiTest final : public mp::test::Test {
public:
  ReverbWetMidiTest() : Test("functional.midi.reverb-wet-knob", Category::Functional) {}
  void run() override {
    MP_CHECK(mp::MidiMap::isContinuousTarget(mp::MidiTargetKind::ReverbWet), "moved like a shoe");
    MP_CHECK(mp::MidiMap::isConsoleTarget(mp::MidiTargetKind::ReverbWet), "kept for every organ");

    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    const float was = proc.convolver().mix();

    mp::MidiBinding knob;
    knob.source.kind = mp::MidiSourceKind::ControlChange;
    knob.source.channel = 1;
    knob.source.number = 91;
    knob.targetKind = mp::MidiTargetKind::ReverbWet;
    knob.targetId = 0;
    proc.midiMap().bind(knob);

    juce::AudioBuffer<float> buf(2, 256);
    auto send = [&](int value) {
      juce::MidiBuffer m;
      m.addEvent(juce::MidiMessage::controllerEvent(1, 91, value), 0);
      buf.clear();
      proc.processBlock(buf, m);
      return proc.convolver().mix();
    };
    MP_CHECK(std::fabs(send(127) - 1.0f) < 1.0e-6f, "the knob at the top: all wet");
    MP_CHECK(send(0) == 0.0f, "at the bottom: dry, got " + std::to_string(proc.convolver().mix()));
    const float half = send(64);
    MP_CHECK(std::fabs(half - 64.0f / 127.0f) < 1.0e-6f, "halfway: " + std::to_string(half));

    // Written to the map file under its own word, and read back.
    const std::string text = proc.midiMap().toText();
    MP_CHECK(text.find("reverb-wet") != std::string::npos, "named in the map file");
    mp::MidiMap again;
    MP_CHECK(again.fromText(text) && again.bindingFor(mp::MidiTargetKind::ReverbWet, 0) != nullptr,
             "read back from the map file");

    proc.convolver().setMix(was);
  }
};
static ReverbWetMidiTest g_reverbWetMidi;

}  // namespace

#endif
