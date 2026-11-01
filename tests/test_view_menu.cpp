// The View menu with no organ loaded: there are no stops to list or to put on
// a panel, so "Stop list" is neither ticked nor offered, and "Panel of the
// whole organ" is greyed out (reported on D#565, 2026-10-31).
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Ui.h"

using mp::test::Category;

class ViewMenuNoOrganTest final : public mp::test::Test {
public:
  ViewMenuNoOrganTest() : Test("functional.ui.view-menu-no-organ", Category::Functional) {}

  void run() override {
    mp::MasterpieceProcessor proc;
    MP_CHECK(proc.loadedOrganFile() == juce::File(), "no organ is loaded");
    mp::ui::MasterpieceEditor editor(proc);
    juce::PopupMenu menu;
    editor.addPanelItems(menu);

    bool sawStopList = false, sawPanel = false;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
      const auto& item = it.getItem();
      if (item.itemID == 1002) {
        sawStopList = true;
        MP_CHECK(!item.isTicked, "Stop list is not ticked with no organ");
        MP_CHECK(!item.isEnabled, "and is greyed out");
      }
      if (item.itemID == 1001) {
        sawPanel = true;
        MP_CHECK(!item.isEnabled, "Panel of the whole organ is greyed out with no organ");
      }
    }
    MP_CHECK(sawStopList && sawPanel, "both items are in the menu");
  }
};
static ViewMenuNoOrganTest g_viewMenuNoOrgan;

#endif  // MP_TEST_HAS_AUDIO
