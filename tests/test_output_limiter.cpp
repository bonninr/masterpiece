// The peak limiter on the final output.
//
// Off by default: while it is off the audio thread does not call it at all
// and the output is bit for bit what the engine made. On, it holds the
// signal under a ceiling, with the two channels of a pair linked.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/OutputLimiter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

double db(double linear) { return 20.0 * std::log10(linear); }

// A sine of this amplitude, one second of it.
std::vector<float> sine(double freq, double amplitude, int frames) {
  std::vector<float> out(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i)
    out[static_cast<size_t>(i)] =
        static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * i / kRate));
  return out;
}

// --------------------------------------------------------- the limiter alone

// Off: the samples come out exactly as they went in, and nothing was
// processed at all.
class LimiterOffTest final : public mp::test::Test {
public:
  LimiterOffTest() : Test("functional.limiter.off", Category::Functional) {}
  void run() override {
    const int frames = 4096;
    mp::OutputLimiter limiter;
    limiter.prepare(kRate);
    auto left = sine(440.0, 0.7, frames);
    auto right = sine(660.0, 0.5, frames);
    const auto left0 = left;
    const auto right0 = right;
    float* channels[2] = {left.data(), right.data()};
    limiter.process(channels, 2, frames);
    MP_CHECK(std::memcmp(left0.data(), left.data(), frames * sizeof(float)) == 0,
             "off: the left channel is untouched");
    MP_CHECK(std::memcmp(right0.data(), right.data(), frames * sizeof(float)) == 0,
             "off: the right channel is untouched");
    MP_CHECK(limiter.blocksProcessed() == 0, "off: no block was processed");
  }
};

// A full-scale sine, 6 dB over a -6 dBFS ceiling, comes out at the ceiling
// and nothing comes out above it.
class LimiterCeilingTest final : public mp::test::Test {
public:
  LimiterCeilingTest() : Test("functional.limiter.ceiling", Category::Functional) {}
  void run() override {
    const int frames = 48000;
    mp::OutputLimiter limiter;
    limiter.prepare(kRate);
    limiter.setEnabled(true);
    limiter.setCeilingDb(-6.0f);
    limiter.setReleaseMs(100.0f);
    auto left = sine(440.0, 1.0, frames);
    auto right = left;
    float* channels[2] = {left.data(), right.data()};
    limiter.process(channels, 2, frames);

    float peak = 0.0f;
    float worstOver = -100.0f;
    for (int i = 0; i < frames; ++i) {
      peak = std::max(peak, std::fabs(left[static_cast<size_t>(i)]));
      const float over = static_cast<float>(db(std::fabs(left[static_cast<size_t>(i)])) + 6.0);
      if (over > worstOver) worstOver = over;
    }
    MP_CHECK(std::fabs(db(peak) + 6.0) < 0.2,
             "the output sits at the ceiling, got " + std::to_string(db(peak)));
    MP_CHECK(worstOver <= 0.5f,
             "no sample is more than 0.5 dB over the ceiling, the worst is " +
                 std::to_string(worstOver));
    // The two sides of the pair get the same gain.
    MP_CHECK(right[1000] == left[1000], "the pair is linked");
  }
};

// A signal under the threshold comes out as it went in.
class LimiterQuietTest final : public mp::test::Test {
public:
  LimiterQuietTest() : Test("functional.limiter.quiet", Category::Functional) {}
  void run() override {
    const int frames = 48000;
    mp::OutputLimiter limiter;
    limiter.prepare(kRate);
    limiter.setEnabled(true);
    limiter.setCeilingDb(-1.0f);
    limiter.setReleaseMs(100.0f);
    auto left = sine(220.0, 0.1, frames);  // -20 dBFS
    auto right = left;
    const auto left0 = left;
    float* channels[2] = {left.data(), right.data()};
    limiter.process(channels, 2, frames);

    float worst = 0.0f;
    for (int i = 0; i < frames; ++i) {
      const float a = static_cast<float>(db(std::fabs(left0[static_cast<size_t>(i)])));
      const float b = static_cast<float>(db(std::fabs(left[static_cast<size_t>(i)])));
      if (std::fabs(a - b) > worst) worst = std::fabs(a - b);
    }
    MP_CHECK(worst < 0.01f, "a quiet signal is unchanged to within 0.01 dB, the worst is " +
                                std::to_string(worst));
  }
};

