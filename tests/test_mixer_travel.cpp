// The mixer strip's travel (#1195): the 10 dB under 0 dB take the most room,
// each 10 dB further down less; the travel only places the cap, so every
// live level step comes back to itself through it.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_audio/LiveLevels.h"
#include "../src/mp_ui/MixerStrip.h"

#include <cmath>
#include <string>

namespace {

using mp::test::Category;
using mp::ChannelStrip;
using mp::LiveLevels;

class MixerTravelTest final : public mp::test::Test {
public:
  MixerTravelTest() : Test("functional.ui.mixer-travel", Category::Functional) {}
  void run() override {
    const auto at = [](float db) { return ChannelStrip::travelOf(LiveLevels::valueOf(db)); };
    MP_CHECK(ChannelStrip::travelOf(0) == 0.0, "silence at the bottom");
    MP_CHECK(ChannelStrip::travelOf(LiveLevels::kTop) == 1.0, "+24 dB at the top");
    MP_CHECK(ChannelStrip::travelOf(LiveLevels::kFull) == ChannelStrip::kUnityAt, "0 dB three quarters up");
    // Each mark within a step of where the table puts it: a step is 40/127 dB.
    const double marks[][2] = {{-10.0, 0.48}, {-20.0, 0.28}, {-30.0, 0.14}};
    for (const auto& m : marks)
      MP_CHECK(std::fabs(at(static_cast<float>(m[0])) - m[1]) < 0.01,
               std::to_string(m[0]) + " dB at " + std::to_string(at(static_cast<float>(m[0]))));
    // The top 10 dB of cut take more travel than any 10 dB under them.
    const double top10 = at(0.0f) - at(-10.0f), next10 = at(-10.0f) - at(-20.0f), low10 = at(-30.0f) - at(-40.0f);
    MP_CHECK(top10 > next10 && next10 > low10, "the travel per 10 dB shrinks going down: " + std::to_string(top10) +
                                                   ", " + std::to_string(next10) + ", " + std::to_string(low10));
    MP_CHECK(top10 > 0.25, "0 to -10 dB takes over a quarter of the travel: " + std::to_string(top10));
    double last = -1.0;
    for (int v = 0; v <= LiveLevels::kTop; ++v) {
      const double p = ChannelStrip::travelOf(v);
      MP_CHECK(p > last, "the travel rises at step " + std::to_string(v));
      last = p;
      MP_CHECK(std::lround(ChannelStrip::valueAt(p)) == v,
               "step " + std::to_string(v) + " comes back from its place: " + std::to_string(ChannelStrip::valueAt(p)));
    }
  }
};
static MixerTravelTest g_mixerTravel;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
