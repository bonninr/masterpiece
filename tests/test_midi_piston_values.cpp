// Pistons on one controller, a value each (#1214). A notation or sequencer
// program changes registration with one controller and a value per
// combination: value 1 the first general, value 2 the second. Each piston
// fires on its own value and on no other.
#include "MpTest.h"

#include "../src/mp_control/MidiMap.h"

using mp::test::Category;

class MidiPistonValuesTest final : public mp::test::Test {
public:
  MidiPistonValuesTest() : Test("functional.midi.piston-one-value", Category::Functional) {}

  static mp::MidiBinding piston(mp::MidiTargetKind kind, mp::Id id, int value) {
    mp::MidiBinding b;
    b.source = {mp::MidiSourceKind::ControlChange, 1, 20};
    b.targetKind = kind;
    b.targetId = id;
    b.exactValues = true;
    b.lowValue = 0;
    b.highValue = value;
    return b;
  }

  void run() override {
    using K = mp::MidiTargetKind;
    mp::MidiMap map;
    for (int n = 1; n <= 3; ++n) map.bind(piston(K::PlayerGeneral, n, n));
    map.bind(piston(K::PlayerGeneralCancel, 0, 10));
    map.bind(piston(K::StepperNext, 0, 11));
    const mp::MidiSource cc20{mp::MidiSourceKind::ControlChange, 1, 20};

    auto fires = [&](int value) {
      const auto a = map.actionFor(cc20, value);
      return a.valid() ? std::make_pair(a.kind, a.targetId) : std::make_pair(K::None, mp::Id{-1});
    };
    for (int n = 1; n <= 3; ++n)
      MP_CHECK(fires(n) == std::make_pair(K::PlayerGeneral, mp::Id{n}),
               "value " + std::to_string(n) + " fires general " + std::to_string(n));
    MP_CHECK(fires(10).first == K::PlayerGeneralCancel, "value 10 fires the general cancel");
    MP_CHECK(fires(11).first == K::StepperNext, "value 11 steps the sequencer");
    MP_CHECK(fires(0).first == K::None && fires(4).first == K::None && fires(127).first == K::None,
             "a value no piston has fires nothing");
    MP_CHECK(map.bindings().size() == 5, "five rows on one controller, got " + std::to_string(map.bindings().size()));

    // A value taken by another piston moves to it.
    map.bind(piston(K::PlayerGeneral, 7, 2));
    MP_CHECK(fires(2) == std::make_pair(K::PlayerGeneral, mp::Id{7}), "value 2 now fires general 7");
    MP_CHECK(fires(1) == std::make_pair(K::PlayerGeneral, mp::Id{1}), "and value 1 still general 1");

    // Saved and read back.
    mp::MidiMap back;
    back.fromText(map.toText());
    const auto a3 = back.actionFor(cc20, 3);
    MP_CHECK(a3.valid() && a3.kind == K::PlayerGeneral && a3.targetId == 3, "the values survive saving");

    // The first piston on the controller cleared: the others still answer.
    back.removeBindings([](const mp::MidiBinding& b) { return b.highValue == 1; });
    MP_CHECK(!back.actionFor(cc20, 1).valid(), "the cleared piston is gone");
    const auto a7 = back.actionFor(cc20, 2);
    MP_CHECK(a7.valid() && a7.targetId == 7, "and the others on its controller still fire");

    // A piston learned on the controller the ordinary way takes it over.
    mp::MidiBinding plain = piston(K::PlayerGeneral, 5, 127);
    plain.exactValues = false;
    back.bind(plain);
    const auto any = back.actionFor(cc20, 100);
    MP_CHECK(any.valid() && any.targetId == 5 && back.bindings().size() == 1,
             "one piston on the whole controller replaces the values");
  }
};
static MidiPistonValuesTest g_midiPistonValues;
