// The program couplers are part of the registration (#637, points 15 and 16):
// the player's generals store them, General Cancel clears them, the organ's
// own generals hold them, and a panel lists each once, in its own section.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Panels.h"

#include <algorithm>
#include <map>
#include <string>

namespace {

using mp::test::Category;

class ProgramCouplersCombinationsTest final : public mp::test::Test {
public:
  ProgramCouplersCombinationsTest()
      : Test("functional.combinations.program-couplers", Category::Functional) {}
  void run() override {
    // Two manuals and a general that draws a stop on each.
    const std::string organ =
        "[Organ]\r\nChurchName=Program couplers\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfGenerals=1\r\n"
        "[General001]\r\nName=G1\r\nNumberOfStops=2\r\nStopManual001=1\r\nStopNumber001=1\r\n"
        "StopManual002=2\r\nStopNumber002=1\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Manual002]\r\nName=Swell\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=2\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Viola 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=V8\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-program-couplers-combos");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("program.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      proc.setProgramCouplers(true);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      const auto& list = proc.programCouplerList();
      MP_CHECK(!list.empty(), "the organ has program couplers");
      if (list.empty()) return;
      const mp::Id coupler = list.front().switchId;

      // The player's combinations hold it.
      bool held = false;
      for (const auto& e : proc.playerCombinations().elements())
        if (e.kind == mp::PlayerCombinations::ElementKind::Switch && e.id == coupler) held = true;
      MP_CHECK(held, "the player's combinations store the program coupler");

      // General Cancel clears it.
      proc.setSwitchEngaged(coupler, true);
      MP_CHECK(proc.switchEngaged(coupler), "the program coupler is drawn");
      proc.pressGeneralCancel();
      MP_CHECK(!proc.switchEngaged(coupler), "General Cancel puts the program coupler in");

      // The organ's own general holds every program coupler.
      int generals = 0;
      for (const auto& [id, c] : proc.organModel().combinations) {
        if (c.divisionId != 0) continue;
        size_t found = 0;
        for (const auto& pc : list)
          found += static_cast<size_t>(std::count_if(c.elements.begin(), c.elements.end(),
                                                     [&](const mp::CombinationElement& e) {
                                                       return e.controlledSwitchId == pc.switchId;
                                                     }));
        if (found == list.size()) ++generals;
      }
      MP_CHECK(generals >= 1, "the organ's general holds the program couplers");

      // Each program coupler appears once across the panel's sections.
      std::map<mp::Id, int> seen;
      for (const auto& s : mp::ui::panelSections(proc))
        for (const auto& e : s.elements)
          if (e.kind == mp::ui::PanelElement::Kind::Switch) ++seen[e.id];
      int twice = 0;
      for (const auto& pc : list)
        if (seen[pc.switchId] != 1) ++twice;
      MP_CHECK(twice == 0, "each program coupler is listed once on the panel; " + std::to_string(twice) +
                               " are not");
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static ProgramCouplersCombinationsTest g_programCouplersCombinations;

}  // namespace

#endif
