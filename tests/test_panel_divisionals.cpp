// The generated panel draws each divisional piston in its own division's
// column, and the generals in Pistons (#489).
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Panels.h"

#include <map>
#include <string>
#include <vector>

namespace {

using mp::test::Category;

class PanelDivisionalsTest final : public mp::test::Test {
public:
  PanelDivisionalsTest() : Test("functional.ui.panel-divisionals", Category::Functional) {}
  void run() override {
    // Two manuals with two divisionals each, all named "1" and "2" as the
    // Wildervank set names them, and one general.
    const std::string organ =
        "[Organ]\r\nChurchName=Divisionals\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfGenerals=1\r\nNumberOfDivisionals=4\r\n"
        "[General001]\r\nName=G1\r\nNumberOfStops=1\r\nStopManual001=1\r\nStopNumber001=1\r\n"
        "[Divisional001]\r\nName=1\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Divisional002]\r\nName=2\r\nNumberOfStops=0\r\n"
        "[Divisional003]\r\nName=1\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Divisional004]\r\nName=2\r\nNumberOfStops=0\r\n"
        "[Manual001]\r\nName=Hauptwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "NumberOfDivisionals=2\r\nDivisional001=1\r\nDivisional002=2\r\n"
        "[Manual002]\r\nName=Schwellwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=2\r\n"
        "NumberOfDivisionals=2\r\nDivisional001=3\r\nDivisional002=4\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Viola 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=V8\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-panel-divisionals");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("divisionals.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      std::map<std::string, std::vector<std::string>> pistonsIn;
      for (const auto& s : mp::ui::panelSections(proc))
        for (const auto& e : s.elements)
          if (e.kind == mp::ui::PanelElement::Kind::Piston) pistonsIn[s.title].push_back(e.name);
      auto list = [&](const std::string& title) {
        std::string out;
        for (const auto& n : pistonsIn[title]) out += " [" + n + "]";
        return out;
      };
      MP_CHECK(pistonsIn["Hauptwerk"] == std::vector<std::string>({"1", "2"}),
               "the Hauptwerk's divisionals are in its column:" + list("Hauptwerk"));
      MP_CHECK(pistonsIn["Schwellwerk"] == std::vector<std::string>({"1", "2"}),
               "the Schwellwerk's in its own, an empty one too:" + list("Schwellwerk"));
      MP_CHECK(pistonsIn["Pistons"] == std::vector<std::string>({"G1"}),
               "and the general stays in Pistons:" + list("Pistons"));
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static PanelDivisionalsTest g_panelDivisionals;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