// The two sides of a pair are linked: the louder one sets the gain, and the
// quieter one is held down with it.
class LimiterLinkedTest final : public mp::test::Test {
public:
  LimiterLinkedTest() : Test("functional.limiter.linked", Category::Functional) {}
  void run() override {
    const int frames = 48000;
    mp::OutputLimiter limiter;
    limiter.prepare(kRate);
    limiter.setEnabled(true);
    limiter.setCeilingDb(-6.0f);
    limiter.setReleaseMs(100.0f);
    auto left = sine(440.0, 1.0, frames);
    auto right = sine(440.0, 0.1, frames);
    float* channels[2] = {left.data(), right.data()};
    limiter.process(channels, 2, frames);
    // The quiet side keeps its own level in proportion: the same gain was
    // applied to both.
    const float ratio = right[1000] / left[1000];
    MP_CHECK(std::fabs(ratio - 0.1f) < 1.0e-4f,
             "the quiet side is held down with the loud one, the ratio is " +
                 std::to_string(ratio));
  }
};

// The gain comes back at the release rate after a peak.
class LimiterReleaseTest final : public mp::test::Test {
public:
  LimiterReleaseTest() : Test("functional.limiter.release", Category::Functional) {}
  void run() override {
    mp::OutputLimiter limiter;
    limiter.prepare(kRate);
    limiter.setEnabled(true);
    limiter.setCeilingDb(-6.0f);
    limiter.setReleaseMs(100.0f);
    std::vector<float> left(4800, 1.0f), right(4800, 1.0f);
    float* channels[2] = {left.data(), right.data()};
    limiter.process(channels, 2, 4800);
    const float held = limiter.gainReductionDb();
    MP_CHECK(std::fabs(held - 6.0f) < 0.2f,
             "the loud passage is held at the ceiling, the reduction is " +
                 std::to_string(held));
    // A tenth of a second of silence: the reduction has fallen to about a
    // third of what it was.
    std::vector<float> quiet(4800, 0.0f), quietRight(4800, 0.0f);
    float* quietChannels[2] = {quiet.data(), quietRight.data()};
    limiter.process(quietChannels, 2, 4800);
    const float after = limiter.gainReductionDb();
    MP_CHECK(std::fabs(after - 6.0f / std::exp(1.0f)) < 0.2f,
             "the gain comes back at the release rate, the reduction is " +
                 std::to_string(after));
  }
};

// Switching the limiter off and on again: the gain starts at unity, with no
// reduction left over from the last time it was in use.
class LimiterSwitchedOnTest final : public mp::test::Test {
public:
  LimiterSwitchedOnTest() : Test("functional.limiter.switched-on", Category::Functional) {}
  void run() override {
    mp::OutputLimiter limiter;
    limiter.prepare(kRate);
    limiter.setEnabled(true);
    limiter.setCeilingDb(-6.0f);
    limiter.setReleaseMs(1000.0f);
    std::vector<float> left(4800, 1.0f), right(4800, 1.0f);
    float* channels[2] = {left.data(), right.data()};
    limiter.process(channels, 2, 4800);
    MP_CHECK(std::fabs(limiter.gainReductionDb() - 6.0f) < 0.2f,
             "the loud passage is held at the ceiling, the reduction is " +
                 std::to_string(limiter.gainReductionDb()));
    limiter.setEnabled(false);
    limiter.setEnabled(true);
    std::vector<float> again(4800, 1.0f), againRight(4800, 1.0f);
    float* againChannels[2] = {again.data(), againRight.data()};
    limiter.process(againChannels, 2, 4800);
    MP_CHECK(std::fabs(limiter.gainReductionDb() - 6.0f) < 0.2f,
             "and the limiter starts from unity when it is switched on, the reduction is " +
                 std::to_string(limiter.gainReductionDb()));
  }
};

// ------------------------------------------------------------- the processor

