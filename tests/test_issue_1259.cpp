// Points from #1259 (ppytprs, dev 94101): the file dialog from the Open
// window, the Organ EQ kept, the voicing range and the ranks it offers.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/VoicingSet.h"
#include "../src/mp_ui/OpenWindow.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One stop of one rank on one manual: enough for the organ to have
// settings of its own.
juce::File writeOrgan(const juce::File& root) {
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
  const auto odf = defs.getChildFile("issue1259.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Issue 1259</Identification_Name>"
      "<Identification_UniqueOrganID>1259</Identification_UniqueOrganID></_General></ObjectList>"
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
      "<ObjectList ObjectType=\"Keyboard\"><Keyboard><KeyboardID>2</KeyboardID><Name>Great</Name>"
      "<DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey>"
      "<Hint_PrimaryAssociatedDivisionID>3</Hint_PrimaryAssociatedDivisionID></Keyboard></ObjectList>"
      "<ObjectList ObjectType=\"Division\"><Division><DivisionID>3</DivisionID><Name>Great</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\"><KeyAction><SourceKeyboardID>2</SourceKeyboardID>"
      "<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>3</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
      "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction></ObjectList>"
      "<ObjectList ObjectType=\"Stop\"><Stop><StopID>901</StopID><Name>Principal 8</Name><DivisionID>3</DivisionID>"
      "<ControllingSwitchID>1302</ControllingSwitchID></Stop></ObjectList>"
      "<ObjectList ObjectType=\"StopRank\"><StopRank><StopID>901</StopID><RankID>201</RankID>"
      "<MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank></ObjectList>"
      "<ObjectList ObjectType=\"Switch\"><Switch><SwitchID>1302</SwitchID><Name>Principal 8</Name>"
      "<Latching>Y</Latching><DefaultToEngaged>N</DefaultToEngaged></Switch></ObjectList></Hauptwerk>");
  return odf;
}

class Issue1259Test final : public mp::test::Test {
public:
  Issue1259Test() : Test("functional.ui.issue-1259", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();

    // Voicing: 24 dB either way, 50 cents of tuning; only loaded ranks.
    MP_CHECK(mp::voicingRange(mp::VoicingField::Level) == 24.0f, "level: 24 dB either way");
    MP_CHECK(mp::voicingRange(mp::VoicingField::Brightness) == 24.0f, "brightness: 24 dB too");
    MP_CHECK(mp::voicingRange(mp::VoicingField::Tuning) == 50.0f, "tuning: still 50 cents");
    const auto loaded = mp::loadedRanks({201, 202, 203}, std::set<mp::Id>{202});
    MP_CHECK(loaded == std::vector<mp::Id>({201, 203}), "a rank the partial load left out is not offered");
    MP_CHECK(mp::loadedRanks({202}, std::set<mp::Id>{202}).empty(), "a stop of left-out ranks offers nothing");

    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-issue-1259");
    const juce::File odf = writeOrgan(root);
    juce::File settings;
    {
      // The Organ EQ, edited on its tab, is the organ's and is kept.
      mp::MasterpieceProcessor proc;
      settings = proc.settingsFileFor(odf);
      settings.deleteFile();
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      auto eq = proc.voicingEq();
      eq.on = true;
      eq.bandsDb[3] = 4.5;
      proc.setVoicingEq(eq);
      proc.voicingEqEdited();
      MP_CHECK(proc.saveSettingsIfDirty(), "the edit marks the organ's settings to be saved");

      // The Open window's file dialog starts after the window, not inside
      // the click while it closes.
      bool browsed = false;
      mp::ui::OpenPanel panel(proc, [](const juce::File&) {}, [&browsed] { browsed = true; });
      juce::Button* another = nullptr;
      for (auto* c : panel.getChildren())
        if (auto* b = dynamic_cast<juce::Button*>(c); b != nullptr && b->getTooltip().startsWith("Any organ"))
          another = b;
      MP_CHECK(another != nullptr, "the Open window has its file dialog button");
      if (another != nullptr && another->onClick) another->onClick();
      MP_CHECK(!browsed, "the file dialog is not started inside the click");
    }
    {
      mp::MasterpieceProcessor proc;
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads again");
      MP_CHECK(proc.voicingEq().on && std::abs(proc.voicingEq().bandsDb[3] - 4.5) < 1e-6,
               "the Organ EQ is as it was left");
    }
    settings.deleteFile();
    root.deleteRecursively();
  }
};
static Issue1259Test g_issue1259;

}  // namespace

#endif
