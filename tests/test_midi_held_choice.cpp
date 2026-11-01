// "Held while pressed" is offered only for a message that has a release
// (#565). A program change or a system exclusive message sends nothing when
// the button comes up, so a drawstop held by one stayed drawn.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/MidiEventDialog.h"

using mp::test::Category;

class MidiHeldChoiceTest final : public mp::test::Test {
public:
  MidiHeldChoiceTest() : Test("functional.midi.held-needs-release", Category::Functional) {}

  void run() override {
    using mp::MidiSourceKind;
    using mp::MidiTargetKind;
    using mp::ui::MidiEventDialog;
    MP_CHECK(MidiEventDialog::heldOffered(MidiTargetKind::Switch, MidiSourceKind::Note),
             "a note has a note off");
    MP_CHECK(MidiEventDialog::heldOffered(MidiTargetKind::Switch, MidiSourceKind::ControlChange),
             "a controller goes back to 0");
    MP_CHECK(MidiEventDialog::heldOffered(MidiTargetKind::Switch, MidiSourceKind::None),
             "a row not yet filled offers every way");
    MP_CHECK(!MidiEventDialog::heldOffered(MidiTargetKind::Switch, MidiSourceKind::ProgramChange),
             "a program change has no release");
    MP_CHECK(!MidiEventDialog::heldOffered(MidiTargetKind::Switch, MidiSourceKind::SysEx),
             "a system exclusive message has no release");
  }
};
static MidiHeldChoiceTest g_midiHeldChoice;

#endif
