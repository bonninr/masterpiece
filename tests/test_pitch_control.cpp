// A pipe layer's pitch control (PitchLvl_IncrementingContinuousControlID),
// followed by the pipes already sounding (#828). Bueckeburg's detune slider
// reaches its pipes only while their keys are down, through the key's own
// division input: a pitch read once, as the pipe started, never moved.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

// One manual, one stop, one pipe, whose layer takes its pitch from control
// 1001 at half a hertz a step, the control at 63 of 0..127.
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
  const auto odf = defs.getChildFile("pitch.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Pitch</Identification_Name>"
      "<Identification_UniqueOrganID>37</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>4</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID>"
      "<PitchLvl_IncrementingContinuousControlID>1001</PitchLvl_IncrementingContinuousControlID>"
      "<PitchLvl_IncrementingCtsCtrlSensitivityHzPerCtrlUnit>0.5</PitchLvl_IncrementingCtsCtrlSensitivityHzPerCtrlUnit>"
      "</Pipe_SoundEngine01_Layer>"
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
      "</ObjectList><ObjectList ObjectType=\"ContinuousControl\">"
      "<ContinuousControl><ControlID>1001</ControlID><Name>Detune</Name><DefaultValue>63</DefaultValue>"
      "<Clickable>Y</Clickable></ContinuousControl>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

// The pitch of a run of frames, from its rising zero crossings, each placed
// between its two frames.
double pitchOf(const std::vector<float>& x, double sampleRate) {
  double first = -1.0, last = -1.0;
  int crossings = 0;
  for (size_t i = 1; i < x.size(); ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double at = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
      if (first < 0.0) first = at;
      last = at;
      ++crossings;
    }
  }
  if (crossings < 2) return 0.0;
  return (crossings - 1) * sampleRate / (last - first);
}

class PitchControlTest final : public mp::test::Test {
public:
  PitchControlTest() : Test("functional.playing.pitch-control-held-notes", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-pitch-control");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
    };
    // The pitch over the next n blocks, left channel.
    const auto pitch = [&](int n) {
      std::vector<float> x;
      for (int i = 0; i < n; ++i) {
        block();
        const float* p = buf.getReadPointer(0);
        x.insert(x.end(), p, p + buf.getNumSamples());
      }
      return pitchOf(x, 48000.0);
    };
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36), 0);

    proc.setStopEngaged(901, true);
    block(on);
    pitch(8);
    const double start = pitch(80);
    MP_CHECK(start > 20.0, "the held note sounds: " + std::to_string(start) + " Hz");

    // The control up by 64 steps, the key still held: half a hertz a step.
    proc.setContinuousControl(1001, 127);
    pitch(4);
    const double up = pitch(80);
    MP_CHECK(std::fabs(up - start - 32.0) < 0.5,
             "the held note follows its pitch control: " + std::to_string(start) + " -> " +
                 std::to_string(up) + " Hz");
    MP_CHECK(proc.voiceStats().activeVoices == 1, "the same voice, still sounding");

    proc.setContinuousControl(1001, 63);
    pitch(4);
    const double back = pitch(80);
    MP_CHECK(std::fabs(back - start) < 0.3, "and back where it began: " + std::to_string(back) + " Hz");

    // A key pressed with the control down starts at the control's pitch, and
    // stays there while nothing moves.
    block(off);
    for (int i = 0; i < 400; ++i) block();
    proc.setContinuousControl(1001, 3);
    block();
    block(on);
    pitch(8);
    const double low = pitch(80);
    MP_CHECK(std::fabs(start - low - 30.0) < 0.5,
             "a new note starts at its control's pitch: " + std::to_string(low) + " Hz");
    MP_CHECK(std::fabs(pitch(80) - low) < 0.2, "and holds it");
    block(off);
    for (int i = 0; i < 400; ++i) block();

    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static PitchControlTest g_pitchControl;

} // namespace

#endif
