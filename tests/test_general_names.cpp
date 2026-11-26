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

    // Divisionals take names too (JPIL04, #1221): "Cornet 3 rangs" on the
    // Positif's 10, kept per division and number.
    mp::PlayerCombinations::Element stop;
    stop.id = 11;
    stop.divisionId = 7;
    stop.name = "Cornet";
    const std::vector<mp::PlayerCombinations::Division> divisions{{7, "Positif"}, {8, "Pedale"}};
    mp::PlayerCombinations div;
    div.reset({stop}, divisions);
    MP_CHECK(!div.anyDivisionalNamed(), "no divisional is named at first");
    div.setDivisionalName(7, 10, " Cornet 3 rangs\t");
    MP_CHECK(!div.anyDivisionalNamed(), "a name on a divisional not shown (5 of them) widens nothing");
    div.setDivisionalCount(10);
    div.setDivisionalName(9, 1, "a division the organ lacks");
    div.setDivisionalName(8, mp::PlayerCombinations::kMaxDivisionals + 1, "past the last");
    MP_CHECK(div.divisionalName(7, 10) == "Cornet 3 rangs", "a divisional's name is kept: [" + div.divisionalName(7, 10) + "]");
    MP_CHECK(div.divisionalName(8, 10).empty(), "the same number on another division has none");
    MP_CHECK(div.divisionalName(9, 1).empty(), "no division, no name");
    MP_CHECK(div.anyDivisionalNamed(), "now one is named");
    const std::string divText = div.toText();
    MP_CHECK(divText.find("divisionalname 7 10 Cornet 3 rangs\n") != std::string::npos, "saved as a line:\n" + divText);
    mp::PlayerCombinations divBack;
    divBack.reset({stop}, divisions);
    MP_CHECK(divBack.fromText(divText), "and read back");
    MP_CHECK(divBack.divisionalName(7, 10) == "Cornet 3 rangs", "with its name");

    // Emptied: unused, no name, unlit.
    MP_CHECK(divBack.storeDivisional(7, 2, [](const mp::PlayerCombinations::Element&) { return true; }),
             "the Positif's 2 stores");
    divBack.setDivisionalName(7, 2, "Fonds 8' 4'");
    divBack.clearDivisional(7, 2);
    MP_CHECK(!divBack.divisionalSet(7, 2) && divBack.divisionalName(7, 2).empty(), "emptied, it is unused and unnamed");
    MP_CHECK(divBack.litDivisional(7) != 2, "and not lit");
    divBack.setDivisionalName(7, 10, "");
    MP_CHECK(!divBack.anyDivisionalNamed() && divBack.toText().find("divisionalname") == std::string::npos,
             "no names left, none written");
  }
};
static GeneralNamesTest g_generalNames;
