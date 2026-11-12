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
#include <vector>

namespace {

using mp::test::Category;

class LiveLevelsLawTest final : public mp::test::Test {
public:
  LiveLevelsLawTest() : Test("functional.levels.law", Category::Functional) {}
  void run() override {
    using mp::LiveLevels;
    MP_CHECK(LiveLevels::gainOf(127) == 1.0f && LiveLevels::gainOf(0) == 0.0f, "full is unity, 0 is silence");
    MP_CHECK(std::fabs(LiveLevels::dbOf(1) + 40.0f * 126.0f / 127.0f) < 1.0e-3f, "the lowest step near -40 dB");
    MP_CHECK(LiveLevels::dbOf(127) == 0.0f && !std::signbit(LiveLevels::dbOf(127)) &&
                 !std::signbit(LiveLevels::dbOf(200)),
             "full reads 0 dB, with no minus sign");
    MP_CHECK(LiveLevels::gainOf(64) < LiveLevels::gainOf(65) && LiveLevels::gainOf(65) < 1.0f, "rising");
    // Above full, up to 24 dB over the voicing (#971), in steps about the
    // size of those below.
    MP_CHECK(std::fabs(LiveLevels::dbOf(LiveLevels::kTop) - 24.0f) < 1.0e-4f &&
                 std::fabs(LiveLevels::gainOf(LiveLevels::kTop) - std::pow(10.0f, 24.0f / 20.0f)) < 1.0e-3f,
             "the top is +24 dB");
    MP_CHECK(std::fabs(LiveLevels::dbOf(165) - 12.0f) < 1.0e-4f, "halfway up the boost is +12 dB");
    const float stepUp = LiveLevels::dbOf(LiveLevels::kFull + 1), stepDown = -LiveLevels::dbOf(LiveLevels::kFull - 1);
    MP_CHECK(stepUp > 0.0f && std::fabs(stepUp - stepDown) < 0.01f,
             "a step above full is the size of one below: " + std::to_string(stepUp) + " and " +
                 std::to_string(stepDown) + " dB");
    MP_CHECK(LiveLevels::dbOf(LiveLevels::kTop + 50) == LiveLevels::dbOf(LiveLevels::kTop), "nothing past the top");
    bool roundTrip = true;
    for (int v = 1; v <= LiveLevels::kTop; ++v) roundTrip = roundTrip && LiveLevels::valueOf(LiveLevels::dbOf(v)) == v;
    MP_CHECK(roundTrip, "a level typed in dB lands on its own step, every step");
    MP_CHECK(LiveLevels::valueOf(0.0f) == LiveLevels::kFull && LiveLevels::valueOf(-90.0f) == 1 &&
                 LiveLevels::valueOf(90.0f) == LiveLevels::kTop,
             "0 dB is full; past either end, the end step");

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
    MP_CHECK(l.rank(1) == LiveLevels::kTop, "held to 0..203");
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(l.advance(256, 48000.0) && std::fabs(l.rankGain(1) - LiveLevels::gainOf(LiveLevels::kTop)) < 1.0e-3f,
             "a fader raised is applied as one lowered: " + std::to_string(l.rankGain(1)));
    l.setRank(1, LiveLevels::kFull);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(!l.advance(256, 48000.0), "and back at full, nothing to apply");
  }
};
static LiveLevelsLawTest g_law;

// Two manuals, a rank on each: A in division G on manual I (channel 1),
// B in division S on manual II (channel 2). `tremmed` gives stop A a tremmed
// rank as well, rank 203, which switch 1103 plays in place of A.
juce::File writeOrgan(const juce::File& root, bool tremmed = false) {
  const auto ifTremmed = [tremmed](const char* text) { return juce::String(tremmed ? text : ""); };
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
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank><Rank><RankID>202</RankID><Name>B</Name></Rank>" +
      ifTremmed("<Rank><RankID>203</RankID><Name>A trem</Name></Rank>") +
      "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
      "<Pipe_SoundEngine01><PipeID>302</PipeID><RankID>202</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>" +
      ifTremmed("<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>203</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>") +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
      "<Pipe_SoundEngine01_Layer><LayerID>401</LayerID><PipeID>301</PipeID></Pipe_SoundEngine01_Layer>"
      "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID></Pipe_SoundEngine01_Layer>" +
      ifTremmed("<Pipe_SoundEngine01_Layer><LayerID>403</LayerID><PipeID>303</PipeID></Pipe_SoundEngine01_Layer>") +
      "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>501</UniqueID><LayerID>401</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>"
      "<Pipe_SoundEngine01_AttackSample><UniqueID>502</UniqueID><LayerID>402</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>" +
      ifTremmed("<Pipe_SoundEngine01_AttackSample><UniqueID>503</UniqueID><LayerID>403</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>") +
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
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes>" +
      ifTremmed("<AlternateRankID>203</AlternateRankID><SwitchIDToSwitchToAlternateRank>1103</SwitchIDToSwitchToAlternateRank>") +
      "</StopRank>"
      "<StopRank><StopID>902</StopID><RankID>202</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1102</SwitchID><Name>B</Name><Latching>Y</Latching></Switch>" +
      ifTremmed("<Switch><SwitchID>1103</SwitchID><Name>Trem</Name><Latching>Y</Latching></Switch>") +
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
    touched.proc.setLiveRankLevel(1, mp::LiveLevels::kTop);
    plain.settle(100);
    touched.settle(100);
    touched.proc.setLiveRankLevel(0, 127);
    touched.proc.setLiveDivisionLevel(1, 127);
    touched.proc.setLivePairLevel(0, 127);
    touched.proc.setLiveRankLevel(1, 127);
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

    // Raised above full (#971): rank A alone, then 12 dB over its voicing.
    p.setLiveDivisionLevel(p.liveDivisionIndexOf(802), 0);
    const float aAlone = touched.settle(120);
    p.setLiveRankLevel(p.liveRankIndexOf(201), 165);
    const float aRaised = touched.settle(120);
    const float ratio = aAlone > 0.0f ? aRaised / aAlone : 0.0f;
    MP_CHECK(aAlone > 1.0e-4f && std::fabs(ratio - mp::LiveLevels::gainOf(165)) < 0.02f * mp::LiveLevels::gainOf(165),
             "rank A raised 12 dB while held: x" + std::to_string(ratio) + ", law x" +
                 std::to_string(mp::LiveLevels::gainOf(165)));
    p.setLiveRankLevel(p.liveRankIndexOf(201), 127);
    p.setLiveDivisionLevel(p.liveDivisionIndexOf(802), 127);
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
    v = 203;
    MP_CHECK(model.set(mp::api::Type::Pair, 0, v) == mp::api::Error::None && p.livePairLevel(0) == 203,
             "a pair raised to the top, +24 dB");
    v = 204;
    MP_CHECK(model.set(mp::api::Type::Pair, 0, v) == mp::api::Error::BadValue, "0..203 only");
    // Mute and solo: the ranks first, then the divisions, groups and pairs.
    const int strips = model.count(mp::api::Type::Mute);
    MP_CHECK(strips >= 2 + 1 + 1 && model.count(mp::api::Type::Solo) == strips, "a mute and a solo per strip");
    v = 1;
    MP_CHECK(model.set(mp::api::Type::Mute, 1, v) == mp::api::Error::None &&
                 p.liveMuted(mp::LiveLevels::Strip::Rank, 1),
             "rank B muted from the API");
    v = 2;
    MP_CHECK(model.set(mp::api::Type::Mute, 1, v) == mp::api::Error::None &&
                 !p.liveMuted(mp::LiveLevels::Strip::Rank, 1) && v == 0,
             "toggled back: " + std::to_string(v));
    v = 1;
    MP_CHECK(model.set(mp::api::Type::Solo, 2, v) == mp::api::Error::None &&
                 p.liveSoloed(mp::LiveLevels::Strip::Division, 0) && model.get(mp::api::Type::Solo, 2, v, e) && v == 1,
             "the first division soloed, and read back");
    v = 0;
    model.set(mp::api::Type::Solo, 2, v);
    v = 3;
    MP_CHECK(model.set(mp::api::Type::Mute, 0, v) == mp::api::Error::BadValue, "0, 1 or 2 only");
#endif

    // A new organ starts every fader at full.
    MP_CHECK(p.loadOrgan(odf, 0, false).ok, "loads again");
    MP_CHECK(p.liveRankLevel(1) == 127 && p.liveDivisionLevel(0) == 127, "at full again");

    plain.proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static LiveLevelsOrganTest g_organ;

// A stop's tremmed rank, swapped in by its switch, is in the stop's division:
// the division's fader takes it as it takes the plain rank (#637).
class LiveLevelsTremmedRankTest final : public mp::test::Test {
public:
  LiveLevelsTremmedRankTest() : Test("functional.levels.tremmed-rank", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-live-levels-trem");
    const juce::File odf = writeOrgan(root, true);
    for (const bool trem : {false, true}) {
      Rig rig(odf);
      MP_CHECK(rig.loaded, "the organ loads");
      auto& p = rig.proc;
      MP_CHECK(p.liveRankCount() == 3, "a fader for the tremmed rank too");
      p.setSwitchEngaged(1103, trem);
      rig.settle(10);
      juce::MidiBuffer on;
      on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
      rig.block(on);
      const std::string what = trem ? "tremmed rank" : "plain rank";
      const float full = rig.settle(30);
      MP_CHECK(full > 0.01f, what + " sounds: " + std::to_string(full));
      p.setLiveDivisionLevel(p.liveDivisionIndexOf(801), 0);
      const float down = rig.settle(120);
      MP_CHECK(down < 1.0e-5f, what + ": division G's fader takes it, " + std::to_string(down));
      p.setLiveDivisionLevel(p.liveDivisionIndexOf(801), 127);
      const float back = rig.settle();
      MP_CHECK(std::fabs(back - full) < full * 0.02f, what + " back up: " + std::to_string(back));
      p.settingsFileFor(odf).deleteFile();
    }
    root.deleteRecursively();
  }
};
static LiveLevelsTremmedRankTest g_tremmed;

// The attack's emphasis: each pipe begins louder, or softer, and comes to
// its own level in a straight line. Off, the organ sounds as without it.
class AttackEmphasisTest final : public mp::test::Test {
public:
  AttackEmphasisTest() : Test("functional.voicing.attack", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-attack-emphasis");
    const juce::File odf = writeOrgan(root);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    const auto mean = [](const std::vector<float>& v, size_t from, size_t to) {
      double sum = 0.0;
      for (size_t i = from; i < to; ++i) sum += static_cast<double>(v[i]) * v[i];
      return static_cast<float>(std::sqrt(sum / static_cast<double>(to - from)));
    };

    Rig plain(odf), off(odf), louder(odf), softer(odf);
    MP_CHECK(plain.loaded && off.loaded && louder.loaded && softer.loaded, "the organ loads");
    off.proc.setAttack(6.0f, 100);
    off.proc.setAttack(0.0f, 100);
    louder.proc.setAttack(6.0f, 100);
    softer.proc.setAttack(-12.0f, 100);
    MP_CHECK(louder.proc.attackDb() == 6.0f && louder.proc.attackMs() == 100, "set");

    bool same = true;
    std::vector<float> ref, more, less;
    for (int i = 0; i < 80; ++i) {
      const juce::MidiBuffer m = i == 0 ? on : juce::MidiBuffer();
      ref.push_back(plain.block(m));
      off.block(m);
      more.push_back(louder.block(m));
      less.push_back(softer.block(m));
      for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < 256; ++s) same = same && plain.buf.getSample(ch, s) == off.buf.getSample(ch, s);
    }
    MP_CHECK(same, "set and put back to 0 dB: bit-identical to an organ never touched");
    // The first 50 ms against the plain organ, and after the attack.
    const float r0 = mean(ref, 1, 10), m0 = mean(more, 1, 10), l0 = mean(less, 1, 10);
    MP_CHECK(m0 > r0 * 1.2f, "+6 dB: the start is louder, " + std::to_string(m0) + " against " + std::to_string(r0));
    MP_CHECK(l0 < r0 * 0.8f, "-12 dB: the start is softer, " + std::to_string(l0) + " against " + std::to_string(r0));
    const float r1 = mean(ref, 40, 80), m1 = mean(more, 40, 80), l1 = mean(less, 40, 80);
    MP_CHECK(std::fabs(m1 - r1) < r1 * 0.01f && std::fabs(l1 - r1) < r1 * 0.01f,
             "after the attack, the pipe at its own level");

    // Saved with the organ and read back; a new organ without it starts off.
    MP_CHECK(louder.proc.saveSettings(), "saved");
    Rig again(odf);
    MP_CHECK(again.proc.attackDb() == 6.0f && again.proc.attackMs() == 100, "read back with the organ");
    again.proc.setAttack(0.0f, 40);
    MP_CHECK(again.proc.saveSettings(), "saved off");

    plain.proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static AttackEmphasisTest g_attack;

// One stop of six ranks, three recorded from the front and three from the
// rear (#930): a fader for each perspective takes its three ranks at once.
juce::File writePerspectiveOrgan(const juce::File& root) {
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
      tone.setSample(0, i, static_cast<float>(0.05 * std::sin(2.0 * 3.14159265 * 130.81 * i / 48000.0)));
    w->writeFromAudioSampleBuffer(tone, 0, 48000);
  }
  juce::String ranks, pipes, layers, attacks, stopRanks;
  const char* names[] = {"Prestant 8 (front)", "Octave 4 (front)", "Mixture (front)",
                         "Prestant 8 (rear)",  "Octave 4 (rear)",  "Mixture (rear)"};
  for (int i = 0; i < 6; ++i) {
    const juce::String n(i + 1);
    ranks += "<Rank><RankID>20" + n + "</RankID><Name>" + names[i] + "</Name></Rank>";
    pipes += "<Pipe_SoundEngine01><PipeID>30" + n + "</PipeID><RankID>20" + n +
             "</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>";
    layers += "<Pipe_SoundEngine01_Layer><LayerID>40" + n + "</LayerID><PipeID>30" + n +
              "</PipeID></Pipe_SoundEngine01_Layer>";
    attacks += "<Pipe_SoundEngine01_AttackSample><UniqueID>50" + n + "</UniqueID><LayerID>40" + n +
               "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
    stopRanks += "<StopRank><StopID>901</StopID><RankID>20" + n +
                 "</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
                 "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>";
  }
  const auto odf = defs.getChildFile("perspectives.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Perspectives</Identification_Name>"
      "<Identification_UniqueOrganID>35</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\">" + ranks + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers + "</ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks + "</ObjectList>"
      "<ObjectList ObjectType=\"Keyboard\">"
      "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
      "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
      "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
      "<ObjectList ObjectType=\"KeyAction\">"
      "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
      "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
      "</ObjectList><ObjectList ObjectType=\"Stop\">"
      "<Stop><StopID>901</StopID><Name>Plenum</Name><DivisionID>801</DivisionID><ControllingSwitchID>1101</ControllingSwitchID></Stop>"
      "</ObjectList><ObjectList ObjectType=\"StopRank\">" + stopRanks + "</ObjectList>"
      "<ObjectList ObjectType=\"Switch\"><Switch><SwitchID>1101</SwitchID><Name>Plenum</Name><Latching>Y</Latching></Switch>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

class LiveLevelsGroupsTest final : public mp::test::Test {
public:
  LiveLevelsGroupsTest() : Test("functional.levels.perspective-groups", Category::Functional) {}
  void run() override {
    // The table on its own: a group's fader reaches its ranks and no others.
    mp::LiveLevels l;
    l.configure({0, 0, 0}, 1, {0, 1, -1}, 2);
    MP_CHECK(l.groupCount() == 2 && l.rankGroup(0) == 0 && l.rankGroup(2) == -1, "a group per rank, or none");
    l.setGroup(1, 0);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(l.rankGain(1) == 0.0f && l.rankGain(0) == 1.0f && l.rankGain(2) == 1.0f, "group 2 takes its rank only");
    l.setDivision(0, 64);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(std::fabs(l.rankGain(0) - mp::LiveLevels::gainOf(64)) < 1.0e-5f, "the division's fader on top");
    l.setGroup(1, 127);
    l.setDivision(0, 127);
    for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
    MP_CHECK(!l.advance(256, 48000.0), "all back at full: nothing to apply");
    mp::LiveLevels none;
    none.configure({0, -1}, 1);
    MP_CHECK(none.groupCount() == 0 && none.rankGroup(0) == -1, "an organ with no groups has no group faders");

    // On the organ: front and rear found from the rank names.
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-live-groups");
    const juce::File odf = writePerspectiveOrgan(root);
    Rig plain(odf), rig(odf);
    MP_CHECK(rig.loaded && plain.loaded, "the organ loads");
    auto& p = rig.proc;
    p.setStopEngaged(901, true);
    plain.proc.setStopEngaged(901, true);
    const auto& names = p.liveGroupNames();
    MP_CHECK(names.size() == 2 && names[0] == "front" && names[1] == "rear" && p.liveGroupCount() == 2,
             "a fader for each perspective, front and rear");
    for (int i = 0; i < 6; ++i)
      MP_CHECK(p.liveGroupOfRank(p.liveRankIndexOf(201 + i)) == (i < 3 ? 0 : 1),
               "rank " + std::to_string(201 + i) + " under its perspective");

    // Rear pulled down and put back before playing: the same to the bit.
    p.setLiveGroupLevel(1, 0);
    rig.settle(100);
    plain.settle(100);
    p.setLiveGroupLevel(1, 127);
    rig.settle(100);
    plain.settle(100);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    bool same = true;
    for (int i = 0; i < 60; ++i) {
      plain.block(i == 0 ? on : juce::MidiBuffer());
      rig.block(i == 0 ? on : juce::MidiBuffer());
      for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < 256; ++s) same = same && plain.buf.getSample(ch, s) == rig.buf.getSample(ch, s);
    }
    MP_CHECK(same, "the rear's fader back at full: bit-identical to an organ never touched");

    // The six ranks play one tone in phase: the rear's three away halve it.
    const float all = rig.settle(30);
    MP_CHECK(all > 0.01f, "all six sound: " + std::to_string(all));
    p.setLiveGroupLevel(1, 0);
    const float front = rig.settle(120);
    MP_CHECK(std::fabs(front - all * 0.5f) < all * 0.05f,
             "the rear's fader takes its three ranks: " + std::to_string(front) + " of " + std::to_string(all));
    p.setLiveGroupLevel(0, 0);
    MP_CHECK(rig.settle(120) < 1.0e-5f, "and the front's takes the rest");

    // A knob learned for the front, by its place from 1.
    mp::MidiBinding knob;
    knob.source.kind = mp::MidiSourceKind::ControlChange;
    knob.source.channel = 3;
    knob.source.number = 7;
    knob.targetKind = mp::MidiTargetKind::GroupLevel;
    knob.targetId = 1;
    p.midiMap().bind(knob);
    juce::MidiBuffer cc;
    cc.addEvent(juce::MidiMessage::controllerEvent(3, 7, 127), 0);
    rig.block(cc);
    MP_CHECK(p.liveGroupLevel(0) == 127 && p.liveGroupLevel(1) == 0, "the knob sets the front's fader only");
    const float knobbed = rig.settle(120);
    MP_CHECK(std::fabs(knobbed - front) < all * 0.05f, "the front heard again: " + std::to_string(knobbed));
    const std::string text = p.midiMap().toText();
    mp::MidiMap reread;
    MP_CHECK(text.find("group-level") != std::string::npos && reread.fromText(text) &&
                 !reread.bindingsFor(mp::MidiTargetKind::GroupLevel, 1).empty(),
             "saved in the map file by name and read back");

    plain.proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static LiveLevelsGroupsTest g_groups;

// The mixer's strips: mute and solo on each fader, and a meter per strip.
// Mute and solo leave the faders where they are; a solo on a rank, a
// division or a group leaves only the ranks under it; mute wins. The meters
// measure only while asked for, and the output with them off is the output
// of an organ that never had them.
class LiveLevelsStripsTest final : public mp::test::Test {
public:
  LiveLevelsStripsTest() : Test("functional.levels.strips", Category::Functional) {}
  void run() override {
    using mp::LiveLevels;
    using Strip = LiveLevels::Strip;
    {
      // Rank 0 in division 0, rank 1 in division 1, rank 2 in none.
      LiveLevels l;
      l.configure({0, 1, -1}, 2);
      const auto settle = [&l] {
        for (int i = 0; i < 200; ++i) l.advance(256, 48000.0);
      };
      l.setMuted(Strip::Rank, 0, true);
      MP_CHECK(l.advance(256, 48000.0) && l.rankGain(0) > 0.0f && l.rankGain(0) < 1.0f,
               "a mute fades, it does not click: " + std::to_string(l.rankGain(0)));
      settle();
      MP_CHECK(l.rankGain(0) == 0.0f && l.rankGain(1) == 1.0f && l.rank(0) == LiveLevels::kFull,
               "a muted rank is silent, its fader where it was, the others untouched");
      l.setMuted(Strip::Rank, 0, false);
      l.setSoloed(Strip::Division, 1, true);
      settle();
      MP_CHECK(l.rankGain(0) == 0.0f && l.rankGain(1) == 1.0f && l.rankGain(2) == 0.0f,
               "a soloed division: only its ranks sound");
      l.setSoloed(Strip::Rank, 2, true);
      settle();
      MP_CHECK(l.rankGain(0) == 0.0f && l.rankGain(1) == 1.0f && l.rankGain(2) == 1.0f,
               "a rank soloed as well joins it");
      l.setMuted(Strip::Rank, 1, true);
      settle();
      MP_CHECK(l.rankGain(1) == 0.0f && l.soloed(Strip::Division, 1), "mute wins over solo");
      l.setMuted(Strip::Rank, 1, false);
      l.setSoloed(Strip::Rank, 2, false);
      l.setSoloed(Strip::Division, 1, false);
      l.setSoloed(Strip::Pair, 3, true);
      settle();
      MP_CHECK(l.pairGain(3) == 1.0f && l.pairGain(0) == 0.0f && l.rankGain(0) == 1.0f,
               "a soloed pair: the other pairs silent, the ranks' own gains untouched");
      l.setMuted(Strip::Pair, 3, true);
      settle();
      MP_CHECK(l.pairGain(3) == 0.0f, "a muted pair under a solo is silent");
      l.setSoloed(Strip::Pair, 3, false);
      l.setMuted(Strip::Pair, 3, false);
      settle();
      MP_CHECK(!l.advance(256, 48000.0), "every button off: nothing to apply again");
      l.setMuted(Strip::Rank, 9, true);
      MP_CHECK(!l.muted(Strip::Rank, 9), "a strip out of range is ignored");
    }

    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-live-strips");
    const juce::File odf = writeOrgan(root);
    Rig plain(odf), metered(odf);
    MP_CHECK(plain.loaded && metered.loaded, "the organ loads");
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
    on.addEvent(juce::MidiMessage::noteOn(2, 36, 0.8f), 0);
    // Metered for 100 blocks, two readings at four a second (one every 44
    // blocks: 11 slices of the voices, each watched 4 blocks), then not:
    // equal to the plain organ within rounding while on, and to the last bit
    // once off again.
    metered.proc.setStripMeters(true);
    float worst = 0.0f;
    bool sameAfter = true;
    int readings = 0;
    float last = 0.0f;
    for (int i = 0; i < 140; ++i) {
      if (i == 100) metered.proc.setStripMeters(false);
      plain.block(i == 0 ? on : juce::MidiBuffer());
      metered.block(i == 0 ? on : juce::MidiBuffer());
      for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < 256; ++s) {
          const float d = std::fabs(plain.buf.getSample(ch, s) - metered.buf.getSample(ch, s));
          if (i < 100) worst = std::max(worst, d);
          else sameAfter = sameAfter && d == 0.0f;
        }
      if (i < 100) {
        const float now = metered.proc.stripLevel(Strip::Rank, 0);
        if (now != last) ++readings;
        last = now;
      }
      if (i == 99) {
        MP_CHECK(readings >= 1 && readings <= 2, "a reading every quarter of a second, the level standing between: " +
                                    std::to_string(readings) + " in 100 blocks of 256 at 48 kHz");
        const float a = metered.proc.stripLevel(Strip::Rank, 0), b = metered.proc.stripLevel(Strip::Rank, 1);
        const float g = metered.proc.stripLevel(Strip::Division, 0);
        const float pair = metered.proc.stripLevel(Strip::Pair, 0);
        const float out = std::max(metered.proc.outputPeak(0), metered.proc.outputPeak(1));
        MP_CHECK(a > 0.01f && b > 0.01f, "each rank's meter reads its pipe: " + std::to_string(a) + ", " + std::to_string(b));
        MP_CHECK(std::fabs(g - a) < 1.0e-6f, "a division with one rank reads that rank");
        // The pair reads its own signal, the two ranks in phase: louder than
        // either, and the output meter's level, the master volume being at 0 dB.
        MP_CHECK(pair > std::max(a, b), "the pair hears both ranks: " + std::to_string(pair));
        MP_CHECK(std::fabs(pair - out) < out * 0.1f,
                 "the pair reads the output's signal: " + std::to_string(pair) + " against " + std::to_string(out));
        MP_CHECK(metered.proc.stripLevel(Strip::Rank, 7) == 0.0f, "a strip out of range reads nothing");
      }
    }
    // The master volume at -15 dB: the pair reads what leaves, as the output
    // meter does, 15 dB down (#1047). Measured before the master it read the
    // unity level and lit red over a quiet output.
    {
      Rig quiet(odf);
      MP_CHECK(quiet.loaded, "the organ loads again");
      if (auto* gp = quiet.proc.apvts().getParameter("masterGain"))
        gp->setValueNotifyingHost(gp->convertTo0to1(juce::Decibels::decibelsToGain(-15.0f)));
      quiet.proc.setStripMeters(true);
      for (int i = 0; i < 100; ++i) quiet.block(i == 0 ? on : juce::MidiBuffer());
      const float pair = quiet.proc.stripLevel(Strip::Pair, 0);
      const float out = std::max(quiet.proc.outputPeak(0), quiet.proc.outputPeak(1));
      const float rank = quiet.proc.stripLevel(Strip::Rank, 0);
      MP_CHECK(out > 1.0e-4f && std::fabs(pair - out) < out * 0.1f,
               "master at -15 dB: the pair reads the output: " + std::to_string(pair) + " against " +
                   std::to_string(out));
      MP_CHECK(pair < rank, "master at -15 dB: the pair below the rank it carries: " + std::to_string(pair) +
                                " against " + std::to_string(rank));
    }
    MP_CHECK(worst < 1.0e-6f, "metered, the output is the same within rounding: " + std::to_string(worst));
    MP_CHECK(sameAfter, "the meters off: bit-identical to an organ never metered");
    MP_CHECK(metered.proc.stripLevel(Strip::Rank, 0) == 0.0f, "the meters off: dark");

    // On the organ: rank A muted, B heard alone; then division G soloed, A
    // alone. Within 10%: sixteen blocks hold a few periods of the tone, and
    // where the window falls moves its RMS by a few percent; a strip off
    // halves it.
    const float both = metered.settle(30);
    metered.proc.setLiveMuted(Strip::Rank, 0, true);
    const float bOnly = metered.settle(120);
    MP_CHECK(std::fabs(bOnly - both * 0.5f) < both * 0.1f,
             "rank A muted: B alone, half the two in phase: " + std::to_string(bOnly) + " of " + std::to_string(both));
    MP_CHECK(metered.proc.liveRankLevel(0) == LiveLevels::kFull, "the fader stays at full");
    metered.proc.setLiveMuted(Strip::Rank, 0, false);
    metered.proc.setLiveSoloed(Strip::Division, 0, true);
    const float aOnly = metered.settle(120);
    MP_CHECK(std::fabs(aOnly - both * 0.5f) < both * 0.1f, "division G soloed: A alone: " + std::to_string(aOnly));
    metered.proc.setLiveSoloed(Strip::Division, 0, false);
    const float again = metered.settle(120);
    MP_CHECK(std::fabs(again - both) < both * 0.1f,
             "every button off: both again: " + std::to_string(again) + " of " + std::to_string(both));

    // Mute and solo from MIDI buttons: each press toggles, a release does
    // nothing, and the bindings are written to the map file.
    {
      auto& p = metered.proc;
      const auto bindCc = [&](int cc, mp::MidiTargetKind kind, mp::Id target) {
        mp::MidiBinding b;
        b.source.kind = mp::MidiSourceKind::ControlChange;
        b.source.channel = 16;
        b.source.number = cc;
        b.targetKind = kind;
        b.targetId = target;
        p.midiMap().bind(b);
      };
      const mp::Id muteA = mp::stripTarget(static_cast<int>(Strip::Rank), 201);
      const mp::Id soloPair = mp::stripTarget(static_cast<int>(Strip::Pair), 1);
      MP_CHECK(mp::stripTargetStrip(muteA) == static_cast<int>(Strip::Rank) && mp::stripTargetLevelId(muteA) == 201,
               "a strip target carries its strip and its level");
      bindCc(20, mp::MidiTargetKind::StripMute, muteA);
      bindCc(21, mp::MidiTargetKind::StripSolo, soloPair);
      const auto press = [&](int cc, int value) {
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage::controllerEvent(16, cc, value), 0);
        metered.block(m);
      };
      press(20, 127);
      MP_CHECK(p.liveMuted(Strip::Rank, 0), "the mute button pressed: rank A muted");
      press(20, 0);
      MP_CHECK(p.liveMuted(Strip::Rank, 0), "released: still muted");
      press(20, 127);
      MP_CHECK(!p.liveMuted(Strip::Rank, 0), "pressed again: heard");
      p.setLiveMuted(Strip::Rank, 0, true);
      press(20, 127);
      MP_CHECK(!p.liveMuted(Strip::Rank, 0), "a press toggles what the strip shows, set from the screen or not");
      press(21, 127);
      MP_CHECK(p.liveSoloed(Strip::Pair, 0), "the solo button pressed: the first pair soloed");
      press(21, 127);
      MP_CHECK(!p.liveSoloed(Strip::Pair, 0), "pressed again: off");

      const std::string text = p.midiMap().toText();
      MP_CHECK(text.find("strip-mute") != std::string::npos && text.find("strip-solo") != std::string::npos,
               "named in the map file");
      mp::MidiMap again;
      MP_CHECK(again.fromText(text) && again.bindingFor(mp::MidiTargetKind::StripMute, muteA) != nullptr &&
                   again.bindingFor(mp::MidiTargetKind::StripSolo, soloPair) != nullptr,
               "read back from the map file");
    }

    plain.proc.settingsFileFor(odf).deleteFile();
    root.deleteRecursively();
  }
};
static LiveLevelsStripsTest g_strips;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
