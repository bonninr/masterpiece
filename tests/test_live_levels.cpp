// Live levels (LiveLevels.h): the law and the smoothing on their own, then on
// a two-manual organ with a rank in each division -- a rank's fader, a
// division's and a pair's heard on a note already sounding, a MIDI knob
// learned for one, and the control API's view of them. With every fader at
// full the engine is handed nothing, and the organ sounds exactly as without
// them.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#if MP_ENABLE_API
#include "../src/mp_api/ControlModel.h"
#endif
#include "../src/mp_audio/LiveLevels.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

class LiveLevelsLawTest final : public mp::test::Test {
public:
  LiveLevelsLawTest() : Test("functional.levels.law", Category::Functional) {}
  void run() override {
    using mp::LiveLevels;
    MP_CHECK(LiveLevels::gainOf(127) == 1.0f && LiveLevels::gainOf(0) == 0.0f, "full is unity, 0 is silence");
    MP_CHECK(std::fabs(LiveLevels::dbOf(1) + 40.0f * 126.0f / 127.0f) < 1.0e-3f, "the lowest step near -40 dB");
    MP_CHECK(LiveLevels::gainOf(64) < LiveLevels::gainOf(65) && LiveLevels::gainOf(65) < 1.0f, "rising");

    LiveLevels l;
    l.configure({0, 1, -1}, 2);
    MP_CHECK(!l.advance(256, 48000.0), "untouched, nothing to apply");
    l.setRank(0, 0);
    MP_CHECK(l.advance(256, 48000.0), "a fader down: applied");
    const float first = l.rankGain(0);
    MP_CHECK(first < 1.0f && first > 0.5f, "smoothed, not stepped: " + std::to_string(first));
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(l.rankGain(0) == 0.0f, "settled at silence");
    l.setDivision(1, 64);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(std::fabs(l.rankGain(1) - LiveLevels::gainOf(64)) < 1.0e-5f, "a division's fader reaches its ranks");
    MP_CHECK(l.rankGain(2) == 1.0f, "a rank in no division is left alone");
    l.setPair(3, 0);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(l.pairGain(3) == 0.0f && l.pairGain(0) == 1.0f, "pairs, each its own");
    l.setRank(0, 127);
    l.setDivision(1, 127);
    l.setPair(3, 127);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(!l.advance(256, 48000.0), "all back at full: nothing to apply again");
    l.setRank(7, 0);
    l.setRank(-1, 0);
    MP_CHECK(l.rank(7) == 127 && !l.advance(256, 48000.0), "an index out of range is ignored");
    l.setRank(1, 300);
    MP_CHECK(l.rank(1) == 127, "held to 0..127");
  }
};
static LiveLevelsLawTest g_law;

// Two manuals, a rank on each: A in division G on manual I (channel 1),
// B in division S on manual II (channel 2).
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
      "<Identification_UniqueOrganID>34</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank><Rank><RankID>202</RankID><Name>B</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>"
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "<Keyboard><KeyboardID>702</KeyboardID><Name>II</Name><DefaultInputOutputKeyboardAsgnCode>2</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division>"
      "<Division><DivisionID>802</DivisionID><Name>S</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "<KeyAction><SourceKeyboardID>702</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>802</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>A</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "<Stop><StopID>902</StopID><Name>B</Name><DivisionID>802</DivisionID><ControllingSwitchID>1102</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">"
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>B</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

// The organ, its two stops drawn, and a block renderer.
struct Rig {
  mp::MasterpieceProcessor proc;
  juce::AudioBuffer<float> buf{2, 256};
  bool loaded = false;
  explicit Rig(const juce::File& odf) {
    proc.prepareToPlay(48000.0, 256);
    loaded = proc.loadOrgan(odf, 0, false).ok;
    proc.setStopEngaged(901, true);
    proc.setStopEngaged(902, true);
  }
  float block(juce::MidiBuffer m = {}) {
    buf.clear();
    proc.processBlock(buf, m);
    return buf.getRMSLevel(0, 0, buf.getNumSamples());
  }
  // The level after the faders have had time to settle.
  // Measured over the last sixteen blocks: one block holds less than one
  // period of the tone at a fraction, and its RMS depends on where it starts.
  float settle(int blocks = 60) {
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < blocks; ++i) {
      const float rms = block();
      if (i >= blocks - 16) {
        sum += static_cast<double>(rms) * rms;
        ++n;
      }
    }
    return n > 0 ? static_cast<float>(std::sqrt(sum / n)) : 0.0f;
  }
};

