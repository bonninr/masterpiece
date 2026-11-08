// The pipe layer's voicing EQ (#815): a high shelf, read from the definition
// and applied to the layer's voices.
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

// A steady sine, looped over its second half.
mp::SampleBuffer tone(double hz) {
  mp::SampleBuffer b;
  b.numChannels = 1;
  b.sampleRate = kRate;
  b.numFrames = 48000;
  b.frames.resize(static_cast<size_t>(b.numFrames));
  for (int i = 0; i < b.numFrames; ++i)
    b.frames[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * i / kRate));
  b.loopStart = 24000;
  b.loopEnd = 48000;
  return b;
}

// One voice of the tone through a layer with the given shelf, rendered in
// blocks of `block` frames.
std::vector<float> play(double hz, double kHz, double widthPct, double db, int block) {
  const mp::SampleBuffer buf = tone(hz);
  mp::Pipe pipe;
  pipe.pipeId = 100;
  pipe.midiNote = 60;
  mp::PipeLayer layer;
  layer.layerId = 1;
  layer.voicingShelfKHz = kHz;
  layer.voicingShelfWidthPct = widthPct;
  layer.voicingShelfDb = db;
  mp::AttackSample a;
  a.id = 11;
  a.sample.sampleId = 1;
  a.sample.resolvedPitchHz = hz;
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

double rmsDb(const std::vector<float>& y) {
  double s = 0.0;
  int n = 0;
  for (size_t i = 9600; i < 19200 && i < y.size(); ++i, ++n) s += double(y[i]) * y[i];
  return n == 0 ? -999.0 : 10.0 * std::log10(s / n);
}

std::string fmt(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.3f", v);
  return b;
}

class VoicingShelfTest final : public mp::test::Test {
public:
  VoicingShelfTest() : Test("functional.sampler.voicing-shelf", Category::Functional) {}
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
                 "<o><a>401</a><b>301</b><i1>4</i1><j1>100</j1><k1>2.2</k1></o>"
                 "<Pipe_SoundEngine01_Layer><LayerID>402</LayerID><PipeID>302</PipeID>"
                 "<VoicingEQ01_TransitionFrequencyKHertz>15</VoicingEQ01_TransitionFrequencyKHertz>"
                 "<VoicingEQ01_TransitionWidthAsPercentOfTransitionFrequency>50</VoicingEQ01_TransitionWidthAsPercentOfTransitionFrequency>"
                 "<VoicingEQ01_HighFrequencyBoostDecibels>-5.5</VoicingEQ01_HighFrequencyBoostDecibels>"
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
    const auto& c = pipes[0].layers[0];
    MP_CHECK(c.voicingShelfKHz == 4.0 && c.voicingShelfWidthPct == 100.0 && c.voicingShelfDb == 2.2,
             "a compact row: 4 kHz, 100 %, +2.2 dB");
    const auto& f = pipes[1].layers[0];
    MP_CHECK(f.voicingShelfKHz == 15.0 && f.voicingShelfWidthPct == 50.0 && f.voicingShelfDb == -5.5,
             "a long-form row: 15 kHz, 50 %, -5.5 dB");
    const auto& n = pipes[2].layers[0];
    MP_CHECK(n.voicingShelfKHz == 2.0 && n.voicingShelfWidthPct == 100.0 && n.voicingShelfDb == 0.0,
             "a row that states none takes the format's defaults: 2 kHz, 100 %, 0 dB");

    // The shelf in the sound: +6 dB above 2 kHz.
    const double low = rmsDb(play(200.0, 2.0, 100.0, 6.0, 256)) - rmsDb(play(200.0, 2.0, 100.0, 0.0, 256));
    const double mid = rmsDb(play(2000.0, 2.0, 100.0, 6.0, 256)) - rmsDb(play(2000.0, 2.0, 100.0, 0.0, 256));
    const double high = rmsDb(play(8000.0, 2.0, 100.0, 6.0, 256)) - rmsDb(play(8000.0, 2.0, 100.0, 0.0, 256));
    MP_CHECK(std::fabs(low) < 0.1, "200 Hz, far below the transition, is left alone: " + fmt(low) + " dB");
    MP_CHECK(std::fabs(mid - 3.0) < 0.2, "at the transition, half the boost: " + fmt(mid) + " dB");
    MP_CHECK(high > 5.5 && high < 6.1, "two octaves above, the boost: " + fmt(high) + " dB");
    const double cut = rmsDb(play(8000.0, 2.0, 100.0, -6.0, 256)) - rmsDb(play(8000.0, 2.0, 100.0, 0.0, 256));
    MP_CHECK(cut < -5.5 && cut > -6.1, "a cut works the same way down: " + fmt(cut) + " dB");

    // No boost is no shelf: the voice is the one it always was, sample for
    // sample, whatever frequency and width the row states.
    const std::vector<float> plain = play(8000.0, 2.0, 100.0, 0.0, 256);
    const std::vector<float> stated = play(8000.0, 9.0, 40.0, 0.0, 256);
    MP_CHECK(!plain.empty() && plain == stated, "0 dB with other figures renders bit for bit the same");

    // The shelf carries its state across blocks: any block size, the same sound.
    const std::vector<float> a256 = play(3000.0, 2.0, 100.0, 4.0, 256);
    const std::vector<float> a100 = play(3000.0, 2.0, 100.0, 4.0, 100);
    const std::vector<float> a1000 = play(3000.0, 2.0, 100.0, 4.0, 1000);
    float worst = 0.0f;
    for (size_t i = 0; i < a256.size() && i < a100.size() && i < a1000.size(); ++i)
      worst = std::max({worst, std::fabs(a256[i] - a100[i]), std::fabs(a256[i] - a1000[i])});
    MP_CHECK(!a256.empty() && worst < 1.0e-5f,
             "blocks of 100, 256 and 1000 frames give the same sound: " + std::to_string(worst));
  }
};
static VoicingShelfTest g_voicingShelf;

}  // namespace
