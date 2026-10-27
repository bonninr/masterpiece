// What a player reads beside a control in a menu, a window title or a list
// (#591). Two consoles wrote the rule: Azzio, whose Sesquialtera has a knob
// per variant and both knobs reach the switch the two stops share, and a set
// that names the switch behind a drawn knob by an internal tag.
#include "MpTest.h"

#include "../src/mp_core/ObjectLabels.h"

#include <string>

namespace {

using mp::test::Category;

mp::OrganModel azzio() {
  // Stops 6 and 7 are the two variants of one Sesquialtera and share the
  // switch that turns it on; the two knobs are switches of their own, drawn on
  // the console and named as the console prints them.
  mp::OrganModel m;
  auto sw = [&m](mp::Id id, const char* name) {
    mp::Switch s;
    s.switchId = id;
    s.name = name;
    m.switches[id] = s;
  };
  auto stop = [&m](mp::Id id, const char* name, mp::Id controlling) {
    mp::Stop s;
    s.stopId = id;
    s.name = name;
    s.divisionId = 2;
    s.controllingSwitchId = controlling;
    m.stops[id] = s;
  };
  auto link = [&m](mp::Id from, mp::Id to) {
    mp::SwitchLinkage l;
    l.sourceSwitchId = from;
    l.destSwitchId = to;
    m.switchLinkages.push_back(l);
  };
  sw(6, "06. GO  Sesquialtera 2 file (first rank only)");
  sw(7, "07. GO  Sesquialtera 2 file (both ranks)");
  sw(21, "Sesq State OFF");
  sw(22, "Sesq State MIDDLE");
  sw(23, "Sesq State ON");
  sw(26, "Sesq On in any variant");
  stop(6, "GO  Sesquialtera 2 file (first rank only)", 26);
  stop(7, "GO  Sesquialtera 2 file (both ranks)", 26);
  link(6, 21);
  link(7, 23);
  link(21, 26);
  link(23, 26);
  return m;
}

class ObjectLabelsTest final : public mp::test::Test {
public:
  ObjectLabelsTest() : Test("functional.core.object-labels", Category::Functional) {}
  void run() override {
    // The case the report came in for: two knobs on one stop. The wiring
    // cannot tell them apart, so each keeps its own words and the two read
    // differently in the MIDI menus.
    const mp::OrganModel m = azzio();
    const auto first = mp::switchLabel(m, 6);
    const auto second = mp::switchLabel(m, 7);
    MP_CHECK(first == "06. GO Sesquialtera 2 file (first rank only)", "knob 6 keeps its own name: " + first);
    MP_CHECK(second == "07. GO Sesquialtera 2 file (both ranks)", "knob 7 keeps its own name: " + second);
    MP_CHECK(first != second, "the two variants read differently");
    MP_CHECK(mp::switchLabel(m, 26) == "Sesq On in any variant", "the shared switch keeps its own name");

    // Where the wiring names one thing, it names the control, as it always
    // has: a set that tags the switch behind a drawn knob "CustPg1_Stop 2224"
    // means the stop it works.
    mp::OrganModel tagged = azzio();
    tagged.switches[30].switchId = 30;
    tagged.switches[30].name = "CustPg1_Stop 2224";
    mp::Stop od;
    od.stopId = 20;
    od.name = "Open Diapason 8";
    od.controllingSwitchId = 30;
    tagged.stops[20] = od;
    MP_CHECK(mp::switchLabel(tagged, 30) == "Open Diapason 8",
             "an internal tag on a switch over one stop reads as the stop: " +
                 mp::switchLabel(tagged, 30));

    // A switch named by a number over a shared stop still names something, and
    // names it the same way every run: the lowest stop id.
    mp::OrganModel numbered = azzio();
    numbered.switches[7].name = "1187";
    MP_CHECK(mp::switchLabel(numbered, 7) == "GO Sesquialtera 2 file (first rank only)",
             "a number-only name over a shared stop gives way to the first stop: " +
                 mp::switchLabel(numbered, 7));
    numbered.switches[7].name = "";
    MP_CHECK(mp::switchLabel(numbered, 7) == "GO Sesquialtera 2 file (first rank only)",
             "and so does no name at all");

    // A switch this organ has not got, and a blank one with nothing wired to
    // it: the caller prints "Switch 1187".
    const mp::OrganModel n0 = azzio();
    MP_CHECK(mp::switchLabel(n0, 99).empty(), "a switch the organ lacks names nothing");
    mp::OrganModel blank = azzio();
    blank.switches[99] = mp::Switch{};
    blank.switches[99].switchId = 99;
    MP_CHECK(mp::switchLabel(blank, 99).empty(), "a blank, unwired switch names nothing");

    // A tremulant is named the same way a stop is.
    mp::OrganModel trem = azzio();
    trem.switches[7].name = "";
    mp::Tremulant t;
    t.tremulantId = 3;
    t.name = "Tremolo GO";
    t.controllingSwitchId = 40;
    trem.tremulants[3] = t;
    mp::SwitchLinkage l;
    l.sourceSwitchId = 7;
    l.destSwitchId = 40;
    trem.switchLinkages.push_back(l);
    MP_CHECK(mp::switchLabel(trem, 40) == "Tremolo GO", "a tremulant's own switch answers with the tremulant");
    MP_CHECK(mp::switchLabel(trem, 7) == "Tremolo GO", "and the blank knob that reaches it reads the same");

    // The names a set uses internally lose their prefix and their spare spaces.
    MP_CHECK(mp::readableName("__Midm_Pedal to Great") == "Pedal to Great", "an internal prefix is dropped");
    MP_CHECK(mp::readableName("__Img_14. GO  Principale 8'") == "14. GO Principale 8'",
             "the image prefix is dropped and the double space squeezed: " +
                 mp::readableName("__Img_14. GO  Principale 8'"));
    MP_CHECK(mp::readableName("   ").empty(), "spaces alone name nothing");
    MP_CHECK(mp::readableName("Subbasso 16'") == "Subbasso 16'", "a plain name is left as it is");
  }
};
static ObjectLabelsTest g_objectLabels;

}  // namespace
