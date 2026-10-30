// Every checkbox on Organ settings > Loading starts at the panel's left edge
// (#565). "Program couplers" sat in the drop-down column, and "Hold samples
// compressed" and "Real-time render threads" in the right half of rows they
// shared, so a player reading down the tab missed all three.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Settings.h"

#include <algorithm>
#include <string>
#include <vector>

using mp::test::Category;

class LoadingLayoutTest final : public mp::test::Test {
public:
  LoadingLayoutTest() : Test("functional.ui.loading-layout", Category::Functional) {}

  // The visible drop-down holding "Custom": 1 when it can be chosen, 0 when
  // greyed, -1 when there is none.
  static int customState(juce::Component& panel) {
    for (auto* child : panel.getChildren()) {
      auto* box = dynamic_cast<juce::ComboBox*>(child);
      if (box == nullptr || !box->isVisible()) continue;
      for (int i = 0; i < box->getNumItems(); ++i)
        if (box->getItemText(i) == "Custom") return box->isItemEnabled(box->getItemId(i)) ? 1 : 0;
    }
    return -1;
  }

  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    // Simple mode first: the Quality presets, and none of the checkboxes.
    proc.setSimpleMode(true, false);
    {
      mp::ui::EnginePanel simple(proc);
      simple.setSize(770, 1350);
      int ticks = 0;
      bool quality = false;
      for (auto* child : simple.getChildren()) {
        if (!child->isVisible()) continue;
        if (dynamic_cast<juce::ToggleButton*>(child) != nullptr) ++ticks;
        if (auto* label = dynamic_cast<juce::Label*>(child); label != nullptr && label->getText() == "Quality")
          quality = true;
      }
      MP_CHECK(quality, "Simple mode shows the Quality presets");
      MP_CHECK(ticks == 0, "and no checkbox: " + std::to_string(ticks) + " shown");
      // Custom stands for the controls Simple mode hides: there it is shown
      // but cannot be chosen (#636).
      mp::ui::EnginePanel performance(proc, mp::ui::EnginePanel::Part::Performance);
      for (auto* panel : {static_cast<juce::Component*>(&simple), static_cast<juce::Component*>(&performance)})
        MP_CHECK(customState(*panel) == 0, "Simple mode: Custom is listed and greyed");
    }
    proc.setSimpleMode(false, false);
    {
      mp::ui::EnginePanel organ(proc), performance(proc, mp::ui::EnginePanel::Part::Performance);
      MP_CHECK(customState(organ) == 1, "Advanced mode: Quality's Custom can be chosen");
      MP_CHECK(customState(performance) == 1, "Advanced mode: Performance's Custom can be chosen");
    }
    mp::ui::EnginePanel panel(proc);
    // The width of the Organ settings window and the height its Loading tab
    // gives the panel (OrganSettings.h).
    panel.setSize(770, 1350);

    std::vector<juce::Component*> shown;
    int left = -1, boxes = 0, indented = 0;
    bool couplers = false, compressed = false;
    for (auto* child : panel.getChildren())
      if (child->isVisible() && dynamic_cast<juce::ToggleButton*>(child) != nullptr)
        left = left < 0 ? child->getX() : std::min(left, child->getX());
    // The settings the quality preset changes sit one step in, under it
    // (#634); every other checkbox at the left edge.
    constexpr int kIndent = 20;
    for (auto* child : panel.getChildren()) {
      if (!child->isVisible()) continue;
      shown.push_back(child);
      auto* box = dynamic_cast<juce::ToggleButton*>(child);
      if (box == nullptr) continue;
      ++boxes;
      const std::string text = box->getButtonText().toStdString();
      if (box->getX() == left + kIndent) ++indented;
      MP_CHECK(box->getX() == left || box->getX() == left + kIndent,
               "\"" + text + "\" starts at x " + std::to_string(box->getX()) + ", the checkboxes at " +
                   std::to_string(left) + " or one step in");
      if (text == "Program couplers") couplers = true;
      if (text == "Lossless compression in memory") compressed = true;
    }
    MP_CHECK(indented > 0 && indented < boxes, "some checkboxes sit under the preset, the rest at the edge");
    MP_CHECK(boxes >= 10, "the Loading tab has its checkboxes");
    MP_CHECK(couplers, "Program couplers is on the tab");
    MP_CHECK(compressed, "Lossless compression in memory is on the tab");

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

    // Shorter than its rows, as on a small screen: every row keeps its
    // height, and the panel asks for the room they need, so the tab scrolls
    // to all of them (the Loading tab on a Mac, D565).
    const int needed = panel.contentHeight();
    panel.setSize(770, 400);
    MP_CHECK(panel.contentHeight() == needed, "a short panel asks for " + std::to_string(needed) +
                                                  " pixels as a tall one does, got " +
                                                  std::to_string(panel.contentHeight()));
    MP_CHECK(needed > 400, "the Loading tab's rows need more than 400 pixels");
    for (auto* child : panel.getChildren())
      if (child->isVisible() && dynamic_cast<juce::ComboBox*>(child) != nullptr)
        MP_CHECK(child->getHeight() > 0, "\"" + child->getName().toStdString() + "\" keeps its height in a short panel");
  }
};
static LoadingLayoutTest g_loadingLayout;

#endif  // MP_TEST_HAS_AUDIO
