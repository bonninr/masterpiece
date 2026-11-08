// The slow drift of a tremulant's rate and depth, and of each pipe's share of
// it (#806): the walk itself, and a held note under a drifting tremulant.
#include "MpTest.h"
#include "../src/mp_core/RandomWalk.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

struct WalkStats {
  double mean = 0.0, spread = 0.0, largest = 0.0, lagCorrelation = 0.0;
};

WalkStats walk(const mp::RandomWalk& w, uint64_t seed, int steps, double dt, int lag) {
  mp::RandomWalkState s;
  s.start(w, seed);
  std::vector<double> x(static_cast<size_t>(steps));
  for (auto& v : x) {
    s.step(dt);
    v = s.percent();
  }
  WalkStats r;
  for (double v : x) {
    r.mean += v;
    r.largest = std::max(r.largest, std::fabs(v));
  }
  r.mean /= steps;
  double var = 0.0, cov = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    var += (x[i] - r.mean) * (x[i] - r.mean);
    if (i >= static_cast<size_t>(lag))
      cov += (x[i] - r.mean) * (x[i - static_cast<size_t>(lag)] - r.mean);
  }
  r.spread = std::sqrt(var / steps);
  r.lagCorrelation = cov / var;
  return r;
}

class RandomWalkTest final : public mp::test::Test {
public:
  RandomWalkTest() : Test("functional.dsp.tremulant-random-walk", Category::Functional) {}
  void run() override {
    const double dt = 256.0 / 48000.0;

    // Off, or stated with no push or no room to move: exactly still.
    for (const mp::RandomWalk w : {mp::RandomWalk{false, 44.0, 4.6, 50, 15.0},
                                   mp::RandomWalk{true, 0.0, 4.6, 50, 15.0},
                                   mp::RandomWalk{true, 44.0, 4.6, 50, 0.0}}) {
      mp::RandomWalkState s;
      s.start(w, 7);
      bool still = !s.on();
      for (int i = 0; i < 1000; ++i) still = still && s.step(dt) == 1.0;
      MP_CHECK(still, "a drift that is off stays at exactly 1");
    }

    // A rate drift as Lemmer states it: 44, 4.6, at most 15 %.
    const mp::RandomWalk lemmer{true, 44.0, 4.6, 50, 15.0};
    const WalkStats a = walk(lemmer, 1, 200000, dt, 10);
    MP_CHECK(a.largest <= 15.0, "held inside 15 %: " + std::to_string(a.largest));
    MP_CHECK(a.largest > 14.0, "and reaching it: " + std::to_string(a.largest));
    // Its free spread (14.5 %) meets the 15 % wall, so the held spread is
    // smaller: between half and all of the maximum.
    MP_CHECK(a.spread > 7.5 && a.spread < 15.0, "spread inside the maximum: " + std::to_string(a.spread));
    MP_CHECK(std::fabs(a.mean) < 1.0, "centred on the stated rate: " + std::to_string(a.mean));
    // Away from its wall the walk settles at accel / sqrt(2 x damping) and
    // forgets at the damping rate: 10 blocks (53 ms) apart it keeps
    // exp(-4.6 x 0.053).
    const WalkStats free = walk(mp::RandomWalk{true, 10.0, 4.6, 50, 50.0}, 3, 200000, dt, 10);
    MP_CHECK(std::fabs(free.spread - 10.0 / std::sqrt(9.2)) < 0.3,
             "settled spread 3.30 %: " + std::to_string(free.spread));
    MP_CHECK(std::fabs(free.lagCorrelation - std::exp(-4.6 * 10.0 * dt)) < 0.05,
             "moving at the stated damping: " + std::to_string(free.lagCorrelation));

    // A gentle one, as St. Maximin states it: 2.6293, 0.8047, at most 2 %.
    const WalkStats b = walk(mp::RandomWalk{true, 2.6293, 0.8047, 5, 2.0}, 1, 200000, dt, 10);
    MP_CHECK(b.largest <= 2.0 && b.spread > 1.0 && b.spread < 2.0,
             "held inside 2 %, spread " + std::to_string(b.spread));

    // The same seed gives the same drift; another probability index another.
    mp::RandomWalkState s1, s2, s3;
    s1.start(lemmer, 5);
    s2.start(lemmer, 5);
    mp::RandomWalk other = lemmer;
    other.fn = 51;
    s3.start(other, 5);
    bool same = true, apart = false;
    for (int i = 0; i < 100; ++i) {
      const double v1 = s1.step(dt), v2 = s2.step(dt), v3 = s3.step(dt);
      same = same && v1 == v2;
      apart = apart || v1 != v3;
    }
    MP_CHECK(same, "one seed, one drift, run after run");
    MP_CHECK(apart, "another probability index drifts apart");
  }
};
static RandomWalkTest g_randomWalk;

}  // namespace

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

