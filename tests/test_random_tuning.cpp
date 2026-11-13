// Each pipe's random tuning error under a temperament (#808).
#include "MpTest.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_core/RandomWalk.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;

class RandomTuningErrorTest final : public mp::test::Test {
public:
  RandomTuningErrorTest() : Test("functional.odf.random-tuning-error", Category::Functional) {}
  void run() override {
    // The draw: the same for a pipe on every load, held inside the largest
    // error, and with the stated share of pipes inside it.
    MP_CHECK(mp::randomTuningUnit(301, 3, 99.0) == mp::randomTuningUnit(301, 3, 99.0),
             "a pipe errs the same way on every load");
    MP_CHECK(mp::randomTuningUnit(301, 3, 99.0) != mp::randomTuningUnit(301, 4, 99.0),
             "another probability index errs another way");
    const auto atLimit = [](double prob) {
      int n = 0, held = 0;
      bool inside = true;
      for (uint64_t id = 1; id <= 20000; ++id) {
        const double u = mp::randomTuningUnit(id, 3, prob);
        inside = inside && std::fabs(u) <= 1.0;
        if (std::fabs(u) >= 1.0) ++held;
        ++n;
      }
      return inside ? static_cast<double>(held) / n : -1.0;
    };
    const double share99 = atLimit(99.0);
    MP_CHECK(share99 > 0.005 && share99 < 0.015,
             "99 %: one pipe in a hundred reaches the largest error: " + std::to_string(share99));
    const double share50 = atLimit(50.0);
    MP_CHECK(share50 > 0.47 && share50 < 0.53,
             "50 %: half the pipes reach it: " + std::to_string(share50));

    // The limit in cents: the smaller of the two stated, the hertz one at the
    // pipe's own pitch, as the sets state them.
    MP_CHECK(std::fabs(mp::tuningErrorCents(1.0, 1.1, 2.0, 440.0) - 1.1) < 1e-9,
             "1.1 % of a semitone or 2 Hz at A4: 1.1 cents");
    MP_CHECK(std::fabs(mp::tuningErrorCents(1.0, 1199.9, 0.25, 440.0) - 0.98338) < 1e-4,
             "0.25 Hz at A4: 0.98 cents");
    MP_CHECK(std::fabs(mp::tuningErrorCents(-0.5, 1199.9, 0.25, 65.406) - -3.3023) < 1e-3,
             "half of 0.25 Hz flat at C2: 3.30 cents");
    MP_CHECK(mp::tuningErrorCents(1.0, 0.0, 0.0, 440.0) == 0.0, "no limit stated, no error");
    // Both limits bound the error (#1050): one left at 0 allows none. The
    // composite's pipes state 1199.9 % of a semitone and no hertz limit; read
    // as a limit alone, that put notes up to an octave off.
    MP_CHECK(mp::tuningErrorCents(1.0, 1199.9, 0.0, 65.406) == 0.0, "1199.9 % and no hertz limit: in tune");
    MP_CHECK(mp::tuningErrorCents(1.0, 0.0, 2.0, 440.0) == 0.0, "2 Hz and no semitone limit: in tune");

    // The loader: a pipe that states a limit carries its draw; one that does
    // not stays exactly in tune.
    mp::OdfLoader l;
    mp::OdfLoader::Options o;
    mp::OrganModel m;
    mp::OdfDiagnostics d;
    MP_CHECK(l.loadFromXmlString(
                 "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
                 "<ObjectList ObjectType=\"_General\"><o><a>1</a></o></ObjectList>"
                 "<ObjectList ObjectType=\"Rank\"><o><a>201</a><b>R</b></o></ObjectList>"
                 "<ObjectList ObjectType=\"Pipe_SoundEngine01\">"
                 "<o><a>301</a><b>201</b><d>69</d><h>99</h><i>1.1</i><j>2</j><k>3</k></o>"
                 "<o><a>302</a><b>201</b><d>70</d></o>"
                 "<Pipe_SoundEngine01><PipeID>303</PipeID><RankID>201</RankID><NormalMIDINoteNumber>71</NormalMIDINoteNumber>"
                 "<Pitch_Tempered_RandomTuningError_MaxDetuningPctSemitones>1199.9</Pitch_Tempered_RandomTuningError_MaxDetuningPctSemitones>"
                 "<Pitch_Tempered_RandomTuningError_MaxDetuningHz>0.25</Pitch_Tempered_RandomTuningError_MaxDetuningHz>"
                 "</Pipe_SoundEngine01>"
                 "<o><a>304</a><b>201</b><d>72</d><h>9.7e+1</h><i>1.1999e+3</i><k>2e+1</k></o>"
                 "</ObjectList></Hauptwerk>",
                 "a.Organ_Hauptwerk_xml", o, m, d),
             "the definition loads");
    const auto& pipes = m.ranks.at(201).pipes;
    MP_CHECK(pipes.size() == 4, "four pipes are read");
    if (pipes.size() != 4) return;
    MP_CHECK(pipes[0].tuningErrorMaxCents == 1.1 && pipes[0].tuningErrorMaxHz == 2.0 &&
                 pipes[0].tuningErrorUnit == mp::randomTuningUnit(301, 3, 99.0),
             "a compact row's limits and draw");
    MP_CHECK(pipes[1].tuningErrorUnit == 0.0, "a row that states no limit stays in tune");
    MP_CHECK(pipes[2].tuningErrorMaxHz == 0.25 && pipes[2].tuningErrorUnit != 0.0 &&
                 pipes[2].tuningErrorUnit == mp::randomTuningUnit(303, 4, 99.0),
             "a long-form row, read with the format's defaults: index 4, 99 %");
    MP_CHECK(pipes[3].tuningErrorMaxCents == 1199.9 && pipes[3].tuningErrorMaxHz == 0.0 &&
                 pipes[3].tuningErrorUnit == 0.0,
             "the composite's row, 1199.9 % with no hertz limit, stays in tune");
  }
};
static RandomTuningErrorTest g_randomTuningError;

}  // namespace
