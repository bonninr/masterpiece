// An output's EQ and high-pass, and a pipe's own shelves (#1188), measured
// from their spectra. The organ is synthetic: one rank whose sample is 36
// sines spread on a log scale from 25 Hz to 16 kHz, each a whole number of
// cycles over the loop. The same note is rendered with and without the
// filter, and the difference of the two spectra, at each sine, is the
// filter's response; it is compared with the response worked out from the
// filter's own formula. With every filter off, or flat, the output is the
// organ's to the last bit: nothing is filtered.
//
// With MP_SPECTRUM_DIR set, each case's two renders are written there as WAV
// files and the measured response as a CSV, for plotting.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/VoicingEq.h"
#if __has_include("../src/mp_audio/OutputHighPass.h")
#include "../src/mp_audio/OutputHighPass.h"
#define MP_OUTPUT_FILTERS 1
#endif

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kLoopStart = 1000, kLoopEnd = 47000;
constexpr int kTones = 36;
constexpr int kFft = 32768;
constexpr int kBlocks = 200;          // 51200 frames
constexpr int kSteadyFrom = 16384;    // the attack is over
constexpr mp::Id kPipe36 = 3000, kPipe37 = 3001;

// The sines' frequencies: whole cycles over the loop, nearest to a log
// spacing from 25 Hz to 16 kHz.
std::vector<double> toneHz() {
  const double step = kRate / (kLoopEnd - kLoopStart);
  std::vector<double> out;
  for (int j = 0; j < kTones; ++j) {
    const double want = 25.0 * std::pow(16000.0 / 25.0, static_cast<double>(j) / (kTones - 1));
    const double hz = std::max(1.0, std::round(want / step)) * step;
    if (out.empty() || hz > out.back()) out.push_back(hz);
  }
  return out;
}

