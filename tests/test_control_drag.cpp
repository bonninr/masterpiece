// A drag along a control's drawn length keeps up with the hand (#871). A swell
// pedal drawn upright went at half a step a pixel, so a drag down its picture
// moved it a fraction of a drag across.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_ui/Console.h"

namespace {

using mp::test::Category;
using mp::ui::ConsoleView;

class ControlDragTest final : public mp::test::Test {
public:
  ControlDragTest() : Test("functional.ui.control-drag", Category::Functional) {}

  void run() override {
    MP_CHECK(ConsoleView::controlDragSteps(0, 127, 60, 127) == 127.0, "the height of a pedal is its whole travel");
    MP_CHECK(ConsoleView::controlDragSteps(0, 100, 40, 80) == 100.0, "a short pedal goes a step a pixel");
    MP_CHECK(ConsoleView::controlDragSteps(0, 254, 40, 400) == 127.0, "a tall one at half a step");
    MP_CHECK(ConsoleView::controlDragSteps(0, 100, 208, 61) == 50.0, "a numeric field drawn wide stays at half a step");
    MP_CHECK(ConsoleView::controlDragSteps(0, -100, 60, 127) == -100.0, "down is less");
    MP_CHECK(ConsoleView::controlDragSteps(200, 0, 254, 40) == 100.0, "sideways goes by the drawn width");
  }
};
static ControlDragTest g_controlDrag;

} // namespace

#endif
