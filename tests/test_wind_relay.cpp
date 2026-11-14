// A regulator built from switches (#1121): the bellows's board height shuts
// its inlet at the top, a delay ramp then opens its exhaust, and the bottom
// opens the inlet and shuts the exhaust again -- the Rotterdam Transept's
// eleven bellows, one of them. The solver runs that chain at its own step:
// the valves answer in the step the board reaches its stop, and the wind is
// the same whatever the audio block.
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

constexpr mp::Id kInlet = 50, kExhaust = 51;
constexpr mp::Id kExtension = 20, kDelay = 22, kOne = 10, kZero = 11;

mp::OrganModel relayOrgan() {
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
  mp::WindCompartment bellows;
  bellows.compartmentId = 3;
  bellows.infiniteVolume = false;
  bellows.volumeM3 = 0.1;
  bellows.hasBellows = true;
  bellows.bellowsWidthM = 1.0;
  bellows.bellowsLengthM = 2.0;
  bellows.bellowsExtensionM = 0.3;
  // Three inches over the 2 m2 frame.
  bellows.bellowsGravityMassKg = 3.0 * 249.09 * 2.0 / 9.81;
  bellows.bellowsMassKg = 30.0;
  bellows.bellowsDamping = 20.0;
  bellows.bellowsShapeCode = 1;
  bellows.bellowsExtensionOutputControlId = kExtension;
  m.wind[3] = bellows;
  mp::WindCompartment chest;
  chest.compartmentId = 4;
  chest.infiniteVolume = false;
  chest.volumeM3 = 0.5;
  m.wind[4] = chest;

  mp::WindCompartmentLink inlet;
  inlet.firstCompartmentId = 2;
  inlet.secondCompartmentId = 3;
  inlet.massFlowKgPerSec = 2.0;
  inlet.refPressureInches = 3.0;
  inlet.valveSwitchId = kInlet;
  m.windLinks.push_back(inlet);
  mp::WindCompartmentLink exhaust;
  exhaust.firstCompartmentId = 3;
  exhaust.secondCompartmentId = 4;
  exhaust.massFlowKgPerSec = 1.0;
  exhaust.refPressureInches = 3.0;
  exhaust.valveSwitchId = kExhaust;
  m.windLinks.push_back(exhaust);
  mp::WindCompartmentLink leak;
  leak.firstCompartmentId = 4;
  leak.secondCompartmentId = 1;
  leak.massFlowKgPerSec = 0.02;
  leak.refPressureInches = 3.0;
  m.windLinks.push_back(leak);

  for (const mp::Id id : {kOne, kZero, kExtension, kDelay}) {
    mp::ContinuousControl c;
    c.controlId = id;
    c.defaultValue = id == kOne ? 1 : 0;
    m.continuousControls[id] = c;
  }
  for (const mp::Id id : {kInlet, kExhaust}) {
    mp::Switch s;
    s.switchId = id;
    m.switches[id] = s;
  }
  const auto row = [&m](mp::Id control, int value, mp::Id sw, bool engUp, bool engDown, bool disUp,
                        bool disDown) {
    mp::ContinuousControlStageSwitch st;
    st.controlId = control;
    st.value = value;
    st.controlledSwitchId = sw;
    st.engageWhenIncreasing = engUp;
    st.engageWhenDecreasing = engDown;
    st.disengageWhenIncreasing = disUp;
    st.disengageWhenDecreasing = disDown;
    m.controlStageSwitches.push_back(st);
  };
  row(kExtension, 127, kInlet, false, false, true, false);   // full: inlet shut
  row(kExtension, 0, kInlet, false, true, false, false);     // empty: inlet open
  row(kExtension, 0, kExhaust, false, false, false, true);   // empty: exhaust shut
  row(kDelay, 127, kExhaust, true, false, false, false);     // the delay run out: exhaust open
  // The delay ramps up while the inlet is shut and drops back as it opens.
  mp::ContinuousControlLinkage ramp;
  ramp.sourceControlId = kOne;
  ramp.destControlId = kDelay;
  ramp.coefficient = 127.0;
  ramp.conditionSwitchId = kInlet;
  ramp.conditionWhenEngaged = false;
  ramp.linkType = 2;
  ramp.willisMsPerStep = 100.0;
  m.controlLinkages.push_back(ramp);
  mp::ContinuousControlLinkage back;
  back.sourceControlId = kZero;
  back.destControlId = kDelay;
  back.conditionSwitchId = kInlet;
  back.conditionWhenEngaged = true;
  m.controlLinkages.push_back(back);
  // The inlet opening shuts the exhaust.
  mp::SwitchLinkage pair;
  pair.sourceSwitchId = kInlet;
  pair.destSwitchId = kExhaust;
  pair.engageAction = 2;
  pair.disengageAction = 7;
  m.switchLinkages.push_back(pair);
  return m;
}