// `ranks` ranks of `keys` pipes from key 36, rank r on stop 901 + r, every
// pipe the same sample at the pitch of key 36, so that key plays it as
// recorded. Rank r's pipe at key 36 + k is pipe 3000 + 1000 r + k.
struct BroadbandOrgan {
  juce::File odf, root;
  explicit BroadbandOrgan(const juce::String& name, int ranks = 1, int keys = 2) {
    root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    root.deleteRecursively();
    const auto defs = root.getChildFile("OrganDefinitions");
    const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
    defs.createDirectory();
    pkg.createDirectory();
    {
      juce::WavAudioFormat fmt;
      std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("broadband.wav").createOutputStream());
      juce::StringPairArray meta;
      meta.set("NumSampleLoops", "1");
      meta.set("Loop0Start", juce::String(kLoopStart));
      meta.set("Loop0End", juce::String(kLoopEnd));
      std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), kRate, 1, 24, meta, 0));
      juce::AudioBuffer<float> wave(1, 48000);
      std::mt19937 rng(1188);
      std::uniform_real_distribution<double> phase(0.0, 2.0 * kPi);
      std::vector<double> phases;
      const auto hz = toneHz();
      for (size_t t = 0; t < hz.size(); ++t) phases.push_back(phase(rng));
      for (int i = 0; i < 48000; ++i) {
        double v = 0.0;
        for (size_t t = 0; t < hz.size(); ++t) v += 0.02 * std::sin(2.0 * kPi * hz[t] * i / kRate + phases[t]);
        wave.setSample(0, i, static_cast<float>(v));
      }
      w->writeFromAudioSampleBuffer(wave, 0, 48000);
    }
    juce::String pipes, layers, attacks, rankRows, stops, stopRanks, switches;
    for (int r = 0; r < ranks; ++r) {
      const juce::String rank(201 + r), stop(901 + r), sw(1101 + r);
      rankRows << "<Rank><RankID>" << rank << "</RankID><Name>R" << rank << "</Name></Rank>";
      stops << "<Stop><StopID>" << stop << "</StopID><Name>S" << stop << "</Name><DivisionID>801</DivisionID>"
            << "<ControllingSwitchID>" << sw << "</ControllingSwitchID></Stop>";
      switches << "<Switch><SwitchID>" << sw << "</SwitchID><Name>W" << sw << "</Name><Latching>Y</Latching></Switch>";
      stopRanks << "<StopRank><StopID>" << stop << "</StopID><RankID>" << rank
                << "</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
                << "<NumberOfMappedDivisionInputNodes>" << keys << "</NumberOfMappedDivisionInputNodes></StopRank>";
      for (int k = 0; k < keys; ++k) {
        const juce::String key(36 + k), pipe(3000 + 1000 * r + k), layer(100000 + 1000 * r + k),
            attack(200000 + 1000 * r + k);
        pipes << "<Pipe_SoundEngine01><PipeID>" << pipe << "</PipeID><RankID>" << rank
              << "</RankID><NormalMIDINoteNumber>" << key << "</NormalMIDINoteNumber></Pipe_SoundEngine01>";
        layers << "<Pipe_SoundEngine01_Layer><LayerID>" << layer << "</LayerID><PipeID>" << pipe
               << "</PipeID></Pipe_SoundEngine01_Layer>";
        attacks << "<Pipe_SoundEngine01_AttackSample><UniqueID>" << attack << "</UniqueID><LayerID>" << layer
                << "</LayerID><SampleID>101</SampleID></Pipe_SoundEngine01_AttackSample>";
      }
    }
    odf = defs.getChildFile("broadband.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Broadband</Identification_Name>"
        "<Identification_UniqueOrganID>1188</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
        "<SampleFilename>broadband.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>65.406</Pitch_ExactSamplePitch></Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\">" +
        rankRows + "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01\">" + pipes + "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">" + layers +
        "</ObjectList><ObjectList ObjectType=\"Pipe_SoundEngine01_AttackSample\">" + attacks +
        "</ObjectList><ObjectList ObjectType=\"Keyboard\">"
        "<Keyboard><KeyboardID>701</KeyboardID><Name>I</Name><DefaultInputOutputKeyboardAsgnCode>1</DefaultInputOutputKeyboardAsgnCode>"
        "<KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys><KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>"
        "</ObjectList><ObjectList ObjectType=\"Division\"><Division><DivisionID>801</DivisionID><Name>G</Name></Division></ObjectList>"
        "<ObjectList ObjectType=\"KeyAction\">"
        "<KeyAction><SourceKeyboardID>701</SourceKeyboardID><DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>801</DestDivisionID>"
        "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode><MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>"
        "</ObjectList><ObjectList ObjectType=\"Stop\">" +
        stops + "</ObjectList><ObjectList ObjectType=\"StopRank\">" + stopRanks +
        "</ObjectList><ObjectList ObjectType=\"Switch\">" + switches + "</ObjectList></Hauptwerk>");
  }
};

// A file put back as it was when the test is over.
struct Keep {
  juce::File file;
  bool existed = false;
  juce::String content;
  explicit Keep(juce::File f) : file(std::move(f)) {
    existed = file.existsAsFile();
    if (existed) content = file.loadFileAsString();
  }
  ~Keep() {
    if (existed)
      file.replaceWithText(content);
    else
      file.deleteFile();
  }
};

using Channels = std::vector<std::vector<float>>;
using SetUp = std::function<void(mp::MasterpieceProcessor&)>;

