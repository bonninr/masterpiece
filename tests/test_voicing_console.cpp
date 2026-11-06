// Voicing from the console (#637): the test note plays the rank chosen on the
// Voicing tab alone, latched, and the MIDI steps change that rank's level and
// tuning a step at a time.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>

namespace {

using mp::test::Category;

// One manual, two stops on it, a rank each, both playing the same tone.
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
  const auto odf = defs.getChildFile("voicing.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Voicing</Identification_Name>"
      "<Identification_UniqueOrganID>37</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank><Rank><RankID>202</RankID><Name>B</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>201</RankID><NormalMIDINoteNumber>37</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>304</PipeID><RankID>202</RankID><NormalMIDINoteNumber>37</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>403</LayerID><PipeID>303</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>404</LayerID><PipeID>304</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>503</UniqueID><LayerID>403</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>504</UniqueID><LayerID>404</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>B</Name><DivisionID>801</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>B</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class VoicingTestNoteTest final : public mp::test::Test {
public:
  VoicingTestNoteTest() : Test("functional.voicing.test-note", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-voicing-console");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto run = [&](int n) {
      for (int i = 0; i < n; ++i) block();
    };
    // The level over 64 blocks, once the attack has settled.
    const auto level = [&] {
      run(32);
      double sum = 0.0;
      for (int i = 0; i < 64; ++i) {
        block();
        sum += buf.getRMSLevel(0, 0, buf.getNumSamples());
      }
      return sum / 64.0;
    };
    const auto note = [](int key, bool on) {
      juce::MidiBuffer m;
      m.addEvent(on ? juce::MidiMessage::noteOn(1, key, 0.8f) : juce::MidiMessage::noteOff(1, key), 0);
      return m;
    };

    // Stop A drawn, B in: the key plays rank A.
    proc.setStopEngaged(901, true);
    block(note(36, true));
    const double a = level();
    MP_CHECK(voices() == 1 && a > 0.05, "the key plays stop A");
    block(note(36, false));
    run(400);
    MP_CHECK(voices() == 0, "and lets go");

    // Rank B chosen, 12 dB down, the test note on: the key plays B alone,
    // with stop A still drawn.
    auto v = proc.voicing().live().rank(202);
    v.gainDb = -12.0f;
    proc.voicing().live().setRank(202, v);
    proc.setVoicingTarget(202, 0);
    proc.setTestNote(true);
    block(note(36, true));
    const double b = level();
    MP_CHECK(voices() == 1, "the test note is one pipe");
    MP_CHECK(std::abs(b / a - 0.251) < 0.03,
             "it is rank B, 12 dB down: " + std::to_string(b) + " of " + std::to_string(a));
    MP_CHECK(proc.testNoteKey() == 36, "the key is reported");
    block(note(36, false));
    run(400);
    MP_CHECK(voices() == 1, "letting go of the key leaves it sounding");

    // A step up while it sounds: struck again, half a decibel louder.
    MP_CHECK(proc.applyVoicingStep(mp::MidiTargetKind::VoicingLevelUp), "a step is taken");
    MP_CHECK(proc.voicing().live().rank(202).gainDb == -11.5f, "-12 dB up a step is -11.5 dB");
    const double up = level();
    MP_CHECK(voices() >= 1, "the test note sounds on through the step");
    MP_CHECK(std::abs(up / b - 1.059) < 0.02,
             "and is heard half a decibel louder: " + std::to_string(up) + " of " + std::to_string(b));

    // The next key stops it, and plays nothing itself.
    block(note(48, true));
    block(note(48, false));
    run(400);
    MP_CHECK(voices() == 0, "the next key stops the test note");
    MP_CHECK(proc.testNoteKey() == -1, "and no key is reported");

    // Turned off, the keys play the organ again.
    block(note(36, true));
    run(4);
    MP_CHECK(voices() == 1, "a key sounds the test note again");
    proc.setTestNote(false);
    run(400);
    MP_CHECK(voices() == 0, "turning the test note off stops it");
    block(note(36, false));
    block(note(36, true));
    const double again = level();
    MP_CHECK(std::abs(again / a - 1.0) < 0.03, "the key plays stop A again");
    block(note(36, false));
    run(400);
  }
};

