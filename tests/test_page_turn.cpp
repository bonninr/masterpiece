// Page turns for a score app: the bytes each kind of message makes, and a
// piston learned for a page turn reaching the output stream.
#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#if MP_ENABLE_API
#include "../src/mp_api/ControlModel.h"
#endif

#include <string>

namespace {

using mp::test::Category;

class ScorePageBytesTest final : public mp::test::Test {
public:
  ScorePageBytesTest() : Test("functional.midi.score-page-bytes", Category::Functional) {}
  void run() override {
    mp::RawMidi raw[2];
    mp::ScorePageMessage m;  // notes 127 and 126 on channel 16
    int n = mp::scorePageTurn(m, true, raw);
    MP_CHECK(n == 2 && raw[0].bytes[0] == 0x9F && raw[0].bytes[1] == 127 && raw[0].bytes[2] == 127,
             "forward: a note struck on channel 16");
    MP_CHECK(raw[1].bytes[0] == 0x8F && raw[1].bytes[1] == 127, "and let go");
    n = mp::scorePageTurn(m, false, raw);
    MP_CHECK(n == 2 && raw[0].bytes[1] == 126, "back: the other note");

    m.kind = mp::ScorePageMessage::Controller;
    m.channel = 1;
    m.forward = 20;
    n = mp::scorePageTurn(m, true, raw);
    MP_CHECK(n == 2 && raw[0].bytes[0] == 0xB0 && raw[0].bytes[1] == 20 && raw[0].bytes[2] == 127 &&
                 raw[1].bytes[0] == 0xB0 && raw[1].bytes[2] == 0,
             "a controller goes to 127 and back to 0");

    m.kind = mp::ScorePageMessage::Program;
    m.back = 5;
    n = mp::scorePageTurn(m, false, raw);
    MP_CHECK(n == 1 && raw[0].bytes[0] == 0xC0 && raw[0].bytes[1] == 5 && raw[0].size == 2,
             "a program change, two bytes");
  }
};
static ScorePageBytesTest g_bytes;

class ScorePagePistonTest final : public mp::test::Test {
public:
  ScorePagePistonTest() : Test("functional.midi.score-page-piston", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/m24.expression.Organ_Hauptwerk_xml");
    MP_CHECK(proc.loadOrgan(odf, 0, true).ok, "the organ loads");

    mp::MidiBinding b;
    b.source.kind = mp::MidiSourceKind::Note;
    b.source.channel = 14;
    b.source.number = 60;
    b.targetKind = mp::MidiTargetKind::ScorePageForward;
    b.trigger = mp::MidiTrigger::Momentary;
    proc.midiMap().bind(b);
    b.source.number = 59;
    b.targetKind = mp::MidiTargetKind::ScorePageBack;
    proc.midiMap().bind(b);

    juce::AudioBuffer<float> buf(2, 256);
    auto block = [&](juce::MidiBuffer m) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    const int before = proc.scorePagesTurned();
    juce::MidiBuffer press;
    press.addEvent(juce::MidiMessage::noteOn(14, 60, 0.8f), 0);
    block(press);
    MP_CHECK(proc.scorePagesTurned() == before + 1, "the piston turns a page");
    juce::MidiBuffer release;
    release.addEvent(juce::MidiMessage::noteOff(14, 60), 0);
    block(release);
    MP_CHECK(proc.scorePagesTurned() == before + 1, "letting it go turns none");
    juce::MidiBuffer backPress;
    backPress.addEvent(juce::MidiMessage::noteOn(14, 59, 0.8f), 0);
    block(backPress);
    MP_CHECK(proc.scorePagesTurned() == before + 2, "the back piston too");

    proc.turnScorePage(true);
    block({});
    MP_CHECK(proc.scorePagesTurned() == before + 3, "and a turn asked for from the window");
#if MP_ENABLE_API
    mp::api::ControlModel model(proc);
    MP_CHECK(model.press(mp::api::Type::Command, static_cast<int>(mp::api::Command::ScorePageBack)) ==
                 mp::api::Error::None,
             "the API's command");
    block({});
    MP_CHECK(proc.scorePagesTurned() == before + 4, "turns one");
#endif
  }
};
static ScorePagePistonTest g_piston;

}  // namespace