// Key 36 held through `outputs` channels, the mixer and voicing as `setUp`
// makes them; `midway`, when given, runs before block kBlocks / 4.
Channels render(const juce::File& odf, int outputs, const SetUp& setUp, const SetUp& midway = {}, int key = 36) {
  mp::MasterpieceProcessor proc;
  juce::AudioProcessor::BusesLayout layout;
  layout.outputBuses.add(outputs == 2 ? juce::AudioChannelSet::stereo()
                                      : juce::AudioChannelSet::discreteChannels(outputs));
  MP_CHECK(proc.setBusesLayout(layout), "the processor takes " + std::to_string(outputs) + " outputs");
  proc.prepareToPlay(kRate, kBlock);
  MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
  setUp(proc);
  proc.setStopEngaged(901, true);
  juce::AudioBuffer<float> buffer(outputs, kBlock);
  juce::MidiBuffer on, none;
  on.addEvent(juce::MidiMessage::noteOn(1, key, 0.8f), 0);
  Channels out(static_cast<size_t>(outputs));
  for (int block = 0; block < kBlocks; ++block) {
    if (midway && block == kBlocks / 4) midway(proc);
    buffer.clear();
    proc.processBlock(buffer, block == 0 ? on : none);
    for (int ch = 0; ch < outputs; ++ch)
      out[static_cast<size_t>(ch)].insert(out[static_cast<size_t>(ch)].end(), buffer.getReadPointer(ch),
                                          buffer.getReadPointer(ch) + kBlock);
  }
  proc.releaseResources();
  return out;
}

// The magnitude spectrum of kFft frames from `from`, Hann-windowed.
std::vector<double> spectrum(const std::vector<float>& x, size_t from) {
  std::vector<std::complex<double>> a(kFft);
  for (int i = 0; i < kFft; ++i) {
    const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kFft);
    a[static_cast<size_t>(i)] = x[from + static_cast<size_t>(i)] * w;
  }
  for (size_t i = 1, j = 0; i < a.size(); ++i) {
    size_t bit = a.size() >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= a.size(); len <<= 1) {
    const std::complex<double> wl = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (size_t i = 0; i < a.size(); i += len) {
      std::complex<double> w = 1.0;
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  std::vector<double> mag(kFft / 2);
  for (size_t k = 0; k < mag.size(); ++k) mag[k] = std::abs(a[k]);
  return mag;
}

struct Point {
  double hz, measuredDb, expectedDb;
};

// At each sine: the level of `test` against `ref`, in dB, from the peak bin
// of each and its neighbours, against `expected` at that frequency.
std::vector<Point> response(const std::vector<float>& ref, const std::vector<float>& test,
                            const std::function<double(double)>& expected) {
  const auto r = spectrum(ref, kSteadyFrom), t = spectrum(test, kSteadyFrom);
  const double binHz = kRate / kFft;
  std::vector<Point> out;
  for (double hz : toneHz()) {
    const auto k = static_cast<size_t>(std::lround(hz / binHz));
    if (k < 2 || k + 2 >= r.size()) continue;
    double er = 0.0, et = 0.0;
    for (size_t j = k - 2; j <= k + 2; ++j) {
      er += r[j] * r[j];
      et += t[j] * t[j];
    }
    if (er <= 0.0) continue;
    out.push_back({hz, 10.0 * std::log10(std::max(et, 1e-30) / er), expected(hz)});
  }
  return out;
}

// Written for plotting when MP_SPECTRUM_DIR is set.
void keepForPlot(const std::string& name, const std::vector<float>& ref, const std::vector<float>& test,
                 const std::vector<Point>& points) {
  const char* dir = std::getenv("MP_SPECTRUM_DIR");
  if (dir == nullptr || *dir == 0) return;
  const juce::File folder(juce::String::fromUTF8(dir));
  folder.createDirectory();
  const auto wav = [&](const std::string& file, const std::vector<float>& x) {
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::FileOutputStream> os(folder.getChildFile(file).createOutputStream());
    if (os == nullptr) return;
    os->setPosition(0);
    os->truncate();
    std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), kRate, 1, 32, {}, 0));
    juce::AudioBuffer<float> b(1, static_cast<int>(x.size()));
    b.copyFrom(0, 0, x.data(), static_cast<int>(x.size()));
    w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
  };
  wav(name + "-ref.wav", ref);
  wav(name + "-test.wav", test);
  juce::String csv = "hz,measured_db,expected_db\n";
  for (const auto& p : points)
    csv << juce::String(p.hz, 2) << "," << juce::String(p.measuredDb, 3) << "," << juce::String(p.expectedDb, 3) << "\n";
  folder.getChildFile(name + ".csv").replaceWithText(csv);
}

