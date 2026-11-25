// A sampled tremulant in the player's combinations (#1235): Alessandria,
// Giubiasco and Melcer have no tremulant rows; their tremulant knobs are
// switches Hauptwerk knows by assignment code, 1710 for the first manual's,
// 1720 for the second's. A general stores them and a general cancel puts
// them in, as it does a tremulant with a row.
#include "MpTest.h"

#include "../src/mp_control/PlayerCombinations.h"

#include <algorithm>
#include <string>

namespace {

using mp::test::Category;

class PlayerTremulantsTest final : public mp::test::Test {
public:
  PlayerTremulantsTest() : Test("functional.control.player-sampled-tremulants", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    for (int d : {1, 2}) {
      m.divisions[d].divisionId = d;
      m.divisions[d].manualNumber = d;
      m.divisions[d].name = d == 1 ? "Grand'Organo" : "Positivo";
    }
    auto addSwitch = [&](mp::Id id, const char* name, int code) {
      mp::Switch sw;
      sw.switchId = id;
      sw.name = name;
      sw.asgnCode = code;
      m.switches[id] = sw;
    };
    addSwitch(10, "Principale 8", 0);
    addSwitch(69, "60. Tremolo I", 1710);
    addSwitch(70, "61. Tremolo II", 1720);
    m.stops[100].stopId = 100;
    m.stops[100].name = "Principale 8";
    m.stops[100].divisionId = 1;
    m.stops[100].controllingSwitchId = 10;

    const auto elements = mp::PlayerCombinations::collect(m, nullptr);
    const auto find = [&](mp::Id sw) {
      return std::find_if(elements.begin(), elements.end(), [&](const auto& e) {
        return e.kind == mp::PlayerCombinations::ElementKind::Switch && e.id == sw;
      });
    };
    MP_CHECK(find(69) != elements.end() && find(70) != elements.end(),
             "both tremulant knobs are registration, with no tremulant row");
    if (find(69) == elements.end() || find(70) == elements.end()) return;
    MP_CHECK(find(69)->divisionId == 1 && find(70)->divisionId == 2,
             "1710 belongs to the first manual, 1720 to the second");

    mp::PlayerCombinations pc;
    pc.reset(elements, mp::PlayerCombinations::divisionsOf(m, elements));
    std::vector<mp::PlayerCombinations::Change> out;
    pc.generalCancel(out);
    const bool cancelled = std::any_of(out.begin(), out.end(), [](const auto& c) {
      return c.kind == mp::PlayerCombinations::ElementKind::Switch && c.id == 69 && !c.engage;
    });
    MP_CHECK(cancelled, "a general cancel puts Tremolo I in");
    const auto read = [](const mp::PlayerCombinations::Element& e) {
      return e.kind == mp::PlayerCombinations::ElementKind::Switch && e.id == 69;
    };
    MP_CHECK(pc.captureGeneral(1, read), "general 1 stores Tremolo I drawn");
    out.clear();
    pc.recallGeneral(1, out);
    MP_CHECK(std::any_of(out.begin(), out.end(),
                         [](const auto& c) { return c.id == 69 && c.engage; }),
             "and recalls it");
  }
};
static PlayerTremulantsTest g_playerTremulants;

// A stop on a switch no player can move is the organ's wiring, not its
// registration (#1218): Alessandria's "GO Tuba 8'" stands on the hidden
// "NoiseSwitch", engaged at load, and the first general cancel pushed it in.
class PlayerHiddenStopTest final : public mp::test::Test {
public:
  PlayerHiddenStopTest() : Test("functional.control.player-hidden-stop", Category::Functional) {}
  void run() override {
    mp::OrganModel m;
    m.divisions[5].divisionId = 5;
    m.divisions[5].name = "Noises";
    m.divisions[1].divisionId = 1;
    m.divisions[1].manualNumber = 1;
    mp::Switch knob;
    knob.switchId = 10;
    knob.name = "Principale 8";
    m.switches[10] = knob;
    mp::Switch hidden;
    hidden.switchId = 10001;
    hidden.name = "NoiseSwitch";
    hidden.defaultEngaged = true;
    hidden.clickable = false;
    hidden.accessibleForInput = false;
    m.switches[10001] = hidden;
    m.stops[100].stopId = 100;
    m.stops[100].name = "Principale 8";
    m.stops[100].divisionId = 1;
    m.stops[100].controllingSwitchId = 10;
    m.stops[25].stopId = 25;
    m.stops[25].name = "GO Tuba 8'";
    m.stops[25].divisionId = 5;
    m.stops[25].controllingSwitchId = 10001;

    const auto elements = mp::PlayerCombinations::collect(m, nullptr);
    const auto has = [&](mp::Id stop) {
      return std::any_of(elements.begin(), elements.end(), [&](const auto& e) {
        return e.kind == mp::PlayerCombinations::ElementKind::Stop && e.id == stop;
      });
    };
    MP_CHECK(has(100), "the drawn stop is registration");
    MP_CHECK(!has(25), "the stop on a hidden switch is not");
    mp::PlayerCombinations pc;
    pc.reset(elements, mp::PlayerCombinations::divisionsOf(m, elements));
    std::vector<mp::PlayerCombinations::Change> out;
    pc.generalCancel(out);
    MP_CHECK(std::none_of(out.begin(), out.end(), [](const auto& c) { return c.id == 25; }),
             "a general cancel leaves the hidden stop alone");
  }
};
static PlayerHiddenStopTest g_playerHiddenStop;

}  // namespace
