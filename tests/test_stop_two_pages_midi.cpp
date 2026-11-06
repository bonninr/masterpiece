// A stop drawn on two pages is one stop with one set of MIDI events (#870).
//
// The organ draws the copy on the second page as a switch of its own, marked
// as taking no MIDI (AccessibleForInput N), and joins it to the stop with a
// plain follow both ways. Events learned on the copy belong to the stop, and
// a map saved before the two shared one set moves them there.
#include "MpTest.h"

#include "../src/mp_control/MidiMap.h"
#include "../src/mp_core/OrganModel.h"

using mp::test::Category;

namespace {

mp::SwitchLinkage follow(mp::Id from, mp::Id to) {
  mp::SwitchLinkage l;
  l.sourceSwitchId = from;
  l.destSwitchId = to;
  return l;
}

mp::MidiBinding note(mp::Id target, int number) {
  mp::MidiBinding b;
  b.source.kind = mp::MidiSourceKind::Note;
  b.source.channel = 1;
  b.source.number = number;
  b.targetKind = mp::MidiTargetKind::Switch;
  b.targetId = target;
  return b;
}

class StopTwoPagesMidiTest final : public mp::test::Test {
public:
  StopTwoPagesMidiTest() : Test("functional.midi.stop-two-pages", Category::Functional) {}

  void run() override {
    mp::OrganModel model;
    mp::Switch stop;
    stop.switchId = 10;
    model.switches[10] = stop;
    mp::Switch copy;
    copy.switchId = 11;
    copy.accessibleForInput = false;
    model.switches[11] = copy;
    // A second copy that only follows one way is not the stop's.
    mp::Switch oneWay;
    oneWay.switchId = 12;
    oneWay.accessibleForInput = false;
    model.switches[12] = oneWay;
    model.switchLinkages = {follow(10, 11), follow(11, 10), follow(10, 12)};

    const auto owners = mp::switchMidiOwners(model);
    MP_CHECK(owners.size() == 1 && owners.count(11) == 1 && owners.at(11) == 10,
             "the copy on the second page belongs to the stop");
    MP_CHECK(owners.count(12) == 0, "a switch that only follows keeps its own events");

    mp::MidiMap map;
    map.setBindingsFor(mp::MidiTargetKind::Switch, 10, {note(10, 36)});
    map.setBindingsFor(mp::MidiTargetKind::Switch, 11, {note(11, 37)});
    const int moved = mp::moveSwitchMidiToOwners(map, owners);
    MP_CHECK(moved == 1, "the copy's one event moves");
    MP_CHECK(map.bindingsFor(mp::MidiTargetKind::Switch, 11).empty(),
             "the copy holds no events of its own after");
    MP_CHECK(map.bindingsFor(mp::MidiTargetKind::Switch, 10).size() == 2,
             "the stop keeps its own event beside the copy's");
  }
};

StopTwoPagesMidiTest g_stopTwoPagesMidi;

} // namespace