// Every sine within `tolerance` dB of the formula where the formula is above
// `floorDb`; below it, at least as far down as `floorDb` + 6.
void checkResponse(const std::string& what, const std::vector<Point>& points, double tolerance,
                   double floorDb = -40.0) {
  MP_CHECK(points.size() >= 30, what + ": every sine measured, " + std::to_string(points.size()));
  double worst = 0.0;
  std::string at;
  for (const auto& p : points) {
    if (p.expectedDb < floorDb) {
      MP_CHECK(p.measuredDb < floorDb + 6.0, what + ": at " + std::to_string(p.hz) + " Hz, " +
                                                 std::to_string(p.measuredDb) + " dB, expected under " +
                                                 std::to_string(floorDb + 6.0));
      continue;
    }
    const double err = std::fabs(p.measuredDb - p.expectedDb);
    if (err > worst) {
      worst = err;
      at = std::to_string(p.hz) + " Hz: measured " + std::to_string(p.measuredDb) + " dB, expected " +
           std::to_string(p.expectedDb);
    }
  }
  MP_CHECK(worst <= tolerance, what + ": the spectrum follows the formula within " + std::to_string(tolerance) +
                                   " dB; worst at " + at);
}

// The cookbook's shelf with a slope of 1, worked out here on its own, at `hz`.
double shelfDb(bool high, double cornerHz, double db, double hz) {
  if (db == 0.0) return 0.0;
  const double A = std::pow(10.0, db / 40.0);
  const double w0 = 2.0 * kPi * cornerHz / kRate;
  const double cw = std::cos(w0), k = 2.0 * std::sqrt(A) * std::sin(w0) / 2.0 * std::sqrt(2.0);
  double b0, b1, b2, a0, a1, a2;
  if (high) {
    b0 = A * ((A + 1) + (A - 1) * cw + k);
    b1 = -2 * A * ((A - 1) + (A + 1) * cw);
    b2 = A * ((A + 1) + (A - 1) * cw - k);
    a0 = (A + 1) - (A - 1) * cw + k;
    a1 = 2 * ((A - 1) - (A + 1) * cw);
    a2 = (A + 1) - (A - 1) * cw - k;
  } else {
    b0 = A * ((A + 1) - (A - 1) * cw + k);
    b1 = 2 * A * ((A - 1) - (A + 1) * cw);
    b2 = A * ((A + 1) - (A - 1) * cw - k);
    a0 = (A + 1) + (A - 1) * cw + k;
    a1 = -2 * ((A - 1) + (A + 1) * cw);
    a2 = (A + 1) + (A - 1) * cw - k;
  }
  const std::complex<double> z = std::polar(1.0, -2.0 * kPi * hz / kRate);
  const auto h = (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
  return 20.0 * std::log10(std::abs(h));
}

#ifdef MP_OUTPUT_FILTERS
void onePair(mp::MasterpieceProcessor& p) { p.setPairCount(1); }

// An EQ that does something everywhere: shelves at both ends, peaks and
// dips between, and some moved off their own frequency and width.
mp::VoicingEqSettings shapedEq() {
  mp::VoicingEqSettings eq;
  eq.on = true;
  eq.bandsDb = {6.0, -4.0, 3.0, -6.0, 0.0, 9.0, -3.0, 0.0, 5.0, -8.0};
  eq.hz[5] = 1500.0;
  eq.q[5] = 4.0;
  eq.hz[3] = 300.0;
  eq.q[3] = 0.7;
  eq.gainDb = -2.0;
  return eq;
}

// The EQ on an output: the spectrum follows the curve the panel draws, the
// other output is not touched, a fader moved while the note sounds is heard,
// and off, flat or never set, nothing is filtered.
class OutputEqSpectrumTest final : public mp::test::Test {
public:
  OutputEqSpectrumTest() : Test("functional.output.eq-spectrum", Category::Functional) {}
  void run() override {
    const BroadbandOrgan organ("mp-output-eq");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const auto ref = render(organ.odf, 2, onePair);
    const auto eq = shapedEq();
    const auto shaped = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusEq(1, eq);
    });
    const auto points = response(ref[0], shaped[0], [&](double hz) { return mp::VoicingEq::responseDb(eq, hz); });
    keepForPlot("output-eq", ref[0], shaped[0], points);
    checkResponse("the output EQ", points, 0.25);
    MP_CHECK(response(ref[1], shaped[1], [&](double hz) { return mp::VoicingEq::responseDb(eq, hz); }).size() >= 30,
             "the right channel is measured too");
    checkResponse("the output EQ, right",
                  response(ref[1], shaped[1], [&](double hz) { return mp::VoicingEq::responseDb(eq, hz); }), 0.25);

    // Off, flat, or on and taken off again: the organ to the last bit.
    auto offEq = eq;
    offEq.on = false;
    const auto off = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusEq(1, offEq);
    });
    MP_CHECK(off == ref, "an EQ turned off filters nothing");
    mp::VoicingEqSettings flat;
    flat.on = true;
    const auto flatOn = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusEq(1, flat);
    });
    MP_CHECK(flatOn == ref, "an EQ on and flat filters nothing");
    const auto backOff = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusEq(1, eq);
      p.setBusEq(1, offEq);
    });
    MP_CHECK(backOff == ref, "an EQ turned on and off again filters nothing");

    // Two outputs, the rank on the first, the EQ on the second: the first
    // is as it was.
    const auto twoPairs = [](mp::MasterpieceProcessor& p) {
      p.setPairCount(2);
      p.setRankRoutes({{201, mp::BusId{1}}});
    };
    const auto refTwo = render(organ.odf, 4, twoPairs);
    const auto eqOnOther = render(organ.odf, 4, [&](mp::MasterpieceProcessor& p) {
      twoPairs(p);
      p.setBusEq(2, eq);
    });
    MP_CHECK(eqOnOther[0] == refTwo[0] && eqOnOther[1] == refTwo[1], "an EQ on output 2 leaves output 1 alone");
    const auto eqOnFirst = render(organ.odf, 4, [&](mp::MasterpieceProcessor& p) {
      twoPairs(p);
      p.setBusEq(1, eq);
    });
    checkResponse("the EQ on output 1 of two",
                  response(refTwo[0], eqOnFirst[0], [&](double hz) { return mp::VoicingEq::responseDb(eq, hz); }),
                  0.25);

    // A fader moved while the note sounds is heard at once: +9 dB at 1 kHz
    // becomes -9 dB, and the steady state after it is the new curve.
    mp::VoicingEqSettings up;
    up.on = true;
    up.bandsDb[5] = 9.0;
    auto down = up;
    down.bandsDb[5] = -9.0;
    const auto moved = render(
        organ.odf, 2,
        [&](mp::MasterpieceProcessor& p) {
          onePair(p);
          p.setBusEq(1, up);
        },
        [&](mp::MasterpieceProcessor& p) { p.setBusEq(1, down); });
    checkResponse("a fader moved while the note sounds",
                  response(ref[0], moved[0], [&](double hz) { return mp::VoicingEq::responseDb(down, hz); }), 0.25);

    // Kept with the outputs, the machine's, and read back.
    mp::MasterpieceProcessor p;
    p.setPairCount(2);
    p.setBusEq(2, eq);
    MP_CHECK(p.saveGlobalDefaults(), "the global file is written");
    const auto text = global.file.loadFileAsString();
    MP_CHECK(text.contains("buseq 2 1 -2 6 -4 3 -6 0 9 -3 0 5 -8 "), "the EQ is saved with the outputs: " + text.toStdString());
    mp::MasterpieceProcessor next;
    next.loadGlobalDefaults();
    const auto back = next.busEq(2);
    MP_CHECK(back.on && back.bandsDb == eq.bandsDb && back.gainDb == eq.gainDb && back.sameShape(eq),
             "and comes back as it was");
    MP_CHECK(!next.busEq(1).on && next.busEq(1).flat(), "output 1 has none");
    organ.root.deleteRecursively();
  }
};
static OutputEqSpectrumTest g_outputEqSpectrum;

