// The drift of the wind (#807): the air a link carries into a chest, and the
// air a pipe draws while it speaks, each moving inside the range the set
// states, with the same model as the tremulant's drift (RandomWalk).
#include "MpTest.h"
#include "../src/mp_control/WindSolver.h"
#include "../src/mp_sampler/VoiceEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using mp::test::Category;

// A blower at five inches feeding one chest through a supply pipe, with a
// leak to the open air.
mp::OrganModel windOrgan(const mp::RandomWalk& supplyDrift) {
  mp::OrganModel m;
  mp::WindCompartment air;
  air.compartmentId = 1;
  air.infiniteVolume = true;
  air.defaultPressureInches = 0.0;
  m.wind[1] = air;
  mp::WindCompartment blower;
  blower.compartmentId = 2;
  blower.infiniteVolume = true;
  blower.defaultPressureInches = 5.0;
  m.wind[2] = blower;
  mp::WindCompartment chest;
  chest.compartmentId = 3;
  chest.infiniteVolume = false;
  chest.volumeM3 = 0.3;
  m.wind[3] = chest;
  mp::WindCompartmentLink supply;
  supply.firstCompartmentId = 2;
  supply.secondCompartmentId = 3;
  supply.massFlowKgPerSec = 0.05;
  supply.refPressureInches = 3.0;
  supply.flowRandom = supplyDrift;
  m.windLinks.push_back(supply);
  mp::WindCompartmentLink leak;
  leak.firstCompartmentId = 3;
  leak.secondCompartmentId = 1;
  leak.massFlowKgPerSec = 0.002;
  leak.refPressureInches = 3.0;
  m.windLinks.push_back(leak);
  return m;
}

// The chest's pressure, block by block, under a steady chord's draw: 2 s to
// settle, then 20 s recorded.
std::vector<double> chestPressure(const mp::OrganModel& m, double* nominal) {
  mp::WindSolver wind;
  wind.reset(m);
  if (nominal != nullptr) *nominal = wind.nominalFor(3);
  const mp::EngineSwitch full;
  const std::unordered_set<mp::Id> nothing;
  const std::vector<mp::Id> chests{3};
  const std::vector<float> demand{0.02f};
  std::vector<double> out;
  const double dt = 256.0 / 48000.0;
  for (int i = 0; i < static_cast<int>(22.0 / dt); ++i) {
    wind.clearDemand();
    wind.addDemandDirect(chests, demand);
    wind.advance(dt, full, nothing);
    if (i * dt >= 2.0) out.push_back(wind.pressureFor(3));
  }
  return out;
}

double spread(const std::vector<double>& v) {
  double mean = 0.0, var = 0.0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  for (double x : v) var += (x - mean) * (x - mean);
  return std::sqrt(var / static_cast<double>(v.size()));
}

class WindRandomLinkTest final : public mp::test::Test {
public:
  WindRandomLinkTest() : Test("functional.control.wind-random-link", Category::Functional) {}
  void run() override {
    // St. John's supply figures: 200, 23, at most 10 %.
    const mp::RandomWalk drift{true, 200.0, 23.0, 50, 10.0};
    mp::RandomWalk declined = drift;
    declined.on = false;

    double nominalSteady = 0.0, nominalDeclined = 0.0, nominalDrift = 0.0;
    const auto steady = chestPressure(windOrgan(mp::RandomWalk{}), &nominalSteady);
    const auto off = chestPressure(windOrgan(declined), &nominalDeclined);
    const auto moving = chestPressure(windOrgan(drift), &nominalDrift);

    MP_CHECK(off == steady && nominalDeclined == nominalSteady,
             "a drift the set declines leaves the wind exactly as none");
    MP_CHECK(nominalDrift == nominalSteady,
             "the working pressure is found from the steady supply");
    const double s0 = spread(steady), s1 = spread(moving);
    std::printf("        chest under a chord: steady spread %.5f in, drifting %.5f in (working %.3f in)\n",
                s0, s1, nominalSteady);
    MP_CHECK(s0 < 1e-6, "the steady supply holds the chest still: " + std::to_string(s0));
    MP_CHECK(s1 > 0.01 && s1 < 0.5,
             "the drifting supply moves the chest's pressure: " + std::to_string(s1) + " in");
    MP_CHECK(chestPressure(windOrgan(drift), nullptr) == moving, "the same drift on every run");
  }
};
static WindRandomLinkTest g_windRandomLink;

