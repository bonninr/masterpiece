// Moving reservoir boards (#812): a reservoir's weighted board as a moving
// body. Off by default, and then the wind is the swallowed-volume model bit
// for bit; on, the reservoir holds the pressure its weight states, dips and
// wobbles when a chord lands, and settles again.
#include "MpTest.h"
#include "../src/mp_control/WindSolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using mp::test::Category;

constexpr double kPaPerInch = 249.09;

// A blower at eight inches feeding a reservoir through its inlet valve, the
// reservoir feeding a chest, the chest leaking to the open air. The board's
// height drives the inlet valve through the linkage the sets use, Klais's
// (127 - extension - 20) * 2.2: open up to 39 % of the travel, shut from 84 %.
mp::OrganModel reservoirOrgan(int shape = 1, double gravityKg = 0.0) {
  mp::OrganModel m;
  mp::WindCompartment air;
  air.compartmentId = 1;
  air.infiniteVolume = true;
  m.wind[1] = air;
  mp::WindCompartment blower;
  blower.compartmentId = 2;
  blower.infiniteVolume = true;
  blower.defaultPressureInches = 8.0;
  m.wind[2] = blower;
  mp::WindCompartment res;
  res.compartmentId = 3;
  res.infiniteVolume = false;
  res.volumeM3 = 0.5;
  res.hasBellows = true;
  res.bellowsWidthM = 1.5;
  res.bellowsLengthM = 1.5;
  res.bellowsExtensionM = 0.4;
  // Four inches over the 2.25 m2 frame.
  res.bellowsGravityMassKg = gravityKg > 0.0 ? gravityKg : 4.0 * kPaPerInch * 2.25 / 9.81;
  res.bellowsMassKg = 300.0;
  res.bellowsDamping = 10.0;
  res.bellowsShapeCode = shape;
  res.bellowsHingeDistanceM = shape == 2 ? 1.2 : 0.0;
  res.bellowsExtensionOutputControlId = 20;
  m.wind[3] = res;
  mp::WindCompartment chest;
  chest.compartmentId = 4;
  chest.infiniteVolume = false;
  // Large enough to be integrated in the solver's 256 steps a block at 256
  // frames: a 0.05 m3 chest on this trunk needs a 12 us step.
  chest.volumeM3 = 0.5;
  m.wind[4] = chest;
  mp::WindCompartmentLink inlet;
  inlet.firstCompartmentId = 2;
  inlet.secondCompartmentId = 3;
  inlet.massFlowKgPerSec = 0.5;
  inlet.refPressureInches = 3.0;
  inlet.valveControlId = 21;
  inlet.valveControlTypeCode = 3;
  m.windLinks.push_back(inlet);
  mp::WindCompartmentLink trunk;
  trunk.firstCompartmentId = 3;
  trunk.secondCompartmentId = 4;
  trunk.massFlowKgPerSec = 0.5;
  trunk.refPressureInches = 1.0;
  m.windLinks.push_back(trunk);
  mp::WindCompartmentLink leak;
  leak.firstCompartmentId = 4;
  leak.secondCompartmentId = 1;
  leak.massFlowKgPerSec = 0.002;
  leak.refPressureInches = 3.0;
  m.windLinks.push_back(leak);
  mp::ContinuousControlLinkage regulator;
  regulator.sourceControlId = 20;
  regulator.destControlId = 21;
  regulator.invert = true;
  regulator.increment = -20.0;
  regulator.coefficient = 2.2;
  m.controlLinkages.push_back(regulator);
  return m;
}

struct Trace {
  double nominalRes = 0.0, nominalChest = 0.0, extension = -1.0;
  std::vector<double> res, chest, ext;
};

