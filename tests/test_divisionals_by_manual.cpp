// A console's divisional pistons on every organ (#1259): learned on one
// organ, a divisional piston of the first manual works the first manual's
// division of the next organ, whatever its division is numbered there.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One manual (keyboard 2, the first manual by Hauptwerk's assignment code)
// playing division `division`.
juce::File writeOrgan(const juce::File& root, int division, int organId) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("tone.wav").createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
    juce::AudioBuffer<float> tone(1, 4800);
    for (int i = 0; i < 4800; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 261.63 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 4800);
  }
  const auto d = std::to_string(division);
  const auto odf = defs.getChildFile("organ" + juce::String(organId) + ".Organ_Hauptwerk_xml");
  odf.replaceWithText(juce::String(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Organ " + std::to_string(organId) +
      "</Identification_Name><Identification_UniqueOrganID>" + std::to_string(organId) +
      "</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>261.63</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>Principal</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\"><Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID>"
      "<NormalMIDINoteNumber>60</NormalMIDINoteNumber></Pipe_SoundEngine01></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\"><Pipe_SoundEngine01_Layer><LayerID>401</LayerID>"
      "<PipeID>301</PipeID></Pipe_SoundEngine01_Layer></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\"><Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID>"
      "<LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample></ObjectList>"
      "<ObjectList ObjectType=\"Keyboard\"><Keyboard><KeyboardID>2</KeyboardID><Name>Manual I</Name>"
      "<DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
      "<Hint_PrimaryAssociatedDivisionID>" + d + "</Hint_PrimaryAssociatedDivisionID></Keyboard></ObjectList>"
      "<ObjectList ObjectType=\"Division\"><Division><DivisionID>" + d + "</DivisionID><Name>Great</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\"><KeyAction><SourceKeyboardID>2</SourceKeyboardID>"
      "<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>" + d + "</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
      "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction></ObjectList>"
      "<ObjectList ObjectType=\"Stop\"><Stop><StopID>901</StopID><Name>Principal 8</Name><DivisionID>" + d +
      "</DivisionID><ControllingSwitchID>1302</ControllingSwitchID></Stop></ObjectList>"
      "<ObjectList ObjectType=\"StopRank\"><StopRank><StopID>901</StopID><RankID>201</RankID>"
      "<MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank></ObjectList>"
      "<ObjectList ObjectType=\"Switch\"><Switch><SwitchID>1302</SwitchID><Name>Principal 8</Name>"
      "<Latching>Y</Latching><DefaultToEngaged>N</DefaultToEngaged></Switch></ObjectList></Hauptwerk>"));
  return odf;
}

class DivisionalsByManualTest final : public mp::test::Test {
public:
  DivisionalsByManualTest() : Test("functional.midi.divisionals-by-manual", Category::Functional) {}
  void run() override {
    const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File odfA = writeOrgan(tmp.getChildFile("mp-divisionals-a"), 3, 12591);
    const juce::File odfB = writeOrgan(tmp.getChildFile("mp-divisionals-b"), 7, 12592);
    mp::MasterpieceProcessor::consoleMidiFile().deleteFile();

    mp::MidiBinding piston;
    piston.source.kind = mp::MidiSourceKind::ControlChange;
    piston.source.channel = 1;
    piston.source.number = 30;
    piston.targetKind = mp::MidiTargetKind::PlayerDivisional;
    piston.trigger = mp::MidiTrigger::Momentary;
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(odfA, 0, false).ok, "organ A loads");
      const mp::Id onA = mp::playerDivisionalTarget(3, 2);
      MP_CHECK(proc.divisionalByManual(mp::MidiTargetKind::PlayerDivisional, onA) != 0,
               "division 3 is known by its manual");
      piston.targetId = onA;
      proc.midiMap().setBindingsFor(mp::MidiTargetKind::PlayerDivisional, onA, {piston});
      MP_CHECK(proc.saveMidiMap(), "the maps are saved");
    }
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(odfB, 0, false).ok, "organ B loads");
      const mp::Id onB = mp::playerDivisionalTarget(7, 2);
      const auto found = proc.midiMap().bindingsFor(mp::MidiTargetKind::PlayerDivisional, onB);
      MP_CHECK(found.size() == 1 && found[0]->source.number == 30,
               "the first manual's divisional 2 is mapped on organ B too: " + std::to_string(found.size()));
      MP_CHECK(proc.divisionalFromManual(mp::MidiTargetKind::PlayerDivisional,
                                         proc.divisionalByManual(mp::MidiTargetKind::PlayerDivisional, onB)) == onB,
               "by manual and back is the same piston");
    }
    mp::MasterpieceProcessor::consoleMidiFile().deleteFile();
    for (const auto& odf : {odfA, odfB}) {
      mp::MasterpieceProcessor p;
      p.settingsFileFor(odf).deleteFile();
      odf.getParentDirectory().getParentDirectory().deleteRecursively();
    }
  }
};
static DivisionalsByManualTest g_divisionalsByManual;

}  // namespace

#endif
