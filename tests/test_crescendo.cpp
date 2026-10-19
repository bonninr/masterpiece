// The crescendo on top of a registration drawn by hand, as on a console: its
// steps draw what is not drawn yet, and stepping back pushes in only what the
// crescendo drew (Magle, 2012 and 2014: hand-drawn stops were pushed in when
// the crescendo stepped down). With the option off, each step recalls its
// registration whole, as before.
#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One manual, stops A, B and C on switches 1101-1103, each with a pipe, and
// a crescendo on control 50: steps at 10, 40, 70 and 100 fire combinations
// holding A; A and B; A, B and C; and A, B and C.
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
  juce::String x;
  x << "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
       "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Crescendo</Identification_Name>"
       "<Identification_UniqueOrganID>35</Identification_UniqueOrganID></_General></ObjectList>"
       "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
       "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
       "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
       "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
       "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>";
  juce::String ranks, pipes, layers, attacks, stops, stopRanks, switches;
  for (int i = 1; i <= 3; ++i) {
    const juce::String n(i), name = juce::String::charToString(static_cast<juce::juce_wchar>('A' + i - 1));
    ranks << "<Rank><RankID>20" << n << "</RankID><Name>" << name << "</Name></Rank>";
    pipes << "<Pipe_SoundEngine01><PipeID>30" << n << "</PipeID><RankID>20" << n
          << "</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    layers << "<Pipe_SoundEngine01_Layer><LayerID>40" << n << "</LayerID><PipeID>30" << n
           << "</PipeID></Pipe_SoundEngine01_Layer>";
    attacks << "<Pipe_SoundEngine01_AttackSample><UniqueID>50" << n << "</UniqueID><LayerID>40" << n
            << "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
    stops << "<Stop><StopID>90" << n << "</StopID><Name>" << name
          << "</Name><DivisionID>801</DivisionID><ControllingSwitchID>110" << n << "</ControllingSwitchID></Stop>";
    stopRanks << "<StopRank><StopID>90" << n << "</StopID><RankID>20" << n
              << "</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
                 "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>";
    switches << "<Switch><SwitchID>110" << n << "</SwitchID><Name>" << name << "</Name><Latching>Y</Latching></Switch>";
  }
  juce::String stages, combos, elements;
  const int at[] = {10, 40, 70, 100};
  for (int s = 1; s <= 4; ++s) {
    const juce::String n(s);
    switches << "<Switch><SwitchID>200" << n << "</SwitchID><Name>Step " << n << "</Name><Latching>N</Latching></Switch>";
    stages << "<ContinuousControlStageSwitch><ContinuousControlID>50</ContinuousControlID><ContinuousControlValue>"
           << at[s - 1] << "</ContinuousControlValue><ControlledSwitchID>200" << n
           << "</ControlledSwitchID></ContinuousControlStageSwitch>";
    combos << "<Combination><CombinationID>300" << n << "</CombinationID><Name>Step " << n
           << "</Name><CombinationTypeCode>4</CombinationTypeCode><ActivatingSwitchID>200" << n
           << "</ActivatingSwitchID></Combination>";
    for (int stop = 1; stop <= 3; ++stop)
      elements << "<CombinationElement><CombinationID>300" << n << "</CombinationID><ControlledSwitchID>110"
               << stop << "</ControlledSwitchID><InitialStoredStateIsEngaged>"
               << (stop <= juce::jmin(s, 3) ? "Y" : "N") << "</InitialStoredStateIsEngaged></CombinationElement>";
  }
  x << "<ObjectList ObjectType=\"Rank\">" << ranks << "</ObjectList>"
    << "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" << pipes << "</ObjectList>"
    << "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" << layers << "</ObjectList>"
    << "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" << attacks << "</ObjectList>"
    << "<ObjectList ObjectType=\"Keyboard\"><Keyboard><KeyboardID>701</KeyboardID><Name>I</Name>"
       "<DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
       "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
       "</Keyboard></ObjectList>"
    << "<ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
    << "<ObjectList ObjectType=\"KeyAction\"><KeyAction><SourceKeyboardID>701</SourceKeyboardID>"
       "<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
       "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
       "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction></ObjectList>"
    << "<ObjectList ObjectType=\"Stop\">" << stops << "</ObjectList>"
    << "<ObjectList ObjectType=\"StopRank\">" << stopRanks << "</ObjectList>"
    << "<ObjectList ObjectType=\"Switch\">" << switches << "</ObjectList>"
    << "<ObjectList ObjectType=\"ContinuousControl\"><ContinuousControl><ControlID>50</ControlID>"
       "<Name>Crescendo</Name></ContinuousControl></ObjectList>"
    << "<ObjectList ObjectType=\"ContinuousControlStageSwitch\">" << stages << "</ObjectList>"
    << "<ObjectList ObjectType=\"Combination\">" << combos << "</ObjectList>"
    << "<ObjectList ObjectType=\"CombinationElement\">" << elements << "</ObjectList>"
    << "</Hauptwerk>";
  const auto odf = defs.getChildFile("crescendo.Organ_Hauptwerk_xml");
  odf.replaceWithText(x);
  return odf;
}