// A small organ with one stop, so that the engine has something to make.
struct Organ {
  juce::File odf;
  juce::File root;

  explicit Organ(const juce::String& name) {
    root = juce::File::getSpecialLocation(juce::File::tempDirectory)
               .getChildFile(name);
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
      std::unique_ptr<juce::AudioFormatWriter> w(
          fmt.createWriterFor(os.release(), kRate, 1, 16, meta, 0));
      juce::AudioBuffer<float> tone(1, 48000);
      for (int i = 0; i < 48000; ++i)
        tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * kPi * 130.81 * i / kRate)));
      w->writeFromAudioSampleBuffer(tone, 0, 48000);
    }
    odf = defs.getChildFile("limiter.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Limiter</Identification_Name>"
        "<Identification_UniqueOrganID>4401</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>101</SampleID><InstallationPackageID>1</InstallationPackageID>"
        "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
        "<Pitch_ExactSamplePitch>130.81</Pitch_ExactSamplePitch></Sample></ObjectList>"
        "<ObjectList ObjectType=\"Rank\"><Rank><RankID>201</RankID><Name>A</Name></Rank></ObjectList>"
        "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
        "<Pipe_SoundEngine01><PipeID>301</PipeID><RankID>201</RankID><NormalMIDINoteNumber>36</NormalMIDINoteNumber></Pipe_SoundEngine01>"
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
        "<StopRank><StopID>901</StopID><RankID>201</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
        "<NumberOfMappedDivisionInputNodes>1</NumberOfMappedDivisionInputNodes></StopRank>"
        "</ObjectList><ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>1101</SwitchID><Name>A</Name><Latching>Y</Latching></Switch>"
        "</ObjectList></Hauptwerk>");
  }
};

// A file that is put back as it was when the test is over.
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

// The engine's output, block by block, with the limiter as told.
std::vector<float> render(const juce::File& odf, bool limiterOn, float ceilingDb,
                          float masterGain) {
  mp::MasterpieceProcessor proc;
  proc.prepareToPlay(kRate, 256);
  const auto loaded = proc.loadOrgan(odf, 0, false);
  MP_CHECK(loaded.ok, "the organ loads");
  proc.setStopEngaged(901, true);
  // The level the limiter works on, set after the load so that the organ's
  // own volume cannot change it.
  if (auto* gain = proc.apvts().getParameter("masterGain"))
    gain->setValueNotifyingHost(masterGain);
  // The limiter is set on the limiter itself, so the test writes no settings
  // of its own.
  proc.limiter().setCeilingDb(ceilingDb);
  proc.limiter().setEnabled(limiterOn);

  juce::AudioBuffer<float> buffer(2, 256);
  juce::MidiBuffer on;
  on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
  juce::MidiBuffer none;
  std::vector<float> out;
  out.reserve(40 * 256 * 2);
  for (int block = 0; block < 40; ++block) {
    buffer.clear();
    proc.processBlock(buffer, block == 0 ? on : none);
    for (int ch = 0; ch < 2; ++ch)
      for (int i = 0; i < 256; ++i) out.push_back(buffer.getSample(ch, i));
  }
  proc.releaseResources();
  return out;
}

float peakOf(const std::vector<float>& samples) {
  float peak = 0.0f;
  for (float v : samples) peak = std::max(peak, std::fabs(v));
  return peak;
}

