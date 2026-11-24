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

}  // namespace
