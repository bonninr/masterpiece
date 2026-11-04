// A stop whose switch the wiring drives only on a condition speaks only while
// the condition holds. Oloron's reeds and mixtures hang on a hidden switch
// that the drawstop engages through a linkage waiting on the division's
// Appel d'anches; the drawstop alone used to draw the stop (#779).
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>

namespace {

using mp::test::Category;

// One manual, three stops on one rank's pipe:
//   901 Montre   switch 1101, the drawstop itself
//   902 Clairon  hidden switch 1216, engaged by drawstop 1116 while the
//                call 1131 is engaged
//   903 Bourdon  "DelayedStop: 3", which nothing drives: the drawstop
//                "03. Bourdon" belongs to it by its name only, as on Friesach
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
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 4800);
    for (int i = 0; i < 4800; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 4800);
  }
  const auto knob = [](int id, const char* name) {
    return std::string("<Switch><SwitchID>") + std::to_string(id) + "</SwitchID><Name>" + name +
           "</Name><Latching>Y</Latching><Clickable>Y</Clickable>"
           "<Disp_ImageSetInstanceID>" + std::to_string(id) + "</Disp_ImageSetInstanceID></Switch>";
  };
  const auto hidden = [](int id, const char* name) {
    return std::string("<Switch><SwitchID>") + std::to_string(id) + "</SwitchID><Name>" + name +
           "</Name><Latching>Y</Latching><Clickable>N</Clickable></Switch>";
  };
  const auto stopRank = [](int stop) {
    return std::string("<StopRank><StopID>") + std::to_string(stop) +
           "</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
           "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>";
  };
  const std::string organ =
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Appel</Identification_Name>"
      "<Identification_UniqueOrganID>36</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>GO</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<Hint_PrimaryAssociatedDivisionID>801</Hint_PrimaryAssociatedDivisionID>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>Grand Orgue</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>Montre</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>Clairon</Name><DivisionID>801</DivisionID><ControllingSwitchID>1216</ControllingSwitchID></Stop>"
      "<Stop><StopID>903</StopID><Name>Bourdon</Name><DivisionID>801</DivisionID><ControllingSwitchID>1303</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">" +
      stopRank(901) + stopRank(902) + stopRank(903) +
      "</ObjectList><ObjectList ObjectType=\"Switch\">" +
      knob(1101, "01. Montre") + knob(1116, "16. Clairon") + knob(1131, "Appel d'anches: GO") +
      knob(1103, "03. Bourdon") + hidden(1216, "Anches: 8") + hidden(1303, "DelayedStop: 3") +
      "</ObjectList><ObjectList ObjectType=\"SwitchLinkage\">"
      "<SwitchLinkage><SourceSwitchID>1116</SourceSwitchID><DestSwitchID>1216</DestSwitchID>"
      "<ConditionSwitchID>1131</ConditionSwitchID><SourceSwitchLinkIfEngaged>Y</SourceSwitchLinkIfEngaged>"
      "<ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged><ReevaluateIfCondSwitchChangesState>Y</ReevaluateIfCondSwitchChangesState>"
      "<EngageLinkActionCode>1</EngageLinkActionCode><DisengageLinkActionCode>2</DisengageLinkActionCode></SwitchLinkage>"
      "</ObjectList></Hauptwerk>";
  const auto odf = defs.getChildFile("appel.Organ_Hauptwerk_xml");
  odf.replaceWithText(juce::String::fromUTF8(organ.c_str()));
  return odf;
}

class ConditionalStopTest final : public mp::test::Test {
public:
  ConditionalStopTest() : Test("functional.control.conditional-stop", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-conditional-stop");
    const juce::File odf = writeOrgan(root);
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      const juce::File settings = proc.settingsFileFor(odf);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

      proc.setSwitchEngaged(1116, true);
      MP_CHECK(!proc.switchEngaged(1216), "the call in: the wiring leaves the reeds' switch off");
      MP_CHECK(!proc.stopEngaged(902), "the call in: the drawn Clairon is silent");
      proc.setSwitchEngaged(1131, true);
      MP_CHECK(proc.stopEngaged(902), "the call drawn: the Clairon speaks");
      proc.setSwitchEngaged(1131, false);
      MP_CHECK(!proc.stopEngaged(902), "the call put back in: the Clairon falls silent again");
      proc.setSwitchEngaged(1116, false);
      proc.setSwitchEngaged(1131, true);
      MP_CHECK(!proc.stopEngaged(902), "the call alone draws no Clairon");
      proc.setSwitchEngaged(1131, false);

      proc.setSwitchEngaged(1101, true);
      MP_CHECK(proc.stopEngaged(901), "a stop on its own drawstop follows it");
      proc.setSwitchEngaged(1101, false);
      MP_CHECK(!proc.stopEngaged(901), "and goes in with it");

      proc.setSwitchEngaged(1103, true);
      MP_CHECK(proc.stopEngaged(903), "a drawstop known by its name still draws a stop nothing wires");
      proc.setSwitchEngaged(1103, false);
      MP_CHECK(!proc.stopEngaged(903), "and puts it back in");
      settings.deleteFile();
    }
    root.deleteRecursively();
  }
};
static ConditionalStopTest g_conditionalStop;

}  // namespace

#endif