// The engine's output with the limiter off is bit for bit what it is
// without the limiter, and a limiter that is working changes it.
class LimiterProcessorTest final : public mp::test::Test {
public:
  LimiterProcessorTest() : Test("functional.limiter.processor", Category::Functional) {}
  void run() override {
    const Organ organ("mp-limiter-processor");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    // The level the limiter works on: 4 dB below full scale. It has to be
    // under the knee for the first run and over the ceiling for the last
    // one, and the ceiling can only go as low as -12 dBFS.
    const auto probeOut = render(organ.odf, /*limiterOn=*/false, -0.1f, 1.0f);
    MP_CHECK(!probeOut.empty(), "the engine made something");
    const float wanted = static_cast<float>(std::pow(10.0, -4.0 / 20.0));
    const float gain = std::clamp(wanted / std::max(peakOf(probeOut), 1.0e-6f),
                                   0.01f, 16.0f);

    const auto off = render(organ.odf, /*limiterOn=*/false, -0.1f, gain);
    const float peakOff = peakOf(off);
    MP_CHECK(peakOff > 0.05f, "the engine's output is audible, the peak is " +
                                   std::to_string(peakOff));
    MP_CHECK(db(peakOff) < -1.6, "and it is under the knee of the limiter, " +
                                      std::to_string(db(peakOff)) + " dBFS");

    // The limiter engaged, with a ceiling the signal never reaches: the
    // output is the engine's own, bit for bit.
    const auto idle = render(organ.odf, /*limiterOn=*/true, -0.1f, gain);
    MP_CHECK(idle.size() == off.size(), "the two runs made the same blocks");
    MP_CHECK(std::memcmp(off.data(), idle.data(), off.size() * sizeof(float)) == 0,
             "a limiter that is not limiting leaves the output bit for bit the same");

    // The same engine with a ceiling under the signal: now it holds the
    // output down, which is what tells the two runs apart.
    const auto held = render(organ.odf, /*limiterOn=*/true, -12.0f, gain);
    const float peakHeld = peakOf(held);
    MP_CHECK(std::fabs(db(peakHeld) + 12.0) < 0.2,
             "the output is held at the ceiling, the peak is " + std::to_string(db(peakHeld)));
    MP_CHECK(std::memcmp(off.data(), held.data(), off.size() * sizeof(float)) != 0,
             "and it is not the same as the engine's own output");

    organ.root.deleteRecursively();
  }
};

// The three settings are kept in the global file.
class LimiterSettingsTest final : public mp::test::Test {
public:
  LimiterSettingsTest() : Test("functional.limiter.settings", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    Keep global(proc.globalSettingsFile());
    global.file.getParentDirectory().createDirectory();

    proc.setLimiterEnabled(true);
    proc.setLimiterCeilingDb(-3.5f);
    proc.setLimiterReleaseMs(250.0f);
    const auto text = global.file.loadFileAsString();
    MP_CHECK(text.contains("limiter 1 -3.5 250"),
             "the global file holds the three settings, it holds: " + text.toStdString());

    // And they come back at the next start.
    mp::MasterpieceProcessor next;
    next.loadGlobalDefaults();
    MP_CHECK(next.limiterEnabled(), "the limiter is on at the next start");
    MP_CHECK(std::fabs(next.limiterCeilingDb() + 3.5f) < 0.01f, "with the ceiling");
    MP_CHECK(std::fabs(next.limiterReleaseMs() - 250.0f) < 0.01f, "and the release");
  }
};
// ------------------------------------------- the mixer's pairs on their own channels

// The organ through a processor with `outputs` output channels and the mixer
// as told, the stop drawn and one key held: every channel, block after block.
std::vector<std::vector<float>> renderPairs(const juce::File& odf, int outputs, int pairs, int rankPair) {
  mp::MasterpieceProcessor proc;
  juce::AudioProcessor::BusesLayout layout;
  layout.outputBuses.add(outputs == 2 ? juce::AudioChannelSet::stereo()
                                      : juce::AudioChannelSet::discreteChannels(outputs));
  MP_CHECK(proc.setBusesLayout(layout), "the processor takes " + std::to_string(outputs) + " outputs");
  proc.prepareToPlay(kRate, 256);
  const auto loaded = proc.loadOrgan(odf, 0, false);
  MP_CHECK(loaded.ok, "the organ loads");
  auto& mixer = proc.mixer();
  mixer.buses.clear();
  for (int i = 0; i < pairs; ++i) {
    mp::MixerBus b;
    b.id = mp::BusId{i + 1};
    b.deviceChannels = {2 * i, 2 * i + 1};
    mixer.buses.push_back(b);
  }
  mp::RankRouting r;
  r.rankId = 201;
  r.perspectives[0].dest = mp::BusId{rankPair};
  mixer.rankRoutings[201] = r;
  proc.refreshMixerBuses();
  proc.setStopEngaged(901, true);

  juce::AudioBuffer<float> buffer(outputs, 256);
  juce::MidiBuffer on;
  on.addEvent(juce::MidiMessage::noteOn(1, 36, 0.8f), 0);
  juce::MidiBuffer none;
  std::vector<std::vector<float>> out(static_cast<size_t>(outputs));
  for (int block = 0; block < 20; ++block) {
    buffer.clear();
    proc.processBlock(buffer, block == 0 ? on : none);
    for (int ch = 0; ch < outputs; ++ch)
      out[static_cast<size_t>(ch)].insert(out[static_cast<size_t>(ch)].end(), buffer.getReadPointer(ch),
                                          buffer.getReadPointer(ch) + 256);
  }
  proc.releaseResources();
  return out;
}

