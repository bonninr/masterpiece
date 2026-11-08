// Timed control linkages (LinkTypeCode 2 and 3, #811). A Willis linkage moves
// its destination at a speed proportional to the distance left; an inertia
// linkage is a damped spring. Both act at once until the bank is timed, as
// they do while an organ loads.
#include "MpTest.h"
#include "../src/mp_control/Control.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

namespace {

using mp::test::Category;

mp::ContinuousControl control(mp::Id id, int dflt) {
  mp::ContinuousControl c;
  c.controlId = id;
  c.defaultValue = dflt;
  c.minValue = 0;
  c.maxValue = 127;
  return c;
}

mp::ContinuousControlLinkage wire(mp::Id from, mp::Id to) {
  mp::ContinuousControlLinkage l;
  l.sourceControlId = from;
  l.destControlId = to;
  return l;
}

constexpr double kBlock = 256.0 / 48000.0;

// Advance and settle for `seconds`, a block at a time.
void run(mp::ContinuousControlBank& bank, double seconds,
         const std::unordered_set<mp::Id>* sw = nullptr) {
  for (double t = 0.0; t < seconds - 1e-9; t += kBlock) {
    bank.advance(kBlock);
    bank.propagate(0, sw);
  }
}

class WillisLinkageTest final : public mp::test::Test {
public:
  WillisLinkageTest() : Test("functional.control.willis-linkage", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    m.continuousControls[1] = control(1, 0);
    m.continuousControls[2] = control(2, 0);
    m.continuousControls[3] = control(3, 0);
    auto l = wire(1, 2);
    l.linkType = 2;
    l.willisMsPerStep = 100.0;  // a 0.1 s lag
    m.controlLinkages.push_back(l);
    m.controlLinkages.push_back(wire(2, 3));  // and a plain wire after it

    mp::ContinuousControlBank bank;
    bank.reset(m);
    bank.setValue(1, 127);
    bank.propagate(1);
    MP_CHECK(bank.value(2) == 127 && bank.value(3) == 127,
             "untimed, as while loading, the linkage acts at once");

    bank.setTimed(true);
    bank.setValue(1, 0);
    bank.propagate(1);
    MP_CHECK(bank.value(2) == 127, "timed, the destination has not moved before any time passes");
    ::run(bank, 0.1);
    // One time constant: 127 * e^-1 = 46.7 left.
    const int at = bank.value(2);
    MP_CHECK(at >= 45 && at <= 49, "one time constant on, a 1/e of the way is left: " + std::to_string(at));
    MP_CHECK(bank.value(3) == at, "the plain wire after it follows it as it goes: " + std::to_string(bank.value(3)));
    MP_CHECK(bank.moving(), "and it is still on its way");
    ::run(bank, 1.0);
    MP_CHECK(bank.value(2) == 0 && bank.value(3) == 0, "ten time constants on it is there");
    MP_CHECK(!bank.moving(), "and it stops being worked on");

    // A stop-action delay: the constant 127 reaches the ramp only while its
    // stop is drawn, and the constant 0 while it is not.
    mp::OrganModel d;
    d.continuousControls[10] = control(10, 127);  // Hidden OffToOn, after load
    d.continuousControls[11] = control(11, 0);    // Hidden OnToOff
    d.continuousControls[20] = control(20, 0);    // __DelayStop
    auto on = wire(10, 20), off = wire(11, 20);
    on.conditionSwitchId = off.conditionSwitchId = 5;
    on.conditionWhenEngaged = true;
    off.conditionWhenEngaged = false;
    on.linkType = off.linkType = 2;
    on.willisMsPerStep = off.willisMsPerStep = 8.0;
    d.controlLinkages = {on, off};
    mp::ContinuousControlBank ramp;
    ramp.reset(d);
    std::unordered_set<mp::Id> sw;
    ramp.propagate(0, &sw);
    ramp.setTimed(true);
    sw.insert(5);
    ramp.propagate(0, &sw);
    MP_CHECK(ramp.value(20) == 0, "the stop just drawn: its ramp has not started");
    ::run(ramp, kBlock, &sw);
    const int first = ramp.value(20);
    MP_CHECK(first > 30 && first < 127, "a block later it is on its way up: " + std::to_string(first));
    sw.erase(5);
    ramp.propagate(0, &sw);
    ::run(ramp, kBlock, &sw);
    MP_CHECK(ramp.value(20) < first, "pushed in again, it turns back from where it was: " +
             std::to_string(ramp.value(20)));
    ::run(ramp, 0.2, &sw);
    MP_CHECK(ramp.value(20) == 0, "and comes to rest at the bottom");
  }
};
WillisLinkageTest g_willis;

class InertiaLinkageTest final : public mp::test::Test {
public:
  InertiaLinkageTest() : Test("functional.control.inertia-linkage", Category::Functional) {}
  void run() override {
    // The shutter inertia sets give their swell: 44.7 and 4.6.
    mp::OrganModel m;
    m.continuousControls[1] = control(1, 0);
    m.continuousControls[2] = control(2, 0);
    auto l = wire(1, 2);
    l.linkType = 3;
    l.inertiaAccel = 44.7;
    l.inertiaDamping = 4.6;
    m.controlLinkages.push_back(l);
    mp::ContinuousControlBank bank;
    bank.reset(m);
    bank.setTimed(true);
    bank.setValue(1, 64);
    bank.propagate(1);
    // Track the shutter: it accelerates away, passes the shoe, turns back.
    int peak = 0, at100ms = -1;
    for (double t = 0.0; t < 2.0; t += kBlock) {
      bank.advance(kBlock);
      bank.propagate();
      peak = std::max(peak, bank.value(2));
      if (at100ms < 0 && t >= 0.1) at100ms = bank.value(2);
    }
    MP_CHECK(at100ms > 5 && at100ms < 60, "a tenth of a second on the shutter is part of the way: " +
             std::to_string(at100ms));
    // The spring's own overshoot: e^(-pi z / sqrt(1 - z^2)), z = 4.6 / (2 sqrt 44.7) = 0.344: 32 %.
    MP_CHECK(peak >= 80 && peak <= 88, "it swings past the shoe by about a third: " + std::to_string(peak));
    ::run(bank, 4.0);
    MP_CHECK(bank.value(2) == 64 && !bank.moving(), "and settles on it: " + std::to_string(bank.value(2)));

    // The ends of the range are walls: slammed open, it stops at the top.
    bank.setValue(1, 127);
    bank.propagate(1);
    int over = 0;
    for (double t = 0.0; t < 2.0; t += kBlock) {
      bank.advance(kBlock);
      bank.propagate();
      over = std::max(over, bank.value(2));
    }
    MP_CHECK(over == 127 && bank.value(2) == 127, "fully open, it stays fully open");

    // Too stiff to follow at all: a combination stepper's 1000000 acts at once.
    mp::OrganModel s;
    s.continuousControls[1] = control(1, 0);
    s.continuousControls[2] = control(2, 0);
    auto st = wire(1, 2);
    st.linkType = 3;
    st.inertiaAccel = 1.0e9;
    st.inertiaDamping = 1.0;
    s.controlLinkages.push_back(st);
    mp::ContinuousControlBank stiff;
    stiff.reset(s);
    stiff.setTimed(true);
    stiff.setValue(1, 90);
    stiff.propagate(1);
    ::run(stiff, kBlock);
    MP_CHECK(stiff.value(2) == 90, "a stiffness past following lands in one block: " +
             std::to_string(stiff.value(2)));

    // A figure of 0 is no timing at all.
    mp::OrganModel z;
    z.continuousControls[1] = control(1, 0);
    z.continuousControls[2] = control(2, 0);
    auto zl = wire(1, 2);
    zl.linkType = 3;
    z.controlLinkages.push_back(zl);
    mp::ContinuousControlBank zero;
    zero.reset(z);
    zero.setTimed(true);
    zero.setValue(1, 50);
    zero.propagate(1);
    MP_CHECK(zero.value(2) == 50, "an inertia linkage without figures acts at once");
  }
};
InertiaLinkageTest g_inertia;

}  // namespace
