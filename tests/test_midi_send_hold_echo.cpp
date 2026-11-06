// A console that lights its own lamp: its message moves the stop, and the
// Send event that repeats the message would turn the lamp off again (#797).
// The option leaves that one send out. A move from anywhere else still goes
// out, and so does a send on another message.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <vector>

namespace {

using mp::test::Category;

class MidiSendHoldEchoTest final : public mp::test::Test {
public:
  MidiSendHoldEchoTest() : Test("functional.midi.send-hold-echo", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-send-hold-echo");
    dir.deleteRecursively();
    dir.createDirectory();
    // The program's own map files are not this test's to change.
    const auto previous = mp::dataFolder();
    mp::setDataFolder(dir);
    {
      const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      int programs = 0, notes = 0;
      proc.outgoingProbe = [&](const juce::MidiBuffer& b) {
        for (const auto meta : b) {
          const auto m = meta.getMessage();
          if (m.isProgramChange() && m.getProgramChangeNumber() == 7) ++programs;
          if (m.isNoteOn() && m.getNoteNumber() == 50) ++notes;
        }
      };
      juce::AudioBuffer<float> buf(2, 256);
      auto block = [&](juce::MidiBuffer m) {
        buf.clear();
        proc.processBlock(buf, m);
      };
      auto press = [&] {
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage::programChange(1, 7), 0);
        block(m);
        block({});
      };

      mp::MidiBinding b;
      b.source.kind = mp::MidiSourceKind::ProgramChange;
      b.source.channel = 1;
      b.source.number = 7;
      b.targetKind = mp::MidiTargetKind::Switch;
      b.targetId = 1101;
      b.trigger = mp::MidiTrigger::Toggle;
      b.latching = true;
      proc.midiMap().bind(b);
      mp::MidiSend same;
      same.targetKind = mp::MidiTargetKind::Switch;
      same.targetId = 1101;
      same.kind = mp::MidiSourceKind::ProgramChange;
      same.channel = 1;
      same.number = 7;
      same.offProgram = 7;
      auto other = same;
      other.kind = mp::MidiSourceKind::Note;
      other.number = 50;
      other.offProgram = -1;
      block({});  // the first block after a load sends every lamp's state
      proc.setObjectSends(mp::MidiTargetKind::Switch, 1101, {same, other}, true);
      block({});
      programs = notes = 0;

      press();
      MP_CHECK(proc.switchEngaged(1101), "the message draws the stop");
      MP_CHECK(programs == 1, "as it is, the send repeats it, got " + std::to_string(programs));

      proc.setMidiSendsHoldEcho(true);
      programs = notes = 0;
      press();
      MP_CHECK(!proc.switchEngaged(1101), "the message puts the stop off");
      MP_CHECK(programs == 0, "with the option the send stays out, got " + std::to_string(programs));
      press();
      MP_CHECK(proc.switchEngaged(1101) && programs == 0, "both ways");
      MP_CHECK(notes == 1, "a send on another message goes out, got " + std::to_string(notes));

      proc.setSwitchEngaged(1101, false);
      block({});
      MP_CHECK(programs == 1, "a move from the program itself goes out, got " + std::to_string(programs));
      proc.outgoingProbe = nullptr;
    }
    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static MidiSendHoldEchoTest g_midiSendHoldEcho;

} // namespace

#endif
