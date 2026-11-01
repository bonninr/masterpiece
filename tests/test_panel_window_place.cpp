// A detached panel's window opens where it was left at the last quit, also
// when it was moved after it was detached; a place no longer on a screen
// opens centred (#637, the owner's choice of 2026-11-01).
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Mobile.h"
#include "../src/mp_ui/Panels.h"
#include "../src/mp_ui/Ui.h"

using mp::test::Category;

class PanelWindowPlaceTest final : public mp::test::Test {
public:
  PanelWindowPlaceTest() : Test("functional.ui.panel-window-place", Category::Functional) {}

  void run() override {
    if (mp::ui::kMobile) return;
    juce::MessageManager::getInstance();
    const auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    // A machine with no screen (a build box) has nowhere to put the window.
    if (primary == nullptr || primary->userArea.getWidth() < 900 || primary->userArea.getHeight() < 600) return;
    const auto area = primary->userArea;

    const auto data =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-panel-window-place");
    data.deleteRecursively();
    data.createDirectory();
    // The player's own panels are not this test's to change.
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);
    {
      const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
      mp::MasterpieceProcessor proc;
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");

      mp::ui::PanelLayout layout;
      layout.detached = true;
      const juce::Rectangle<int> left{area.getX() + 50, area.getY() + 70, 640, 420};
      layout.x = left.getX();
      layout.y = left.getY();
      layout.w = left.getWidth();
      layout.h = left.getHeight();
      MP_CHECK(mp::ui::savePanelLayouts(proc, {layout}), "a detached panel is written");

      const juce::Rectangle<int> moved = left.translated(40, 30);
      {
        mp::ui::MasterpieceEditor editor(proc);
        editor.loadPanelsForTest();
        auto* window = editor.panelWindowForTest(0);
        MP_CHECK(window != nullptr, "the panel opens in its own window");
        if (window != nullptr) {
          MP_CHECK(window->getBounds() == left,
                   "where it was left, got " + window->getBounds().toString().toStdString());
          // Moved by the player, and the program quit straight away.
          window->setBounds(moved);
        }
      }
      const auto after = mp::ui::loadPanelLayouts(proc);
      MP_CHECK(after.size() == 1 && juce::Rectangle<int>(after[0].x, after[0].y, after[0].w, after[0].h) == moved,
               "the place it was moved to is kept, got " +
                   (after.empty() ? std::string("nothing")
                                  : juce::Rectangle<int>(after[0].x, after[0].y, after[0].w, after[0].h)
                                        .toString()
                                        .toStdString()));

      // Left on a screen that has since gone.
      layout.x = -6000;
      layout.y = -6000;
      MP_CHECK(mp::ui::savePanelLayouts(proc, {layout}), "a place off every screen is written");
      {
        mp::ui::MasterpieceEditor editor(proc);
        editor.loadPanelsForTest();
        auto* window = editor.panelWindowForTest(0);
        MP_CHECK(window != nullptr && area.contains(window->getBounds().getTopLeft()) &&
                     window->getWidth() == 640,
                 "it opens on the screen at its size, got " +
                     (window != nullptr ? window->getBounds().toString().toStdString() : std::string()));
      }
    }
    mp::setDataFolder(previous);
    data.deleteRecursively();
  }
};
static PanelWindowPlaceTest g_panelWindowPlace;

#endif  // MP_TEST_HAS_AUDIO
