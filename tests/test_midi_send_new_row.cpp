// A new Send row goes out from its first change (#565). New event used to
// send the row at once, before its message was set, and every change sent
// every lamp in the map again.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <vector>

namespace {

using mp::test::Category;

class MidiSendNewRowTest final : public mp::test::Test {
public:
  MidiSendNewRowTest() : Test("functional.midi.send-new-row-quiet", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-send-new-row");
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
      std::vector<juce::MidiMessage> sent;
      proc.outgoingProbe = [&sent](const juce::MidiBuffer& b) {
        for (const auto meta : b) sent.push_back(meta.getMessage());
      };
      juce::AudioBuffer<float> buf(2, 256);
      auto blocks = [&](int n) {
        for (int i = 0; i < n; ++i) {
          juce::MidiBuffer m;
          buf.clear();
          proc.processBlock(buf, m);
        }
      };
      auto notesSent = [&sent](int number) {
        int n = 0;
        for (const auto& m : sent)
          if ((m.isNoteOn() || m.isNoteOff()) && m.getNoteNumber() == number) ++n;
        return n;
      };
      auto row = [](mp::Id id, int number) {
        mp::MidiSend s;
        s.targetKind = mp::MidiTargetKind::Switch;
        s.targetId = id;
        s.kind = mp::MidiSourceKind::Note;
        s.number = number;
        return s;
      };
      proc.setSwitchEngaged(1101, true);
      proc.setObjectSends(mp::MidiTargetKind::Switch, 1102, {row(1102, 50)});
      blocks(2);
      MP_CHECK(notesSent(50) == 1, "an edited row goes out with its switch's state");

      sent.clear();
      proc.setObjectSends(mp::MidiTargetKind::Switch, 1101, {row(1101, 40)}, true);
      blocks(2);
      MP_CHECK(notesSent(40) == 0, "a new row is not sent, got " + std::to_string(notesSent(40)));
      MP_CHECK(notesSent(50) == 0, "and the other lamps are not sent again");
      MP_CHECK(proc.midiMap().sendsFor(mp::MidiTargetKind::Switch, 1101).size() == 1, "the new row is kept");

      proc.setSwitchEngaged(1101, false);
      blocks(2);
      MP_CHECK(notesSent(40) == 1, "the new row goes out when its switch moves");

      sent.clear();
      proc.setObjectSends(mp::MidiTargetKind::Switch, 1101, {row(1101, 41)});
      blocks(2);
      MP_CHECK(notesSent(41) == 1 && notesSent(50) == 0, "a changed row goes out on its own");
    }
    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};
static MidiSendNewRowTest g_midiSendNewRow;

} // namespace

#endif
