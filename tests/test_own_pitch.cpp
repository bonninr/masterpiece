// The organ's own pitch where the definition does not state it (mattrixed,
// #1271). Alessandria states no AudioEngine_BasePitchHz and its A pipes are
// recorded at 448 Hz: taken as 440, "the organ's own" was called 440 while it
// sounded at 448, and choosing 440 changed nothing. Now it is read from the
// A pipes' recorded pitch; a stated base pitch is kept as stated.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One rank: A pipes over four octaves and a C, recorded at A = `recordedA`.
juce::File writeOrgan(const juce::File& root, double recordedA, bool statePitch) {
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
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 448.0 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  juce::String samples, pipes, layers, attacks;
  const int notes[] = {45, 57, 60, 69, 81};
  int k = 0;
  for (const int note : notes) {
    const juce::String n(k++);
    const double hz = recordedA * std::pow(2.0, (note - 69) / 12.0);
    samples << "<Sample><SampleID>10" << n << "</SampleID><InstallationPackageID>1</InstallationPackageID>"
            << "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>4</Pitch_SpecificationMethodCode>"
            << "<Pitch_ExactSamplePitch>" << juce::String(hz, 3) << "</Pitch_ExactSamplePitch></Sample>";
    pipes << "<Pipe_SoundEngine01><PipeID>30" << n << "</PipeID><RankID>201</RankID><NormalMIDINoteNumber>" << note
          << "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    layers << "<Pipe_SoundEngine01_Layer><LayerID>40" << n << "</LayerID><PipeID>30" << n
           << "</PipeID></Pipe_SoundEngine01_Layer>";
    attacks << "<Pipe_SoundEngine01_AttackSample><UniqueID>50" << n << "</UniqueID><LayerID>40" << n
            << "</LayerID><SampleID>10" << n << "</SampleID></Pipe_SoundEngine01_AttackSample>";
  }
  const auto odf = defs.getChildFile("pitch.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Own pitch</Identification_Name>"
      "<Identification_UniqueOrganID>1271</Identification_UniqueOrganID>" +
      juce::String(statePitch ? "<AudioEngine_BasePitchHz>440</AudioEngine_BasePitchHz>" : "") +
      "</_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\">" + samples + "</ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>Principale 8</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks + "</ObjectList>"
      "<ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>GO</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>Principale 8</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>Principale 8</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class OwnPitchTest final : public mp::test::Test {
public:
  OwnPitchTest() : Test("functional.tuning.own-pitch", Category::Functional) {}
  void run() override {
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-own-pitch");
    struct Case {
      double recordedA;
      bool stated;
      double expected;
      const char* what;
    };
    const Case cases[] = {
        {448.0, false, 448.0, "no base pitch stated, recorded at 448: the organ's own is 448"},
        {440.0, false, 440.0, "no base pitch stated, recorded at 440: 440"},
        {448.0, true, 440.0, "a stated 440 is kept as stated"},
    };
    for (const auto& c : cases) {
      const juce::File odf = writeOrgan(root, c.recordedA, c.stated);
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(odf);
      settings.deleteFile();
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, std::string("the organ loads: ") + c.what);
      MP_CHECK(std::abs(proc.nativePitchHz() - c.expected) < 0.15,
               std::string(c.what) + ", got " + std::to_string(proc.nativePitchHz()));
      if (!c.stated && c.recordedA == 448.0) {
        MP_CHECK(std::abs(proc.masterPitchHz() - 448.0) < 0.15, "and it plays there until another is chosen");
        proc.setMasterPitchHz(440.0);
        MP_CHECK(std::abs(proc.masterPitchHz() - 440.0) < 0.01, "440 chosen is 440, below the organ's own");
        proc.setMasterPitchHz(0.0);
      }
      settings.deleteFile();
    }
    root.deleteRecursively();
  }
};
static OwnPitchTest g_ownPitch;

}  // namespace

#endif
