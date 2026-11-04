// The Send tab's events, GrandOrgue's set (#766): a note whose off is a Note
// Off, the two parameter controllers, a manual at a fixed velocity, and the
// player's pistons and the setter lit by sends of their own. A general used
// to light only as the note or controller it was learned on, at 127.
#include "MpTest.h"

#include "../src/mp_control/MidiMap.h"

#include <vector>

namespace {

using mp::test::Category;
using K = mp::MidiTargetKind;

mp::MidiSend lamp(K target, mp::MidiSourceKind kind, int number) {
  mp::MidiSend s;
  s.targetKind = target;
  s.kind = kind;
  s.channel = 2;
  s.number = number;
  return s;
}

class MidiSendEventsTest final : public mp::test::Test {
public:
  MidiSendEventsTest() : Test("functional.midi.send-events", Category::Functional) {}

  void run() override {
    using Bytes = std::vector<uint8_t>;
    auto note = lamp(K::Switch, mp::MidiSourceKind::Note, 36);
    note.high = 45;
    note.low = 3;
    MP_CHECK(mp::MidiMap::sendBytes(note, 1) == Bytes({0x91, 36, 45}), "a note lights at its on velocity");
    MP_CHECK(mp::MidiMap::sendBytes(note, 0) == Bytes({0x91, 36, 3}), "and goes out as a note at the off one");
    note.noteOff = true;
    MP_CHECK(mp::MidiMap::sendBytes(note, 0) == Bytes({0x81, 36, 3}), "or as a Note Off when asked");
    MP_CHECK(mp::MidiMap::sendBytes(note, 1) == Bytes({0x91, 36, 45}), "which leaves its on alone");
    note.low = -1;
    MP_CHECK(mp::MidiMap::sendBytes(note, 0).empty(), "an off of none sends nothing");

    auto nrpn = lamp(K::Switch, mp::MidiSourceKind::ControlChange, 300);
    nrpn.parameter = mp::MidiParameter::NonRegistered;
    MP_CHECK(mp::MidiMap::sendBytes(nrpn, 1) == Bytes({0xB1, 99, 2, 0xB1, 98, 44, 0xB1, 6, 127}),
             "an NRPN names its parameter, then sends the value by data entry");
    auto rpn = nrpn;
    rpn.parameter = mp::MidiParameter::Registered;
    rpn.number = 1;
    MP_CHECK(mp::MidiMap::sendBytes(rpn, 0) == Bytes({0xB1, 101, 0, 0xB1, 100, 1, 0xB1, 6, 0}),
             "an RPN does the same on 101 and 100");
    auto fader = rpn;
    fader.targetKind = K::ContinuousControl;
    fader.low = 0;
    fader.high = 100;
    MP_CHECK(mp::MidiMap::sendBytes(fader, 127).back() == 100, "a control's parameter goes across its range");

    auto hex = lamp(K::Switch, mp::MidiSourceKind::SysEx, 0);
    hex.sysExOn = {0xF0, 0x7D, 0x01, 0xF7, 0xB0, 0x14, 0x7F};
    MP_CHECK(mp::MidiMap::sendBytes(hex, 1) == hex.sysExOn && mp::MidiMap::sendBytes(hex, 0).empty(),
             "a hex send puts out its messages as written");

    auto keys = lamp(K::Keyboard, mp::MidiSourceKind::Note, 0);
    mp::RawMidi out;
    MP_CHECK(mp::MidiMap::keyMessage(keys, 60, 90, true, out) && out.bytes[2] == 90, "a manual sends as played");
    keys.low = 64;
    MP_CHECK(mp::MidiMap::keyMessage(keys, 60, 90, true, out) && out.bytes[2] == 64,
             "or at its fixed velocity");

    for (const K k : {K::Switch, K::PlayerGeneral, K::PlayerDivisional, K::Setter, K::RecorderRecord,
                      K::PedalCombinationsCoupled})
      MP_CHECK(mp::MidiMap::sendsOnOff(k) && mp::MidiMap::hasSendState(k), "a target that lights offers a send");
    MP_CHECK(mp::MidiMap::hasSendState(K::ContinuousControl) && mp::MidiMap::hasSendState(K::Keyboard) &&
                 !mp::MidiMap::sendsOnOff(K::ContinuousControl),
             "a control and a manual send, with no on and off");
    MP_CHECK(!mp::MidiMap::hasSendState(K::ConsoleNextPage) && !mp::MidiMap::hasSendState(K::StepperNext),
             "a button that only acts has nothing to send");

    // Saved and read back, each by its own word.
    mp::MidiMap saved;
    auto general = lamp(K::PlayerGeneral, mp::MidiSourceKind::Note, 40);
    general.noteOff = true;
    general.high = 21;
    saved.setSendsFor(K::PlayerGeneral, 3, {general, nrpn});
    saved.setSendsFor(K::Keyboard, 701, {keys});
    const std::string text = saved.toText();
    MP_CHECK(text.find("send general 3 noteoff ") != std::string::npos &&
                 text.find(" nrpn 2 300 ") != std::string::npos,
             "the file names a Note Off send and an NRPN one");
    mp::MidiMap again;
    MP_CHECK(again.fromText(text), "the file reads back");
    const auto back = again.sendsFor(K::PlayerGeneral, 3);
    MP_CHECK(back.size() == 2 && back[0].noteOff && back[0].kind == mp::MidiSourceKind::Note && back[0].high == 21 &&
                 back[1].parameter == mp::MidiParameter::NonRegistered && back[1].number == 300,
             "and every field comes back");
    MP_CHECK(again.sendsFor(K::Keyboard, 701).size() == 1 && again.sendsFor(K::Keyboard, 701)[0].low == 64,
             "the manual's velocity comes back");
  }
};

MidiSendEventsTest midiSendEventsTest;

}  // namespace

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

namespace {

// The setter's lamp from a send of its own: the colour a pad grid wants, on
// and off, where it used to light only as its binding's note at 127.
class MidiSendSetterLampTest final : public mp::test::Test {
public:
  MidiSendSetterLampTest() : Test("functional.midi.send-setter-lamp", Category::Functional) {}

  void run() override {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-send-setter-lamp");
    dir.deleteRecursively();
    dir.createDirectory();
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
      auto s = lamp(K::Setter, mp::MidiSourceKind::Note, 60);
      s.high = 45;
      s.low = 3;
      s.noteOff = true;
      proc.setObjectSends(K::Setter, 0, {s});
      blocks(2);
      sent.clear();
      proc.setCaptureMode(true);
      blocks(2);
      auto has = [&sent](int status, int number, int value) {
        for (const auto& m : sent)
          if (m.getRawDataSize() == 3 && m.getRawData()[0] == status && m.getRawData()[1] == number &&
              m.getRawData()[2] == value)
            return true;
        return false;
      };
      MP_CHECK(has(0x91, 60, 45), "the setter lights with its own colour, got " + std::to_string(sent.size()));
      sent.clear();
      proc.setCaptureMode(false);
      blocks(2);
      MP_CHECK(has(0x81, 60, 3), "and goes out with its Note Off");
    }
    mp::setDataFolder(previous);
    dir.deleteRecursively();
  }
};

MidiSendSetterLampTest midiSendSetterLampTest;

}  // namespace

#endif