class VoicingStepsTest final : public mp::test::Test {
public:
  VoicingStepsTest() : Test("functional.voicing.console-steps", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-voicing-steps");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    MP_CHECK(!proc.applyVoicingStep(mp::MidiTargetKind::VoicingLevelUp), "no rank chosen: no step");

    // The whole rank, then one pipe of it: the pipe's own, added to the rank.
    proc.setVoicingTarget(201, 0);
    for (int i = 0; i < 30; ++i) proc.applyVoicingStep(mp::MidiTargetKind::VoicingLevelDown);
    MP_CHECK(proc.voicing().live().rank(201).gainDb == -12.0f, "level stops at -12 dB");
    for (int i = 0; i < 55; ++i) proc.applyVoicingStep(mp::MidiTargetKind::VoicingTuneUp);
    MP_CHECK(proc.voicing().live().rank(201).tuningCents == 50.0f, "tuning stops at +50 cents");
    proc.applyVoicingStep(mp::MidiTargetKind::VoicingTuneDown);
    MP_CHECK(proc.voicing().live().rank(201).tuningCents == 49.0f, "a step down is a cent");
    proc.setVoicingTarget(201, 301);
    proc.applyVoicingStep(mp::MidiTargetKind::VoicingLevelUp);
    MP_CHECK(proc.voicing().live().pipe(301).gainDb == 0.5f, "one pipe: its own level");
    MP_CHECK(proc.voicing().live().rank(201).gainDb == -12.0f, "and the rank's is left");
    MP_CHECK(proc.voicingSteps() == 87, "every step is counted for the Voicing tab");

    // A console button: the press raises the step, the release does not.
    mp::MidiBinding button;
    button.source.kind = mp::MidiSourceKind::ControlChange;
    button.source.channel = 1;
    button.source.number = 20;
    button.targetKind = mp::MidiTargetKind::VoicingTuneUp;
    button.targetId = 0;
    button.trigger = mp::MidiTrigger::Momentary;
    proc.midiMap().setBindingsFor(mp::MidiTargetKind::VoicingTuneUp, 0, {button});
    MP_CHECK(mp::MidiMap::isConsoleTarget(mp::MidiTargetKind::VoicingTuneUp), "a console button, on every organ");
    juce::AudioBuffer<float> buf(2, 256);
    juce::MidiBuffer press, release;
    press.addEvent(juce::MidiMessage::controllerEvent(1, 20, 127), 0);
    release.addEvent(juce::MidiMessage::controllerEvent(1, 20, 0), 0);
    proc.processBlock(buf, press);
    MP_CHECK(proc.takeVoicingStep() == mp::MidiTargetKind::VoicingTuneUp, "the press asks for a step");
    MP_CHECK(proc.takeVoicingStep() == mp::MidiTargetKind::None, "taken once");
    proc.processBlock(buf, release);
    MP_CHECK(proc.takeVoicingStep() == mp::MidiTargetKind::None, "the release asks for none");
    mp::MidiMap copy;
    MP_CHECK(copy.fromText(proc.midiMap().toText()) && copy.bindingsFor(mp::MidiTargetKind::VoicingTuneUp, 0).size() == 1,
             "the mapping file names it and reads it back");
  }
};

// Several ranks chosen at once, a stop's or a chest's (#848): the test note
// sounds every one of them, another key moves it to its neighbour, and a
// step from the console moves each rank or pipe reached.
class VoicingGroupTest final : public mp::test::Test {
public:
  VoicingGroupTest() : Test("functional.voicing.group", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-voicing-group");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    const auto run = [&](int n) {
      for (int i = 0; i < n; ++i) block();
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto press = [&](int key) {
      juce::MidiBuffer on, off;
      on.addEvent(juce::MidiMessage::noteOn(1, key, 0.8f), 0);
      off.addEvent(juce::MidiMessage::noteOff(1, key), 0);
      block(on);
      block(off);
      run(400);
    };

    mp::VoicingTarget both;
    both.ranks = {201, 202};
    proc.setVoicingTarget(both);
    MP_CHECK(proc.voicingRank() == 201 && proc.voicingPipe() == 0, "the first rank is reported");
    proc.setTestNote(true);
    press(36);
    MP_CHECK(voices() == 2 && proc.testNoteKey() == 36, "the test note sounds both ranks");
    press(37);
    MP_CHECK(voices() == 2 && proc.testNoteKey() == 37, "another key moves it to its neighbour");
    press(37);
    MP_CHECK(voices() == 0 && proc.testNoteKey() == -1, "the same key again stops it");
    press(37);
    MP_CHECK(voices() == 2, "and plays it again");
    proc.setTestNote(false);
    run(400);
    MP_CHECK(voices() == 0, "turned off, it stops");

    // A step on both ranks, then on the C# side of both chests.
    MP_CHECK(proc.applyVoicingStep(mp::MidiTargetKind::VoicingLevelDown), "a step is taken");
    MP_CHECK(proc.voicing().live().rank(201).gainDb == -0.5f && proc.voicing().live().rank(202).gainDb == -0.5f,
             "both ranks move half a decibel");
    both.scope = mp::VoicingScope::CSharpSide;
    proc.setVoicingTarget(both);
    MP_CHECK(proc.applyVoicingStep(mp::MidiTargetKind::VoicingTuneUp), "a step on one side is taken");
    const auto& v = proc.voicing().live();
    MP_CHECK(v.pipe(303).tuningCents == 1.0f && v.pipe(304).tuningCents == 1.0f, "the C# pipes move a cent");
    MP_CHECK(v.pipe(301).isNeutral() && v.pipe(302).isNeutral(), "the C pipes stay");
    both.scope = mp::VoicingScope::Pipe;
    both.note = 48;
    proc.setVoicingTarget(both);
    MP_CHECK(!proc.applyVoicingStep(mp::MidiTargetKind::VoicingTuneUp), "a note with no pipe takes no step");
  }
};

VoicingTestNoteTest g_testNote;
VoicingStepsTest g_steps;
VoicingGroupTest g_group;

}  // namespace

#endif
