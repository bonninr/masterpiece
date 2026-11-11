// A blower off at load, and a blower switched off while playing (#1028): the
// chest has no wind, and its pipes are silent until the blower fills it. The
// working pressure is the chest's with the blower running, whichever way the
// organ loaded.
#include "MpTest.h"
#include "../src/mp_control/WindSolver.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace {

using mp::test::Category;

constexpr mp::Id kBlowerSwitch = 99;
constexpr mp::Id kBlowerControl = 8999;

// A blower at four inches feeding a chest through a valve its motor switch
// opens, the chest leaking to the open air.
mp::OrganModel blowerOrgan() {
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
  chest.volumeM3 = 0.3;
  m.wind[3] = chest;
  mp::WindCompartmentLink supply;
  supply.firstCompartmentId = 2;
  supply.secondCompartmentId = 3;
  supply.massFlowKgPerSec = 0.05;
  supply.refPressureInches = 3.0;
  supply.valveSwitchId = kBlowerSwitch;
  supply.valveOpenWhenEngaged = true;
  m.windLinks.push_back(supply);
  mp::WindCompartmentLink leak;
  leak.firstCompartmentId = 3;
  leak.secondCompartmentId = 1;
  leak.massFlowKgPerSec = 0.01;
  leak.refPressureInches = 3.0;
  m.windLinks.push_back(leak);
  return m;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", v);
  return b;
}

class WindBlowerOffTest final : public mp::test::Test {
public:
  WindBlowerOffTest() : Test("functional.control.wind-blower-off", Category::Functional) {}
  void run() override {
    const mp::OrganModel organ = blowerOrgan();
    const mp::EngineSwitch full;
    const std::unordered_set<mp::Id> on{kBlowerSwitch}, off;
    const double dt = 256.0 / 48000.0;
    const auto run = [&](mp::WindSolver& wind, const std::unordered_set<mp::Id>& sw, double seconds) {
      for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        wind.clearDemand();
        wind.advance(dt, full, sw);
      }
    };

    // Loaded with the blower running: the chest at its working pressure, the
    // pipes at their own level.
    mp::WindSolver running;
    running.reset(organ);
    running.settleWith(on);
    run(running, on, dt);  // the solver modulates once it plays
    const double working = running.nominalFor(3);
    std::printf("    blower on at load: chest %.4f of %.4f in, amp %.4f\n", running.pressureFor(3), working,
                running.modFor(3).ampMul);
    MP_CHECK(working > 1.0, "the chest has wind: " + num(working));
    MP_CHECK(std::abs(running.modFor(3).ampMul - 1.0) < 1.0e-3,
             "at its working pressure the pipe is at its level: " + num(running.modFor(3).ampMul));

    // Loaded with the blower off: the same working pressure, an empty chest,
    // and silence.
    mp::WindSolver stopped;
    stopped.reset(organ);
    stopped.settleWith(off);
    run(stopped, off, dt);
    std::printf("    blower off at load: chest %.4f of %.4f in, amp %.4f\n", stopped.pressureFor(3),
                stopped.nominalFor(3), stopped.modFor(3).ampMul);
    MP_CHECK(std::abs(stopped.nominalFor(3) - working) < 1.0e-6 * working,
             "the working pressure is the running blower's: " + num(stopped.nominalFor(3)));
    MP_CHECK(stopped.pressureFor(3) < 1.0e-3, "the chest is empty: " + num(stopped.pressureFor(3)));
    MP_CHECK(stopped.modFor(3).ampMul == 0.0, "no wind, no sound: " + num(stopped.modFor(3).ampMul));

    // The blower started: the chest fills and the pipes come back to their level.
    run(stopped, off, 2.0);
    MP_CHECK(stopped.modFor(3).ampMul == 0.0, "still silent while the blower stays off");
    run(stopped, on, 20.0);
    std::printf("    blower started: chest %.4f in, amp %.4f\n", stopped.pressureFor(3), stopped.modFor(3).ampMul);
    MP_CHECK(std::abs(stopped.modFor(3).ampMul - 1.0) < 0.01,
             "the blower fills the chest: amp " + num(stopped.modFor(3).ampMul));

    // The blower switched off while playing: the chest drains to silence.
    run(running, off, 60.0);
    std::printf("    blower stopped: chest %.4f in, amp %.4f\n", running.pressureFor(3), running.modFor(3).ampMul);
    MP_CHECK(running.modFor(3).ampMul < 0.05, "the chest drains to silence: amp " + num(running.modFor(3).ampMul));

