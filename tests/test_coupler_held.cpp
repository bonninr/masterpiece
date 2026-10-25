// A piston that pushes in a stop and an octave coupler together, while a key
// is held, silences the key and its octave (#589). The cancel moved the stop
// first and the coupler second: the coupler let go of the octave with the
// stop already gone, so no pipe was found to let go, and the stop then let go
// of the key along the couplers still drawn. The octave sounded on until the
// key came up.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One manual, one stop with pipes at C2 and C3, a super-octave coupler, a
// cancel (stop first, then the coupler) and a general drawing both.
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
  const auto odf = defs.getChildFile("coupled.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Coupled</Identification_Name>"
      "<Identification_UniqueOrganID>36</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>201</RankID><NormalMIDINoteNumber>48</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ConditionSwitchID>1201</ConditionSwitchID><ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
      "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys>"
      "<MIDINoteNumberIncrement>12</MIDINoteNumberIncrement></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>13</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1201</SwitchID><Name>I Super</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1301</SwitchID><Name>Cancel</Name><Latching>N</Latching></Switch>"
      "<Switch><SwitchID>1302</SwitchID><Name>General</Name><Latching>N</Latching></Switch>"
      "</ObjectList><ObjectList ObjectType=\"Combination\">"
      "<Combination><CombinationID>1</CombinationID><Name>Cancel</Name><CombinationTypeCode>1</CombinationTypeCode>"
      "<ActivatingSwitchID>1301</ActivatingSwitchID></Combination>"
      "<Combination><CombinationID>2</CombinationID><Name>General</Name><CombinationTypeCode>1</CombinationTypeCode>"
      "<ActivatingSwitchID>1302</ActivatingSwitchID></Combination>"
      "</ObjectList><ObjectList ObjectType=\"CombinationElement\">"
      "<CombinationElement><CombinationID>1</CombinationID><ControlledSwitchID>1101</ControlledSwitchID>"
      "<InitialStoredStateIsEngaged>N</InitialStoredStateIsEngaged></CombinationElement>"
      "<CombinationElement><CombinationID>1</CombinationID><ControlledSwitchID>1201</ControlledSwitchID>"
      "<InitialStoredStateIsEngaged>N</InitialStoredStateIsEngaged></CombinationElement>"
      "<CombinationElement><CombinationID>2</CombinationID><ControlledSwitchID>1101</ControlledSwitchID>"
      "<InitialStoredStateIsEngaged>Y</InitialStoredStateIsEngaged></CombinationElement>"
      "<CombinationElement><CombinationID>2</CombinationID><ControlledSwitchID>1201</ControlledSwitchID>"
      "<InitialStoredStateIsEngaged>Y</InitialStoredStateIsEngaged></CombinationElement>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class CancelReleasesCoupledNotesTest final : public mp::test::Test {
public:
  CancelReleasesCoupledNotesTest()
      : Test("functional.playing.cancel-releases-coupled-notes", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-coupled-held");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    const bool leave = proc.stopsLeaveHeldNotes();
    proc.setStopsLeaveHeldNotes(false);
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

    // The stop and the coupler drawn by hand, then the cancel.
    proc.setSwitchEngaged(1101, true);
    proc.setSwitchEngaged(1201, true);
    block(on);
    run(4);
    MP_CHECK(voices() == 2, "the key and its octave sound");
    proc.setSwitchEngaged(1301, true);
    run(400);
    MP_CHECK(!proc.switchEngaged(1101) && !proc.switchEngaged(1201), "the cancel pushed both in");
    MP_CHECK(voices() == 0, "the key held, the cancel silences it and its octave: " +
                                std::to_string(voices()) + " voice(s) left");
    block(off);
    run(400);
    MP_CHECK(voices() == 0, "nothing left once the key is up");

    // The general draws both again while the key is held: each pipe once.
    block(on);
    run(4);
    MP_CHECK(voices() == 0, "nothing is drawn");
    proc.setSwitchEngaged(1302, true);
    run(4);
    MP_CHECK(voices() == 2, "the general sounds the key and its octave, each once: " +
                                std::to_string(voices()) + " voice(s)");
    // The same two moves by hand in one go, the stop first.
    proc.setSwitchEngaged(1101, false);
    proc.setSwitchEngaged(1201, false);
    run(400);
    MP_CHECK(voices() == 0, "stop and coupler pushed in together silence both: " +
                                std::to_string(voices()) + " voice(s) left");
    block(off);
    run(400);

    proc.setStopsLeaveHeldNotes(leave);
    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static CancelReleasesCoupledNotesTest g_cancelReleasesCoupledNotes;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
