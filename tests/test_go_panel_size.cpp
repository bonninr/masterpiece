// A GrandOrgue panel is the size it declares (#1220): Alkmaar's 700 x 400
// panels lie on a far larger background image, and the page took the image's
// size. The converted page carries the panel's screen, and the console cuts
// the page to it.
#include "MpTest.h"

#include "../src/mp_core/GrandOrgueImport.h"
#include "../src/mp_core/OdfLoader.h"
#include "../src/mp_core/OrganModel.h"

#include <string>

namespace {

using mp::test::Category;

mp::OrganModel load(const std::string& panelSize) {
  const std::string organ =
      "[Organ]\nChurchName=Panels\nHasPedals=N\nNumberOfManuals=1\nNumberOfWindchestGroups=1\n"
      "NumberOfPanels=0\n" + panelSize +
      "[WindchestGroup001]\nName=Main\n"
      "[Manual001]\nName=Great\nNumberOfLogicalKeys=12\nFirstAccessibleKeyMIDINoteNumber=36\n"
      "NumberOfAccessibleKeys=12\nNumberOfStops=1\nStop001=1\n"
      "[Stop001]\nName=Principal 8\nDisplayed=Y\nNumberOfLogicalPipes=1\nNumberOfAccessiblePipes=1\n"
      "FirstAccessiblePipeLogicalKeyNumber=1\nWindchestGroup=1\nPipe001=P\\036.wav\n";
  const mp::GrandOrgueImportReport rep = mp::convertGrandOrgueText(organ);
  MP_CHECK(rep.ok, "the GrandOrgue definition converts: " + rep.error);
  mp::OdfLoader loader;
  mp::OrganModel model;
  mp::OdfDiagnostics diag;
  mp::OdfLoader::Options options;
  MP_CHECK(loader.loadFromXmlString(rep.xml, "panels.organ", options, model, diag), "the converted definition loads");
  return model;
}

class GrandOrguePanelSizeTest final : public mp::test::Test {
public:
  GrandOrguePanelSizeTest() : Test("functional.grandorgue.panel-size", Category::Functional) {}
  void run() override {
    const auto sized = load("DispScreenSizeHoriz=700\nDispScreenSizeVert=400\n");
    MP_CHECK(!sized.displayPages.empty(), "the console page is there");
    if (sized.displayPages.empty()) return;
    const auto& page = sized.displayPages.begin()->second;
    MP_CHECK(page.widthPx == 700 && page.heightPx == 400,
             "the page is the 700 x 400 the panel declares, got " + std::to_string(page.widthPx) + " x " +
                 std::to_string(page.heightPx));
    const auto named = load("DispScreenSizeHoriz=MEDIUM\nDispScreenSizeVert=MEDIUM\n");
    MP_CHECK(!named.displayPages.empty() && named.displayPages.begin()->second.widthPx == 1007 &&
                 named.displayPages.begin()->second.heightPx == 663,
             "a named size, MEDIUM, is 1007 x 663 as in GrandOrgue");
  }
};
static GrandOrguePanelSizeTest g_grandOrguePanelSize;

}  // namespace
