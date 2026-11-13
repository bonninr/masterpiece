// Recorded tremulant waveforms (#805). A set that records its tremulants gives
// a pipe a file whose channel 0 is the pitch's deviation as a fraction of its
// frequency and channel 1 the fundamental's level. The pipe swings by the
// file's shape and depth, at the tremulant's own rate; a pipe whose file
// cannot be read swings by the tremulant's sine, as before.
#include "MpTest.h"
#include "../src/mp_core/OrganModel.h"
#include "../src/mp_core/TremulantShape.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kPi = 3.14159265358979323846;

class TremulantShapeTest final : public mp::test::Test {
public:
  TremulantShapeTest() : Test("functional.dsp.tremulant-waveform-shape", Category::Functional) {}
  void run() override {
    // Two cycles of 4000 frames in the loop 1000..9000: a pitch of 12 cents
    // with a second harmonic of 3, and a level of 0.03 about -0.02.
    constexpr size_t kFrames = 10000, kStart = 1000, kEnd = 9000;
    constexpr double kPeriod = 4000.0;
    const auto centsAt = [&](double t) {
      const double x = 2.0 * kPi * t / kPeriod;
      return 12.0 * std::sin(x) + 3.0 * std::sin(2.0 * x);
    };
    std::vector<float> pitch(kFrames), level(kFrames);
    for (size_t i = 0; i < kFrames; ++i) {
      pitch[i] = static_cast<float>(std::pow(2.0, centsAt(static_cast<double>(i)) / 1200.0) - 1.0);
      level[i] = static_cast<float>(-0.02 + 0.03 * std::cos(2.0 * kPi * static_cast<double>(i) / kPeriod));
    }
    mp::TremulantShape shape;
    MP_CHECK(mp::buildTremulantShape(pitch.data(), level.data(), kFrames, kStart, kEnd, shape),
             "a looped waveform makes a shape");
    MP_CHECK(shape.cyclesInLoop == 2, "two cycles found in the loop: " + std::to_string(shape.cyclesInLoop));
    double truePeak = 0.0;
    for (int i = 0; i < 4000; ++i) truePeak = std::max(truePeak, std::fabs(centsAt(i)));
    MP_CHECK(std::fabs(shape.pitchPeakCents - truePeak) < 0.2,
             "the pitch peak is the file's: " + std::to_string(shape.pitchPeakCents) + " of " +
                 std::to_string(truePeak));
    MP_CHECK(!shape.pitchCapped, "a 14-cent swing is played as it is");
    // Counted from the file's start: phase p is frame p x period.
    double worstPitch = 0.0, worstAmp = 0.0;
    for (int k = 0; k < 64; ++k) {
      const double phase = k / 64.0;
      const double cents = centsAt(phase * kPeriod);
      worstPitch = std::max(worstPitch,
                            std::fabs(shape.pitchAt(phase) * mp::kTremulantWaveformPitchCents - cents));
      worstAmp = std::max(worstAmp, std::fabs(shape.ampAt(phase) - 0.03 * std::cos(2.0 * kPi * phase)));
    }
    MP_CHECK(worstPitch < 0.3, "the pitch table follows the file to " + std::to_string(worstPitch) + " cents");
    MP_CHECK(worstAmp < 0.002, "the level table follows the file about its mean: " + std::to_string(worstAmp));

    // Across an organ the median level peak becomes 1.
    std::vector<mp::TremulantShape> organ(3, shape);
    for (auto& v : organ[1].amp) v *= 2.0f;
    organ[1].ampPeak *= 2.0;
    for (auto& v : organ[2].amp) v *= 4.0f;
    organ[2].ampPeak *= 4.0;
    mp::normaliseTremulantShapes(organ);
    const auto peak = [](const mp::TremulantShape& s) {
      float p = 0.0f;
      for (float v : s.amp) p = std::max(p, std::fabs(v));
      return p;
    };
    MP_CHECK(std::fabs(peak(organ[1]) - 1.0f) < 1.0e-4f, "the median waveform swings by the standard depth");
    MP_CHECK(std::fabs(peak(organ[0]) - 0.5f) < 1.0e-4f && std::fabs(peak(organ[2]) - 2.0f) < 1.0e-4f,
             "the others by their own depth beside it: " + std::to_string(peak(organ[0])) + ", " +
                 std::to_string(peak(organ[2])));

    // A swing no tremulant makes: the shape at the standard depth.
    std::vector<float> wild(kFrames);
    for (size_t i = 0; i < kFrames; ++i)
      wild[i] = static_cast<float>(0.1585 * std::sin(2.0 * kPi * static_cast<double>(i) / kPeriod));
    mp::TremulantShape capped;
    MP_CHECK(mp::buildTremulantShape(wild.data(), wild.data(), kFrames, kStart, kEnd, capped) &&
                 capped.pitchCapped,
             "a 250-cent swing is capped");
    float cappedPeak = 0.0f;
    for (float v : capped.pitch) cappedPeak = std::max(cappedPeak, std::fabs(v));
    MP_CHECK(std::fabs(cappedPeak - 1.0f) < 1.0e-3f, "to the standard depth: " + std::to_string(cappedPeak));

    // A loop cut from a recording: seconds long, many cycles, not always a
    // whole number of them (#1074). 5.9 Hz at 48 kHz over 3 s is 17.7
    // cycles; the shape is still one cycle of the file, at its full swing.
    for (const double hz : {6.0, 5.9}) {
      constexpr size_t kLong = 160000, kLs = 8000, kLe = 8000 + 144000;
      const double per = 48000.0 / hz;
      const auto longCents = [&](double t) {
        const double x = 2.0 * kPi * t / per;
        return 10.0 * std::sin(x) + 2.0 * std::sin(2.0 * x);
      };
      std::vector<float> lp(kLong), ll(kLong);
      for (size_t i = 0; i < kLong; ++i) {
        lp[i] = static_cast<float>(std::pow(2.0, longCents(static_cast<double>(i)) / 1200.0) - 1.0);
        // A level with two humps a cycle: its second harmonic above its first.
        const double x = 2.0 * kPi * static_cast<double>(i) / per;
        ll[i] = static_cast<float>(0.01 * std::cos(x) + 0.03 * std::cos(2.0 * x));
      }
      mp::TremulantShape longShape;
      MP_CHECK(mp::buildTremulantShape(lp.data(), ll.data(), kLong, kLs, kLe, longShape),
               "a long loop makes a shape");
      const int want = static_cast<int>(std::lround(3.0 * hz));
      MP_CHECK(longShape.cyclesInLoop == want, "the loop's " + std::to_string(want) +
                                                   " cycles found: " + std::to_string(longShape.cyclesInLoop));
      double worstLong = 0.0, worstLevel = 0.0;
      for (int k = 0; k < 64; ++k) {
        const double phase = k / 64.0;
        worstLong = std::max(worstLong, std::fabs(longShape.pitchAt(phase) * mp::kTremulantWaveformPitchCents -
                                                  longCents(phase * per)));
        const double x = 2.0 * kPi * phase;
        worstLevel = std::max(worstLevel, std::fabs(longShape.ampAt(phase) -
                                                    (0.01 * std::cos(x) + 0.03 * std::cos(2.0 * x))));
      }
      MP_CHECK(worstLong < 0.5, "at " + std::to_string(hz) + " Hz the pitch table follows one cycle to " +
                                    std::to_string(worstLong) + " cents");
      MP_CHECK(worstLevel < 0.003, "and the level table to " + std::to_string(worstLevel));
    }

    // One cycle whose pitch hardly moves, its strongest line the third
    // harmonic, and whose level moves plainly once: one cycle, as the level
    // shows (#1074). Counted from the pitch alone it was three, and the
    // level's swing averaged away.
    {
      constexpr size_t kOne = 6000, kOs = 1000, kOe = 5410;  // 4410 frames: 5 Hz at 22050
      const double per = 4410.0;
      std::vector<float> op(kOne), ol(kOne);
      uint32_t seed = 12345u;
      for (size_t i = 0; i < kOne; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.4;
        const double x = 2.0 * kPi * static_cast<double>(i) / per;
        const double c = 0.3 * std::sin(x) + 1.2 * std::sin(3.0 * x) + noise;
        op[i] = static_cast<float>(std::pow(2.0, c / 1200.0) - 1.0);
        ol[i] = static_cast<float>(0.1 + 0.05 * std::cos(x));
      }
      mp::TremulantShape one;
      MP_CHECK(mp::buildTremulantShape(op.data(), ol.data(), kOne, kOs, kOe, one), "a one-cycle loop makes a shape");
      MP_CHECK(one.cyclesInLoop == 1, "its one cycle found: " + std::to_string(one.cyclesInLoop));
      double worstOne = 0.0;
      for (int k = 0; k < 64; ++k) {
        const double phase = k / 64.0;
        worstOne = std::max(worstOne, std::fabs(one.ampAt(phase) - 0.05 * std::cos(2.0 * kPi * phase)));
      }
      MP_CHECK(worstOne < 0.003, "the level table keeps its swing: " + std::to_string(worstOne));
    }

    // Nothing that moves, and a loop too short for a cycle.
    std::vector<float> still(kFrames, 0.0f);
    mp::TremulantShape none;
    MP_CHECK(!mp::buildTremulantShape(still.data(), still.data(), kFrames, kStart, kEnd, none),
             "a file that does not move makes no shape");
    MP_CHECK(!mp::buildTremulantShape(pitch.data(), level.data(), kFrames, kStart, kStart + 32, none),
             "a loop of 32 frames makes no shape");
    MP_CHECK(!mp::buildTremulantShape(pitch.data(), level.data(), kFrames, kStart, kFrames + 1, none),
             "a loop past the file's end makes no shape");
  }
};
static TremulantShapeTest g_tremulantShape;

}  // namespace

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"

