// A send goes to the MIDI output it names (#1001). Two pad controllers of one
// model, each lighting its own stops, both lit every stop: every send went to
// every open output.
#include "MpTest.h"

#include "../src/mp_control/MidiMap.h"

#include <string>
#include <vector>

namespace {

using mp::test::Category;

class MidiSendDeviceRulesTest final : public mp::test::Test {
public:
  MidiSendDeviceRulesTest() : Test("functional.midi.send-device-rules", Category::Functional) {}

  void run() override {
    const std::vector<std::string> labels{"Launchpad Mini (USB 1.1)", "Launchpad Mini (USB 1.2)", "Organ Out"};
    const std::vector<std::string> names{"Launchpad Mini", "Launchpad Mini", "Organ Out"};
    MP_CHECK(mp::outputForSend("", labels, names) == -1, "no device: every output");
    MP_CHECK(mp::outputForSend("Launchpad Mini (USB 1.2)", labels, names) == 1, "a label names its port");
    MP_CHECK(mp::outputForSend("Launchpad Mini", labels, names) == 0, "a bare name, the first port of that name");
    MP_CHECK(mp::outputForSend("Launchpad X", labels, names) == -1, "a port not open is none");

    MP_CHECK(mp::sameMidiDevice("usb-0000:00:14.0-1.2/0", "usb-0000:00:14.0-1.2/0"),
             "Linux: the input and the output of one USB port");
    MP_CHECK(!mp::sameMidiDevice("usb-0000:00:14.0-1.2/0", "usb-0000:00:14.0-1.1/0"), "another socket is another");
    MP_CHECK(mp::sameMidiDevice("\\\\?\\usb#vid_1235&pid_0113#6&1a&0&2#{6dc23320-ab33-4ce4-80d4-bbb3ebbf2814}",
                                "\\\\?\\usb#vid_1235&pid_0113#6&1a&0&2#{3705dc2b-17a7-4452-98ce-bf12c6f48a0b}"),
             "Windows: one device's input and output interfaces");
    MP_CHECK(!mp::sameMidiDevice("\\\\?\\usb#vid_1235&pid_0113#6&1a&0&2#{6dc23320}",
                                 "\\\\?\\usb#vid_1235&pid_0113#6&1a&0&3#{3705dc2b}"),
             "and its twin's are another");
    MP_CHECK(!mp::sameMidiDevice("", ""), "no identifier names nothing");

    // The output of an input device, as the lamps and a new send take it.
    const std::vector<std::string> lasting{"usb-0000:00:14.0-1.1/0", "usb-0000:00:14.0-1.2/0", ""};
    MP_CHECK(mp::outputForInput("Launchpad Mini-1", "usb-0000:00:14.0-1.2/0", names, lasting) == 1,
             "the second pad, by its socket");
    MP_CHECK(mp::outputForInput("Launchpad Mini", "usb-0000:00:14.0-1.1/0", names, lasting) == 0,
             "the first, by its own");
    MP_CHECK(mp::outputForInput("Launchpad Mini", "", names, lasting) == -1,
             "with no identifier, a name two ports share tells neither");
    MP_CHECK(mp::outputForInput("Organ Out", "", names, lasting) == 2, "a name one port has tells that one");
    MP_CHECK(mp::outputForInput("Keys", "", names, lasting) == -1, "a device with no output has none");

    // Kept in the organ's map, and read back.
    mp::MidiMap map;
    mp::MidiSend s;
    s.targetKind = mp::MidiTargetKind::Switch;
    s.targetId = 7;
    s.kind = mp::MidiSourceKind::Note;
    s.channel = 1;
    s.number = 36;
    s.low = 5;
    s.high = 21;
    s.device = "Launchpad Mini (USB 1.2)";
    mp::MidiSend every = s;
    every.targetId = 8;
    every.device.clear();
    mp::MidiSend program = s;
    program.targetId = 9;
    program.kind = mp::MidiSourceKind::ProgramChange;
    program.offProgram = 4;
    program.device = "Pads_2";
    map.setSendsFor(mp::MidiTargetKind::Switch, 7, {s});
    map.setSendsFor(mp::MidiTargetKind::Switch, 8, {every});
    map.setSendsFor(mp::MidiTargetKind::Switch, 9, {program});
    const std::string text = map.toText();
    MP_CHECK(text.find("| device=Launchpad_Mini_(USB_1.2)") != std::string::npos,
             "the device is written after a bar: " + text);
    mp::MidiMap back;
    MP_CHECK(back.fromText(text), "the map reads back");
    const auto sends = back.sends();
    MP_CHECK(sends.size() == 3, "three sends: " + std::to_string(sends.size()));
    if (sends.size() == 3) {
      for (const auto& r : sends) {
        const mp::MidiSend& want = r.targetId == 7 ? s : r.targetId == 8 ? every : program;
        MP_CHECK(r == want, "send " + std::to_string(r.targetId) + " reads back whole, device '" + r.device + "'");
      }
    }
    // A line from before the device: every output.
    mp::MidiMap old;
    MP_CHECK(old.fromText("send switch 7 note 1 36 5 21\n") && old.sends().size() == 1 &&
                 old.sends()[0].device.empty(),
             "a send saved before devices goes to every output");
  }
};

MidiSendDeviceRulesTest g_midiSendDeviceRules;

}  // namespace

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

