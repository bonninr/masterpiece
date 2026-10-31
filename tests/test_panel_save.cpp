// A panel's Save button: a change gives it an asterisk, and saving takes the
// asterisk away again (reported on D#565, 2026-10-31: "The * won't go away").
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Panels.h"

using mp::test::Category;

class PanelSaveTest final : public mp::test::Test {
public:
  PanelSaveTest() : Test("functional.ui.panel-save-asterisk", Category::Functional) {}

  void run() override {
    mp::MasterpieceProcessor proc;
    mp::ui::PanelView panel(proc, 0, mp::ui::PanelLayout{});
    panel.setSize(800, 600);
    MP_CHECK(panel.saveTextForTest() == "Save", "a new panel reads Save");
    panel.setScaleForTest(150);
    MP_CHECK(panel.hasUnsavedChanges() && panel.saveTextForTest() == "Save *",
             "a change gives Save its asterisk");
    panel.markSaved();
    MP_CHECK(!panel.hasUnsavedChanges(), "saved, nothing is left unsaved");
    MP_CHECK(panel.saveTextForTest() == "Save",
             "and the asterisk goes: " + panel.saveTextForTest().toStdString());
  }
};
static PanelSaveTest g_panelSave;

#endif  // MP_TEST_HAS_AUDIO
