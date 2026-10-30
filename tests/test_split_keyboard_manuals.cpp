// The split keyboard offers the organ's manuals when the organ leaves out
// KeyGen_NumberOfKeys: Nancy and St. Maximin name no key count on any
// keyboard, and the split's lists offered only "Nothing" there.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/SplitKeyboardHost.h"
#include "../src/mp_control/Control.h"

namespace {

using mp::test::Category;

class SplitKeyboardManualsTest final : public mp::test::Test {
public:
  SplitKeyboardManualsTest() : Test("functional.split.manuals-without-key-count", Category::Functional) {}
  void run() override {
    // A pedal and two manuals, each sounding its division; no key counts.
    mp::OrganModel m;
    const struct { mp::Id id; int code; const char* name; } kbs[] = {
        {1, 1, "Pedal"}, {2, 2, "Grand Orgue"}, {3, 3, "R\xc3\xa9" "cit"}};
    for (const auto& k : kbs) {
      mp::Keyboard kb;
      kb.keyboardId = k.id;
      kb.assignmentCode = k.code;
      kb.name = k.name;
      m.keyboards[k.id] = kb;
      mp::Division d;
      d.divisionId = 10 + k.id;
      d.name = k.name;
      m.divisions[d.divisionId] = d;
      mp::KeyAction a;
      a.sourceKeyboard = static_cast<int>(k.id);
      a.destIsKeyboard = false;
      a.destDivision = static_cast<int>(d.divisionId);
      m.keyActions.push_back(a);
    }
    mp::CouplerMatrix couplers;
    couplers.reset(m);
    const auto& played = couplers.inputKeyboards();
    MP_CHECK(played.size() == 3, "all three keyboards played, key count or not");

    using mp::split::Role;
    MP_CHECK(mp::SplitKeyboardHost::proposedKeyboard(m, played, Role::Lower) == 2, "lower: the first manual");
    MP_CHECK(mp::SplitKeyboardHost::proposedKeyboard(m, played, Role::Upper) == 3, "upper: the second manual");
    MP_CHECK(mp::SplitKeyboardHost::proposedKeyboard(m, played, Role::Pedal) == 1, "pedal: the pedal");
    MP_CHECK(mp::SplitKeyboardHost::proposedKeyboard(m, {}, Role::Lower) == 0, "nothing played: nothing proposed");
  }
};
static SplitKeyboardManualsTest g_splitManuals;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