namespace {

void writeWav(const juce::File& file, int channels, const std::vector<std::vector<float>>& data,
              int loopStart, int loopLastFrame, int cue = -1) {
  juce::WavAudioFormat fmt;
  file.deleteFile();
  std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
  juce::StringPairArray meta;
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", juce::String(loopStart));
  meta.set("Loop0End", juce::String(loopLastFrame));
  if (cue >= 0) {
    meta.set("NumCuePoints", "1");
    meta.set("Cue0Identifier", "1");
    meta.set("Cue0Offset", juce::String(cue));
  }
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0,
                                                                  static_cast<unsigned>(channels), 24, meta, 0));
  const int frames = static_cast<int>(data[0].size());
  juce::AudioBuffer<float> buf(channels, frames);
  for (int c = 0; c < channels; ++c)
    for (int i = 0; i < frames; ++i) buf.setSample(c, i, data[static_cast<size_t>(c)][static_cast<size_t>(i)]);
  w->writeFromAudioSampleBuffer(buf, 0, frames);
}

// One pipe at A4 on a manual, under a tremulant at 6 Hz whose recorded
// waveform swings the pitch by 20 cents, one cycle of 4 Hz in its loop.
juce::File writeOrgan(const juce::File& root) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  // 440 Hz: 11 cycles in 1200 frames, so a loop of whole cycles is seamless.
  std::vector<std::vector<float>> tone(1, std::vector<float>(86400));
  for (size_t i = 0; i < tone[0].size(); ++i)
    tone[0][i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / 48000.0));
  writeWav(pkg.getChildFile("tone.wav"), 1, tone, 1200, 85199);
  std::vector<std::vector<float>> wave(2, std::vector<float>(13000));
  for (size_t i = 0; i < 13000; ++i) {
    const double x = 2.0 * kPi * static_cast<double>(i) / 12000.0;
    wave[0][i] = static_cast<float>(std::pow(2.0, 20.0 * std::sin(x) / 1200.0) - 1.0);
    wave[1][i] = static_cast<float>(0.05 * std::sin(x));
  }
  // A cue inside the loop, as Lemmer's files have: it marks no release in a
  // waveform, and the loop is still the waveform's.
  writeWav(pkg.getChildFile("wave.wav"), 2, wave, 500, 12499, 3000);

  const auto odf = defs.getChildFile("trem.Organ_Hauptwerk_xml");
  odf.replaceWithText(
      "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
      "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Trem</Identification_Name>"
      "<Identification_UniqueOrganID>36</Identification_UniqueOrganID></_General></ObjectList>"
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>440</Pitch_ExactSamplePitch></Sample>"
      "<Sample><SampleID>150</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>wave.wav</SampleFilename></Sample></ObjectList>"
      "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
      "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
      "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>69</NormalMIDINoteNumber></Pipe_SoundEngine01>"
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
      "<StartRatePercent>100</StartRatePercent><StopRatePercent>100</StopRatePercent></Tremulant>"
      "</ObjectList><ObjectList ObjectType=\"TremulantWaveform\">"
      "<TremulantWaveform><TremulantWaveformID>611</TremulantWaveformID><Name>W</Name><TremulantID>601</TremulantID>"
      "<PitchAndFundamentalWaveformSampleID>150</PitchAndFundamentalWaveformSampleID></TremulantWaveform>"
      "</ObjectList><ObjectList ObjectType=\"TremulantWaveformPipe\">"
      "<TremulantWaveformPipe><PipeID>301</PipeID><TremulantWaveformID>611</TremulantWaveformID>"
      "<AmplitudeModDepthAdjustDecibels>0</AmplitudeModDepthAdjustDecibels>"
      "<PitchModDepthAdjustPercent>100</PitchModDepthAdjustPercent></TremulantWaveformPipe>"
      "</ObjectList></Hauptwerk>");
  return odf;
}

