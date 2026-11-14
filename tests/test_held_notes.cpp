// Stop changes and the notes already held (GrandOrgue #1833). By default a
// stop drawn while a key is held speaks on it at once and one pushed in lets
// go of it, as on a console with electric action; with "Stop changes leave
// the notes already held alone" the held note keeps what it started with, and
// only keys pressed afterwards play the new registration.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One manual, two stops on it, a rank each. Stop A has a tremmed rank too,
// rank 203 with a softer tone, which switch 1103 plays in place of A's and
// which re-sounds the notes held when it moves. `throughInputs` plays the
// division through its inputs, as Ashton and Buckeburg do: key C2's switch
// 5036 closes input 33036 of the division, with no key action.
juce::File writeOrgan(const juce::File& root, bool throughInputs = false) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  for (const auto& [name, level] : {std::pair<const char*, double>{"tone.wav", 0.3}, {"soft.wav", 0.1}}) {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile(name).createOutputStream());
    juce::StringPairArray meta;
    meta.set("NumSampleLoops", "1");
    meta.set("Loop0Start", "1000");
    meta.set("Loop0End", "47000");
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 48000);
    for (int i = 0; i < 48000; ++i)
      tone.setSample(0, i, static_cast<float>(level * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  const auto odf = defs.getChildFile("held.Organ_Hauptwerk_xml");
  const juce::String keyAction =
      throughInputs
          ? "<ObjectList ObjectType=\"KeyboardKey\"><KeyboardKey><KeyboardID>701</KeyboardID><SwitchID>5036</SwitchID>"
            "<NormalMIDINoteNumber>36</NormalMIDINoteNumber></KeyboardKey></ObjectList>"
            "<ObjectList ObjectType=\"DivisionInput\"><DivisionInput><DivisionID>801</DivisionID><SwitchID>33036</SwitchID>"
            "<NormalMIDINoteNumber>36</NormalMIDINoteNumber></DivisionInput></ObjectList>"
            "<ObjectList ObjectType=\"SwitchLinkage\"><SwitchLinkage><SourceSwitchID>5036</SourceSwitchID>"
            "<DestSwitchID>33036</DestSwitchID><SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
            "<EngageLinkActionCode>1</EngageLinkActionCode><DisengageLinkActionCode>2</DisengageLinkActionCode>"
            "</SwitchLinkage></ObjectList>"
          : "<ObjectList ObjectType=\"KeyAction\">"
            "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
            "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
            "</ObjectList>";
  const juce::String inputSwitches =
      throughInputs ? "<Switch><SwitchID>5036</SwitchID><Name>Key 36</Name><Latching>N</Latching></Switch>"
                      "<Switch><SwitchID>33036</SwitchID><Name>Input 36</Name><Latching>N</Latching></Switch>"
                    : "";
  odf.replaceWithText(
      juce::String("<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">")+
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Held</Identification_Name>"
      "<Identification_UniqueOrganID>35</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample>"
      "<Sample><SampleID>102</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>soft.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank><Rank><RankID>202</RankID><Name>B</Name></Rank>"
      "<Rank><RankID>203</RankID><Name>A trem</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>203</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>403</LayerID><PipeID>303</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>503</UniqueID><LayerID>403</LayerID><SampleID>102</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
      "<Hint_PrimaryAssociatedDivisionID>801</Hint_PrimaryAssociatedDivisionID></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>" +
      keyAction + "<ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>B</Name><DivisionID>801</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes><AlternateRankID>203</AlternateRankID>"
      "<SwitchIDToSwitchToAlternateRank>1103</SwitchIDToSwitchToAlternateRank>"
      "<RetriggerNotesWhenSwitchingBetweenNormalAndAlternateRanks>Y</RetriggerNotesWhenSwitchingBetweenNormalAndAlternateRanks></StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>B</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1103</SwitchID><Name>Trem</Name><Latching>Y</Latching></Switch>" +
      inputSwitches + "</ObjectList></Hauptwerk>");
  return odf;
}

class HeldNotesTest final : public mp::test::Test {
public:
  HeldNotesTest() : Test("functional.playing.stops-leave-held-notes", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-held-notes");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    const bool before = proc.stopsLeaveHeldNotes();
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
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36), 0);

    // Electric action, the default.
    proc.setStopsLeaveHeldNotes(false);
    proc.setStopEngaged(901, true);
    block(on);
    run(4);
    MP_CHECK(voices() == 1, "stop A sounds the held key");
    proc.setStopEngaged(902, true);
    run(4);
    MP_CHECK(voices() == 2, "stop B drawn on the held key speaks at once");
    proc.setStopEngaged(902, false);
    run(400);
    MP_CHECK(voices() == 1, "pushed in, it lets go while the key stays down");
    block(off);
    run(400);
    MP_CHECK(voices() == 0, "the key let go");

    // Left alone.
    proc.setStopsLeaveHeldNotes(true);
    block(on);
    run(4);
    MP_CHECK(voices() == 1, "stop A sounds the held key");
    proc.setStopEngaged(902, true);
    run(4);
    MP_CHECK(voices() == 1, "stop B drawn now does not reach the held key");
    proc.setStopEngaged(901, false);
    run(400);
    MP_CHECK(voices() == 1, "and stop A pushed in leaves it sounding");
    block(off);
    run(400);
    MP_CHECK(voices() == 0, "the key let go ends it");
    block(on);
    run(4);
    MP_CHECK(voices() == 1, "the next key plays the new registration: B only");
    block(off);
    run(400);

    proc.setStopsLeaveHeldNotes(before);
    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static HeldNotesTest g_heldNotes;

// A tremulant that swaps in the tremmed rank re-sounds the held notes, as the
// organ asks, unless the player has asked for held notes to be left alone
// (#637): then the held note keeps the rank it started with.
class HeldNotesTremulantTest final : public mp::test::Test {
public:
  HeldNotesTremulantTest() : Test("functional.playing.tremulant-leaves-held-notes", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-held-notes-trem");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    const bool before = proc.stopsLeaveHeldNotes();
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    // The level once it has settled: over 64 blocks, as one block holds less
    // than a period of the tone.
    const auto level = [&] {
      for (int i = 0; i < 400; ++i) block();
      double sum = 0.0;
      for (int i = 0; i < 64; ++i) {
        block();
        const float r = buf.getRMSLevel(0, 0, buf.getNumSamples());
        sum += static_cast<double>(r) * r;
      }
      return static_cast<float>(std::sqrt(sum / 64.0));
    };
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
    proc.setStopEngaged(901, true);

    // Electric action, the default: the held note changes to the tremmed rank.
    proc.setStopsLeaveHeldNotes(false);
    block(on);
    const float plain = level();
    MP_CHECK(plain > 0.01f, "rank A sounds the held key: " + std::to_string(plain));
    proc.setSwitchEngaged(1103, true);
    const float tremmed = level();
    MP_CHECK(tremmed < plain * 0.6f && tremmed > plain * 0.2f,
             "the tremulant re-sounds it on the tremmed rank: " + std::to_string(tremmed));
    block(off);
    proc.setSwitchEngaged(1103, false);
    MP_CHECK(level() < 1.0e-5f, "the key let go");

    // Left alone: the held note keeps rank A; the next key plays the tremmed rank.
    proc.setStopsLeaveHeldNotes(true);
    block(on);
    const float first = level();
    proc.setSwitchEngaged(1103, true);
    const float kept = level();
    MP_CHECK(std::fabs(kept - first) < first * 0.05f,
             "the tremulant leaves the held note on rank A: " + std::to_string(kept) + " of " + std::to_string(first));
    block(off);
    MP_CHECK(level() < 1.0e-5f, "the key let go ends it");
    block(on);
    const float next = level();
    MP_CHECK(std::fabs(next - tremmed) < tremmed * 0.05f,
             "the next key plays the tremmed rank: " + std::to_string(next));
    block(off);
    level();

    proc.setStopsLeaveHeldNotes(before);
    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static HeldNotesTremulantTest g_heldNotesTremulant;

// The same tremulant on a division the keys play through its inputs: the held
// note changes rank at once, and back (#1116). With held notes left alone it
// keeps the rank it started with.
class HeldNotesTremulantInputsTest final : public mp::test::Test {
public:
  HeldNotesTremulantInputsTest()
      : Test("functional.playing.tremulant-division-inputs", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-held-notes-trem-inputs");
    const juce::File odf = writeOrgan(root, true);
    mp::MasterpieceProcessor proc;
    const bool before = proc.stopsLeaveHeldNotes();
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    proc.clearChannelAssignments();
    proc.setKeyboardForChannel(1, 701);

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    const auto level = [&] {
      for (int i = 0; i < 400; ++i) block();
      double sum = 0.0;
      for (int i = 0; i < 64; ++i) {
        block();
        const float r = buf.getRMSLevel(0, 0, buf.getNumSamples());
        sum += static_cast<double>(r) * r;
      }
      return static_cast<float>(std::sqrt(sum / 64.0));
    };
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
    proc.setStopEngaged(901, true);

    proc.setStopsLeaveHeldNotes(false);
    block(on);
    const float plain = level();
    MP_CHECK(plain > 0.01f, "rank A sounds the key held through the division's input: " + std::to_string(plain));
    proc.setSwitchEngaged(1103, true);
    const float tremmed = level();
    MP_CHECK(tremmed < plain * 0.6f && tremmed > plain * 0.2f,
             "the tremulant re-sounds the held note on the tremmed rank: " + std::to_string(tremmed) + " of " +
                 std::to_string(plain));
    proc.setSwitchEngaged(1103, false);
    const float back = level();
    MP_CHECK(std::fabs(back - plain) < plain * 0.05f,
             "and back on rank A when it stops: " + std::to_string(back) + " of " + std::to_string(plain));
    block(off);
    MP_CHECK(level() < 1.0e-5f, "the key let go");

    proc.setStopsLeaveHeldNotes(true);
    block(on);
    const float first = level();
    proc.setSwitchEngaged(1103, true);
    const float kept = level();
    MP_CHECK(std::fabs(kept - first) < first * 0.05f,
             "left alone, the held note stays on rank A: " + std::to_string(kept) + " of " + std::to_string(first));
    block(off);
    MP_CHECK(level() < 1.0e-5f, "the key let go ends it");
    block(on);
    const float next = level();
    MP_CHECK(std::fabs(next - tremmed) < tremmed * 0.05f, "the next key plays the tremmed rank: " + std::to_string(next));
    block(off);
    level();

    proc.setStopsLeaveHeldNotes(before);
    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static HeldNotesTremulantInputsTest g_heldNotesTremulantInputs;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