void writeTone(const juce::File& file) {
  juce::WavAudioFormat fmt;
  file.deleteFile();
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  juce::StringPairArray meta;
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "1200");
  meta.set("Loop0End", "85199");
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 24, meta, 0));
  juce::AudioBuffer<float> buf(1, 86400);
  for (int i = 0; i < 86400; ++i)
    buf.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * kPi * 440.0 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(buf, 0, 86400);
}

// A one-pipe organ under a sine tremulant; `trem` and `pipe` are the extra
// fields of the Tremulant and Pipe_SoundEngine01 rows.
juce::File writeOrgan(const juce::File& root, const std::string& trem, const std::string& pipe) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  writeTone(pkg.getChildFile("tone.wav"));
  const auto odf = defs.getChildFile("drift.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Drift</Identification_Name>"
      "<Identification_UniqueOrganID>37</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>440</Pitch_ExactSamplePitch></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>69</NormalMIDINoteNumber>" +
      pipe +
      "</Pipe_SoundEngine01>"
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
      "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>69</MIDINoteNumOfFirstMappedDivisionInputNode>"
      "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
      "</ObjectList><ObjectList ObjectType=\"Switch\">"
      "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
      "<Switch><SwitchID>1103</SwitchID><Name>Trem</Name><Latching>Y</Latching></Switch>"
      "</ObjectList><ObjectList ObjectType=\"Tremulant\">"
      "<Tremulant><TremulantID>601</TremulantID><Name>Trem</Name><ControllingSwitchID>1103</ControllingSwitchID>"
      "<FrequencyWhenEngagedHz>6</FrequencyWhenEngagedHz><FrequencyWhenDisengagedHz>6</FrequencyWhenDisengagedHz>"
      "<StartRatePercent>100</StartRatePercent><StopRatePercent>100</StopRatePercent>" +
      trem +
      "</Tremulant>"
      "</ObjectList><ObjectList ObjectType=\"TremulantWaveform\">"
      "<TremulantWaveform><TremulantWaveformID>611</TremulantWaveformID><Name>W</Name><TremulantID>601</TremulantID></TremulantWaveform>"
      "</ObjectList><ObjectList ObjectType=\"TremulantWaveformPipe\">"
      "<TremulantWaveformPipe><PipeID>301</PipeID><TremulantWaveformID>611</TremulantWaveformID>"
      "<AmplitudeModDepthAdjustDecibels>0</AmplitudeModDepthAdjustDecibels>"
      "<PitchModDepthAdjustPercent>100</PitchModDepthAdjustPercent></TremulantWaveformPipe>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

// A held A4 for six seconds under the tremulant, after it has wound up.
std::vector<float> render(const juce::File& odf) {
  mp::MasterpieceProcessor proc;
  proc.prepareToPlay(48000.0, 256);
  std::vector<float> out;
  if (!proc.loadOrgan(odf, 0, false).ok) return out;
  juce::AudioBuffer<float> buf(2, 256);
  const auto block = [&](juce::MidiBuffer m = {}) {
    buf.clear();
    proc.processBlock(buf, m);
  };
  juce::MidiBuffer on, off;
  on.addEvent(juce::MidiMessage::noteOn(1, 69, 0.8f), 0);
  off.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
  proc.setStopEngaged(901, true);
  proc.setSwitchEngaged(1103, true);
  block(on);
  for (int i = 0; i < 188; ++i) block();
  for (int i = 0; i < 1125; ++i) {
    block();
    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 256);
  }
  block(off);
  proc.setSwitchEngaged(1103, false);
  for (int i = 0; i < 200; ++i) block();
  proc.settingsFileFor(odf).deleteFile();
  return out;
}

// The tremulant cycle by cycle: its rate and its pitch swing, each as a
// spread about its mean (a fraction).
struct Cycles {
  int count = 0;
  double rateSpread = 0.0, depthSpread = 0.0, meanDepthCents = 0.0;
};

Cycles cycles(const std::vector<float>& out) {
  std::vector<double> t, cents;
  double last = -1.0;
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i - 1] < 0.0f && out[i] >= 0.0f) {
      const double x = static_cast<double>(i - 1) + out[i - 1] / (out[i - 1] - out[i]);
      if (last >= 0.0) {
        t.push_back(x / 48000.0);
        cents.push_back(1200.0 * std::log2(48000.0 / (x - last) / 440.0));
      }
      last = x;
    }
  Cycles c;
  if (cents.size() < 200) return c;
  double mid = 0.0;
  for (double v : cents) mid += v;
  mid /= static_cast<double>(cents.size());
  std::vector<size_t> ups;
  for (size_t i = 1; i < cents.size(); ++i)
    if (cents[i - 1] < mid && cents[i] >= mid) ups.push_back(i);
  std::vector<double> rates, depths;
  for (size_t k = 1; k < ups.size(); ++k) {
    rates.push_back(1.0 / (t[ups[k]] - t[ups[k - 1]]));
    const auto lo = std::min_element(cents.begin() + static_cast<long>(ups[k - 1]), cents.begin() + static_cast<long>(ups[k]));
    const auto hi = std::max_element(cents.begin() + static_cast<long>(ups[k - 1]), cents.begin() + static_cast<long>(ups[k]));
    depths.push_back(*hi - *lo);
  }
  const auto spread = [](const std::vector<double>& v, double& meanOut) {
    double m = 0.0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    meanOut = m;
    return std::sqrt(s / static_cast<double>(v.size())) / m;
  };
  double meanRate = 0.0;
  c.count = static_cast<int>(rates.size());
  c.rateSpread = spread(rates, meanRate);
  c.depthSpread = spread(depths, c.meanDepthCents);
  return c;
}