struct Swing {
  double centsPeakToPeak = 0.0;
  double rateHz = 0.0;
};

// The pitch swing of a held A4 with the tremulant drawn: the frequency of
// every cycle of the tone from its rising zero crossings, over 3 s once the
// tremulant has started.
Swing measure(mp::MasterpieceProcessor& proc) {
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
  std::vector<float> out;
  for (int i = 0; i < 563; ++i) {
    block();
    out.insert(out.end(), buf.getReadPointer(0), buf.getReadPointer(0) + 256);
  }
  block(off);
  proc.setSwitchEngaged(1103, false);
  for (int i = 0; i < 400; ++i) block();

  std::vector<double> crossings;
  for (size_t i = 1; i < out.size(); ++i)
    if (out[i - 1] < 0.0f && out[i] >= 0.0f)
      crossings.push_back(static_cast<double>(i - 1) + out[i - 1] / (out[i - 1] - out[i]));
  std::vector<double> cents;
  for (size_t i = 1; i < crossings.size(); ++i)
    cents.push_back(1200.0 * std::log2(48000.0 / (crossings[i] - crossings[i - 1])));
  Swing s;
  if (cents.size() < 100) return s;
  std::vector<double> sorted = cents;
  std::sort(sorted.begin(), sorted.end());
  s.centsPeakToPeak = sorted[sorted.size() * 98 / 100] - sorted[sorted.size() * 2 / 100];
  const double mid = sorted[sorted.size() / 2];
  int turns = 0;
  for (size_t i = 1; i < cents.size(); ++i)
    if (cents[i - 1] < mid && cents[i] >= mid) ++turns;
  s.rateHz = turns / (static_cast<double>(out.size()) / 48000.0);
  return s;
}