// The high-pass on an output: a Butterworth curve, 12 or 24 dB an octave,
// -3 dB at its frequency; nothing filtered while it is off; with bass
// management, after the subwoofer has taken its lows.
class OutputHighPassSpectrumTest final : public mp::test::Test {
public:
  OutputHighPassSpectrumTest() : Test("functional.output.high-pass-spectrum", Category::Functional) {}
  void run() override {
    const BroadbandOrgan organ("mp-output-high-pass");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const auto ref = render(organ.odf, 2, onePair);
    for (const auto& [hz, slope] : std::vector<std::pair<float, int>>{{80.0f, 12}, {120.0f, 24}, {300.0f, 24}}) {
      const auto filtered = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
        onePair(p);
        p.setBusHighPass(1, hz, slope);
      });
      mp::BusHighPass h;
      h.hz = hz;
      h.slope = slope;
      const auto points =
          response(ref[0], filtered[0], [&](double f) { return mp::OutputHighPass::responseDb(h, f); });
      const std::string name = "high-pass-" + std::to_string(static_cast<int>(hz)) + "-" + std::to_string(slope);
      keepForPlot(name, ref[0], filtered[0], points);
      checkResponse("the high-pass at " + std::to_string(static_cast<int>(hz)) + " Hz, " + std::to_string(slope) +
                        " dB an octave",
                    points, 0.25, -40.0);
      // -3 dB at its frequency, by the formula the curve is checked against.
      MP_CHECK(std::fabs(mp::OutputHighPass::responseDb(h, hz) + 3.0103) < 0.01, "-3 dB at the corner");
    }

    const auto off = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusHighPass(1, 80.0f, 24);
      p.setBusHighPass(1, 0.0f, 24);
    });
    MP_CHECK(off == ref, "a high-pass turned off filters nothing");

    // With bass management to channel 3 at 120 Hz and a high-pass at 40 Hz:
    // the pair is the crossover's highs and then the high-pass; the
    // subwoofer keeps everything under the crossover, the high-pass's lows
    // included.
    const auto bassOnly = render(organ.odf, 3, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusBass(1, 120.0f, 2);
    });
    const auto both = render(organ.odf, 3, [&](mp::MasterpieceProcessor& p) {
      onePair(p);
      p.setBusBass(1, 120.0f, 2);
      p.setBusHighPass(1, 40.0f, 24);
    });
    MP_CHECK(both[2] == bassOnly[2], "the subwoofer is as the crossover makes it");
    mp::BusHighPass h40;
    h40.hz = 40.0f;
    h40.slope = 24;
    checkResponse("the high-pass after the crossover",
                  response(bassOnly[0], both[0], [&](double f) { return mp::OutputHighPass::responseDb(h40, f); }),
                  0.25, -30.0);

    mp::MasterpieceProcessor p;
    p.setPairCount(2);
    p.setBusHighPass(2, 80.0f, 24);
    p.setBusHighPass(1, 5.0f, 7);
    MP_CHECK(p.mixer().highPassFor(mp::BusId{1}) != nullptr && p.mixer().highPassFor(mp::BusId{1})->hz == 20.0f &&
                 p.mixer().highPassFor(mp::BusId{1})->slope == 12,
             "a frequency under 20 Hz is 20 Hz, a slope other than 24 is 12");
    MP_CHECK(p.saveGlobalDefaults(), "the global file is written");
    const auto text = global.file.loadFileAsString();
    MP_CHECK(text.contains("bushp 2 80 24") && text.contains("bushp 1 20 12"),
             "the high-passes are saved with the outputs: " + text.toStdString());
    mp::MasterpieceProcessor next;
    next.loadGlobalDefaults();
    const auto* back = next.mixer().highPassFor(mp::BusId{2});
    MP_CHECK(back != nullptr && back->hz == 80.0f && back->slope == 24, "and come back");
    organ.root.deleteRecursively();
  }
};
static OutputHighPassSpectrumTest g_outputHighPassSpectrum;

