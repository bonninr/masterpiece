// A panel fitted to its window keeps its size in edit mode: the hidden
// elements edit mode shows are added below, in reach by scrolling (reported
// on D#565: Edit shrank every button of the whole-organ panel).
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Panels.h"

#include <cmath>
#include <memory>
#include <string>

namespace {

using mp::test::Category;

class PanelEditSizeTest final : public mp::test::Test {
public:
  PanelEditSizeTest() : Test("functional.ui.panel-edit-size", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-panel-edit-size");
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
    // One manual, one division, four stops on one rank.
    std::string stops, stopRanks, switches;
    for (int i = 1; i <= 4; ++i) {
      const std::string n = std::to_string(i);
      stops += "<Stop><StopID>90" + n + "</StopID><Name>Stop " + n +
               "</Name><DivisionID>801</DivisionID><ControllingSwitchID>110" + n + "</ControllingSwitchID></Stop>";
      stopRanks += "<StopRank><StopID>90" + n + "</StopID><RankID>201</RankID>"
                   "<MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
                   "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>";
      switches += "<Switch><SwitchID>110" + n + "</SwitchID><Name>Stop " + n + "</Name><Latching>Y</Latching></Switch>";
    }
    const std::string odfText =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Edit size</Identification_Name>"
        "<Identification_UniqueOrganID>34</Identification_UniqueOrganID></_General></ObjectList>"
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
        "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
        "<ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList><ObjectList ObjectType=\"Stop\">" + stops +
        "</ObjectList><ObjectList ObjectType=\"StopRank\">" + stopRanks +
        "</ObjectList><ObjectList ObjectType=\"Switch\">" + switches + "</ObjectList></Hauptwerk>";
    const auto odf = defs.getChildFile("editsize.Organ_Hauptwerk_xml");
    odf.replaceWithText(juce::String::fromUTF8(odfText.c_str()));

    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

      // Three of the four stops hidden, the panel fitted to a small window.
      mp::ui::PanelLayout layout;
      layout.fitToWindow = true;
      int stopCount = 0;
      for (const auto& s : mp::ui::panelSections(proc))
        for (const auto& e : s.elements)
          if (e.kind == mp::ui::PanelElement::Kind::Stop && stopCount++ > 0) layout.hidden.insert(e.key());
      MP_CHECK(stopCount == 4, "the panel holds the four stops: " + std::to_string(stopCount));

      juce::MessageManager::getInstance();
      mp::ui::PanelView view(proc, 0, layout);
      view.setSize(400, 300);
      const auto playing = view.cellBoundsForTest();
      MP_CHECK(!playing.empty(), "the one stop shown is drawn");

      view.setEditingForTest(true);
      const auto editing = view.cellBoundsForTest();
      MP_CHECK(editing.size() == playing.size() + 3, "edit mode shows the three hidden stops too: " +
                                                         std::to_string(editing.size()) + " cells");
      if (!playing.empty() && !editing.empty())
        MP_CHECK(editing[0].getHeight() == playing[0].getHeight() &&
                     editing[0].getWidth() == playing[0].getWidth(),
                 "edit mode keeps the stops' size: " + std::to_string(playing[0].getHeight()) + " high, then " +
                     std::to_string(editing[0].getHeight()));

      view.setEditingForTest(false);
      const auto after = view.cellBoundsForTest();
      MP_CHECK(after.size() == playing.size() && !after.empty() && after[0] == playing[0],
               "and leaving it the panel is as it was");
      proc.settingsFileFor(odf).deleteFile();
    }
    root.deleteRecursively();
  }
};
static PanelEditSizeTest g_panelEditSize;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
