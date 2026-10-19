// Unevenness: pipe detune and wind sway (MasterpieceProcessor). Off, the
// output is bit-identical to an organ never touched; pipe detune puts a
// pipe off true by its own amount; wind sway moves the pitch of a note
// while it sounds, within its depth.
#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/VoicingSet.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

// One rank in one division on one manual, a 130.81 Hz tone looping past the
// part measured.
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
    meta.set("Loop0End", "95000");
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
    juce::AudioBuffer<float> tone(1, 96000);
    for (int i = 0; i < 96000; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265358979 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 96000);
  }
  const auto odf = defs.getChildFile("uneven.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Uneven</Identification_Name>"
      "<Identification_UniqueOrganID>35</Identification_UniqueOrganID></_General></ObjectList>"
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
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

struct Rig {
  mp::MasterpieceProcessor proc;
  bool loaded = false;
  explicit Rig(const juce::File& odf) {
    proc.prepareToPlay(48000.0, 256);
    loaded = proc.loadOrgan(odf, 0, false).ok;
    proc.setStopEngaged(901, true);
  }
  // `blocks` blocks of the left channel, the note struck at the first when
  // `strike`.
  std::vector<float> play(int blocks, bool strike) {
    std::vector<float> out;
    juce::AudioBuffer<float> buf(2, 256);
    for (int b = 0; b < blocks; ++b) {
      juce::MidiBuffer m;
      if (strike && b == 0) m.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
      buf.clear();
      proc.processBlock(buf, m);
      for (int s = 0; s < 256; ++s) out.push_back(buf.getSample(0, s));
    }
    return out;
  }
};

// The frequency between two sample positions, from the rising zero
// crossings found to a fraction of a sample.
double frequency(const std::vector<float>& x, size_t from, size_t to) {
  double first = -1.0, last = -1.0;
  int crossings = 0;
  for (size_t i = from + 1; i < to && i < x.size(); ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
      if (first < 0.0) first = t;
      else ++crossings;
      last = t;
    }
  }
  return crossings > 0 ? crossings * 48000.0 / (last - first) : 0.0;
}

double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

class UnevennessTest final : public mp::test::Test {
public:
  UnevennessTest() : Test("functional.unevenness", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-unevenness");
    const juce::File odf = writeOrgan(root);

    // Off, the output is equal to the last bit to an organ never touched,
    // after both were turned up and back down.
    Rig plain(odf), touched(odf);
    MP_CHECK(plain.loaded && touched.loaded, "the organ loads");
    touched.proc.setPipeDetuneCents(8.0f);
    touched.proc.setWindSwayCents(8.0f);
    plain.play(40, false);
    touched.play(40, false);
    touched.proc.setPipeDetuneCents(0.0f);
    touched.proc.setWindSwayCents(0.0f);
    MP_CHECK(plain.play(160, true) == touched.play(160, true),
             "both off: the output is bit-identical to an organ never touched");

    // Pipe detune: this pipe's own amount, every time.
    Rig straight(odf), detuned(odf);
    detuned.proc.setPipeDetuneCents(8.0f);
    const auto a = straight.play(170, true);
    const auto b = detuned.play(170, true);
    const double fa = frequency(a, 4800, 43000);
    const double fb = frequency(b, 4800, 43000);
    const double expect = 8.0 * mp::MasterpieceProcessor::pipeDetuneOffset(301);
    MP_CHECK(std::fabs(cents(fb, fa) - expect) < 0.3,
             "the pipe stands " + std::to_string(expect) + " cents off: measured " + std::to_string(cents(fb, fa)));
    MP_CHECK(std::fabs(mp::MasterpieceProcessor::pipeDetuneOffset(301) -
                       mp::MasterpieceProcessor::pipeDetuneOffset(301)) == 0.0 &&
                 mp::MasterpieceProcessor::pipeDetuneOffset(301) != mp::MasterpieceProcessor::pipeDetuneOffset(302),
             "the same pipe by the same amount, another pipe by another");

    // Wind sway: the pitch of a held note moves, within its depth.
    Rig swaying(odf);
    swaying.proc.setWindSwayCents(10.0f);
    const auto c = swaying.play(360, true);
    double lo = 1.0e9, hi = -1.0e9;
    for (size_t at = 4800; at + 9600 < 90000; at += 9600) {
      const double d = cents(frequency(c, at, at + 9600), fa);
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    }
    MP_CHECK(hi - lo > 0.3, "the pitch moves while the note sounds: " + std::to_string(hi - lo) + " cents");
    MP_CHECK(lo > -10.5 && hi < 10.5, "within the depth: " + std::to_string(lo) + " to " + std::to_string(hi));

    // Saved with the organ.
    swaying.proc.setPipeDetuneCents(3.5f);
    MP_CHECK(swaying.proc.saveSettings(), "saved");
    mp::MasterpieceProcessor again;
    again.prepareToPlay(48000.0, 256);
    MP_CHECK(again.loadOrgan(odf, 0, false).ok, "loads again");
    MP_CHECK(std::fabs(again.pipeDetuneCents() - 3.5f) < 0.01f && std::fabs(again.windSwayCents() - 10.0f) < 0.01f,
             "both read back from the organ's settings");

    plain.proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static UnevennessTest g_unevenness;

}  // namespace
