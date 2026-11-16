// A noise's voicing (#1160): a noise rank set quieter in Voicing sounds
// quieter. A swell shutter, a blower or any noise its switch sets off starts
// at the rank's voicing, and follows a change made while it sounds, the way
// a pipe played from a key does.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

// A noise rank of one pipe, a two-second tone, set off by switch 1102.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("shutter.wav").createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, {}, 0));
    juce::AudioBuffer<float> tone(1, 96000);
    for (int i = 0; i < 96000; ++i)
      tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 220.0 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 96000);
  }
  const auto odf = defs.getChildFile("noisevoicing.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>NoiseVoicing</Identification_Name>"
      "<Identification_UniqueOrganID>37</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>shutter.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>220</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>202</RankID><Name>Noises</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1102</SwitchID><Name>Shutter</Name><Latching>Y</Latching></Switch>"
      "</ObjectList><ObjectList ObjectType=\"Noise\">"
      "<Noise><RankID>202</RankID><TriggerSwitchID>1102</TriggerSwitchID></Noise>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class NoiseVoicingTest final : public mp::test::Test {
public:
  NoiseVoicingTest() : Test("functional.voicing.noise-rank-level", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-noise-voicing");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    juce::AudioBuffer<float> buf(2, 256);
    const auto block = [&] {
      buf.clear();
      juce::MidiBuffer m;
      proc.processBlock(buf, m);
      return buf.getMagnitude(0, 0, buf.getNumSamples());
    };
    // The noise set off, and its peak over blocks 8 to 16, past the start.
    const auto shutter = [&] {
      proc.setSwitchEngaged(1102, true);
      float peak = 0.0f;
      for (int i = 0; i < 16; ++i) {
        const float p = block();
        if (i >= 8) peak = std::max(peak, p);
      }
      proc.setSwitchEngaged(1102, false);
      for (int i = 0; i < 600; ++i) block();  // to silence: the tone is two seconds
      return peak;
    };
    for (int i = 0; i < 40; ++i) block();
    const float recorded = shutter();
    MP_CHECK(recorded > 0.01f, "the noise sounds when its switch moves: " + std::to_string(recorded));

    // The noise rank 12 dB down in Voicing: the next shutter is 12 dB quieter.
    mp::PipeVoicing pv = proc.voicing().live().rank(202);
    pv.gainDb = -12.0f;
    proc.voicing().live().setRank(202, pv);
    proc.voicingChanged();
    const float voiced = shutter();
    const float ratio = voiced / recorded;
    MP_CHECK(std::fabs(ratio - 0.251f) < 0.02f, "the noise takes the rank's voicing: " + std::to_string(ratio));

    // And back as recorded.
    proc.voicing().live().clear();
    proc.voicingChanged();
    MP_CHECK(std::fabs(shutter() / recorded - 1.0f) < 0.02f, "and back as recorded");

    proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static NoiseVoicingTest g_noiseVoicing;

}  // namespace

#endif
