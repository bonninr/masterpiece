// A pipe whose sample file carries no loop is the sound of a strike, as a
// bell or a carillon pipe is: once struck it rings to the end of its
// recording, whether or not the key is still down (#591), as GrandOrgue plays
// it. A sample that HAS a loop, or a pipe with a release sample to go to, is
// released by the key exactly as before.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

// One tone, one second long, at the pitch of the key it serves so that the
// engine plays it at its own speed. Looped, it carries a sample loop of the
// usual length; struck, it carries no smpl chunk at all.
void writeTone(const juce::File& file, bool looped) {
  constexpr int kFrames = 48000;
  constexpr double kPitchHz = 65.41;
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  juce::StringPairArray meta;
  if (looped) {
    meta.set("NumSampleLoops", "1");
    meta.set("Loop0Start", "1000");
    meta.set("Loop0End", "47000");
  }
  std::unique_ptr<juce::AudioFormatWriter> w(
      fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, kFrames);
  for (int i = 0; i < kFrames; ++i)
    tone.setSample(0, i,
                   static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * kPitchHz * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, kFrames);
}

// One manual, three stops. Stop A is a sustaining pipe whose file loops; stop
// B is a struck one whose file does not; stop C plays the same unlooped file
// with a release sample to go to.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  writeTone(pkg.getChildFile("sustain.wav"), true);
  writeTone(pkg.getChildFile("struck.wav"), false);
  writeTone(pkg.getChildFile("release.wav"), false);
  const auto odf = defs.getChildFile("struck.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Struck</Identification_Name>"
      "<Identification_UniqueOrganID>37</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\">"
      "<Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>sustain.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>65.41</Pitch_ExactSamplePitch></Sample>"
      "<Sample><SampleID>102</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>struck.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>65.41</Pitch_ExactSamplePitch></Sample>"
      "<Sample><SampleID>103</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>release.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>65.41</Pitch_ExactSamplePitch></Sample>"
      "</ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank>"
      "<Rank><RankID>202</RankID><Name>B</Name></Rank><Rank><RankID>203</RankID><Name>C</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>203</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>403</LayerID><PipeID>303</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>102</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>503</UniqueID><LayerID>403</LayerID><SampleID>102</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_ReleaseSample\">"
      "<Pipe_SoundEngine01_ReleaseSample><UniqueID>601</UniqueID><LayerID>403</LayerID><SampleID>103</SampleID></Pipe_SoundEngine01_ReleaseSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>B</Name><DivisionID>801</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "<Stop><StopID>903</StopID><Name>C</Name><DivisionID>801</DivisionID><ControllingSwitchID>1103</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "<StopRank><StopID>903</StopID><RankID>203</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>B</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1103</SwitchID><Name>C</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class PercussivePlaysThroughTest final : public mp::test::Test {
public:
  PercussivePlaysThroughTest()
      : Test("functional.playing.percussive-plays-through", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-percussive");
    const juce::File odf = writeOrgan(root);
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

    constexpr int kBlock = 256;
    juce::AudioBuffer<float> buf(2, kBlock);
    float lastPeak = 0.0f;
    const auto block = [&](juce::MidiBuffer m = {}) {
      buf.clear();
      proc.processBlock(buf, m);
      lastPeak = 0.0f;
      for (int c = 0; c < buf.getNumChannels(); ++c)
        for (int i = 0; i < kBlock; ++i)
          lastPeak = std::max(lastPeak, std::fabs(buf.getSample(c, i)));
    };
    // The loudest sample of any block run, so a check does not depend on the
    // phase the last block happened to stop at.
    float peak = 0.0f;
    const auto run = [&](int n) {
      peak = 0.0f;
      for (int i = 0; i < n; ++i) {
        block();
        peak = std::max(peak, lastPeak);
      }
    };
    const auto voices = [&] { return proc.voiceStats().activeVoices; };
    const auto at = [&] {
      return " (" + std::to_string(voices()) + " voice(s), peak " +
             std::to_string(peak) + ")";
    };
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(1, 36), 0);

    // The control: a file with a loop sustains while the key is held and is
    // released by the key, as it always was.
    proc.setStopEngaged(901, true);
    block(on);
    run(400);  // past the end of the recording
    MP_CHECK(voices() == 1, "a looped sample sustains while the key is held" + at());
    MP_CHECK(peak > 0.02f, "and is audible" + at());
    block(off);
    run(40);  // its release fade is 120 ms
    MP_CHECK(voices() == 0, "the key releases it" + at());

    // The strike: no loop in the file, so the key letting go must leave it
    // to ring to its end rather than cut it.
    proc.setStopEngaged(901, false);
    proc.setStopEngaged(902, true);
    block(on);
    run(4);
    MP_CHECK(voices() == 1, "the struck pipe speaks" + at());
    block(off);
    run(40);  // 213 ms, well past any release fade
    MP_CHECK(voices() == 1, "a sample with no loop rings on after the key is up" + at());
    MP_CHECK(peak > 0.02f, "and is still audible" + at());

    // Struck again while the first is still ringing, it sounds a second time
    // rather than joining the one that is already sounding.
    block(on);
    run(4);
    MP_CHECK(voices() == 2, "a second strike rings alongside the first" + at());
    block(off);
    run(300);  // past the end of both recordings
    MP_CHECK(voices() == 0, "each frees its voice when its recording ends" + at());

    // An unlooped attack with a release sample is released by the key: the
    // release plays, and the voice ends with it.
    proc.setStopEngaged(902, false);
    proc.setStopEngaged(903, true);
    block(on);
    run(20);
    MP_CHECK(voices() == 1, "the pipe with a release speaks" + at());
    block(off);
    run(250);  // past the release, well short of the attack's own end
    MP_CHECK(voices() == 0, "the key lets it go into its release" + at());
  }
};

PercussivePlaysThroughTest percussivePlaysThroughTest;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
