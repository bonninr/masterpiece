// Stop names on a generated panel (#1012): a division's prefix goes with every
// space after it, and a half stop's mark stays on the footage's line.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_ui/Panels.h"

#include <string>

namespace {

using mp::test::Category;

class PanelNamesTest final : public mp::test::Test {
public:
  PanelNamesTest() : Test("functional.ui.panel-names", Category::Functional) {}
  void run() override {
    mp::ui::PanelSection sec;
    for (const char* n : {"P  Subbas 16'", "P Octavbas 8'", "P   Posaune 16'", "I / P"}) {
      mp::ui::PanelElement e;
      e.name = n;
      sec.elements.push_back(e);
    }
    mp::ui::dropSharedPrefix(sec);
    MP_CHECK(sec.elements[0].name == "Subbas 16'", "two spaces after the prefix go too: '" + sec.elements[0].name + "'");
    MP_CHECK(sec.elements[1].name == "Octavbas 8'", "one space: '" + sec.elements[1].name + "'");
    MP_CHECK(sec.elements[2].name == "Posaune 16'", "three spaces: '" + sec.elements[2].name + "'");
    MP_CHECK(sec.elements[3].name == "I / P", "a coupler keeps its name: '" + sec.elements[3].name + "'");

    std::string name, foot;
    mp::ui::splitFootage("Gedackt 8' D", name, foot);
    MP_CHECK(name == "Gedackt" && foot == "8' D", "a discant's mark on the footage's line: " + name + " / " + foot);
    mp::ui::splitFootage("Trompete 8' B", name, foot);
    MP_CHECK(name == "Trompete" && foot == "8' B", "a bass half's mark: " + name + " / " + foot);
    mp::ui::splitFootage("Cornet V (Dessus)", name, foot);
    MP_CHECK(name == "Cornet" && foot == "V (Dessus)", "a mark in brackets: " + name + " / " + foot);
  }
};
static PanelNamesTest g_panelNames;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
