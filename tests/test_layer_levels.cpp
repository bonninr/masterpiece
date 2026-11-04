// A pipe layer's level (AmpLvl_ScalingContinuousControlID), followed by the
// pipes already sounding (#765). A blower sounds from the load on, so a
// mixer slider for it that only reached the next note never reached it.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

// One manual, one stop, one pipe, whose layer takes its level from control
// 1001, a slider at full.
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
  const auto odf = defs.getChildFile("levels.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Levels</Identification_Name>"
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
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID>"
      "<AmpLvl_ScalingContinuousControlID>1001</AmpLvl_ScalingContinuousControlID></Pipe_SoundEngine01_Layer>"
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
      "<ContinuousControl><ControlID>1001</ControlID><Name>Level</Name><DefaultValue>127</DefaultValue>"
      "<Clickable>Y</Clickable></ContinuousControl>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class LayerLevelsTest final : public mp::test::Test {
public:
  LayerLevelsTest() : Test("functional.playing.layer-level-held-notes", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-layer-levels");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    // The peak of one block, left channel. A block is shorter than a cycle
    // of the tone, so a level is read as the peak over several (level()).
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
      return buf.getMagnitude(0, 0, buf.getNumSamples());
    };
    const auto run = [&](int n) {
      std::vector<float> r;
      for (int i = 0; i < n; ++i) r.push_back(block());
      return r;
    };
    // The peak over blocks [from, from + 4): 1024 frames, more than a cycle.
    const auto level = [](const std::vector<float>& r, size_t from) {
      return *std::max_element(r.begin() + static_cast<long>(from), r.begin() + static_cast<long>(from + 4));
    };
    const auto last = [&](const std::vector<float>& r) { return level(r, r.size() - 4); };
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36), 0);

    proc.setStopEngaged(901, true);
    block(on);
    const float full = last(run(20));
    MP_CHECK(full > 0.01f, "the held note sounds at the slider's full level: " + std::to_string(full));

    // The slider down to a quarter, the key still held: the note follows.
    proc.setContinuousControl(1001, 32);
    const auto down = run(40);
    const float quarter = last(down);
    MP_CHECK(std::fabs(quarter / full - 32.0f / 127.0f) < 0.02f,
             "the held note follows the slider: " + std::to_string(quarter / full) + " of full");
    // In a short fade: the first four blocks stand between the two levels.
    const float first = level(down, 0) / full;
    MP_CHECK(first > 0.55f && first < 0.95f, "in a short fade: " + std::to_string(first) + " of full at first");
    MP_CHECK(proc.voiceStats().activeVoices == 1, "the same voice, still sounding");

    // To nothing, and back.
    proc.setContinuousControl(1001, 0);
    MP_CHECK(last(run(40)) < 1.0e-4f, "the slider at 0 silences the held note");
    proc.setContinuousControl(1001, 127);
    MP_CHECK(std::fabs(last(run(40)) / full - 1.0f) < 0.02f, "and back at full, it sounds as it began");

    // A key pressed with the slider down starts at the slider's level.
    block(off);
    run(400);
    proc.setContinuousControl(1001, 64);
    run(4);
    block(on);
    const float half = last(run(20));
    MP_CHECK(std::fabs(half / full - 64.0f / 127.0f) < 0.02f,
             "a new note starts at the slider's level: " + std::to_string(half / full));

    // The voicing, the same way: the pipe 6 dB down while its key is held.
    mp::PipeVoicing pv = proc.voicing().live().pipe(301);
    pv.gainDb = -6.0f;
    proc.voicing().live().setPipe(301, pv);
    proc.voicingChanged();
    const auto voiced = run(40);
    const float ratio = last(voiced) / half;
    MP_CHECK(std::fabs(ratio - 0.501f) < 0.02f, "the held pipe takes its new voicing: " + std::to_string(ratio));
    MP_CHECK(level(voiced, 0) > last(voiced) * 1.2f, "in a short fade");
    MP_CHECK(proc.voiceStats().activeVoices == 1, "without being played again");
    pv.gainDb = 0.0f;
    proc.voicing().live().setPipe(301, pv);
    proc.voicingChanged();
    MP_CHECK(std::fabs(last(run(40)) / half - 1.0f) < 0.02f, "and back to as recorded");
    block(off);
    run(400);
    proc.voicing().live().clear();

    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static LayerLevelsTest g_layerLevels;

} // namespace

#endif
