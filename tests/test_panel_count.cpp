// An organ has up to ten panels (#637, item 34): View > New panel makes them
// one after another, each is saved, and all ten come back with the organ.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Panels.h"
#include "../src/mp_ui/Ui.h"

using mp::test::Category;

class PanelCountTest final : public mp::test::Test {
public:
  PanelCountTest() : Test("functional.ui.panel-count", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    const auto data = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-panel-count");
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
      {
        mp::ui::MasterpieceEditor editor(proc);
        for (int i = 0; i < 12; ++i) editor.addPanelForTest();
        MP_CHECK(editor.panelCountForTest() == 10,
                 "New panel makes ten, got " + std::to_string(editor.panelCountForTest()));
      }
      MP_CHECK(mp::ui::loadPanelLayouts(proc).size() == 10, "all ten are saved");
      {
        mp::ui::MasterpieceEditor editor(proc);
        editor.loadPanelsForTest();
        MP_CHECK(editor.panelCountForTest() == 10,
                 "and come back with the organ, got " + std::to_string(editor.panelCountForTest()));
      }
    }
    mp::setDataFolder(previous);
    data.deleteRecursively();
  }
};
static PanelCountTest g_panelCount;

#endif  // MP_TEST_HAS_AUDIO
