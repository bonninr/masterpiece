// The level of a pipe layer's third and upper harmonics (#809): read from the
// definition and heard as a shelf between the second and third harmonic of
// the pipe's pitch.
#include "MpTest.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_sampler/VoiceEngine.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kRate = 48000.0;
constexpr double kPi = 3.141592653589793;
constexpr int kHarmonics[] = {1, 2, 3, 5};

// A steady tone of the first, second, third and fifth harmonic of f0, looped
// over its second half.
mp::SampleBuffer tone(double f0) {
  mp::SampleBuffer b;
  b.numChannels = 1;
  b.sampleRate = kRate;
  b.numFrames = 48000;
  b.frames.resize(static_cast<size_t>(b.numFrames));
  for (int i = 0; i < b.numFrames; ++i) {
    double s = 0.0;
    for (int h : kHarmonics) s += 0.15 * std::sin(2.0 * kPi * f0 * h * i / kRate);
    b.frames[static_cast<size_t>(i)] = static_cast<float>(s);
  }
  b.loopStart = 24000;
  b.loopEnd = 48000;
  return b;
}

struct Layer {
  double recordedHz = 0.0;  // the attack's resolved pitch; 0 for none
  int midiNote = 57;
  int harmonicNum = 8;
  double harmonicDb = 0.0;
  double shelfDb = 0.0;  // the voicing EQ (#815), above 2 kHz
};

// One voice of the tone through a layer, rendered in blocks of `block` frames.
std::vector<float> play(double f0, const Layer& l, int block) {
  const mp::SampleBuffer buf = tone(f0);
  mp::Pipe pipe;
  pipe.pipeId = 100;
  pipe.midiNote = l.midiNote;
  pipe.basePitch64ftHarmonicNum = l.harmonicNum;
  mp::PipeLayer layer;
  layer.layerId = 1;
  layer.harmonicLevelDb = l.harmonicDb;
  layer.voicingShelfKHz = 2.0;
  layer.voicingShelfDb = l.shelfDb;
  mp::AttackSample a;
  a.id = 11;
  a.sample.sampleId = 1;
  a.sample.resolvedPitchHz = l.recordedHz;
  layer.attacks.push_back(a);
  pipe.layers.push_back(std::move(layer));
  mp::VoiceEngine eng;
  eng.prepare(kRate, 4, 1);
  eng.setSampleProvider([&](mp::Id id) -> const mp::SampleBuffer* { return id == 1 ? &buf : nullptr; });
  mp::VoiceStart start;
  start.pipe = &pipe;
  start.layer = &pipe.layers[0];
  start.velocity = 127;
  start.ratio = 1.0;
  start.gain = 1.0f;
  std::vector<float> y(24000, 0.0f);
  if (eng.startVoice(start, 7) < 0) return {};
  for (size_t from = 0; from < y.size();) {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), y.size() - from));
    float* out[1] = {y.data() + from};
    eng.render(out, 1, n);
    from += static_cast<size_t>(n);
  }
  return y;
}

// The level of one frequency over 9600 frames, a whole number of its periods
// for every harmonic of 220 Hz.
double levelDb(const std::vector<float>& y, double hz) {
  double re = 0.0, im = 0.0;
  for (size_t i = 9600; i < 19200 && i < y.size(); ++i) {
    const double w = 2.0 * kPi * hz * static_cast<double>(i) / kRate;
    re += y[i] * std::cos(w);
    im += y[i] * std::sin(w);
  }
  return 10.0 * std::log10(re * re + im * im + 1e-30);
}

