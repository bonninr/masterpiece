// A division played through its inputs alone (#1228). Barritt's Sherborne
// reaches each division from its keyboard by a plain key action, with no
// key switches; the division's input switch, through the stop's node, opens
// the pipe's pallet, and the stops name only their action noises. Nothing
// closed the inputs, and only the noises sounded.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

std::string sw(int id, const char* name, bool latching) {
  return "<Switch><SwitchID>" + std::to_string(id) + "</SwitchID><Name>" + name + "</Name><Latching>" +
         (latching ? "Y" : "N") + "</Latching><DefaultToEngaged>N</DefaultToEngaged></Switch>";
}

// Keyboard 2 plays division 3 by a key action. Input 33060 (middle C) opens
// pallet 7060 while the stop's knob 1302 is drawn.
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
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 261.63 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  const auto odf = defs.getChildFile("inputs.Organ_Hauptwerk_xml");
  odf.replaceWithText(juce::String(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Input-only division</Identification_Name>"
      "<Identification_UniqueOrganID>1228</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>261.63</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>Open Diapason</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\"><Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID>"
      "<ControllingPalletSwitchID>7060</ControllingPalletSwitchID>"
      "<NormalMIDINoteNumber>60</NormalMIDINoteNumber></Pipe_SoundEngine01></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\"><Pipe_SoundEngine01_Layer><LayerID>401</LayerID>"
      "<PipeID>301</PipeID></Pipe_SoundEngine01_Layer></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\"><Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID>"
      "<LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample></ObjectList>"
      "<ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>2</KeyboardID><Name>Great</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
      "<Hint_PrimaryAssociatedDivisionID>3</Hint_PrimaryAssociatedDivisionID></Keyboard></ObjectList>"
      "<ObjectList ObjectType=\"Division\"><Division><DivisionID>3</DivisionID><Name>Great</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\"><KeyAction><SourceKeyboardID>2</SourceKeyboardID>"
      "<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>3</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
      "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction></ObjectList>"
      "<ObjectList ObjectType=\"DivisionInput\"><DivisionInput><DivisionID>3</DivisionID><SwitchID>33060</SwitchID>"
      "<NormalMIDINoteNumber>60</NormalMIDINoteNumber></DivisionInput></ObjectList>"
      "<ObjectList ObjectType=\"Stop\"><Stop><StopID>901</StopID><Name>Gt. Open Diapason 8</Name><DivisionID>3</DivisionID>"
      "<ControllingSwitchID>1302</ControllingSwitchID></Stop></ObjectList>"
      "<ObjectList ObjectType=\"Switch\">" +
      sw(1302, "Gt. Open Diapason 8", true) + sw(33060, "Gt input 60", false) + sw(7060, "Pallet 60", false) +
      "</ObjectList><ObjectList ObjectType=\"SwitchLinkage\"><SwitchLinkage><SourceSwitchID>33060</SourceSwitchID>"
      "<DestSwitchID>7060</DestSwitchID><ConditionSwitchID>1302</ConditionSwitchID>"
      "<ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged><SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
      "<EngageLinkActionCode>1</EngageLinkActionCode><DisengageLinkActionCode>2</DisengageLinkActionCode></SwitchLinkage>"
      "</ObjectList></Hauptwerk>"));
  return odf;
}

class InputOnlyDivisionTest final : public mp::test::Test {
public:
  InputOnlyDivisionTest() : Test("functional.control.input-only-division", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-input-only-division");
    const juce::File odf = writeOrgan(root);
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(odf);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      proc.clearChannelAssignments();
      proc.setKeyboardForChannel(1, 2);
      juce::AudioBuffer<float> buf(2, 256);
      auto block = [&](juce::MidiBuffer m) {
        buf.clear();
        proc.processBlock(buf, m);
      };
      auto key = [&](bool down) {
        juce::MidiBuffer m;
        m.addEvent(down ? juce::MidiMessage::noteOn(1, 60, 0.8f) : juce::MidiMessage::noteOff(1, 60), 0);
        block(m);
        for (int i = 0; i < 4; ++i) block({});
      };
      auto quiet = [&] {
        for (int i = 0; i < 400; ++i) block({});
      };
      auto voices = [&] { return proc.voiceStats().activeVoices; };
      auto played = [&](const std::string& what, int expected) {
        block({});
        key(true);
        const int v = voices();
        MP_CHECK(v == expected, what + ": " + std::to_string(v) + " voice(s), " + std::to_string(expected) + " expected");
        key(false);
        quiet();
        MP_CHECK(voices() == 0, what + ": silent again once the key is up");
      };

      played("nothing drawn, the key is silent", 0);
      proc.setSwitchEngaged(1302, true);
      played("the stop drawn, the key closes the division's input and the pipe sounds", 1);
      proc.setSwitchEngaged(1302, false);
      played("the stop pushed in, silent again", 0);
      settings.deleteFile();
    }
    root.deleteRecursively();
  }
};
InputOnlyDivisionTest g_inputOnlyDivision;

}  // namespace

#endif