class TremulantRecordedWaveformTest final : public mp::test::Test {
public:
  TremulantRecordedWaveformTest()
      : Test("functional.voice.tremulant-recorded-waveform", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-trem-waveform");
    const juce::File odf = writeOrgan(root);
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      const Swing s = measure(proc);
      MP_CHECK(std::fabs(s.centsPeakToPeak - 40.0) < 3.0,
               "the pipe swings by its file's 40 cents: " + std::to_string(s.centsPeakToPeak));
      MP_CHECK(std::fabs(s.rateHz - 6.0) < 0.5,
               "at the tremulant's 6 Hz, the file's 4 Hz aside: " + std::to_string(s.rateHz));
      proc.settingsFileFor(odf).deleteFile();
    }
    // Without its file, the pipe swings by the tremulant's sine.
    root.getChildFile("OrganInstallationPackages").getChildFile("000001").getChildFile("wave.wav").deleteFile();
    {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads without its waveform");
      const Swing s = measure(proc);
      const double sine = 2.0 * mp::kTremulantWaveformPitchCents;
      MP_CHECK(std::fabs(s.centsPeakToPeak - sine) < 2.0,
               "the pipe swings by the sine's " + std::to_string(sine) + " cents: " +
                   std::to_string(s.centsPeakToPeak));
      MP_CHECK(std::fabs(s.rateHz - 6.0) < 0.5, "at 6 Hz: " + std::to_string(s.rateHz));
      proc.settingsFileFor(odf).deleteFile();
    }
    root.deleteRecursively();
  }
};
static TremulantRecordedWaveformTest g_tremulantRecordedWaveform;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
