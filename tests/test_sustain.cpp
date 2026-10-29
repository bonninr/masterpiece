// The sustain pedal: controller 64 holds the keys let go while it is down,
// when the player has turned it on, and a controller 64 learned for something
// else keeps doing that.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One manual, one stop.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("tone.wav").createOutputStream());
    juce::StringPairArray meta;
    meta.set("NumSampleLoops", "1");
    meta.set("Loop0Start", "1000");
    meta.set("Loop0End", "47000");
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 48000);
    for (int i = 0; i < 48000; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  const auto odf = defs.getChildFile("sustain.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Sustain</Identification_Name>"
      "<Identification_UniqueOrganID>36</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>201</RankID><NormalMIDINoteNumber>38</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>403</LayerID><PipeID>303</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>503</UniqueID><LayerID>403</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>3</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>Spare</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class SustainPedalTest final : public mp::test::Test {
public:
  SustainPedalTest() : Test("functional.playing.sustain-pedal", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-sustain");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    const bool before = proc.sustainPedal();
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    proc.setStopEngaged(901, true);

    juce::AudioBuffer<float> buf(2, 256);
    const auto send = [&](std::initializer_list<juce::MidiMessage> msgs) {
      juce::MidiBuffer m;
      for (const auto& msg : msgs) m.addEvent(msg, 0);
      buf.clear();
      proc.processBlock(buf, m);
    };
    const auto run = [&](int n) {
      for (int i = 0; i < n; ++i) send({});
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto on = [](int note) { return juce::MidiMessage::noteOn(1, note, 0.8f); };
    const auto off = [](int note) { return juce::MidiMessage::noteOff(1, note); };
    const auto pedal = [](int v) { return juce::MidiMessage::controllerEvent(1, 64, v); };

    // Off, the default: the pedal is ignored.
    proc.setSustainPedal(false);
    send({pedal(127), on(36)});
    send({off(36)});
    run(400);
    MP_CHECK(voices() == 0, "turned off, a key let go stops under the pedal");
    send({pedal(0)});

    // On.
    proc.setSustainPedal(true);
    send({on(36)});
    send({pedal(127)});
    send({off(36)});
    run(400);
    MP_CHECK(voices() == 1, "a key let go with the pedal down keeps sounding");
    send({on(38), off(38)});
    run(4);
    MP_CHECK(voices() == 2, "and so does the next one played under it");
    send({on(36)});
    run(4);
    send({pedal(0)});
    run(400);
    MP_CHECK(voices() == 1, "lifted: the key held over stops, the key struck again and held sounds on");
    send({off(36)});
    run(400);
    MP_CHECK(voices() == 0, "and stops when let go");

    // All Notes Off ends what the pedal holds too.
    send({pedal(127), on(36), off(36)});
    run(4);
    send({juce::MidiMessage::allNotesOff(1)});
    run(400);
    MP_CHECK(voices() == 0, "All Notes Off releases a key the pedal holds");
    send({pedal(0)});

    // A controller 64 learned for a switch keeps working that switch.
    mp::MidiBinding b;
    b.source.kind = mp::MidiSourceKind::ControlChange;
    b.source.channel = 1;
    b.source.number = 64;
    b.targetKind = mp::MidiTargetKind::Switch;
    b.targetId = 1102;
    b.trigger = mp::MidiTrigger::Momentary;
    b.latching = false;
    proc.midiMap().bind(b);
    send({on(36), pedal(127), off(36)});
    run(400);
    MP_CHECK(proc.switchEngaged(1102), "the learned controller works its switch");
    MP_CHECK(voices() == 0, "and holds no key");
    send({pedal(0)});

    proc.setSustainPedal(before);
    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static SustainPedalTest g_sustain;

// A second Note On for a key still held changes nothing: the pipe keeps
// speaking, no new attack starts, and the one Note Off lets it go (#565).
class RepeatedNoteOnTest final : public mp::test::Test {
public:
  RepeatedNoteOnTest() : Test("functional.playing.repeated-note-on", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-repeated-on");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    proc.setStopEngaged(901, true);

    juce::AudioBuffer<float> buf(2, 256);
    const auto send = [&](std::initializer_list<juce::MidiMessage> msgs) {
      juce::MidiBuffer m;
      for (const auto& msg : msgs) m.addEvent(msg, 0);
      buf.clear();
      proc.processBlock(buf, m);
    };
    const auto run = [&](int n) {
      for (int i = 0; i < n; ++i) send({});
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto on = [](int note) { return juce::MidiMessage::noteOn(1, note, 0.8f); };
    const auto off = [](int note) { return juce::MidiMessage::noteOff(1, note); };

    send({on(36)});
    run(40);
    MP_CHECK(voices() == 1, "sanity: the key sounds");
    send({on(36)});
    MP_CHECK(voices() == 1, "struck again while held: still the one pipe, no second attack");
    send({off(36)});
    run(400);
    MP_CHECK(voices() == 0, "the one Note Off lets it go");

    // A key let go and struck again speaks anew.
    send({on(36)});
    send({off(36)});
    send({on(36)});
    MP_CHECK(voices() == 2, "sanity: a key let go and struck again starts a new attack");
    send({off(36)});
    run(400);
    MP_CHECK(voices() == 0, "and stops when let go");

    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static RepeatedNoteOnTest g_repeatedNoteOn;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
