// Names for the player's generals (#1214): "Tutti", "Plenum", a piece's
// "Adagio". Kept in the combination set file with the registrations, so a
// set made for a piece keeps its own names.
#include "MpTest.h"

#include "../src/mp_control/PlayerCombinations.h"

using mp::test::Category;

class GeneralNamesTest final : public mp::test::Test {
public:
  GeneralNamesTest() : Test("functional.combinations.general-names", Category::Functional) {}

  void run() override {
    mp::PlayerCombinations pc;
    MP_CHECK(pc.generalName(1).empty(), "a general has no name at first");

    pc.setGeneralName(1, "Tutti");
    // "Plein jeu à 8’", in UTF-8 bytes.
    const std::string pleinJeu = "Plein jeu \xC3\xA0" " 8\xE2\x80\x99";
    pc.setGeneralName(3, "  " + pleinJeu + "\n");
    pc.setGeneralName(0, "nothing");
    pc.setGeneralName(mp::PlayerCombinations::kMaxGenerals + 1, "nothing");
    MP_CHECK(pc.generalName(1) == "Tutti", "a name is kept");
    MP_CHECK(pc.generalName(3) == pleinJeu, "spaces and line ends around it go: [" +
                                                                    pc.generalName(3) + "]");
    MP_CHECK(pc.generalName(0).empty() && pc.generalName(mp::PlayerCombinations::kMaxGenerals + 1).empty(),
             "a general that does not exist takes no name");

    const std::string text = pc.toText();
    MP_CHECK(text.find("generalname 1 Tutti\n") != std::string::npos, "saved as a line of its own:\n" + text);

    mp::PlayerCombinations back;
    MP_CHECK(back.fromText(text), "the file reads back");
    MP_CHECK(back.generalName(1) == "Tutti" && back.generalName(3) == pc.generalName(3) &&
                 back.generalName(2).empty(),
             "the names read back, and only they");

    back.setGeneralName(1, "");
    MP_CHECK(back.toText().find("generalname 1") == std::string::npos, "an empty name is not written");

    // Another set, read in its place, brings its own names and none of these.
    mp::PlayerCombinations other;
    other.fromText("counts 10 5\n");
    MP_CHECK(other.generalName(3).empty(), "a set without names has none");
  }
};
static GeneralNamesTest g_generalNames;