float peakOfChannel(const std::vector<float>& c) {
  float peak = 0.0f;
  for (float v : c) peak = std::max(peak, std::fabs(v));
  return peak;
}

// A rank on the second of three pairs plays on device channels 3 and 4 of a
// six-channel output, and nowhere else; on a stereo output the same organ is
// summed into the pair it has, bit for bit as with one pair.
class OutputPairsTest final : public mp::test::Test {
public:
  OutputPairsTest() : Test("functional.output.pairs", Category::Functional) {}
  void run() override {
    const Organ organ("mp-output-pairs");
    mp::MasterpieceProcessor probe;
    Keep global(probe.globalSettingsFile());
    Keep organSettings(probe.settingsFileFor(organ.odf));
    global.file.getParentDirectory().createDirectory();

    const auto wide = renderPairs(organ.odf, 6, 3, 2);
    MP_CHECK(peakOfChannel(wide[2]) > 0.01f && peakOfChannel(wide[3]) > 0.01f,
             "the rank sounds on channels 3 and 4: " + std::to_string(peakOfChannel(wide[2])));
    for (int ch : {0, 1, 4, 5})
      MP_CHECK(peakOfChannel(wide[static_cast<size_t>(ch)]) == 0.0f,
               "and channel " + std::to_string(ch + 1) + " is silent");

    // Stereo: the pairs are summed into the one there is, and that is the
    // organ as one pair plays it, bit for bit.
    const auto summed = renderPairs(organ.odf, 2, 3, 2);
    const auto single = renderPairs(organ.odf, 2, 1, 1);
    MP_CHECK(peakOfChannel(summed[0]) > 0.01f, "on a stereo output the pair is heard on 1 and 2");
    MP_CHECK(summed[0] == single[0] && summed[1] == single[1],
             "and it is bit for bit the organ with one pair");
    // The pair on its own channels is the same signal the stereo output has.
    MP_CHECK(wide[2] == summed[0] && wide[3] == summed[1],
             "the pair's own channels carry exactly what the first pair carries in stereo");

    // What the standalone asks the device for.
    mp::MasterpieceProcessor p;
    MP_CHECK(p.outputChannelsWanted() == 2, "one pair wants two channels");
    for (int i = 0; i < 3; ++i) {
      mp::MixerBus b;
      b.id = mp::BusId{i + 2};
      b.deviceChannels = {2 * i + 2, 2 * i + 3};
      p.mixer().buses.push_back(b);
    }
    MP_CHECK(p.outputChannelsWanted() == 8, "pairs up to channels 7 and 8 want eight");
    juce::AudioProcessor::BusesLayout l33;
    l33.outputBuses.add(juce::AudioChannelSet::discreteChannels(33));
    MP_CHECK(!p.checkBusesLayoutSupported(l33), "33 outputs are more than the engine takes");

    organ.root.deleteRecursively();
  }
};
static OutputPairsTest g_outputPairs;

static LimiterOffTest g_limiterOff;
static LimiterCeilingTest g_limiterCeiling;
static LimiterQuietTest g_limiterQuiet;
static LimiterLinkedTest g_limiterLinked;
static LimiterReleaseTest g_limiterRelease;
static LimiterSwitchedOnTest g_limiterSwitchedOn;
static LimiterProcessorTest g_limiterProcessor;
static LimiterSettingsTest g_limiterSettings;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
