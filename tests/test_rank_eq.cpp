// EQ per rank and per division (RankEq.h): the groups on their own, then on a
// two-manual organ with a rank in each division. With no EQ set, or an EQ set
// and taken away again, the organ sounds to the last bit as one never given
// one; a rank's EQ reaches that rank and no other, a division's its ranks;
// and both are saved with the voicing.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/RankEq.h"
#if MP_ENABLE_API
#include "../src/mp_api/ControlModel.h"
#endif

#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

class RankEqGroupsTest final : public mp::test::Test {
public:
  RankEqGroupsTest() : Test("functional.rank-eq.groups", Category::Functional) {}
  void run() override {
    using mp::RankEq;
    using mp::ShelfEq;
    RankEq eq;
    // Three ranks: 0 in division 0, 1 in division 1, 2 in none; three enclosure buses.
    eq.configure({0, 1, -1}, 2, 3, 48000.0);
    MP_CHECK(!eq.wanted() && eq.groupBuses() == 0 && eq.busCount() == 3, "nothing set: no group");
    MP_CHECK(eq.busFor(0, 1) == 1, "a rank with no EQ plays into its enclosure bus");

    eq.setParams({ShelfEq{6.0f, 0.0f}}, {});
    MP_CHECK(eq.wanted(), "a rank with an EQ");
    const int b = eq.busFor(0, 1);
    MP_CHECK(b == 3 && eq.parentOf(0) == 1 && eq.groupBuses() == 1, "its group's bus, after the enclosure buses");
    MP_CHECK(eq.busFor(0, 1) == b, "the same group for its next pipe");
    MP_CHECK(eq.busFor(0, 2) == 4 && eq.parentOf(1) == 2, "a group of its own behind another enclosure");
    MP_CHECK(eq.busFor(1, 0) == 0 && eq.busFor(2, 0) == 0, "ranks with no EQ play as before");

    eq.setParams({ShelfEq{6.0f, 0.0f}}, {ShelfEq{}, ShelfEq{0.0f, 3.0f}});
    MP_CHECK(eq.busFor(1, 0) == 5, "a division's EQ makes a group for its ranks");
    MP_CHECK(eq.busFor(2, 0) == 0, "a rank in no division is left alone");

    // Taken away: the groups go once their pipes have stopped.
    eq.setParams({}, {ShelfEq{}, ShelfEq{0.0f, 3.0f}});
    eq.beginBlock([](int) { return 1; });
    MP_CHECK(eq.groupBuses() == 3, "kept while a pipe still sounds through it");
    eq.beginBlock([](int) { return 0; });
    MP_CHECK(eq.groupBuses() == 3, "the division's group is still wanted, so the count stays");
    MP_CHECK(eq.busFor(0, 1) == 1, "and rank 0 plays as before");
    eq.setParams({}, {});
    eq.beginBlock([](int) { return 0; });
    MP_CHECK(eq.groupBuses() == 0 && !eq.wanted(), "every EQ gone: no group at all");

    // The filters: a flat group leaves a signal alone; a bass shelf lifts a
    // low tone and leaves a high one nearly as it was.
    const auto level = [&](ShelfEq e, double hz) {
      RankEq f;
      f.configure({-1}, 0, 1, 48000.0);
      f.setParams({e}, {});
      const int bus = f.busFor(0, 0);
      std::vector<float> x(48000);
      for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.25 * std::sin(2.0 * 3.141592653589793 * hz * i / 48000.0));
      float* planes[1] = {x.data()};
      if (bus > 0) f.process(bus - 1, 0, planes, 1, static_cast<int>(x.size()));
      double s = 0.0;
      for (size_t i = 24000; i < x.size(); ++i) s += x[i] * x[i];
      return std::sqrt(s / 24000.0);
    };
    const double flatLow = level({}, 60.0);
    MP_CHECK(level({12.0f, 0.0f}, 60.0) > flatLow * 3.0, "+12 dB of bass lifts 60 Hz");
    MP_CHECK(std::fabs(level({12.0f, 0.0f}, 8000.0) - level({}, 8000.0)) < level({}, 8000.0) * 0.05,
             "and leaves 8 kHz alone");
    MP_CHECK(level({0.0f, 12.0f}, 8000.0) > level({}, 8000.0) * 3.0, "+12 dB of brightness lifts 8 kHz");
  }
};
static RankEqGroupsTest g_groups;

