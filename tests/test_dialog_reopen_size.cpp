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

    // Left at 320,250 at 100 % text size, reopened at 150 % on a 1600 x 1000
    // screen: 1067 x 667 in the program's units. Half of it still showed, so
    // the place was used, and the dialog ran off the right and the bottom.
    const juce::Array<R> scaled{R{0, 0, 1067, 667}};
    const R kept{320, 250, 968, 594};
    MP_CHECK(mp::ui::dialogPlaceUsable(kept, scaled), "the remembered place is still usable");
    const R moved = mp::ui::keptOnScreen(kept, scaled);
    MP_CHECK(scaled[0].contains(moved), "and the dialog opens whole on the screen, at " +
                                            moved.toString().toStdString());
    MP_CHECK(moved.getWidth() == 968 && moved.getHeight() == 594, "at its remembered size");
    MP_CHECK(moved.getY() >= 32, "below the strip the title bar takes, at y " + std::to_string(moved.getY()));
    const R inside{100, 80, 600, 400};
    MP_CHECK(mp::ui::keptOnScreen(inside, scaled) == inside, "a place already on the screen stays");
    // Two screens side by side: it goes whole onto the one it overlaps most.
    const juce::Array<R> two{R{0, 0, 1920, 1080}, R{1920, 0, 1280, 1024}};
    const R across{1700, 100, 800, 600};
    MP_CHECK(two[1].contains(mp::ui::keptOnScreen(across, two)), "across two screens, onto the one with more of it");
  }
};
static DialogReopenSizeTest g_dialogReopenSize;

#endif