// 1 s at rest, a chord drawing 0.3 kg/s from the chest for 3 s, then 6 s at
// rest, in blocks of `frames` at 48 kHz.
Trace play(const mp::OrganModel& m, bool boards, int frames = 256) {
  mp::WindSolver wind;
  wind.setBellowsBoard(boards);
  wind.reset(m);
  Trace t;
  t.nominalRes = wind.nominalFor(3);
  t.nominalChest = wind.nominalFor(4);
  t.extension = wind.extensionFor(3);
  const mp::EngineSwitch full;
  const std::unordered_set<mp::Id> nothing;
  const std::vector<mp::Id> chests{4};
  const double dt = frames / 48000.0;
  const int blocks = static_cast<int>(10.0 / dt);
  for (int i = 0; i < blocks; ++i) {
    // In frames, so the chord lands on the same frame whatever the block.
    const long long at = static_cast<long long>(i) * frames;
    const std::vector<float> demand{at >= 48000 && at < 4 * 48000 ? 0.3f : 0.0f};
    wind.clearDemand();
    wind.addDemandDirect(chests, demand);
    wind.advance(dt, full, nothing);
    t.res.push_back(wind.pressureFor(3));
    t.chest.push_back(wind.pressureFor(4));
    t.ext.push_back(wind.extensionFor(3));
  }
  return t;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", v);
  return b;
}

class BellowsBoardTest final : public mp::test::Test {
public:
  BellowsBoardTest() : Test("functional.control.bellows-board", Category::Functional) {}
  void run() override {
    const mp::OrganModel organ = reservoirOrgan();
    const double dt = 256.0 / 48000.0;
    const auto block = [dt](double s) { return static_cast<size_t>(s / dt); };

    // Off: the swallowed-volume model, the same as a solver never told.
    {
      mp::WindSolver never;
      never.reset(organ);
      const Trace off = play(organ, false);
      MP_CHECK(off.extension < 0.0, "off: no moving board");
      MP_CHECK(off.nominalRes == never.nominalFor(3), "off: the working pressure unchanged");
      // A board the file leaves without its weight stays the swallowed volume.
      mp::OrganModel weightless = organ;
      weightless.wind[3].bellowsGravityMassKg = 0.0;
      const Trace a = play(weightless, true), b = play(weightless, false);
      MP_CHECK(a.res == b.res && a.chest == b.chest, "on, a board with no weight: bit-identical to off");
      // An organ without bellows: bit-identical with the option on.
      mp::OrganModel plain = organ;
      plain.wind[3].hasBellows = false;
      const Trace c = play(plain, true), d = play(plain, false);
      MP_CHECK(c.res == d.res && c.chest == d.chest, "on, no bellows: bit-identical to off");
    }

    // On: the reservoir holds the pressure its weight states.
    const Trace on = play(organ, true);
    std::printf("    on: reservoir %.4f in, chest %.4f in, board at %.1f of 127\n", on.nominalRes,
                on.nominalChest, on.extension);
    MP_CHECK(std::abs(on.nominalRes - 4.0) < 0.04, "settles at the weight's 4 inches: " + num(on.nominalRes));
    MP_CHECK(on.extension > 49.0 && on.extension < 107.0,
             "the board floats where the inlet valve is part open: " + num(on.extension));
    double restSwing = 0.0;
    for (size_t i = 0; i < block(1.0); ++i) restSwing = std::max(restSwing, std::abs(on.res[i] - on.nominalRes));
    MP_CHECK(restSwing < 1.0e-3, "at rest it stays there: " + num(restSwing));

    // The chord: the reservoir dips, the board falls and opens the valve.
    double low = 1e9, lowExt = 1e9;
    for (size_t i = block(1.0); i < block(4.0); ++i) {
      low = std::min(low, on.res[i]);
      lowExt = std::min(lowExt, on.ext[i]);
    }
    std::printf("    chord: reservoir down to %.4f in, board down to %.1f\n", low, lowExt);
    MP_CHECK(low < on.nominalRes - 0.01, "a chord dips the reservoir: " + num(low));
    MP_CHECK(lowExt < on.extension - 1.0, "and the board falls: " + num(lowExt));

    // Released: it overshoots, swings about its pressure, and settles.
    double high = -1e9;
    int crossings = 0;
    for (size_t i = block(4.0); i < block(7.0); ++i) {
      high = std::max(high, on.res[i]);
      if (i > block(4.0) && (on.res[i] - on.nominalRes) * (on.res[i - 1] - on.nominalRes) < 0.0) ++crossings;
    }
    double tailSwing = 0.0;
    for (size_t i = block(9.0); i < on.res.size(); ++i)
      tailSwing = std::max(tailSwing, std::abs(on.res[i] - on.nominalRes));
    std::printf("    release: up to %.4f in, %d crossings in 3 s, within %.5f in at 9-10 s\n", high, crossings,
                tailSwing);
    MP_CHECK(high > on.nominalRes + 0.01, "released, the pressure overshoots: " + num(high));
    MP_CHECK(crossings >= 2, "and swings about its pressure: " + std::to_string(crossings));
    // Within half a percent; what is left is the damper's push while the
    // board climbs back to where its valve shuts.
    MP_CHECK(tailSwing < 0.02, "and settles: " + num(tailSwing));

    // The same with the board fixed: the swallowed-volume reservoir neither
    // overshoots nor swings.
    const Trace off = play(organ, false);
    double offHigh = -1e9;
    for (size_t i = block(4.0); i < block(7.0); ++i) offHigh = std::max(offHigh, off.res[i]);
    std::printf("    off: reservoir %.4f in, after release up to %.4f in\n", off.nominalRes, offHigh);
    MP_CHECK(offHigh < off.nominalRes + 0.001, "off, no overshoot: " + num(offHigh - off.nominalRes));

    // The host's block size does not reach the physics. Blocks of 64 and
    // 192 frames both start the chord on its frame; compared at every 192.
    const Trace small = play(organ, true, 64), mid = play(organ, true, 192);
    double apart = 0.0;
    for (size_t k = 0; k < mid.res.size() && 3 * k + 2 < small.res.size(); ++k)
      apart = std::max(apart, std::abs(small.res[3 * k + 2] - mid.res[k]));
    std::printf("    blocks of 64 and 192: at most %.5f in apart\n", apart);
    MP_CHECK(apart < 0.01, "blocks of 64 and 192 alike throughout: " + num(apart));

    // A hinged board: its weight acts at its centre of mass, q from the
    // hinge, against the pressure's turn about the hinge: p = m g q / (A L / 2).
    const double hingedKg = 3.0 * kPaPerInch * 2.25 * 1.5 / 2.0 / 9.81 / 1.2;
    const Trace hinged = play(reservoirOrgan(2, hingedKg), true);
    std::printf("    hinged: reservoir %.4f in, board at %.1f\n", hinged.nominalRes, hinged.extension);
    MP_CHECK(std::abs(hinged.nominalRes - 3.0) < 0.03, "a hinged board holds 3 inches: " + num(hinged.nominalRes));

    // Every run alike.
    MP_CHECK(play(organ, true).res == on.res, "the same on every run");
  }
};
static BellowsBoardTest g_bellowsBoard;