// A pipe's own bass and treble shelves (#1188): heard on that pipe, by the
// cookbook's shelves at 200 Hz and 3 kHz, the same as a rank's; a pipe
// without them, beside one with them, is not filtered at all.
class PipeShelvesSpectrumTest final : public mp::test::Test {
public:
  PipeShelvesSpectrumTest() : Test("functional.voicing.pipe-shelves-spectrum", Category::Functional) {}
  void run() override {
    const BroadbandOrgan organ("mp-pipe-shelves");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const auto ref = render(organ.odf, 2, [](mp::MasterpieceProcessor&) {});
    const auto shelves = [](float bass, float bright) {
      mp::PipeVoicing pv;
      pv.bassDb = bass;
      pv.brightnessDb = bright;
      return pv;
    };
    const auto pipeShelved = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      p.voicing().live().setPipe(kPipe36, shelves(9.0f, -9.0f));
      p.voicingChanged();
    });
    const auto expected = [](double hz) {
      return shelfDb(false, 200.0, 9.0, hz) + shelfDb(true, 3000.0, -9.0, hz);
    };
    const auto points = response(ref[0], pipeShelved[0], expected);
    keepForPlot("pipe-shelves", ref[0], pipeShelved[0], points);
    checkResponse("a pipe's own shelves", points, 0.25);

    // The same shelves on the rank: the same spectrum.
    const auto rankShelved = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      p.voicing().live().setRank(201, shelves(9.0f, -9.0f));
      p.applyVoicingEq();
    });
    const auto rankPoints = response(ref[0], rankShelved[0], expected);
    checkResponse("the rank's shelves", rankPoints, 0.25);
    double worst = 0.0;
    for (size_t i = 0; i < points.size() && i < rankPoints.size(); ++i)
      worst = std::max(worst, std::fabs(points[i].measuredDb - rankPoints[i].measuredDb));
    MP_CHECK(worst < 0.1, "a pipe's shelves sound as its rank's: " + std::to_string(worst) + " dB apart at most");

    // Key 37's pipe has shelves; key 36's has none and plays as recorded.
    const auto other = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      p.voicing().live().setPipe(kPipe37, shelves(9.0f, -9.0f));
      p.voicingChanged();
    });
    MP_CHECK(other == ref, "a pipe without shelves is not filtered");
    // A level of its own and no shelves: only the level.
    mp::PipeVoicing level;
    level.gainDb = -6.0f;
    const auto quieter = render(organ.odf, 2, [&](mp::MasterpieceProcessor& p) {
      p.voicing().live().setPipe(kPipe36, level);
      p.voicingChanged();
    });
    checkResponse("a pipe's level with no shelves", response(ref[0], quieter[0], [](double) { return -6.0; }), 0.05);
    organ.root.deleteRecursively();
  }
};
static PipeShelvesSpectrumTest g_pipeShelvesSpectrum;

