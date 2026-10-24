// A block bigger than the one the audio device announced (#554). A Jack or
// PipeWire server can raise its buffer size while the player runs, and the
// device then hands over blocks larger than prepareToPlay was told of. The
// swell box acts on every frame of such a block: with the shoe closed, an
// organ prepared for 256 frames and played in blocks of 1024 sounds as soft
// as one played in blocks of 256, and to the last bit the same.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <vector>

namespace {

using mp::test::Category;

// One manual, one rank in a swell box, a 523.25 Hz tone.
juce::File writeSwellOrgan(const juce::File& root) {
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
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 523.25 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  const auto odf = defs.getChildFile("swell.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Swell block</Identification_Name>"
      "<Identification_UniqueOrganID>554</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>523.25</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>72</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>Sw</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "</ObjectList><ObjectList ObjectType=\"Enclosure\">"
      "<Enclosure><EnclosureID>1</EnclosureID><Name>Swell</Name>"
      "<ShutterPositionContinuousControlID>9</ShutterPositionContinuousControlID></Enclosure>"
      "</ObjectList><ObjectList ObjectType=\"ContinuousControl\">"
      "<ContinuousControl><ControlID>9</ControlID><Name>Swell shoe</Name><DefaultValue>127</DefaultValue></ContinuousControl>"
      "</ObjectList><ObjectList ObjectType=\"EnclosurePipe\">"
      "<EnclosurePipe><EnclosureID>1</EnclosureID><PipeID>301</PipeID>"
      "<FiltParamWhenClsd_OverallAttnDb>12</FiltParamWhenClsd_OverallAttnDb>"
      "<FiltParamWhenClsd_MaxFreqHz>20000</FiltParamWhenClsd_MaxFreqHz>"
      "<FiltParamWhenClsd_ExtraAttnAtMinDb>0</FiltParamWhenClsd_ExtraAttnAtMinDb>"
      "<FiltParamWhenOpen_MaxFreqHz>20000</FiltParamWhenOpen_MaxFreqHz>"
      "</EnclosurePipe></ObjectList></Hauptwerk>");
  return odf;
}

// The organ prepared for 256 frames, the stop drawn, the shoe at `shoe`, a
// key held, and `total` frames played in blocks of `frames`. Every sample of
// the left channel, in order.
std::vector<float> playInBlocks(const juce::File& odf, int shoe, int frames, int total) {
  mp::MasterpieceProcessor proc;
  proc.prepareToPlay(48000.0, 256);
  if (!proc.loadOrgan(odf, 0, false).ok) return {};
  proc.setStopEngaged(901, true);
  proc.setContinuousControl(9, shoe);
  std::vector<float> heard;
  juce::AudioBuffer<float> buf(2, frames);
  for (int done = 0; done < total; done += frames) {
    juce::MidiBuffer m;
    if (done == 0) m.addEvent(juce::MidiMessage::noteOn(1, 72, 0.8f), 0);
    buf.clear();
    proc.processBlock(buf, m);
    for (int s = 0; s < frames; ++s) heard.push_back(buf.getSample(0, s));
  }
  return heard;
}

// The level of the second half: past the attack and the shades' first move.
double levelOfSecondHalf(const std::vector<float>& x) {
  double sum = 0.0;
  for (size_t i = x.size() / 2; i < x.size(); ++i) sum += static_cast<double>(x[i]) * x[i];
  return std::sqrt(sum / static_cast<double>(x.size() - x.size() / 2));
}

class BigBlockSwellTest final : public mp::test::Test {
public:
  BigBlockSwellTest() : Test("functional.swell.block-bigger-than-prepared", Category::Functional) {}
  void run() override {
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-big-block");
    const juce::File odf = writeSwellOrgan(root);
    const int total = 48 * 1024;  // about a second

    const auto open = playInBlocks(odf, 127, 256, total);
    const auto closedSmall = playInBlocks(odf, 0, 256, total);
    const auto closedBig = playInBlocks(odf, 0, 1024, total);
    MP_CHECK(!open.empty() && !closedSmall.empty() && !closedBig.empty(), "the organ loads");
    if (open.empty() || closedSmall.empty() || closedBig.empty()) return;

    const double lOpen = levelOfSecondHalf(open);
    const double lSmall = levelOfSecondHalf(closedSmall);
    const double lBig = levelOfSecondHalf(closedBig);
    MP_CHECK(lOpen > 0.01, "the open box sounds");
    const double dropSmall = 20.0 * std::log10(lOpen / std::max(lSmall, 1e-9));
    const double dropBig = 20.0 * std::log10(lOpen / std::max(lBig, 1e-9));
    MP_CHECK(dropSmall > 6.0, "in blocks of the prepared size, the closed box is softer");
    MP_CHECK(std::fabs(dropBig - dropSmall) < 0.5,
             "in blocks four times the prepared size, the closed box is as soft (" +
                 std::to_string(dropBig) + " dB against " + std::to_string(dropSmall) + " dB)");
    MP_CHECK(closedBig == closedSmall, "and to the last bit the same as blocks of the prepared size");
    root.deleteRecursively();
  }
};
static BigBlockSwellTest g_bigBlockSwell;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
