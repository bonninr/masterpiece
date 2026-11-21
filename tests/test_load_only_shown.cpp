// Load only shown ranks (#1207): a filter such as "middle", then this button,
// loads the ranks in the filtered list and leaves out every other one. Load
// shown ranks only adds to what is chosen, so on an organ loaded in full it
// changed nothing, and a player filtering for one perspective saw nothing
// happen.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/OrganSettings.h"

using mp::test::Category;

namespace {
template <typename T>
T* findChild(juce::Component& root, const juce::String& text = {}) {
  for (auto* c : root.getChildren()) {
    if (auto* t = dynamic_cast<T*>(c); t != nullptr) {
      if constexpr (std::is_base_of_v<juce::Button, T>) {
        if (t->getButtonText() == text) return t;
      } else {
        return t;
      }
    }
    if (auto* deeper = findChild<T>(*c, text); deeper != nullptr) return deeper;
  }
  return nullptr;
}
}  // namespace

class LoadOnlyShownRanksTest final : public mp::test::Test {
public:
  LoadOnlyShownRanksTest() : Test("functional.ui.load-only-shown-ranks", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    const auto data =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-load-only-shown");
    data.deleteRecursively();
    data.createDirectory();
    // Put back however the test ends: a failed check throws.
    struct Restore {
      juce::File previous, data;
      ~Restore() {
        mp::setDataFolder(previous);
        data.deleteRecursively();
      }
    } restore{mp::dataFolder(), data};
    mp::setDataFolder(data);
    {
      mp::MasterpieceProcessor proc;
      const juce::File odf(juce::String(MP_TEST_FIXTURES_DIR) + "/m24.expression.Organ_Hauptwerk_xml");
      MP_CHECK(proc.loadOrgan(odf, 0, true).ok, "the fixture loads");
      MP_CHECK(proc.excludedRanks().empty(), "every rank is chosen at first");

      mp::ui::StopsLoadPanel panel(proc, {});
      panel.setSize(900, 600);
      auto* filter = findChild<juce::TextEditor>(panel);
      auto* only = findChild<juce::TextButton>(panel, "Load only shown ranks");
      auto* load = findChild<juce::TextButton>(panel, "Load shown ranks");
      auto* leave = findChild<juce::TextButton>(panel, "Leave out shown ranks");
      MP_CHECK(filter != nullptr && only != nullptr && load != nullptr && leave != nullptr,
               "the filter and its three buttons are there");
      if (filter == nullptr || only == nullptr || load == nullptr || leave == nullptr) return;

      // A TextEditor tells of a change through the message loop; the test
      // types and then hands the change on itself.
      auto type = [filter](const juce::String& text) {
        filter->setText(text, juce::dontSendNotification);
        if (filter->onTextChange) filter->onTextChange();
      };
      MP_CHECK(!only->isEnabled(), "with no filter, Load only shown ranks is off: it would be every rank");
      type("blower");
      MP_CHECK(only->isEnabled(), "with a filter, it is on");

      // What the reporter did: everything chosen, filter, Load shown ranks.
      load->onClick();
      MP_CHECK(proc.excludedRanks().empty(), "Load shown ranks still only adds: nothing changes");

      only->onClick();
      MP_CHECK(proc.excludedRanks() == std::set<mp::Id>{92},
               "Load only shown ranks leaves out the rank the filter hides");

      // The other two keep working as before.
      type("clunk");
      load->onClick();
      MP_CHECK(proc.excludedRanks().empty(), "Load shown ranks brings the hidden one back");
      leave->onClick();
      MP_CHECK(proc.excludedRanks() == std::set<mp::Id>{92}, "Leave out shown ranks leaves it out again");
    }
  }
};
static LoadOnlyShownRanksTest g_loadOnlyShownRanks;

#endif  // MP_TEST_HAS_AUDIO
