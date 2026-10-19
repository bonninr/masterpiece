// A console with no screen: the ready lamp, the quit and power-off buttons
// with their safeguards, and a button for one temperament. The power hook
// here only records what it was asked; nothing is ever shut down.
#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <string>
#include <vector>

namespace {

using mp::test::Category;

juce::File fixture() { return juce::File(juce::String(MP_TEST_FIXTURES_DIR) + "/m24.expression.Organ_Hauptwerk_xml"); }

class ReadyLampTest final : public mp::test::Test {
public:
  ReadyLampTest() : Test("functional.midi.ready-lamp", Category::Functional) {}
  void run() override {
    using Proc = mp::MasterpieceProcessor;
    Proc::ReadyLamp lamp;
    mp::RawMidi raw;
    MP_CHECK(!Proc::readyLampMessage(lamp, true, raw), "off by default: nothing is sent");
    lamp.kind = mp::MidiSourceKind::Note;
    lamp.channel = 3;
    lamp.number = 60;
    MP_CHECK(Proc::readyLampMessage(lamp, true, raw) && raw.size == 3 && raw.bytes[0] == 0x92 &&
                 raw.bytes[1] == 60 && raw.bytes[2] == 127,
             "lit: a note on at full velocity");
    MP_CHECK(Proc::readyLampMessage(lamp, false, raw) && raw.bytes[0] == 0x82 && raw.bytes[2] == 0, "dark: its note off");
    lamp.kind = mp::MidiSourceKind::ControlChange;
    lamp.number = 20;
    MP_CHECK(Proc::readyLampMessage(lamp, true, raw) && raw.bytes[0] == 0xB2 && raw.bytes[1] == 20 &&
                 raw.bytes[2] == 127,
             "or a controller at 127");

    Proc proc;
    proc.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buf(2, 512);
    juce::MidiBuffer none;
    buf.clear();
    proc.processBlock(buf, none);
    MP_CHECK(proc.readyLampState() == 0, "no organ: dark");
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    buf.clear();
    proc.processBlock(buf, none);
    MP_CHECK(proc.readyLampState() == 1, "an organ ready: lit");
  }
};
static ReadyLampTest g_lamp;

class PowerButtonsTest final : public mp::test::Test {
public:
  PowerButtonsTest() : Test("functional.midi.power-buttons", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    constexpr int kBlock = 4096;
    constexpr double kRate = 48000.0;
    proc.prepareToPlay(kRate, kBlock);
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    std::vector<bool> asked;  // true for power off, false for quit
    proc.onPowerRequest = [&](bool powerOff) { asked.push_back(powerOff); };

    // Power off on note 100, channel 16, held; quit on program change 5,
    // pressed twice.
    mp::MidiBinding hold;
    hold.source.kind = mp::MidiSourceKind::Note;
    hold.source.channel = 16;
    hold.source.number = 100;
    hold.targetKind = mp::MidiTargetKind::PowerOffComputer;
    hold.trigger = mp::MidiTrigger::Momentary;
    hold.latching = false;
    proc.midiMap().bind(hold);
    mp::MidiBinding twice;
    twice.source.kind = mp::MidiSourceKind::ProgramChange;
    twice.source.channel = 16;
    twice.source.number = 5;
    twice.targetKind = mp::MidiTargetKind::QuitProgram;
    proc.midiMap().bind(twice);

    juce::AudioBuffer<float> buf(2, kBlock);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
      proc.serviceMidiRequests();
    };
    const auto seconds = [&](double s) {
      for (int i = 0; i < static_cast<int>(s * kRate / kBlock) + 1; ++i) block();
    };
    const auto msg = [](const juce::MidiMessage& m) {
      juce::MidiBuffer b;
      b.addEvent(m, 0);
      return b;
    };
    const auto down = msg(juce::MidiMessage::noteOn(16, 100, 1.0f));
    const auto up = msg(juce::MidiMessage::noteOff(16, 100));
    const auto pc = msg(juce::MidiMessage::programChange(16, 5));

    // Before two minutes nothing counts: held five seconds, pressed twice.
    block(down);
    seconds(5.0);
    block(up);
    block(pc);
    block(pc);
    MP_CHECK(asked.empty(), "not armed in the first two minutes");

    seconds(mp::MasterpieceProcessor::kPowerArmSeconds);

    // Held a second and let go: nothing.
    block(down);
    seconds(1.0);
    block(up);
    seconds(4.0);
    MP_CHECK(asked.empty(), "let go before three seconds: nothing happens");

    // Held: the lamp blinks while it is held, then the power off is asked.
    block(down);
    int changes = 0, last = proc.readyLampState();
    for (int i = 0; i < static_cast<int>(2.0 * kRate / kBlock); ++i) {
      block();
      if (proc.readyLampState() != last) ++changes;
      last = proc.readyLampState();
    }
    MP_CHECK(changes >= 4, "the lamp blinks during the hold: " + std::to_string(changes) + " changes");
    MP_CHECK(asked.empty(), "two seconds held: not yet");
    seconds(1.5);
    MP_CHECK(asked.empty(), "three seconds held, shutting down not allowed: refused");
    block(up);
    MP_CHECK(proc.readyLampState() == 1, "and the lamp lit again");

    // Allowed (General settings, Remote), as the control API's powerOff needs:
    // three seconds held asks for power off. The global file this writes is
    // put back as it was.
    const juce::File global = proc.globalSettingsFile();
    const bool hadGlobal = global.existsAsFile();
    juce::MemoryBlock savedGlobal;
    if (hadGlobal) global.loadFileAsData(savedGlobal);
    auto remote = proc.remoteSettings();
    remote.allowPowerOff = true;
    proc.setRemoteSettings(remote);
    block(down);
    seconds(3.5);
    block(up);
    MP_CHECK(asked.size() == 1 && asked[0], "allowed: three seconds held, power off asked");
    remote.allowPowerOff = false;
    proc.setRemoteSettings(remote);
    if (hadGlobal)
      global.replaceWithData(savedGlobal.getData(), savedGlobal.getSize());
    else
      global.deleteFile();

    // A program change has no release: two presses within two seconds.
    block(pc);
    seconds(3.0);
    block(pc);
    MP_CHECK(asked.size() == 1, "two presses three seconds apart: nothing");
    seconds(0.5);
    block(pc);
    MP_CHECK(asked.size() == 2 && !asked[1], "two presses within two seconds: quit asked");

    // One temperament on a button of its own.
    const auto& lib = mp::temperamentLibrary();
    MP_CHECK(lib.size() > 1, "a temperament library");
    const std::string name = lib[1].name;
    mp::MidiBinding temp;
    temp.source.kind = mp::MidiSourceKind::Note;
    temp.source.channel = 16;
    temp.source.number = 90;
    temp.targetKind = mp::MidiTargetKind::Temperament;
    temp.targetId = mp::temperamentTarget(name);
    proc.midiMap().bind(temp);
    block(msg(juce::MidiMessage::noteOn(16, 90, 1.0f)));
    MP_CHECK(proc.temperamentChoice() == name, "the button chose " + name + ", got " + proc.temperamentChoice());
    MP_CHECK(mp::temperamentTarget("") == 0 && mp::temperamentTarget(name) != mp::temperamentTarget(lib[2].name),
             "each temperament its own target");
    proc.onPowerRequest = nullptr;
  }
};
static PowerButtonsTest g_power;

}  // namespace