// A sounding pipe's draw: steady as stated without the fields, drifting
// inside the stated range with them.
class WindRandomPipeTest final : public mp::test::Test {
public:
  WindRandomPipeTest() : Test("functional.voice.wind-random-pipe", Category::Functional) {}
  void run() override {
    mp::SampleBuffer tone;
    tone.numChannels = 1;
    tone.sampleRate = 48000.0;
    tone.numFrames = 4800;
    tone.frames.resize(4800);
    for (int i = 0; i < 4800; ++i)
      tone.frames[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 480.0 * i / 48000.0));
    tone.loopStart = 2400;
    tone.loopEnd = 4800;

    mp::Pipe pipe;
    pipe.pipeId = 100;
    pipe.midiNote = 69;
    mp::PipeLayer layer;
    layer.layerId = 1;
    mp::AttackSample a;
    a.id = 11;
    a.sample.sampleId = 1;
    layer.attacks.push_back(a);
    pipe.layers.push_back(layer);

    const double dt = 256.0 / 48000.0;
    const auto draws = [&](const mp::RandomWalk& w, uint64_t seed) {
      mp::VoiceEngine eng;
      eng.prepare(48000.0, 256, 1);
      eng.setSampleProvider([&tone](mp::Id id) -> const mp::SampleBuffer* {
        return id == 1 ? &tone : nullptr;
      });
      mp::VoiceStart s;
      s.pipe = &pipe;
      s.layer = &pipe.layers[0];
      s.attackIndex = 0;
      s.velocity = 100;
      s.ratio = 1.0;
      s.gain = 1.0f;
      s.windIndex = 0;
      s.windFlowKgPerSec = 0.01f;
      s.windFlowRandom = w;
      s.windRandomSeed = seed;
      std::vector<float> out;
      if (eng.startVoice(s, 1) < 0) return out;
      for (int i = 0; i < 4000; ++i) {
        float d = 0.0f;
        eng.gatherWindDemand(&d, 1, dt);
        out.push_back(d);
      }
      return out;
    };

    const auto steady = draws(mp::RandomWalk{}, 0);
    MP_CHECK(!steady.empty(), "the pipe sounds");
    MP_CHECK(std::all_of(steady.begin(), steady.end(), [](float d) { return d == 0.01f; }),
             "without a drift the pipe draws what it states");

    // The wind-flow figures most sets state: 200, 7, at most 5 %.
    const mp::RandomWalk drift{true, 200.0, 7.0, 300, 5.0};
    const auto moving = draws(drift, 42);
    MP_CHECK(moving.size() == steady.size(), "the drifting pipe sounds");
    float lo = 1.0f, hi = 0.0f;
    std::vector<double> asDouble;
    for (float d : moving) {
      lo = std::min(lo, d);
      hi = std::max(hi, d);
      asDouble.push_back(d);
    }
    MP_CHECK(lo >= 0.01f * 0.95f - 1e-7f && hi <= 0.01f * 1.05f + 1e-7f,
             "inside 5 %: " + std::to_string(lo) + " .. " + std::to_string(hi));
    MP_CHECK(hi - lo > 0.01f * 0.05f, "and over most of it: " + std::to_string(lo) + " .. " + std::to_string(hi));
    MP_CHECK(spread(asDouble) > 0.01 * 0.02, "spread across the range: " + std::to_string(spread(asDouble)));
    MP_CHECK(draws(drift, 42) == moving, "one seed, one drift");
    MP_CHECK(draws(drift, 43) != moving, "another voice drifts apart");
  }
};
static WindRandomPipeTest g_windRandomPipe;

}  // namespace
