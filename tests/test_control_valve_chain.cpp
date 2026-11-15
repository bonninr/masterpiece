// A non-return valve built from controls (#1121). Rotterdam's Transept organ
// opens the valve from each bellows to its trunk on the komurka's gauge minus
// the trunk's gauge, times 10000, times 4. The gauges read half a step per
// inch of wind, so a trunk a tenth of an inch below its komurka is a
// difference of 0.05 of a step: rounded at the subtraction it was nothing, and
// the valve stayed shut until the trunk had lost more than an inch.
#include "MpTest.h"
#include "../src/mp_control/Control.h"

#include <string>
#include <unordered_set>

namespace {

using mp::test::Category;

class ControlValveChainTest final : public mp::test::Test {
public:
  ControlValveChainTest() : Test("functional.control.valve-chain-keeps-fraction", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    auto control = [&](mp::Id id, int def) {
      mp::ContinuousControl c;
      c.controlId = id;
      c.defaultValue = def;
      c.clickable = false;
      m.continuousControls[id] = c;
    };
    control(101, 64);  // the komurka's gauge
    control(102, 64);  // the trunk's gauge
    control(103, 0);   // their difference
    control(104, 0);   // the scaled difference
    control(105, 0);   // the valve
    mp::ContinuousControlDoubleLinkage diff;
    diff.operationCode = 2;
    diff.firstControlId = 101;
    diff.secondControlId = 102;
    diff.destControlId = 103;
    m.controlDoubleLinkages.push_back(diff);
    mp::ContinuousControlLinkage gain;
    gain.sourceControlId = 103;
    gain.destControlId = 104;
    gain.coefficient = 1e4;
    m.controlLinkages.push_back(gain);
    mp::ContinuousControlLinkage valve;
    valve.sourceControlId = 104;
    valve.destControlId = 105;
    valve.coefficient = 4.0;
    m.controlLinkages.push_back(valve);

    mp::ContinuousControlBank bank;
    bank.reset(m);
    auto gauge = [](double inches) { return 63.5 + inches * 0.5; };
    bank.setExact(101, gauge(1.29), 0.0);
    bank.setExact(102, gauge(1.29), 0.0);
    bank.propagate();
    MP_CHECK(bank.value(105) == 0, "the trunk at the komurka's pressure keeps the valve shut: " +
                                       std::to_string(bank.value(105)));

    bank.setExact(102, gauge(1.19), 0.0);
    bank.propagate();
    MP_CHECK(bank.value(103) == 0, "the difference is still step 0: " + std::to_string(bank.value(103)));
    MP_CHECK(bank.readingOf(103) > 0.049 && bank.readingOf(103) < 0.051,
             "and reads 0.05 between the steps: " + std::to_string(bank.readingOf(103)));
    MP_CHECK(bank.value(105) == 127,
             "a trunk a tenth of an inch low opens the valve fully: " + std::to_string(bank.value(105)));

    bank.setExact(102, gauge(1.29), 0.0);
    bank.propagate();
    MP_CHECK(bank.value(105) == 0, "and the trunk filled again closes it: " + std::to_string(bank.value(105)));

    // A change finer than an eighth of a step wakes the graph only when asked.
    bank.setExact(102, gauge(1.28));
    bank.propagate();
    MP_CHECK(bank.value(105) == 0, "the default wake waits for an eighth of a step: " +
                                       std::to_string(bank.value(105)));
    bank.setExact(102, gauge(1.27), 0.0);
    bank.propagate();
    MP_CHECK(bank.value(105) == 127, "a wake of 0 carries every change: " + std::to_string(bank.value(105)));

    // Whole steps behave as they always did.
    bank.setValue(101, 70);
    bank.setValue(102, 64);
    bank.propagate();
    MP_CHECK(bank.value(103) == 6 && bank.readingOf(103) == 6.0,
             "whole steps give a whole step: " + std::to_string(bank.readingOf(103)));
  }
};
static ControlValveChainTest g_controlValveChain;

// A start-up ramp that waits for time (#1121). The Transept's "StartUp
// bellows" control rises from a constant through a Willis linkage of 11 s
// and opens one bellows' inlet every 11 steps. Settled at once at load it
// stood at 127 before the organ was live, and every inlet opened together.
class StartRampWaitsTest final : public mp::test::Test {
public:
  StartRampWaitsTest() : Test("functional.control.start-ramp-waits-for-time", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    auto control = [&](mp::Id id, int def) {
      mp::ContinuousControl c;
      c.controlId = id;
      c.defaultValue = def;
      c.clickable = false;
      m.continuousControls[id] = c;
    };
    control(10, 1);  // a constant of 1
    control(12, 0);  // the start-up ramp
    mp::ContinuousControlLinkage ramp;
    ramp.sourceControlId = 10;
    ramp.destControlId = 12;
    ramp.coefficient = 127.0;
    ramp.linkType = 2;
    ramp.willisMsPerStep = 11000.0;
    m.controlLinkages.push_back(ramp);

    {
      mp::ContinuousControlBank bank;
      bank.reset(m);
      bank.propagate();
      MP_CHECK(bank.value(12) == 127, "a ramp left to settle stands at the top at once: " +
                                          std::to_string(bank.value(12)));
    }
    const std::unordered_set<mp::Id> wait = {12};
    mp::ContinuousControlBank bank;
    bank.reset(m, &wait);
    bank.propagate();
    MP_CHECK(bank.value(12) == 0, "a ramp that waits for time stays at rest through the settle: " +
                                      std::to_string(bank.value(12)));
    bank.setTimed(true);
    bank.propagate();
    for (int i = 0; i < 100; ++i) bank.advance(0.01);
    bank.propagate();
    const int after1 = bank.value(12);
    MP_CHECK(after1 >= 9 && after1 <= 13, "and a second after going live it is near step 11: " +
                                              std::to_string(after1));
    for (int i = 0; i < 400; ++i) bank.advance(0.01);
    bank.propagate();
    const int after5 = bank.value(12);
    MP_CHECK(after5 >= 55 && after5 <= 61, "an even ramp: near step 58 after five seconds: " +
                                               std::to_string(after5));
    for (int i = 0; i < 700; ++i) bank.advance(0.01);
    bank.propagate();
    MP_CHECK(bank.value(12) == 127, "and at the top, past the last inlet, after its 11 s: " +
                                        std::to_string(bank.value(12)));
  }
};
static StartRampWaitsTest g_startRampWaits;

}  // namespace
