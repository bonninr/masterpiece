// A wind system with a compartment too small for the explicit step (#1130):
// a three-litre feeder on a wide link between the blower and the reservoir,
// as each of the Rotterdam Transept's bellows has. The explicit step at its
// floor let only a fraction of the feeder's flow through, and the chest
// behind it starved. The implicit step carries what the links carry: under
// a steady draw the chest stands at the pressure the orifice law gives for
// the chain, at any audio block.
#include "MpTest.h"
#include "../src/mp_control/WindSolver.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kBlowerInches = 4.0;

struct Link {
  mp::Id a, b;
  double flow, ref;
};

// The blower, the feeder, the reservoir and the chest, in a line.
const Link kChain[] = {{2, 3, 24.0, 0.3}, {3, 4, 6.0, 0.3}, {4, 5, 3.0, 0.5}};

mp::OrganModel stiffOrgan() {
  mp::OrganModel m;
  mp::WindCompartment blower;
  blower.compartmentId = 2;
  blower.infiniteVolume = true;
  blower.defaultPressureInches = kBlowerInches;
  m.wind[2] = blower;
  const double volumes[] = {0.003, 0.3, 0.2};
  for (int i = 0; i < 3; ++i) {
    mp::WindCompartment c;
    c.compartmentId = 3 + i;
    c.infiniteVolume = false;
    c.volumeM3 = volumes[i];
    m.wind[c.compartmentId] = c;
  }
  for (const Link& l : kChain) {
    mp::WindCompartmentLink w;
    w.firstCompartmentId = l.a;
    w.secondCompartmentId = l.b;
    w.massFlowKgPerSec = l.flow;
    w.refPressureInches = l.ref;
    m.windLinks.push_back(w);
  }
  return m;
}

// The chest's pressure under a draw of `demand` at working pressure: the flow
// q = demand * sqrt(p / p0) passes every link, each dropping ref * (q / F)^2.
double expectedChest(double demand, double nominal) {
  double p = nominal;
  for (int i = 0; i < 100; ++i) {
    const double q = demand * std::sqrt(p / nominal);
    double drop = 0.0;
    for (const Link& l : kChain) drop += l.ref * (q / l.flow) * (q / l.flow);
    p = kBlowerInches - drop;
  }
  return p;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}

// Ten seconds under the draw in blocks of `frames` at 48 kHz; the chest's
// mean over the last five.
double play(int frames, double demand, double* nominal, double* stepSeconds) {
  const mp::OrganModel organ = stiffOrgan();
  mp::WindSolver wind;
  wind.reset(organ);
  const std::unordered_set<mp::Id> none;
  wind.settleWith(none);
  *nominal = wind.nominalFor(5);
  *stepSeconds = wind.stepSeconds();
  mp::EngineSwitch sw;
  sw.enableWindModel = true;
  const double dt = frames / 48000.0;
  const int blocks = static_cast<int>(10.0 / dt);
  double sum = 0.0;
  int n = 0;
  for (int b = 0; b < blocks; ++b) {
    wind.clearDemand();
    wind.addDemandDirect({5}, {static_cast<float>(demand)});
    wind.advance(dt, sw, none);
    if (b >= blocks / 2) {
      sum += wind.pressureFor(5);
      ++n;
    }
  }
  return sum / n;
}

class WindStiffTest final : public mp::test::Test {
public:
  WindStiffTest() : Test("functional.control.wind-stiff-feeder", Category::Functional) {}
  void run() override {
    double nominal = 0.0, step = 0.0;
    const double demand = 1.0;
    const double small = play(64, demand, &nominal, &step);
    double nominal2 = 0.0, step2 = 0.0;
    const double large = play(1024, demand, &nominal2, &step2);
    const double want = expectedChest(demand, nominal);
    std::printf("    step %.2e s; chest at rest %.4f in; under %.1f kg/s: %.4f in (64 frames), %.4f in (1024), "
                "the orifice law %.4f in\n",
                step, nominal, demand, small, large, want);
    MP_CHECK(std::abs(nominal - kBlowerInches) < 1e-3, "the chest settles at the blower's pressure: " + num(nominal));
    MP_CHECK(step > 1e-4, "a feeder this small takes the implicit step: " + num(step) + " s");
    MP_CHECK(std::abs(small - want) < 0.005 * want,
             "the chest stands where the links put it: " + num(small) + " in, the law says " + num(want));
    MP_CHECK(std::abs(small - large) < 1e-3, "the audio block does not move the wind: " + num(small) + " and " +
                                                  num(large));
  }
};
static WindStiffTest g_windStiff;

}  // namespace
