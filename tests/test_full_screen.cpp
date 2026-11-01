// View > Full screen on a desktop (Discussion #717): the console takes the
// whole window, the menus and the top bar come over it while the mouse is at
// the top edge with a button to leave, and Escape or F11 leave.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Ui.h"

using mp::test::Category;

class FullScreenTest final : public mp::test::Test {
public:
  FullScreenTest() : Test("functional.ui.full-screen", Category::Functional) {}

  void run() override {
    mp::MasterpieceProcessor proc;
    mp::ui::MasterpieceEditor editor(proc);
    editor.setSize(1200, 700);
    int calls = 0;
    bool asked = false;
    editor.onFullScreen = [&](bool on) {
      ++calls;
      asked = on;
    };

    const auto windowed = editor.contentBoundsForTest();
    MP_CHECK(windowed.getY() > 0, "in a window the band sits above the console");
    MP_CHECK(!editor.fileMenuBoundsForTest().isEmpty(), "and the menus are there");

    editor.setFullScreen(true);
    MP_CHECK(editor.fullScreen() && calls == 1 && asked, "the window is asked to cover the display");
    const auto full = editor.contentBoundsForTest();
    MP_CHECK(full.getY() == 0 && full.getHeight() > windowed.getHeight(),
             "full screen, the console has the whole window: top " + std::to_string(full.getY()));
    MP_CHECK(editor.fileMenuBoundsForTest().isEmpty(), "and the menus are away");
    MP_CHECK(!editor.exitFullScreenShownForTest(), "as is the way out");

    editor.showFullScreenBandForTest(true);
    MP_CHECK(!editor.fileMenuBoundsForTest().isEmpty(), "the mouse at the top brings the menus back");
    MP_CHECK(editor.exitFullScreenShownForTest(), "with the button to leave");
    MP_CHECK(editor.contentBoundsForTest() == full, "laid over the console, which stays where it is");
    editor.showFullScreenBandForTest(false);
    MP_CHECK(editor.fileMenuBoundsForTest().isEmpty(), "and away again when the mouse leaves");

    editor.pressKeyForTest(juce::KeyPress(juce::KeyPress::escapeKey));
    MP_CHECK(!editor.fullScreen() && calls == 2 && !asked, "Escape leaves full screen");
    MP_CHECK(editor.contentBoundsForTest() == windowed, "the console is back under the band");
    MP_CHECK(!editor.pressKeyForTest(juce::KeyPress(juce::KeyPress::escapeKey)),
             "out of full screen, Escape is left to the rest of the program");

    editor.pressKeyForTest(juce::KeyPress(juce::KeyPress::F11Key));
    MP_CHECK(editor.fullScreen(), "F11 goes in");
    editor.pressKeyForTest(juce::KeyPress(juce::KeyPress::F11Key));
    MP_CHECK(!editor.fullScreen(), "and out");

    // A plug-in's editor, or a phone's: no window of its own to cover.
    editor.onFullScreen = nullptr;
    MP_CHECK(!editor.pressKeyForTest(juce::KeyPress(juce::KeyPress::F11Key)) && !editor.fullScreen(),
             "with no window to cover, F11 is left alone");
  }
};
static FullScreenTest g_fullScreen;

#endif  // MP_TEST_HAS_AUDIO