// Two manuals, a rank on each: A in division G on manual I (channel 1),
// B in division S on manual II (channel 2), sounding a 130.81 Hz tone.
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
  const auto odf = defs.getChildFile("rankeq.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Rank EQ</Identification_Name>"
      "<Identification_UniqueOrganID>35</Identification_UniqueOrganID></_General></ObjectList>"
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

struct Rig {
  mp::MasterpieceProcessor proc;
  juce::AudioBuffer<float> buf{2, 256};
  std::vector<float> heard;  // every block's samples, left then right
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
    for (int ch = 0; ch < 2; ++ch)
      for (int s = 0; s < 256; ++s) heard.push_back(buf.getSample(ch, s));
    return buf.getRMSLevel(0, 0, 256);
  }
  // A key on a channel held for `held` blocks, then let go, and `after`
  // more blocks. The loudest block's level.
  float play(int channel, int held = 30, int after = 0) {
    juce::MidiBuffer on, off;
    on.addEvent(juce::MidiMessage::noteOn(channel, 36, 0.8f), 0);
    off.addEvent(juce::MidiMessage::noteOff(channel, 36), 0);
    float loudest = block(on);
    for (int i = 1; i < held; ++i) loudest = std::max(loudest, block());
    block(off);
    for (int i = 0; i < after; ++i) block();
    return loudest;
  }
  // The level of the last `blocks` blocks, as the root mean square over
  // their left channel. One block is too short a window for a low tone: its
  // own phase decides how loud that block reads.
  float levelOver(int blocks) {
    const size_t perBlock = 512;  // two channels of 256 samples each
    const size_t have = heard.size() / perBlock;
    const size_t from = have > static_cast<size_t>(blocks) ? have - static_cast<size_t>(blocks) : 0;
    double sum = 0.0;
    size_t n = 0;
    for (size_t b = from; b < have; ++b)
      for (size_t i = b * perBlock; i < b * perBlock + 256; ++i) {
        sum += static_cast<double>(heard[i]) * heard[i];
        ++n;
      }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(n > 0 ? n : 1)));
  }
  void setRankEq(mp::Id rank, float bass, float bright) {
    mp::PipeVoicing pv = proc.voicing().live().rank(rank);
    pv.bassDb = bass;
    pv.brightnessDb = bright;
    proc.voicing().live().setRank(rank, pv);
    proc.applyVoicingEq();
  }
  void setDivisionEq(mp::Id division, float bass, float bright) {
    mp::PipeVoicing pv;
    pv.bassDb = bass;
    pv.brightnessDb = bright;
    proc.voicing().live().setDivision(division, pv);
    proc.applyVoicingEq();
  }
};