class LiveLevelsOrganTest final : public mp::test::Test {
public:
  LiveLevelsOrganTest() : Test("functional.levels.organ", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-live-levels");
    const juce::File odf = writeOrgan(root);

    // The same performance on two organs: one never touches a fader, the
    // other pulls them all down and puts them back before playing. Equal to
    // the last bit.
    Rig plain(odf), touched(odf);
    MP_CHECK(plain.loaded && touched.loaded, "the organ loads");
    MP_CHECK(touched.proc.liveRankCount() == 2 && touched.proc.liveDivisionCount() == 2, "a fader per rank and division");
    touched.proc.setLiveRankLevel(0, 0);
    touched.proc.setLiveDivisionLevel(1, 10);
    touched.proc.setLivePairLevel(0, 30);
    plain.settle(100);
    touched.settle(100);
    touched.proc.setLiveRankLevel(0, 127);
    touched.proc.setLiveDivisionLevel(1, 127);
    touched.proc.setLivePairLevel(0, 127);
    plain.settle(100);
    touched.settle(100);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    on.addEvent(juce::MidiMessage::noteOn(2, 36, 0.8f), 0);
    bool same = true;
    for (int i = 0; i < 60; ++i) {
      plain.block(i == 0 ? on : juce::MidiBuffer());
      touched.block(i == 0 ? on : juce::MidiBuffer());
      for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < 256; ++s)
          same = same && plain.buf.getSample(ch, s) == touched.buf.getSample(ch, s);
    }
    MP_CHECK(same, "every fader back at full: the output is bit-identical to an organ never touched");

    // Both notes now sound on `touched`. Its rank A's fader down: heard on
    // the note already sounding.
    auto& p = touched.proc;
    const float both = touched.settle(30);
    MP_CHECK(both > 0.01f, "both ranks sound: " + std::to_string(both));
    p.setLiveRankLevel(p.liveRankIndexOf(201), 0);
    const float oneRank = touched.settle();
    MP_CHECK(oneRank < both * 0.75f && oneRank > both * 0.25f,
             "rank A silenced while held, B sounds on: " + std::to_string(oneRank) + " of " + std::to_string(both));
    p.setLiveDivisionLevel(p.liveDivisionIndexOf(802), 0);
    MP_CHECK(touched.settle(120) < 1.0e-5f, "and division S's fader takes B");
    p.setLiveRankLevel(p.liveRankIndexOf(201), 127);
    p.setLiveDivisionLevel(p.liveDivisionIndexOf(802), 127);
    const float back = touched.settle();
    MP_CHECK(std::fabs(back - both) < both * 0.02f, "both back up: " + std::to_string(back));
    p.setLivePairLevel(0, 0);
    MP_CHECK(touched.settle(120) < 1.0e-5f, "the first pair's fader takes everything through it");
    p.setLivePairLevel(0, 127);
    touched.settle();

    // A knob: controller 7 on channel 3 learned for rank B.
    mp::MidiBinding knob;
    knob.source.kind = mp::MidiSourceKind::ControlChange;
    knob.source.channel = 3;
    knob.source.number = 7;
    knob.targetKind = mp::MidiTargetKind::RankLevel;
    knob.targetId = 202;
    p.midiMap().bind(knob);
    juce::MidiBuffer cc;
    cc.addEvent(juce::MidiMessage::controllerEvent(3, 7, 40), 0);
    touched.block(cc);
    MP_CHECK(p.liveRankLevel(p.liveRankIndexOf(202)) == 40, "the knob sets rank B's fader");
    MP_CHECK(p.liveRankLevel(p.liveRankIndexOf(201)) == 127, "and leaves A's");

#if MP_ENABLE_API
    // The control API's view.
    mp::api::ControlModel model(p);
    mp::api::Error e;
    int v = 0;
    MP_CHECK(model.count(mp::api::Type::Ranklevel) == 2 && model.count(mp::api::Type::Pair) >= 1, "listed");
    MP_CHECK(model.get(mp::api::Type::Ranklevel, 1, v, e) && v == 40, "rank B's fader read: " + std::to_string(v));
    v = 90;
    MP_CHECK(model.set(mp::api::Type::Division, 0, v) == mp::api::Error::None &&
                 p.liveDivisionLevel(p.liveDivisionIndexOf(801)) == 90,
             "a division's fader set");
    v = 128;
    MP_CHECK(model.set(mp::api::Type::Pair, 0, v) == mp::api::Error::BadValue, "0..127 only");
#endif

    // A new organ starts every fader at full.
    MP_CHECK(p.loadOrgan(odf, 0, false).ok, "loads again");
    MP_CHECK(p.liveRankLevel(1) == 127 && p.liveDivisionLevel(0) == 127, "at full again");

    plain.proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static LiveLevelsOrganTest g_organ;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