#endif  // MP_OUTPUT_FILTERS

// What the filters cost (#1188): eight ranks, a chord of twenty keys on each,
// 160 pipes sounding, rendered for ten seconds of audio, the best of three.
// Plain is the organ with nothing set; it builds against the code before the
// filters too, to compare the two. Off is every filter set and then turned
// off or to 0 dB, and must cost what plain does; then each filter on.
class OutputFiltersPerfTest final : public mp::test::Test {
public:
  OutputFiltersPerfTest() : Test("perf.output.filters", Category::Perf) {}
  void run() override {
    const BroadbandOrgan organ("mp-output-filters-perf", 8, 61);
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();
    const double plain = best(organ, [](mp::MasterpieceProcessor& p) { p.setPairCount(1); });
    report("plain", plain, plain);
#ifdef MP_OUTPUT_FILTERS
    const auto eq = shapedEq();
    const double off = best(organ, [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(1);
      auto o = eq;
      o.on = false;
      p.setBusEq(1, o);
      p.setBusHighPass(1, 80.0f, 24);
      p.setBusHighPass(1, 0.0f, 24);
      mp::PipeVoicing level;  // a voicing, but no shelves
      level.gainDb = -1.0f;
      for (int r = 0; r < 8; ++r)
        for (int k = 0; k < 61; ++k) p.voicing().live().setPipe(static_cast<mp::Id>(3000 + 1000 * r + k), level);
    });
    report("filters off", off, plain);
    const double eqOn = best(organ, [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(1);
      p.setBusEq(1, eq);
    });
    report("output EQ on", eqOn, plain);
    const double hpOn = best(organ, [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(1);
      p.setBusHighPass(1, 80.0f, 24);
    });
    report("high-pass on", hpOn, plain);
    const double shelvesOn = best(organ, [&](mp::MasterpieceProcessor& p) {
      p.setPairCount(1);
      mp::PipeVoicing pv;
      pv.bassDb = 4.0f;
      pv.brightnessDb = -4.0f;
      for (int r = 0; r < 8; ++r)
        for (int k = 0; k < 61; ++k) p.voicing().live().setPipe(static_cast<mp::Id>(3000 + 1000 * r + k), pv);
    });
    report("every pipe shelved", shelvesOn, plain);
#ifdef NDEBUG
    MP_CHECK(off < plain * 1.05, "filters off cost what nothing set does");
#endif
#endif
    organ.root.deleteRecursively();
  }

private:
  static constexpr int kPerfBlocks = 1875;  // 10 s at 48 kHz in 256-frame blocks