    // An organ with its blower running at load settles exactly as before:
    // only an empty chest takes the second settle.
    mp::WindSolver again;
    again.reset(organ);
    again.settleWith(on);
    MP_CHECK(again.nominalFor(3) == working && again.pressureFor(3) == working,
             "blower on at load: the same figures, bit for bit");

    // The blower's valve on a continuous control, as Alessandria has it: the
    // same working pressure, and the chest follows the control.
    mp::OrganModel byControl = organ;
    byControl.windLinks[0].valveSwitchId = 0;
    byControl.windLinks[0].valveControlTypeCode = 3;
    byControl.windLinks[0].valveControlId = kBlowerControl;
    mp::ContinuousControl blowerControl;
    blowerControl.controlId = kBlowerControl;
    byControl.continuousControls[kBlowerControl] = blowerControl;
    mp::WindSolver valve;
    valve.reset(byControl);
    valve.settleWith(off);
    MP_CHECK(valve.controlValves().size() == 1, "the control valve is found");
    MP_CHECK(std::abs(valve.nominalFor(3) - working) < 1.0e-6 * working,
             "settled with the control valve open: " + num(valve.nominalFor(3)));
    valve.setControlValve(0, 0);
    run(valve, off, 60.0);
    std::printf("    control valve shut: chest %.4f in, amp %.4f\n", valve.pressureFor(3), valve.modFor(3).ampMul);
    MP_CHECK(valve.modFor(3).ampMul < 0.05, "the valve shut drains the chest: amp " + num(valve.modFor(3).ampMul));
    valve.setControlValve(0, 127);
    run(valve, off, 20.0);
    std::printf("    control valve open: chest %.4f in, amp %.4f\n", valve.pressureFor(3), valve.modFor(3).ampMul);
    MP_CHECK(std::abs(valve.modFor(3).ampMul - 1.0) < 0.01,
             "the valve open fills it: amp " + num(valve.modFor(3).ampMul));

    // Loaded with the control valve shut, the blower's control not run up: a
    // chest with a way out to the open air loads empty, at the running
    // blower's working pressure ...
    mp::WindSolver shutAtLoad;
    shutAtLoad.reset(byControl);
    shutAtLoad.setControlValve(0, 0);
    shutAtLoad.settleWith(off);
    run(shutAtLoad, off, dt);
    std::printf("    control valve shut at load: chest %.4f of %.4f in, amp %.4f\n", shutAtLoad.pressureFor(3),
                shutAtLoad.nominalFor(3), shutAtLoad.modFor(3).ampMul);
    MP_CHECK(std::abs(shutAtLoad.nominalFor(3) - working) < 1.0e-6 * working,
             "shut at load: the working pressure is the running blower's: " + num(shutAtLoad.nominalFor(3)));
    MP_CHECK(shutAtLoad.modFor(3).ampMul == 0.0, "shut at load, with a leak: silent");

    // ... and a chest the shut valve seals keeps its wind.
    mp::OrganModel sealed = byControl;
    sealed.windLinks.pop_back();  // no leak
    mp::WindSolver sealedOpen, sealedShut;
    sealedOpen.reset(sealed);
    sealedOpen.settleWith(off);
    sealedShut.reset(sealed);
    sealedShut.setControlValve(0, 0);
    sealedShut.settleWith(off);
    std::printf("    sealed chest: %.4f in with the valve open at load, %.4f in shut\n", sealedOpen.pressureFor(3),
                sealedShut.pressureFor(3));
    MP_CHECK(sealedShut.pressureFor(3) == sealedOpen.pressureFor(3) &&
                 sealedShut.nominalFor(3) == sealedOpen.nominalFor(3),
             "a sealed chest keeps its wind, bit for bit");
    MP_CHECK(sealedShut.pressureFor(3) > 1.0, "the sealed chest has wind: " + num(sealedShut.pressureFor(3)));

    // A reservoir's own regulator is no blower valve: its control is the
    // board's height, solved only with the boards on.
    mp::OrganModel regulated = byControl;
    regulated.wind[3].bellowsExtensionOutputControlId = kBlowerControl;
    mp::WindSolver board;
    board.reset(regulated);
    MP_CHECK(board.controlValves().empty(), "a regulator's valve is left to the board");
  }
};
static WindBlowerOffTest g_windBlowerOff;

}  // namespace