std::string fmt(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

class HarmonicLevelTest final : public mp::test::Test {
public:
  HarmonicLevelTest() : Test("functional.sampler.harmonic-level", Category::Functional) {}
  void run() override {
    // The loader: a compact row, a long-form row, and a row that states none.
    mp::OdfLoader l;
    mp::OdfLoader::Options o;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><o><a>1</a></o></ObjectList>"
                 "<ObjectList ObjectType=\"Rank\"><o><a>201</a><b>R</b></o></ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
                 "<o><a>301</a><b>201</b><d>69</d></o><o><a>302</a><b>201</b><d>70</d></o>"
                 "<o><a>303</a><b>201</b><d>71</d></o></ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01_Layer\">"
                 "<o><a>401</a><b>301</b><c1>1.25</c1></o>"
                 "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID>"
                 "<HarmonicShaping_ThirdAndUpperHarmonicsLevelAdjustDecibels>-0.4"
                 "</HarmonicShaping_ThirdAndUpperHarmonicsLevelAdjustDecibels>"
                 "</Pipe_SoundEngine01_Layer>"
                 "<o><a>403</a><b>303</b></o>"
                 "</ObjectList></Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "the definition loads");
    const auto& pipes = m.ranks.at(201).pipes;
    MP_CHECK(pipes.size() == 3 && pipes[0].layers.size() == 1 && pipes[1].layers.size() == 1 &&
                 pipes[2].layers.size() == 1,
             "three pipes of one layer each");
    if (pipes.size() != 3 || pipes[2].layers.empty()) return;
    MP_CHECK(pipes[0].layers[0].harmonicLevelDb == 1.25, "a compact row: +1.25 dB");
    MP_CHECK(pipes[1].layers[0].harmonicLevelDb == -0.4, "a long-form row: -0.4 dB");
    MP_CHECK(pipes[2].layers[0].harmonicLevelDb == 0.0, "a row that states none: 0 dB");

    // In the sound: +6 dB on a 220 Hz pipe. The fundamental stays, the third
    // harmonic takes most of the lift and the fifth nearly all of it.
    const double f0 = 220.0;
    Layer plain;
    plain.recordedHz = f0;
    Layer lifted = plain;
    lifted.harmonicDb = 6.0;
    const std::vector<float> p = play(f0, plain, 256);
    const std::vector<float> q = play(f0, lifted, 256);
    const double expect[] = {0.17, 1.88, 4.12, 5.65};
    for (size_t i = 0; i < 4; ++i) {
      const double hz = f0 * kHarmonics[i];
      const double got = levelDb(q, hz) - levelDb(p, hz);
      MP_CHECK(std::fabs(got - expect[i]) < 0.25, "harmonic " + std::to_string(kHarmonics[i]) + ": " +
                                                      fmt(got) + " dB, expected " + fmt(expect[i]));
    }
    const double h3 = levelDb(q, 3 * f0) - levelDb(p, 3 * f0);
    const double cut = levelDb(play(f0, [&] { Layer c = plain; c.harmonicDb = -6.0; return c; }(), 256), 3 * f0) -
                       levelDb(p, 3 * f0);
    MP_CHECK(std::fabs(cut + h3) < 0.05, "a cut is the lift mirrored: " + fmt(cut) + " dB");

    // A recording with no stated pitch takes its key's at its footage: A3 on
    // an 8' rank is 220 Hz, the same shelf.
    Layer keyed = lifted;
    keyed.recordedHz = 0.0;
    keyed.midiNote = 57;
    keyed.harmonicNum = 8;
    MP_CHECK(play(f0, keyed, 256) == q, "a pipe with no recorded pitch takes its key's");
    Layer octave = keyed;
    octave.midiNote = 45;
    octave.harmonicNum = 16;  // A2 on a 4' rank sounds A3
    MP_CHECK(play(f0, octave, 256) == q, "and its rank's footage");

    // 0 dB is no shelf: the voice is the one it always was, sample for sample.
    Layer none = plain;
    none.harmonicDb = 0.0;
    MP_CHECK(!p.empty() && play(f0, none, 256) == p, "0 dB renders bit for bit the same");
    // A pipe whose third harmonic lies above the band has nothing to adjust.
    Layer high;
    high.recordedHz = 8000.0;
    Layer highLifted = high;
    highLifted.harmonicDb = 6.0;
    MP_CHECK(play(8000.0, highLifted, 256) == play(8000.0, high, 256),
             "an 8 kHz pipe renders bit for bit the same");

    // With the voicing EQ: two shelves in a row, their levels added.
    Layer eq = plain;
    eq.shelfDb = 6.0;
    Layer both = lifted;
    both.shelfDb = 6.0;
    const std::vector<float> e = play(f0, eq, 256);
    const std::vector<float> b = play(f0, both, 256);
    for (int h : kHarmonics) {
      const double hz = f0 * h;
      const double sum = (levelDb(e, hz) - levelDb(p, hz)) + (levelDb(q, hz) - levelDb(p, hz));
      const double got = levelDb(b, hz) - levelDb(p, hz);
      MP_CHECK(std::fabs(got - sum) < 0.05, "harmonic " + std::to_string(h) + " through both: " + fmt(got) +
                                                " dB, the two apart add to " + fmt(sum));
    }

    // The shelves carry their state across blocks: any block size, the same
    // sound.
    const std::vector<float> b100 = play(f0, both, 100);
    const std::vector<float> b1000 = play(f0, both, 1000);
    float worst = 0.0f;
    for (size_t i = 0; i < b.size() && i < b100.size() && i < b1000.size(); ++i)
      worst = std::max({worst, std::fabs(b[i] - b100[i]), std::fabs(b[i] - b1000[i])});
    MP_CHECK(!b.empty() && worst < 1.0e-5f,
             "blocks of 100, 256 and 1000 frames give the same sound: " + std::to_string(worst));
  }
};
static HarmonicLevelTest g_harmonicLevel;

}  // namespace