  // Seconds of CPU per second of audio, the best of three.
  static double best(const BroadbandOrgan& organ, const SetUp& setUp) {
    double bestLoad = 1.0e9;
    for (int round = 0; round < 3; ++round) {
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(kRate, kBlock);
      MP_CHECK(proc.loadOrgan(organ.odf, 0, false).ok, "the organ loads");
      setUp(proc);
      for (int r = 0; r < 8; ++r) proc.setStopEngaged(static_cast<mp::Id>(901 + r), true);
      juce::AudioBuffer<float> buffer(2, kBlock);
      juce::MidiBuffer chord, none;
      for (int k = 0; k < 20; ++k) chord.addEvent(juce::MidiMessage::noteOn(1, 40 + k, 0.8f), 0);
      buffer.clear();
      proc.processBlock(buffer, chord);
      for (int b = 0; b < 50; ++b) proc.processBlock(buffer, none);  // past the attacks
      const auto t0 = std::clock();
      for (int b = 0; b < kPerfBlocks; ++b) {
        buffer.clear();
        proc.processBlock(buffer, none);
      }
      const double cpu = static_cast<double>(std::clock() - t0) / CLOCKS_PER_SEC;
      bestLoad = std::min(bestLoad, cpu / (kPerfBlocks * kBlock / kRate));
      proc.releaseResources();
    }
    return bestLoad;
  }
  static void report(const char* what, double load, double plain) {
    std::printf("        %-20s %6.2f%% of one core  (%+.1f%% against plain)\n", what, load * 100.0,
                (load / plain - 1.0) * 100.0);
  }
};
static OutputFiltersPerfTest g_outputFiltersPerf;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
