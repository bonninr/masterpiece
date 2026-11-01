// The Couplers window (#722): the organ's couplers and the extra couplers,
// and nothing else, each drawn and retired from its button, and following
// the engine when a coupler is drawn elsewhere.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_ui/Ui.h"

#include <string>

namespace {

using mp::test::Category;

class CouplersWindowTest final : public mp::test::Test {
public:
  CouplersWindowTest() : Test("functional.ui.couplers-window", Category::Functional) {}
  void run() override {
    juce::MessageManager::getInstance();
    // Two manuals, a stop on each, and one coupler of the organ's own.
    const std::string organ =
        "[Organ]\r\nChurchName=Couplers window\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "NumberOfCouplers=1\r\nCoupler001=1\r\n"
        "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=2\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Viola 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=V8\\036.wav\r\n"
        "[Coupler001]\r\nName=Swell to Great\r\nDestinationManual=2\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-couplers-window");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("couplers.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");

      // The organ's coupler only: the stops stay on the stop list.
      int flagged = 0, stopsFlagged = 0;
      for (const auto& s : mp::ui::panelSections(proc))
        for (const auto& e : s.elements) {
          if (!e.coupler) continue;
          ++flagged;
          if (e.kind == mp::ui::PanelElement::Kind::Stop &&
              proc.organModel().stops.at(e.id).ranks.size() != 0)
            ++stopsFlagged;
        }
      MP_CHECK(flagged == 1, "one coupler of the organ's own, got " + std::to_string(flagged));
      MP_CHECK(stopsFlagged == 0, "no stop with pipes counts as a coupler");

      mp::ui::CouplersPanel panel(proc);
      panel.setSize(500, panel.preferredHeight(500));
      MP_CHECK(panel.couplerCount() == 1, "the window lists the organ's coupler, got " +
                                              std::to_string(panel.couplerCount()));
      auto* button = panel.couplerButton(0);
      MP_CHECK(button != nullptr && button->getButtonText().contains("Swell"), "the button carries its name");
      if (button == nullptr) return;
      MP_CHECK(button->getBounds().getWidth() > 0, "the button is laid out");

      // A click draws it in the engine.
      const auto engagedNow = [&] {
        for (const auto& s : mp::ui::panelSections(proc))
          for (const auto& e : s.elements)
            if (e.coupler)
              return e.kind == mp::ui::PanelElement::Kind::Stop ? proc.shownStopEngaged(e.id)
                                                                : proc.shownSwitchEngaged(e.id);
        return false;
      };
      MP_CHECK(!engagedNow(), "the coupler starts retired");
      button->setToggleState(true, juce::dontSendNotification);
      button->onClick();
      MP_CHECK(engagedNow(), "the button draws the coupler");

      // General Cancel retires it, and the button follows.
      proc.pressGeneralCancel();
      panel.refresh();
      MP_CHECK(!engagedNow() && !button->getToggleState(), "the button follows the engine");

      // With the extra couplers the window lists them too, after the organ's.
      proc.setProgramCouplers(true);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads with the extra couplers");
      panel.rebuild();
      const int extra = static_cast<int>(proc.programCouplerList().size());
      MP_CHECK(extra > 0, "the organ has extra couplers");
      MP_CHECK(panel.couplerCount() == 1 + extra, "the window lists the organ's coupler and " +
                                                      std::to_string(extra) + " extra, got " +
                                                      std::to_string(panel.couplerCount()));
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static CouplersWindowTest g_couplersWindow;

}  // namespace

#endif
