// Reservoirs whose boards shut their own intakes as they rise (#1130), with
// the figures and the chain of Burton-Berlin Hill: a blower at 21.8 inches,
// its output chamber, bellows in parallel each fed through a valve its board
// drives, a trunk and a chest. The board, the air under it and the valve
// close a loop. At the set's damping, three such bellows in parallel hunted
// together, and under a steady draw the chest swung between half and one and
// a half times its pressure ten times a second; one alone held. With the
// bellows boards on, the chest holds steady under the draw either way.
#include "MpTest.h"
#include "../src/mp_control/WindSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace {

using mp::test::Category;

constexpr mp::Id kBlower = 2, kOutput = 3, kTrunk = 4, kChest = 5;
constexpr mp::Id kFirstBellows = 10;
constexpr mp::Id kFirstExtension = 30, kFirstValve = 50;

mp::WindCompartment finite(mp::Id id, double volume) {
  mp::WindCompartment c;
  c.compartmentId = id;
  c.infiniteVolume = false;
  c.volumeM3 = volume;
  return c;
}

void link(mp::OrganModel& m, mp::Id a, mp::Id b, double flow, double ref, mp::Id valve = 0) {
  mp::WindCompartmentLink l;
  l.firstCompartmentId = a;
  l.secondCompartmentId = b;
  l.massFlowKgPerSec = flow;
  l.refPressureInches = ref;
  if (valve != 0) {
    l.valveControlId = valve;
    l.valveControlTypeCode = 3;
  }
  m.windLinks.push_back(l);
}

mp::OrganModel regulatorOrgan(int bellows) {
  mp::OrganModel m;
  mp::WindCompartment blower;
  blower.compartmentId = kBlower;
  blower.infiniteVolume = true;
  blower.defaultPressureInches = 21.8;
  m.wind[kBlower] = blower;
  m.wind[kOutput] = finite(kOutput, 0.3);
  m.wind[kTrunk] = finite(kTrunk, 0.171);
  m.wind[kChest] = finite(kChest, 0.65);
  link(m, kBlower, kOutput, 150.0, 21.8);
  for (int i = 0; i < bellows; ++i) {
    const mp::Id id = kFirstBellows + i;
    mp::WindCompartment res = finite(id, 0.1296);
    res.hasBellows = true;
    res.bellowsShapeCode = 2;
    res.bellowsWidthM = 0.9;
    res.bellowsLengthM = 2.88;
    res.bellowsExtensionM = 0.39;
    res.bellowsGravityMassKg = 145.13;
    res.bellowsMassKg = 101.59;
    res.bellowsHingeDistanceM = 2.016;
    res.bellowsDamping = 4.07;
    res.bellowsExtensionOutputControlId = kFirstExtension + i;
    m.wind[id] = res;
    // The intake: open while the board is low, shut over its last tenth.
    link(m, kOutput, id, 10.0, 8.8, kFirstValve + i);
    mp::ContinuousControlLinkage drive;
    drive.sourceControlId = kFirstExtension + i;
    drive.destControlId = kFirstValve + i;
    drive.invert = true;
    drive.increment = -10.0;
    m.controlLinkages.push_back(drive);
    link(m, id, kTrunk, 10.0, 8.8);
  }
  link(m, kTrunk, kChest, 10.0, 1.4);
  return m;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}

class WindRegulatorTest final : public mp::test::Test {
public:
  WindRegulatorTest() : Test("functional.control.wind-regulator-steady", Category::Functional) {}
  void run() override {
    for (const int bellows : {1, 3}) {
      const mp::OrganModel organ = regulatorOrgan(bellows);
      mp::WindSolver wind;
      wind.setBellowsBoard(true);
      wind.reset(organ);
      const std::unordered_set<mp::Id> none;
      const double nominal = wind.nominalFor(kChest);
      mp::EngineSwitch sw;
      sw.enableWindModel = true;
      const double dt = 480 / 48000.0;
      const int blocks = static_cast<int>(20.0 / dt);
      double low = 1e9, high = -1e9;
      for (int b = 0; b < blocks; ++b) {
        wind.clearDemand();
        wind.addDemandDirect({kChest}, {1.0f});
        wind.advance(dt, sw, none);
        if (b >= blocks / 2) {
          low = std::min(low, wind.pressureFor(kChest));
          high = std::max(high, wind.pressureFor(kChest));
        }
      }
      const std::string n = std::to_string(bellows) + " bellows";
      std::printf("    %s: chest at rest %.3f in; under the draw, the last ten seconds %.3f to %.3f in; board at %.0f "
                  "of 127\n",
                  n.c_str(), nominal, low, high, wind.extensionFor(kFirstBellows));
      MP_CHECK(nominal > 1.0, n + ": the chest has wind at rest: " + num(nominal) + " in");
      MP_CHECK(low > 0.8 * nominal, n + ": the chest keeps its wind under the draw: lowest " + num(low) + " of " +
                                        num(nominal));
      MP_CHECK(high - low < 0.02 * nominal, n + ": the chest holds steady: it moves " + num(high - low) + " in");
    }
  }
};
static WindRegulatorTest g_windRegulator;

}  // namespace
