// A division no keyboard plays as its own, reached from a manual's keys
// through its division inputs (#1067). Caen St. Etienne keeps each manual's
// reeds in a division of their own: the key's switch closes the flue
// division's input at once, and the reed division's input only while the
// manual's "Anches" ventil is drawn. Only a keyboard's own division was played
// through its inputs, so the reeds never sounded.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

std::string pipe(int id, int rank) {
  return "<Pipe_SoundEngine01><PipeID>" + std::to_string(id) + "</PipeID><RankID>" + std::to_string(rank) +
         "</RankID><NormalMIDINoteNumber>60</NormalMIDINoteNumber></Pipe_SoundEngine01>";
}

std::string layer(int id) {
  return "<Pipe_SoundEngine01_Layer><LayerID>" + std::to_string(id + 100) + "</LayerID><PipeID>" +
         std::to_string(id) + "</PipeID></Pipe_SoundEngine01_Layer>";
}

std::string attack(int id) {
  return "<Pipe_SoundEngine01_AttackSample><UniqueID>" + std::to_string(id + 200) + "</UniqueID><LayerID>" +
         std::to_string(id + 100) + "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
}

std::string sw(int id, const char* name, bool latching, bool engaged = false) {
  return "<Switch><SwitchID>" + std::to_string(id) + "</SwitchID><Name>" + name + "</Name><Latching>" +
         (latching ? "Y" : "N") + "</Latching><DefaultToEngaged>" + (engaged ? "Y" : "N") +
         "</DefaultToEngaged></Switch>";
}

std::string stop(int id, const char* name, int division, int knob) {
  return "<Stop><StopID>" + std::to_string(id) + "</StopID><Name>" + name + "</Name><DivisionID>" +
         std::to_string(division) + "</DivisionID><ControllingSwitchID>" + std::to_string(knob) +
         "</ControllingSwitchID></Stop>";
}

std::string stopRank(int id, int rank) {
  return "<StopRank><StopID>" + std::to_string(id) + "</StopID><RankID>" + std::to_string(rank) +
         "</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
         "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>";
}

// The key's switch to a division's input. With a condition, the link holds
// while the condition switch is engaged.
std::string link(int from, int to, int condition) {
  return "<SwitchLinkage><SourceSwitchID>" + std::to_string(from) + "</SourceSwitchID><DestSwitchID>" +
         std::to_string(to) + "</DestSwitchID>" +
         (condition != 0 ? "<ConditionSwitchID>" + std::to_string(condition) +
                               "</ConditionSwitchID><ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged>"
                         : std::string()) +
         "<SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
         "<EngageLinkActionCode>1</EngageLinkActionCode><DisengageLinkActionCode>2</DisengageLinkActionCode>"
         "</SwitchLinkage>";
}

// The Grand Orgue, keyboard 2: its own division 3 holds the Montre, division
// 4 the Trompette. Key C4's switch 5060 closes input 33060 of division 3
// always, and input 34060 of division 4 while the Anches GO (1368) is drawn.
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
  const auto odf = defs.getChildFile("caen.Organ_Hauptwerk_xml");
  odf.replaceWithText(juce::String(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Floating division inputs</Identification_Name>"
      "<Identification_UniqueOrganID>1067</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>261.63</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>Montre</Name></Rank>"
      "<Rank><RankID>202</RankID><Name>Trompette</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" +
      pipe(301, 201) + pipe(302, 202) +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layer(301) + layer(302) +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attack(301) + attack(302) +
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>2</KeyboardID><Name>Grand Orgue</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
      "<Hint_PrimaryAssociatedDivisionID>3</Hint_PrimaryAssociatedDivisionID></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>3</DivisionID><Name>GO</Name></Division>"
      "<Division><DivisionID>4</DivisionID><Name>GO Anches</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyboardKey\"><KeyboardKey><KeyboardID>2</KeyboardID><SwitchID>5060</SwitchID>"
      "<NormalMIDINoteNumber>60</NormalMIDINoteNumber></KeyboardKey></ObjectList>"
      "<ObjectList ObjectType=\"DivisionInput\">"
      "<DivisionInput><DivisionID>3</DivisionID><SwitchID>33060</SwitchID><NormalMIDINoteNumber>60</NormalMIDINoteNumber></DivisionInput>"
      "<DivisionInput><DivisionID>4</DivisionID><SwitchID>34060</SwitchID><NormalMIDINoteNumber>60</NormalMIDINoteNumber></DivisionInput>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">" +
      stop(901, "GO: Montre 8", 3, 1302) + stop(902, "GO: Trompette 8", 4, 1314) +
      "</ObjectList><ObjectList ObjectType=\"StopRank\">" + stopRank(901, 201) + stopRank(902, 202) +
      "</ObjectList><ObjectList ObjectType=\"Switch\">" +
      sw(1302, "GO: Montre 8", true) + sw(1314, "GO: Trompette 8", true) + sw(1368, "Cp: Anches GO", true, true) +
      sw(5060, "GO key 60", false) + sw(33060, "GO DivInp 60", false) + sw(34060, "GO Anches DivInp 60", false) +
      "</ObjectList><ObjectList ObjectType=\"SwitchLinkage\">" + link(5060, 33060, 0) + link(5060, 34060, 1368) +
      "</ObjectList></Hauptwerk>"));
  return odf;
}

class FloatingDivisionInputsTest final : public mp::test::Test {
public:
  FloatingDivisionInputsTest()
      : Test("functional.control.floating-division-inputs", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-floating-division-inputs");
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
      };

      played("nothing drawn, the key is silent", 0);
      proc.setStopEngaged(901, true);
      played("the Montre sounds from the manual's own division", 1);
      proc.setStopEngaged(901, false);
      proc.setStopEngaged(902, true);
      played("the Trompette sounds from the reed division, the Anches drawn", 1);
      proc.setSwitchEngaged(1368, false);
      played("the Anches pushed in silences the Trompette", 0);
      proc.setSwitchEngaged(1368, true);
      proc.setStopEngaged(901, true);
      played("the Anches drawn again, Montre and Trompette together", 2);
      proc.setStopEngaged(901, false);
      proc.setStopEngaged(902, false);
      played("both pushed in, the key is silent again", 0);
      settings.deleteFile();
    }
    root.deleteRecursively();
  }
};
FloatingDivisionInputsTest g_floatingDivisionInputs;

}  // namespace

#endif