class TremulantDriftTest final : public mp::test::Test {
public:
  TremulantDriftTest() : Test("functional.voice.tremulant-random-drift", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-trem-drift");
    const std::string rate =
        "<FrequencyRandomisation_RandomiseFrequency>Y</FrequencyRandomisation_RandomiseFrequency>"
        "<FrequencyRandomisation_PositiveAcceleratingCoeff>44</FrequencyRandomisation_PositiveAcceleratingCoeff>"
        "<FrequencyRandomisation_PositiveDampingCoeff>4.6</FrequencyRandomisation_PositiveDampingCoeff>"
        "<FrequencyRandomisation_IndexGeneratingProbabilityFn>50</FrequencyRandomisation_IndexGeneratingProbabilityFn>"
        "<FrequencyRandomisation_MaxFreqFluctuationPct>15</FrequencyRandomisation_MaxFreqFluctuationPct>";
    const std::string depth =
        "<DepthRandomisation_RandomiseDepth>Y</DepthRandomisation_RandomiseDepth>"
        "<DepthRandomisation_PositiveAcceleratingCoeff>44</DepthRandomisation_PositiveAcceleratingCoeff>"
        "<DepthRandomisation_PositiveDampingCoeff>4.6</DepthRandomisation_PositiveDampingCoeff>"
        "<DepthRandomisation_IndexGeneratingProbabilityFn>25</DepthRandomisation_IndexGeneratingProbabilityFn>"
        "<DepthRandomisation_fMaxDepthFluctuationPct>15</DepthRandomisation_fMaxDepthFluctuationPct>";
    const std::string pipe =
        "<TremulantDepthRandomisation_RandomiseTremDepth>Y</TremulantDepthRandomisation_RandomiseTremDepth>"
        "<TremulantDepthRandomisation_PositiveAcceleratingCoeff>44</TremulantDepthRandomisation_PositiveAcceleratingCoeff>"
        "<TremulantDepthRandomisation_PositiveDampingCoeff>5</TremulantDepthRandomisation_PositiveDampingCoeff>"
        "<TremulantDepthRandomisation_IndexGeneratingProbabilityFn>50</TremulantDepthRandomisation_IndexGeneratingProbabilityFn>"
        "<TremulantDepthRandomisation_MaxDepthFluctuationPct>12</TremulantDepthRandomisation_MaxDepthFluctuationPct>";
    // The same fields switched off: the set states a drift and declines it.
    std::string declined = rate + depth;
    for (size_t at = declined.find(">Y<"); at != std::string::npos; at = declined.find(">Y<"))
      declined.replace(at, 3, ">N<");

    const std::vector<float> steady = render(writeOrgan(root, "", ""));
    const Cycles s = cycles(steady);
    MP_CHECK(s.count > 30, "the steady tremulant is measured: " + std::to_string(s.count) + " cycles");
    MP_CHECK(s.rateSpread < 0.01 && s.depthSpread < 0.02,
             "a tremulant with no drift holds its rate and depth: " + std::to_string(s.rateSpread) + ", " +
                 std::to_string(s.depthSpread));

    const std::vector<float> off = render(writeOrgan(root, declined, ""));
    MP_CHECK(off == steady, "a drift the set switches off sounds exactly as none");

    const Cycles r = cycles(render(writeOrgan(root, rate, "")));
    MP_CHECK(r.rateSpread > 0.03 && r.rateSpread < 0.15,
             "the rate drifts cycle to cycle: " + std::to_string(r.rateSpread));

    const Cycles d = cycles(render(writeOrgan(root, depth, "")));
    MP_CHECK(d.depthSpread > 0.04 && d.rateSpread < 0.01,
             "the depth drifts, the rate holds: " + std::to_string(d.depthSpread) + ", " +
                 std::to_string(d.rateSpread));

    const Cycles p = cycles(render(writeOrgan(root, "", pipe)));
    MP_CHECK(p.depthSpread > 0.04 && p.rateSpread < 0.01,
             "the pipe's own share drifts: " + std::to_string(p.depthSpread) + ", " +
                 std::to_string(p.rateSpread));
    MP_CHECK(std::fabs(p.meanDepthCents - s.meanDepthCents) < 0.15 * s.meanDepthCents,
             "about the pipe's stated depth: " + std::to_string(p.meanDepthCents) + " against " +
                 std::to_string(s.meanDepthCents));
    root.deleteRecursively();
  }
};
static TremulantDriftTest g_tremulantDrift;

}  // namespace

#endif