struct Run {
  std::vector<double> chest;  // every 3072 frames
  int inletChanges = 0, exhaustOpenings = 0;
  bool inletShutAtTop = true, exhaustShutAtBottom = true;
};

// 30 s with the inlet engaged at load and a draw on the chest, in blocks of
// `frames` at 48 kHz.
Run play(const mp::OrganModel& m, int frames) {
  mp::WindSolver wind;
  wind.reset(m);
  const std::unordered_set<mp::Id> engaged{kInlet};
  wind.settleWith(engaged);
  mp::EngineSwitch sw;
  sw.enableWindModel = true;
  const std::vector<mp::Id> chests{4};
  const std::vector<float> draw{0.2f};
  Run r;
  int inlet = wind.relaySwitchState(kInlet), exhaust = wind.relaySwitchState(kExhaust);
  const long long total = 30LL * 48000;
  for (long long at = 0; at < total; at += frames) {
    wind.clearDemand();
    wind.addDemandDirect(chests, draw);
    wind.advance(frames / 48000.0, sw, engaged);
    const int i = wind.relaySwitchState(kInlet), x = wind.relaySwitchState(kExhaust);
    if (i != inlet) ++r.inletChanges;
    if (x == 1 && exhaust != 1) ++r.exhaustOpenings;
    inlet = i;
    exhaust = x;
    const double ext = wind.extensionFor(3);
    if (ext >= 126.5 && i != 0) r.inletShutAtTop = false;
    if (ext <= 0.5 && x != 0) r.exhaustShutAtBottom = false;
    if ((at + frames) % 3072 == 0) r.chest.push_back(wind.pressureFor(4));
  }
  return r;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.6g", v);
  return b;
}

class WindRelayTest final : public mp::test::Test {
public:
  WindRelayTest() : Test("functional.control.wind-relay", Category::Functional) {}
  void run() override {
    const mp::OrganModel organ = relayOrgan();

    // The board moves with the option off: the bellows has no wind without it.
    {
      mp::WindSolver wind;
      wind.reset(organ);
      MP_CHECK(wind.extensionOutputs().size() == 1, "the switching board moves with the option off");
      MP_CHECK(wind.relaySwitchState(kInlet) >= 0 && wind.relaySwitchState(kExhaust) >= 0,
            "the inlet and exhaust are the solver's");
      MP_CHECK(std::find(wind.relayInputs().begin(), wind.relayInputs().end(), kOne) != wind.relayInputs().end(),
            "the constant the delay ramps toward is read from outside");
    }

    const Run small = play(organ, 64);
    const Run large = play(organ, 192);
    MP_CHECK(small.exhaustOpenings >= 3, "the bellows cycles: " + std::to_string(small.exhaustOpenings) + " deliveries");
    MP_CHECK(small.inletChanges >= 6, "the inlet opens and shuts: " + std::to_string(small.inletChanges) + " changes");
    MP_CHECK(small.inletShutAtTop && large.inletShutAtTop, "the inlet is shut whenever the board stands at the top");
    MP_CHECK(small.exhaustShutAtBottom && large.exhaustShutAtBottom,
          "the exhaust is shut whenever the board stands at the bottom");
    double sum = 0.0;
    for (size_t i = small.chest.size() / 2; i < small.chest.size(); ++i) sum += small.chest[i];
    const double mean = sum / static_cast<double>(small.chest.size() - small.chest.size() / 2);
    MP_CHECK(mean > 1.0, "the chest has wind: " + num(mean) + " inches on average");

    // The audio block does not move the valves. Both block sizes stay within
    // the 256 steps the solver takes in one block at this organ's 18 us step.
    MP_CHECK(small.chest.size() == large.chest.size(), "both runs sampled alike");
    double worst = 0.0;
    for (size_t i = 0; i < std::min(small.chest.size(), large.chest.size()); ++i)
      worst = std::max(worst, std::abs(small.chest[i] - large.chest[i]));
    MP_CHECK(worst < 1.0e-9, "64- and 192-frame blocks give the same wind: differ by " + num(worst) + " inches");
  }
};
static WindRelayTest g_windRelay;

}  // namespace
