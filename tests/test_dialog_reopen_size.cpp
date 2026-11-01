// A dialog opens at the size it was last left at, never smaller than its own
// (#565). The MIDI windows share one remembered size, and a manual's 600-wide
// window left a drawstop's rows without their Listen and X buttons.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/Mobile.h"

using mp::test::Category;

class DialogReopenSizeTest final : public mp::test::Test {
public:
  DialogReopenSizeTest() : Test("functional.ui.dialog-reopen-size", Category::Functional) {}

  void run() override {
    using R = juce::Rectangle<int>;
    const R drawstop{0, 0, 776, 497};
    const R screen{0, 0, 1920, 1040};

    const R afterManual = mp::ui::reopenedDialogSize({0, 0, 616, 638}, drawstop, screen);
    MP_CHECK(afterManual.getWidth() == 776, "a narrower remembered size keeps the drawstop's width, got " +
                                                std::to_string(afterManual.getWidth()));
    MP_CHECK(afterManual.getHeight() == 638, "and takes the remembered height, which is larger");

    const R larger = mp::ui::reopenedDialogSize({0, 0, 1000, 700}, drawstop, screen);
    MP_CHECK(larger.getWidth() == 1000 && larger.getHeight() == 700, "a larger remembered size is kept");

    const R small = mp::ui::reopenedDialogSize({0, 0, 1000, 700}, drawstop, {0, 0, 800, 600});
    MP_CHECK(small.getWidth() == 800 && small.getHeight() == 600, "the screen still sets the limit");

    const R noScreen = mp::ui::reopenedDialogSize({0, 0, 616, 638}, drawstop, {});
    MP_CHECK(noScreen.getWidth() == 776 && noScreen.getHeight() == 638, "with no screen known, no limit");
  }
};
static DialogReopenSizeTest g_dialogReopenSize;

#endif
