// The Combinations window in the Simple view (#692): the sequencer is the
// generals, with no frame numbers, Insert, Delete or second row of generals;
// the Advanced view shows them all. The window follows the display mode.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_ui/CombinationsWindow.h"

#include <string>

namespace {

using mp::test::Category;

// The first visible-or-not child button whose text is `text`.
juce::Component* buttonCalled(juce::Component& parent, const juce::String& text) {
  for (auto* c : parent.getChildren())
    if (auto* b = dynamic_cast<juce::Button*>(c); b != nullptr && b->getButtonText() == text) return b;
  return nullptr;
}

class CombinationsSimpleViewTest final : public mp::test::Test {
public:
  CombinationsSimpleViewTest() : Test("functional.ui.combinations-simple-view", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    const bool was = proc.simpleMode();

    proc.setSimpleMode(true, false);
    mp::ui::CombinationsPanel panel(proc);
    panel.setSize(620, panel.preferredHeight(620));
    const int simpleHeight = panel.preferredHeight(620);
    auto* insert = buttonCalled(panel, "Insert");
    auto* plus = buttonCalled(panel, "+");
    MP_CHECK(insert != nullptr && !insert->isVisible(), "Simple: no Insert");
    MP_CHECK(plus != nullptr && plus->isVisible(), "Simple: + steps through the generals");
    // The player's generals 1 to n are a second row of generals: hidden.
    auto* one = buttonCalled(panel, "1");
    int onesShown = 0;
    for (auto* c : panel.getChildren())
      if (auto* b = dynamic_cast<juce::Button*>(c); b != nullptr && b->getButtonText() == "1" && b->isVisible())
        ++onesShown;
    MP_CHECK(one != nullptr, "the general 1 is there");
    MP_CHECK(onesShown == 1, "Simple: one row of generals, got " + std::to_string(onesShown) + " buttons \"1\"");

    proc.setSimpleMode(false, false);
    panel.refresh();
    MP_CHECK(insert != nullptr && insert->isVisible(), "Advanced: Insert shown");
    MP_CHECK(buttonCalled(panel, ">") != nullptr, "Advanced: > steps through the frames");
    MP_CHECK(panel.preferredHeight(620) > simpleHeight, "Advanced: the player's generals take room");

    // #841: one side of the window at a time.
    const int viewWas = proc.combinationsView();
    const int bothHeight = panel.preferredHeight(620);
    proc.setCombinationsView(2);
    panel.showView();
    MP_CHECK(insert != nullptr && insert->isVisible(), "the sequencer alone keeps Insert");
    MP_CHECK(panel.preferredHeight(620) < bothHeight, "and takes less room");
    proc.setCombinationsView(1);
    panel.showView();
    MP_CHECK(insert != nullptr && !insert->isVisible(), "the combinations alone leave the sequencer out");
    MP_CHECK(panel.preferredHeight(620) < bothHeight, "and take less room");
    proc.setCombinationsView(viewWas);

    proc.setSimpleMode(was, false);
  }
};
static CombinationsSimpleViewTest g_combinationsSimpleView;

}  // namespace

#endif
