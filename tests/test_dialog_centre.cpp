// Where a dialog opens: in the main window's middle when the active window is
// one kept above the others, as the Recorder is; a page or panel window keeps
// its own dialogs (reported on D#565, 2026-10-31: the loading dialog opened
// over the Recorder at the side). No window is put on the screen.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/Mobile.h"

using mp::test::Category;

class DialogCentreTest final : public mp::test::Test {
public:
  DialogCentreTest() : Test("functional.ui.dialog-centre", Category::Functional) {}

  void run() override {
    juce::Component main;
    mp::ui::setDialogCentre(&main);

    juce::DocumentWindow recorder("Recorder", juce::Colours::black, 0, false);
    recorder.setAlwaysOnTop(true);
    MP_CHECK(mp::ui::dialogCentreFor(&recorder) == &main,
             "with the Recorder active, a dialog opens over the main window");

    juce::DocumentWindow page("Page", juce::Colours::black, 0, false);
    MP_CHECK(mp::ui::dialogCentreFor(&page) == nullptr,
             "with a page window active, it opens over that window");

    MP_CHECK(mp::ui::dialogCentreFor(nullptr) == &main,
             "with none active, over the main window");

    mp::ui::setDialogCentre(nullptr);
  }
};
static DialogCentreTest g_dialogCentre;

#endif  // MP_TEST_HAS_AUDIO