class RankEqOrganTest final : public mp::test::Test {
public:
  RankEqOrganTest() : Test("functional.rank-eq.organ", Category::Functional) {}
  void run() override {
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-rank-eq");
    const juce::File odf = writeOrgan(root);

    // Bit-identical: the same performance on an organ never given an EQ,
    // and on one whose rank A was given one, played with it, then had it
    // taken away again.
    {
      Rig plain(odf), touched(odf);
      MP_CHECK(plain.loaded && touched.loaded, "the organ loads");
      touched.setRankEq(201, 12.0f, 6.0f);
      plain.play(1, 30, 400);
      touched.play(1, 30, 400);
      MP_CHECK(touched.proc.voiceStats().activeVoices == 0, "the note has ended");
      touched.setRankEq(201, 0.0f, 0.0f);
      plain.heard.clear();
      touched.heard.clear();
      for (int ch : {1, 2, 1}) {
        plain.play(ch, 20, 20);
        touched.play(ch, 20, 20);
      }
      MP_CHECK(plain.heard.size() == touched.heard.size() && plain.heard == touched.heard,
               "the EQ taken away: bit-identical to an organ never given one");
    }

    // A rank's EQ reaches that rank only.
    {
      Rig plain(odf), eq(odf);
      eq.setRankEq(201, 12.0f, 0.0f);
      const float plainB = plain.play(2, 30, 400);
      const float eqB = eq.play(2, 30, 400);
      MP_CHECK(plain.heard == eq.heard, "rank B, with no EQ of its own, sounds exactly as before");
      MP_CHECK(eqB == plainB, "the same level");
      const float plainA = plain.play(1);
      const float eqA = eq.play(1);
      MP_CHECK(eqA > plainA * 2.0f, "rank A's 130 Hz lifted by its bass: " + std::to_string(eqA) + " against " +
                                        std::to_string(plainA));
    }

    // A division's EQ reaches its ranks.
    {
      Rig plain(odf), eq(odf);
      eq.setDivisionEq(802, 12.0f, 0.0f);
      const float plainA = plain.play(1, 30, 400);
      const float eqA = eq.play(1, 30, 400);
      MP_CHECK(eqA == plainA && plain.heard == eq.heard, "division G's rank A untouched by division S's EQ");
      const float plainB = plain.play(2);
      const float eqB = eq.play(2);
      MP_CHECK(eqB > plainB * 2.0f, "division S's rank B lifted");
    }

    // Heard at once on a pipe already sounding.
    {
      Rig r(odf);
      juce::MidiBuffer on;
      on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
      r.setDivisionEq(801, 0.1f, 0.0f);  // the group exists before the note, nearly flat
      r.block(on);
      for (int i = 0; i < 30; ++i) r.block();
      const float before = r.levelOver(30);
      r.setDivisionEq(801, 12.0f, 0.0f);
      for (int i = 0; i < 30; ++i) r.block();
      const float after = r.levelOver(30);
      MP_CHECK(after > before * 2.0f, "a held note hears its division's EQ change: " + std::to_string(after) +
                                           " against " + std::to_string(before));
    }

    // Saved with the voicing, and read back.
    {
      Rig r(odf);
      r.setRankEq(202, -3.0f, 4.5f);
      r.setDivisionEq(801, 2.0f, -1.5f);
      MP_CHECK(r.proc.saveSettings(), "saved");
      Rig back(odf);
      const auto rank = back.proc.voicing().live().rank(202);
      const auto div = back.proc.voicing().live().division(801);
      MP_CHECK(rank.bassDb == -3.0f && rank.brightnessDb == 4.5f, "a rank's EQ read back");
      MP_CHECK(div.bassDb == 2.0f && div.brightnessDb == -1.5f, "a division's EQ read back");
#if MP_ENABLE_API
      // The control API: ranks first, then divisions.
      mp::api::ControlModel model(back.proc);
      mp::api::Error e;
      int v = 0;
      MP_CHECK(model.count(mp::api::Type::Brightness) == 4, "two ranks and two divisions");
      MP_CHECK(model.get(mp::api::Type::Brightness, 1, v, e) && v == 45, "rank B's brightness in tenths of a dB");
      MP_CHECK(model.get(mp::api::Type::Bass, 2, v, e) && v == 20, "division G's bass");
      v = 60;
      MP_CHECK(model.set(mp::api::Type::Bass, 0, v) == mp::api::Error::None &&
                   back.proc.voicing().live().rank(201).bassDb == 6.0f,
               "set through the API");
      v = 130;
      MP_CHECK(model.set(mp::api::Type::Bass, 0, v) == mp::api::Error::BadValue, "within 12 dB");
#endif
      back.proc.settingsFileFor(odf).deleteFile();
    }
    root.deleteRecursively();
  }
};
static RankEqOrganTest g_organ;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