namespace {

// Two pads of one model and a console with no output: each pad's send and
// lamp reach that pad alone.
class MidiSendToOwnDeviceTest final : public mp::test::Test {
public:
  MidiSendToOwnDeviceTest() : Test("functional.midi.send-to-own-device", Category::Functional) {}

  void run() override {
    const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
    const juce::File global = mp::dataFolder().getChildFile("settings.mpglobal");
    const bool had = global.existsAsFile();
    const juce::String before = had ? global.loadFileAsString() : juce::String();
    {
      mp::MasterpieceProcessor proc;
      proc.loadOrgan(odf, 0, false);
      proc.prepareToPlay(48000.0, 256);
      const int padA = proc.registerMidiDevice("Pad A");
      const int padB = proc.registerMidiDevice("Pad B");
      const int keys = proc.registerMidiDevice("Keys");
      proc.setMidiOutputPortsForTest({{"Pad A", "Pad A"}, {"Pad B", "Pad B"}});

      std::vector<std::pair<std::string, juce::MidiMessage>> sent;  // port, message; "" for every port
      proc.outgoingProbe = [&sent](const juce::MidiBuffer& b) {
        for (const auto meta : b) sent.emplace_back("", meta.getMessage());
      };
      proc.portProbe = [&sent](const std::string& port, const juce::MidiBuffer& b) {
        for (const auto meta : b) sent.emplace_back(port, meta.getMessage());
      };
      juce::AudioBuffer<float> buf(2, 256);
      auto block = [&] {
        juce::MidiBuffer m;
        buf.clear();
        proc.processBlock(buf, m);
      };
      // Where note `n` went with velocity `v` (0 for its off), as "port,port".
      auto where = [&sent](int n, int v) {
        std::string at;
        for (const auto& [port, m] : sent)
          if (m.isNoteOnOrOff() && m.getNoteNumber() == n &&
              (v == 0 ? (m.isNoteOff() || m.getVelocity() == 0) : (m.isNoteOn() && m.getVelocity() == v)))
            at += (at.empty() ? "" : ",") + (port.empty() ? std::string("every") : port);
        return at;
      };

      // Principal 8 learned on a pad's note, and lit by a send of its own or
      // by the echo of what it was learned on.
      auto learn = [&proc](int device, int note) {
        mp::MidiBinding b;
        b.source.kind = mp::MidiSourceKind::Note;
        b.source.channel = 1;
        b.source.number = note;
        b.source.deviceId = device;
        b.targetKind = mp::MidiTargetKind::Switch;
        b.targetId = 1101;
        b.trigger = mp::MidiTrigger::Toggle;
        b.latching = true;
        proc.midiMap().setBindingsFor(mp::MidiTargetKind::Switch, 1101, {b});
      };
      auto toggle = [&](bool on) {
        proc.setSwitchEngaged(1101, on);
        block();
      };
      mp::MidiSend lamp;
      lamp.targetKind = mp::MidiTargetKind::Switch;
      lamp.targetId = 1101;
      lamp.kind = mp::MidiSourceKind::Note;
      lamp.channel = 1;
      lamp.number = 36;
      lamp.low = 5;
      lamp.high = 21;
      lamp.device = "Pad B";
      learn(padB, 36);
      proc.midiMap().setSendsFor(mp::MidiTargetKind::Switch, 1101, {lamp});
      proc.setMidiFeedbackEnabled(true);
      block();
      sent.clear();

      toggle(true);
      MP_CHECK(where(36, 21) == "Pad B", "the send lights pad B alone: '" + where(36, 21) + "'");
      sent.clear();
      toggle(false);
      MP_CHECK(where(36, 5) == "Pad B", "and puts it out there alone: '" + where(36, 5) + "'");
      sent.clear();

      // A send that names no device goes to every output, as before.
      lamp.device.clear();
      proc.midiMap().setSendsFor(mp::MidiTargetKind::Switch, 1101, {lamp});
      toggle(true);
      MP_CHECK(where(36, 21) == "every", "a send with no device: every output: '" + where(36, 21) + "'");
      sent.clear();
      // One naming an output that is not open goes nowhere.
      lamp.device = "Pad C";
      proc.midiMap().setSendsFor(mp::MidiTargetKind::Switch, 1101, {lamp});
      toggle(false);
      MP_CHECK(where(36, 5).empty(), "a send to an output not open is not sent: '" + where(36, 5) + "'");
      sent.clear();

      // No send: the lamp is the echo of the binding, to its own pad.
      proc.midiMap().setSendsFor(mp::MidiTargetKind::Switch, 1101, {});
      learn(padA, 37);
      toggle(true);
      MP_CHECK(where(37, 127) == "Pad A", "a lamp echoed from pad A's binding goes to pad A: '" + where(37, 127) + "'");
      sent.clear();
      learn(keys, 38);
      toggle(false);
      MP_CHECK(where(38, 0) == "every", "a console with no output of its own: every output, as before: '" +
                                            where(38, 0) + "'");
      sent.clear();

      // The Send tab's test buttons, to the event's own output.
      proc.sendMidiNow({0x90, 40, 99}, "Pad A");
      MP_CHECK(where(40, 99) == "Pad A", "a test send reaches its own output: '" + where(40, 99) + "'");

      proc.outgoingProbe = nullptr;
      proc.portProbe = nullptr;
      proc.releaseResources();
    }
    if (had) global.replaceWithText(before);
    else global.deleteFile();
  }
};

MidiSendToOwnDeviceTest g_midiSendToOwnDevice;

}  // namespace

#endif
