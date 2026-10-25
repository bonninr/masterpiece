// Every checkbox on Organ settings > Loading starts at the panel's left edge
// (#565). "Program couplers" sat in the drop-down column, and "Hold samples
// compressed" and "Real-time render threads" in the right half of rows they
// shared, so a player reading down the tab missed all three.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Settings.h"

#include <string>
#include <vector>

using mp::test::Category;

class LoadingLayoutTest final : public mp::test::Test {
public:
  LoadingLayoutTest() : Test("functional.ui.loading-layout", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    mp::ui::EnginePanel panel(proc);
    // The width of the Organ settings window and the height its Loading tab
    // gives the panel (OrganSettings.h).
    panel.setSize(770, 1350);

    std::vector<juce::Component*> shown;
    int left = -1, boxes = 0;
    bool couplers = false, compressed = false;
    for (auto* child : panel.getChildren()) {
      if (!child->isVisible()) continue;
      shown.push_back(child);
      auto* box = dynamic_cast<juce::ToggleButton*>(child);
      if (box == nullptr) continue;
      ++boxes;
      const std::string text = box->getButtonText().toStdString();
      if (left < 0) left = box->getX();
      MP_CHECK(box->getX() == left, "\"" + text + "\" starts at x " + std::to_string(box->getX()) +
                                        ", the other checkboxes at " + std::to_string(left));
      if (text == "Program couplers") couplers = true;
      if (text.rfind("Hold samples compressed", 0) == 0) compressed = true;
    }
    MP_CHECK(boxes >= 10, "the Loading tab has its checkboxes");
    MP_CHECK(couplers, "Program couplers is on the tab");
    MP_CHECK(compressed, "Hold samples compressed is on the tab");

    // Rows were added: nothing runs into its neighbour or under the buttons.
    for (size_t i = 0; i < shown.size(); ++i)
      for (size_t j = i + 1; j < shown.size(); ++j) {
        const auto a = shown[i]->getBounds(), b = shown[j]->getBounds();
        if (a.isEmpty() || b.isEmpty()) continue;
        MP_CHECK(!a.intersects(b), "\"" + shown[i]->getName().toStdString() + "\" at " +
                                       a.toString().toStdString() + " overlaps \"" +
                                       shown[j]->getName().toStdString() + "\" at " +
                                       b.toString().toStdString());
      }
  }
};
static LoadingLayoutTest g_loadingLayout;

#endif  // MP_TEST_HAS_AUDIO