// A blower outlet of 0.4 m3 fed at 10 kg/s, as Pinerolo's is: at the smallest
// step it chatters around its balance for ever. The settle with the boards
// ends once nothing drifts, well before its sixty seconds, and the reservoir
// holds the same pressure as with the full settle.
class BellowsBoardSettleTest final : public mp::test::Test {
public:
  BellowsBoardSettleTest() : Test("functional.control.bellows-board-settle", Category::Functional) {}
  void run() override {
    mp::OrganModel m = reservoirOrgan();
    mp::WindCompartment outlet;
    outlet.compartmentId = 5;
    outlet.infiniteVolume = false;
    outlet.volumeM3 = 0.4;
    m.wind[5] = outlet;
    m.windLinks[0].firstCompartmentId = 5;
    mp::WindCompartmentLink fan;
    fan.firstCompartmentId = 2;
    fan.secondCompartmentId = 5;
    fan.massFlowKgPerSec = 10.0;
    fan.refPressureInches = 1.0;
    m.windLinks.push_back(fan);

    mp::WindSolver wind;
    wind.setBellowsBoard(true);
    wind.reset(m);
    wind.settleWith({});
    const double seconds = static_cast<double>(wind.lastSettleSteps()) * wind.stepSeconds();
    std::printf("    settle: %lld steps of %.3g s, %.1f s of air; reservoir %.4f in, board at %.1f\n",
                wind.lastSettleSteps(), wind.stepSeconds(), seconds, wind.nominalFor(3), wind.extensionFor(3));
    MP_CHECK(seconds < 30.0, "the settle ends before its limit: " + num(seconds) + " s");
    MP_CHECK(std::abs(wind.nominalFor(3) - 4.0) < 0.05, "the reservoir holds its 4 inches: " + num(wind.nominalFor(3)));
  }
};
static BellowsBoardSettleTest g_bellowsBoardSettle;

}  // namespace
