// Ready-made registrations from the stop names (#1221): PP to Tutti, each
// holding the one before, built from what the names say.
#include "MpTest.h"

#include "../src/mp_control/PlayerCombinations.h"

#include <string>

namespace {

using mp::test::Category;
using E = mp::PlayerCombinations::Element;
using Kind = mp::PlayerCombinations::ElementKind;

class ReadyMadeTest final : public mp::test::Test {
public:
  ReadyMadeTest() : Test("functional.control.ready-made-registrations", Category::Functional) {}
  void run() override {
    std::vector<E> els;
    mp::Id next = 1;
    auto stop = [&](mp::Id div, const char* name) {
      E e;
      e.kind = Kind::Stop;
      e.id = next++;
      e.divisionId = div;
      e.name = name;
      els.push_back(e);
    };
    auto sw = [&](const char* name) {
      E e;
      e.kind = Kind::Switch;
      e.id = 100 + next++;
      e.name = name;
      els.push_back(e);
    };
    // Hauptwerk (1), Schwellwerk (2), Pedal (3).
    for (const char* n : {"Principal 8", "Rohrflöte 8", "Octave 4", "Spitzflöte 4", "Superoctave 2", "Mixtur IV",
                          "Trompete 8"})
      stop(1, n);
    for (const char* n : {"Gedackt 8", "Salicional 8", "Vox coelestis 8", "Principal 4", "Nasard 2 2/3", "Oboe 8"})
      stop(2, n);
    for (const char* n : {"Subbass 16", "Octavbass 8", "Posaune 16"}) stop(3, n);
    sw("II/I");
    sw("Tremulant");
    const auto keyboardOf = [](mp::Id div) { return div == 3 ? 0 : static_cast<int>(div); };
    const auto isTrem = [&](mp::Id id) {
      for (const auto& e : els)
        if (e.id == id) return e.name == "Tremulant";
      return false;
    };
    const auto levels = mp::PlayerCombinations::suggestRegistrations(els, keyboardOf, isTrem);
    MP_CHECK(levels.size() == 6, "six levels, got " + std::to_string(levels.size()));
    if (levels.size() != 6) return;
    const auto drawn = [&](size_t level) {
      std::string s;
      for (size_t i = 0; i < els.size(); ++i)
        if (levels[level].drawn[i]) s += els[i].name + "|";
      return s;
    };
    MP_CHECK(levels[0].name == "PP" && levels[5].name == "Tutti", "named PP to Tutti");
    MP_CHECK(drawn(0) == "Gedackt 8|Subbass 16|", "PP: the Swell's Gedackt, the Subbass; got " + drawn(0));
    MP_CHECK(drawn(1) == "Rohrflöte 8|Gedackt 8|Subbass 16|", "P: the Great's Rohrflöte too; got " + drawn(1));
    MP_CHECK(drawn(2) == "Principal 8|Rohrflöte 8|Spitzflöte 4|Gedackt 8|Principal 4|Subbass 16|Octavbass 8|",
             "MF: the Great's principal and a 4' flute, a 4' on the Swell, the pedal's 8'; got " + drawn(2));
    MP_CHECK(drawn(3) == "Principal 8|Rohrflöte 8|Octave 4|Spitzflöte 4|Superoctave 2|Gedackt 8|Principal 4|Subbass 16|"
                         "Octavbass 8|",
             "F: the principal chorus; got " + drawn(3));
    MP_CHECK(drawn(4).find("Mixtur IV") != std::string::npos && drawn(4).find("Nasard") != std::string::npos &&
                 drawn(4).find("Trompete") == std::string::npos,
             "Plein jeu: the mixture and the mutation, no reed; got " + drawn(4));
    MP_CHECK(drawn(5).find("Trompete 8") != std::string::npos && drawn(5).find("Posaune 16") != std::string::npos &&
                 drawn(5).find("Vox coelestis") == std::string::npos && drawn(5).find("II/I") != std::string::npos &&
                 drawn(5).find("Tremulant") == std::string::npos,
             "Tutti: the reeds and the coupler, no celeste and no tremulant; got " + drawn(5));
    MP_CHECK(mp::PlayerCombinations::suggestRegistrations({}, keyboardOf, isTrem).empty(),
             "an organ with no stops gets none");
  }
};
static ReadyMadeTest g_readyMade;

}  // namespace
