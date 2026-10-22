// "Pedal combinations coupled" (PedalCoupling.h): which divisions it ties,
// then on a two-division organ the first manual's divisional recalling,
// storing and cancelling the Pedal's of the same number while it is on, and
// leaving it alone while it is off; a MIDI button for it and the control API.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#if MP_ENABLE_API
#include "../src/mp_api/ControlModel.h"
#endif
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_control/PedalCoupling.h"

#include <string>

namespace {

using mp::test::Category;

class PedalCouplingDivisionsTest final : public mp::test::Test {
public:
  PedalCouplingDivisionsTest() : Test("functional.pedal-coupling.divisions", Category::Functional) {}
  void run() override {
    using mp::PlayerCombinations;
    mp::OrganModel m;
    // By name: the Pedal is named so, and the first other division is the manual.
    std::vector<PlayerCombinations::Division> named{{10, "Grand Orgue"}, {11, "P\xc3\xa9" "dale"}, {12, "R\xc3\xa9" "cit"}};
    auto c = mp::coupledDivisions(m, named);
    MP_CHECK(c.pedal == 11 && c.manual == 10 && c.valid(), "the Pedal found by its name");

    // By assignment code: keyboard code 1 is the pedal, the lowest other is the manual.
    mp::Keyboard k1, k2, k3;
    k1.keyboardId = 1; k1.assignmentCode = 3; k1.primaryDivisionHint = 20;
    k2.keyboardId = 2; k2.assignmentCode = 1; k2.primaryDivisionHint = 21;
    k3.keyboardId = 3; k3.assignmentCode = 2; k3.primaryDivisionHint = 22;
    m.keyboards[1] = k1;
    m.keyboards[2] = k2;
    m.keyboards[3] = k3;
    std::vector<PlayerCombinations::Division> coded{{20, "Swell"}, {21, "Basse"}, {22, "Hauptwerk"}};
    c = mp::coupledDivisions(m, coded);
    MP_CHECK(c.pedal == 21 && c.manual == 22, "the Pedal and the first manual found by their keyboards' codes");

    std::vector<PlayerCombinations::Division> manualsOnly{{30, "Great"}, {31, "Swell"}};
    MP_CHECK(!mp::coupledDivisions(mp::OrganModel{}, manualsOnly).valid(), "no pedal: nothing to couple");
  }
};
static PedalCouplingDivisionsTest g_divisions;

// A manual and a pedal, a stop on each; the pedal is named "Basse" and is
// known as the pedal only by its keyboard's assignment code.
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
  const auto odf = defs.getChildFile("coupled.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Coupled</Identification_Name>"
      "<Identification_UniqueOrganID>35</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank><Rank><RankID>202</RankID><Name>B</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<Hint_PrimaryAssociatedDivisionID>801</Hint_PrimaryAssociatedDivisionID>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "<Keyboard><KeyboardID>702</KeyboardID><Name>II</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<Hint_PrimaryAssociatedDivisionID>802</Hint_PrimaryAssociatedDivisionID>"
      "<KeyGen_NumberOfKeys>32</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>Hauptwerk</Name></Division>"
      "<Division><DivisionID>802</DivisionID><Name>Basse</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "<KeyAction><SourceKeyboardID>702</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>802</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>32</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>Principal</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>Subbass</Name><DivisionID>802</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>Principal</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>Subbass</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class PedalCouplingOrganTest final : public mp::test::Test {
public:
  PedalCouplingOrganTest() : Test("functional.pedal-coupling.organ", Category::Functional) {}
  void run() override {
    // The switch is kept in the global file: put that back as it was.
    const juce::File global = mp::dataFolder().getChildFile("settings.mpglobal");
    const bool had = global.existsAsFile();
    juce::MemoryBlock before;
    if (had) global.loadFileAsData(before);

    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-pedal-coupling");
    const juce::File odf = writeOrgan(root);
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      const auto cd = proc.coupledDivisions();
      MP_CHECK(cd.manual == 801 && cd.pedal == 802, "the manual and the Pedal, the Pedal by its keyboard's code");
      const auto drawBoth = [&] {
        proc.setStopEngaged(901, true);
        proc.setStopEngaged(902, true);
      };

      // Off: the manual's piston is the manual's alone.
      proc.setPedalCombinationsCoupled(false);
      drawBoth();
      proc.setCaptureMode(true);
      proc.pressDivisional(801, 1);
      proc.setCaptureMode(false);
      const auto& pc = proc.playerCombinations();
      MP_CHECK(pc.divisionalSet(801, 1) && !pc.divisionalSet(802, 1), "off: Set stores the manual's divisional only");
      proc.pressDivisionalCancel(801);
      MP_CHECK(!proc.stopEngaged(901) && proc.stopEngaged(902), "off: the manual's cancel leaves the Pedal");

      // On: stored together, recalled together, cancelled together.
      proc.setPedalCombinationsCoupled(true);
      MP_CHECK(proc.pedalCombinationsCoupled(), "on");
      drawBoth();
      proc.setCaptureMode(true);
      proc.pressDivisional(801, 2);
      proc.setCaptureMode(false);
      MP_CHECK(pc.divisionalSet(801, 2) && pc.divisionalSet(802, 2), "on: Set stores the Pedal's piston 2 with the manual's");
      proc.pressDivisionalCancel(801);
      MP_CHECK(!proc.stopEngaged(901) && !proc.stopEngaged(902), "on: the manual's cancel takes the Pedal too");
      proc.pressDivisional(801, 2);
      MP_CHECK(proc.stopEngaged(901) && proc.stopEngaged(902), "on: piston 2 draws both");
      MP_CHECK(pc.litDivisional(801) == 2 && pc.litDivisional(802) == 2, "and lights both");
      proc.pressDivisional(802, 1);
      MP_CHECK(pc.litDivisional(801) == 2, "the Pedal's own piston stays the Pedal's");

      // A console button toggles it.
      mp::MidiBinding b;
      b.source.kind = mp::MidiSourceKind::ControlChange;
      b.source.channel = 16;
      b.source.number = 20;
      b.targetKind = mp::MidiTargetKind::PedalCombinationsCoupled;
      b.targetId = mp::kPedalCouplerTarget;
      b.trigger = mp::MidiTrigger::Toggle;
      b.latching = true;
      proc.midiMap().bind(b);
      juce::AudioBuffer<float> buf(2, 256);
      juce::MidiBuffer press;
      press.addEvent(juce::MidiMessage::controllerEvent(16, 20, 127), 0);
      proc.processBlock(buf, press);
      MP_CHECK(!proc.pedalCombinationsCoupled(), "a MIDI button toggles it off");
      proc.saveGlobalIfDirty();

#if MP_ENABLE_API
      mp::api::ControlModel model(proc);
      mp::api::Error e;
      int v = 1;
      MP_CHECK(model.setProgram(mp::api::ProgramProp::PedalCombinationsCoupled, 1) == mp::api::Error::None &&
                   proc.pedalCombinationsCoupled(),
               "the control API turns it on");
      MP_CHECK(model.get(mp::api::Type::Program, static_cast<int>(mp::api::ProgramProp::PedalCombinationsCoupled), v, e) &&
                   v == 1,
               "and reads it");
#endif
      proc.settingsFileFor(odf).deleteFile();
    }
    {
      mp::MasterpieceProcessor proc;  // the next start
      proc.loadGlobalDefaults();
      MP_CHECK(proc.pedalCombinationsCoupled() == (MP_ENABLE_API ? true : false), "kept for the next start");
      proc.setPedalCombinationsCoupled(false);
    }
    if (had) global.replaceWithData(before.getData(), before.getSize());
    else global.deleteFile();
    root.deleteRecursively();
  }
};
static PedalCouplingOrganTest g_organ;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