class CrescendoHandDrawnTest final : public mp::test::Test {
public:
  CrescendoHandDrawnTest() : Test("functional.control.crescendo-keeps-hand-drawn", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-crescendo-hand");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    const bool keepWas = proc.crescendoKeepsHandDrawn();
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    MP_CHECK(proc.stageSwitches().crescendoControl() == 50, "control 50 is the crescendo");
    const auto drawn = [&](mp::Id sw) { return proc.switchEngaged(sw); };
    const auto names = [&] {
      return std::string(drawn(1101) ? "A" : "-") + (drawn(1102) ? "B" : "-") + (drawn(1103) ? "C" : "-");
    };
    proc.setControlValue(50, 0);
    for (mp::Id sw : {mp::Id(1101), mp::Id(1102), mp::Id(1103)}) proc.setSwitchEngaged(sw, false);

    // On: C drawn by hand; the crescendo to step 3 and back to nothing.
    proc.setCrescendoKeepsHandDrawn(true);
    proc.setSwitchEngaged(1103, true);
    proc.setControlValue(50, 20);
    MP_CHECK(names() == "A-C", "step 1 draws A and leaves C drawn by hand: " + names());
    proc.setControlValue(50, 75);
    MP_CHECK(names() == "ABC", "step 3 holds all three: " + names());
    proc.setControlValue(50, 45);
    MP_CHECK(names() == "ABC", "back to step 2: C, drawn by hand, stays: " + names());
    proc.setControlValue(50, 15);
    MP_CHECK(names() == "A-C", "back to step 1: B, drawn by the crescendo, goes: " + names());

    // A stop the crescendo drew and the player then pushed in and drew again
    // is the player's: stepping back leaves it.
    proc.setControlValue(50, 45);
    MP_CHECK(drawn(1102), "step 2 draws B again");
    proc.setSwitchEngaged(1102, false);
    proc.setSwitchEngaged(1102, true);
    proc.setControlValue(50, 15);
    MP_CHECK(drawn(1102), "B, taken over by hand, stays at step 1: " + names());

    // Off: each step recalls its registration whole, as before.
    for (mp::Id sw : {mp::Id(1101), mp::Id(1102), mp::Id(1103)}) proc.setSwitchEngaged(sw, false);
    proc.setControlValue(50, 0);
    proc.setCrescendoKeepsHandDrawn(false);
    proc.setSwitchEngaged(1103, true);
    proc.setControlValue(50, 75);
    proc.setControlValue(50, 45);
    MP_CHECK(!drawn(1103), "off: step 2 recalled whole pushes C in: " + names());

    proc.setCrescendoKeepsHandDrawn(keepWas);
    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static CrescendoHandDrawnTest g_crescendoHand;

}  // namespace
