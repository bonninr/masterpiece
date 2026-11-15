// A small chest fed hard by its blower (#1121): the wind it gives its pipes
// holds steady from one audio block to the next, idle and with a chord on it.
// A chest so small that the solver's step floor binds used to chatter around
// its supply's pressure, and each block ended on a different phase of the
// chatter: the pipes' level jumped at every block boundary, which is a crackle.
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

// A blower at four inches, a chest of the given volume on it, and a leak from
// the chest to the open air.
mp::OrganModel chestOrgan(double chestM3, double feedKgPerSec) {
  mp::OrganModel m;
  mp::WindCompartment air;
  air.compartmentId = 1;
  air.infiniteVolume = true;
  m.wind[1] = air;
  mp::WindCompartment blower;
  blower.compartmentId = 2;
  blower.infiniteVolume = true;
  blower.defaultPressureInches = 4.0;
  m.wind[2] = blower;
  mp::WindCompartment chest;
  chest.compartmentId = 3;
  chest.infiniteVolume = false;
  chest.volumeM3 = chestM3;
  m.wind[3] = chest;
  mp::WindCompartmentLink feed;
  feed.firstCompartmentId = 2;
  feed.secondCompartmentId = 3;
  feed.massFlowKgPerSec = feedKgPerSec;
  feed.refPressureInches = 0.5;
  m.windLinks.push_back(feed);
  mp::WindCompartmentLink leak;
  leak.firstCompartmentId = 3;
  leak.secondCompartmentId = 1;
  leak.massFlowKgPerSec = 0.005;
  leak.refPressureInches = 0.5;
  m.windLinks.push_back(leak);
  return m;
}

struct Jumps {
  double idle = 0.0;   // the largest block-to-block change in level, idle
  double played = 0.0; // and with a chord drawing on the chest
  double sag = 0.0;    // the chest's mean sag under the chord
};

// 528-frame blocks at 44.1 kHz, the reporter's: two seconds idle, then two
// with a chord drawing a third of the feed.
Jumps measure(double chestM3, double feedKgPerSec) {
  const mp::OrganModel organ = chestOrgan(chestM3, feedKgPerSec);
  mp::WindSolver wind;
  wind.reset(organ);
  const std::unordered_set<mp::Id> none;
  wind.settleWith(none);
  const mp::EngineSwitch full;
  const double dt = 528.0 / 44100.0;
  const int blocks = static_cast<int>(2.0 / dt);
  Jumps j;
  for (int phase = 0; phase < 2; ++phase) {
    double last = -1.0, sagSum = 0.0;
    for (int b = 0; b < blocks; ++b) {
      wind.clearDemand();
      if (phase == 1) wind.addDemandDirect({3}, {static_cast<float>(feedKgPerSec / 3.0)});
      wind.advance(dt, full, none);
      const double level = wind.modFor(3).ampMul;
      if (last >= 0.0 && b > blocks / 4) {
        double& worst = phase == 0 ? j.idle : j.played;
        worst = std::max(worst, std::abs(level / last - 1.0));
      }
      if (b > blocks / 4) sagSum += wind.sagFor(3);
      last = level;
    }
    if (phase == 1) j.sag = sagSum / (blocks - blocks / 4 - 1);
  }
  return j;
}

class WindSmallChestTest final : public mp::test::Test {
public:
  WindSmallChestTest() : Test("functional.control.wind-small-chest", Category::Functional) {}
  void run() override {
    struct Case {
      double m3, feed;
    };
    for (const Case c : {Case{0.5, 0.5}, Case{0.02, 0.5}, Case{0.005, 0.5}, Case{0.001, 1.0}, Case{0.0002, 2.0}}) {
      const Jumps j = measure(c.m3, c.feed);
      std::printf("    chest %.4f m3, feed %.1f kg/s: level steps between blocks %.5f idle, %.5f played; sag %.4f\n",
                  c.m3, c.feed, j.idle, j.played, j.sag);
      // A step of a thousandth in level is -60 dB of the note: inaudible.
      MP_CHECK(j.idle < 1e-3, "idle level steps " + std::to_string(j.idle));
      MP_CHECK(j.played < 1e-3, "played level steps " + std::to_string(j.played));
      // The chord still sags the chest: the fix keeps the physics.
      MP_CHECK(j.sag < 0.0 && j.sag > -0.5, "sag under the chord " + std::to_string(j.sag));
    }
  }
};
static WindSmallChestTest g_windSmallChest;

}  // namespace
