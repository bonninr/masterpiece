// What a load reads from the player's choices (LoadPlan.h): perspectives,
// stops and single ranks left out, and the choices that give way when they
// would leave out the whole organ (#574).
#include "MpTest.h"

#include "../src/mp_core/LoadPlan.h"

#include <string>

namespace {

using mp::test::Category;

// Three stops recorded from the front and the rear, and a blower noise no
// stop plays: ranks 1 to 3 front, 4 to 6 rear, 7 the noise.
mp::OrganModel twoPerspectives(bool withNoise) {
  mp::OrganModel m;
  auto rank = [&m](mp::Id id, const char* name) {
    mp::Rank r;
    r.rankId = id;
    r.name = name;
    m.ranks[id] = r;
  };
  rank(1, "Principal 8 (front)");
  rank(2, "Flute 8 (front)");
  rank(3, "Octave 4 (front)");
  rank(4, "Principal 8 (rear)");
  rank(5, "Flute 8 (rear)");
  rank(6, "Octave 4 (rear)");
  if (withNoise) rank(7, "Blower");
  auto stop = [&m](mp::Id id, const char* name, mp::Id a, mp::Id b) {
    mp::Stop s;
    s.stopId = id;
    s.name = name;
    mp::StopRankEntry e;
    e.rankId = a;
    s.ranks.push_back(e);
    e.rankId = b;
    s.ranks.push_back(e);
    m.stops[id] = s;
  };
  stop(10, "Principal 8", 1, 4);
  stop(11, "Flute 8", 2, 5);
  stop(12, "Octave 4", 3, 6);
  return m;
}

std::string ids(const std::unordered_set<mp::Id>& s) {
  std::set<mp::Id> sorted(s.begin(), s.end());
  std::string out;
  for (mp::Id id : sorted) out += " " + std::to_string(id);
  return out;
}

class LoadPlanTest final : public mp::test::Test {
public:
  LoadPlanTest() : Test("functional.loader.load-plan", Category::Functional) {}
  void run() override {
    const mp::OrganModel m = twoPerspectives(true);
    MP_CHECK(mp::perspectivesOf(m).size() == 2, "the model has a front and a rear");

    auto p = mp::planLoad(m, {}, {}, {});
    MP_CHECK(p.onlyRanks.empty() && p.unloadedStops.empty(), "no choice reads every rank");

    p = mp::planLoad(m, {"rear"}, {}, {});
    MP_CHECK(ids(p.onlyRanks) == " 1 2 3 7" && p.perspectivesLeftOut.size() == 1,
             "the rear left out: the front and the noise read:" + ids(p.onlyRanks));

    p = mp::planLoad(m, {"rear", "front"}, {}, {});
    MP_CHECK(p.onlyRanks.empty() && p.perspectivesLeftOut.empty(), "both perspectives left out: both stay");

    p = mp::planLoad(m, {"rear"}, {}, {1});
    MP_CHECK(ids(p.onlyRanks) == " 2 3 7" && p.ranksLeftOut.size() == 1,
             "a single front rank left out too:" + ids(p.onlyRanks));
    MP_CHECK(p.unloadedStops.count(10) == 1 && p.unloadedStops.count(11) == 0,
             "and the stop with no rank left is dimmed, the others not");

    // Ashton's case: the rear left out, and then every rank with "Leave out
    // shown ranks" on an unfiltered list. The single ranks give way, and the
    // front loads as the perspectives say.
    p = mp::planLoad(m, {"rear"}, {}, {1, 2, 3, 4, 5, 6, 7});
    MP_CHECK(ids(p.onlyRanks) == " 1 2 3 7" && p.ranksLeftOut.empty() && p.perspectivesLeftOut.size() == 1,
             "every rank left out: the single ranks load:" + ids(p.onlyRanks));
    MP_CHECK(p.unloadedStops.empty(), "and no stop is dimmed");

    // Covering every rank together, though no single choice does.
    const mp::OrganModel bare = twoPerspectives(false);
    p = mp::planLoad(bare, {"rear"}, {}, {1, 2, 3});
    MP_CHECK(ids(p.onlyRanks) == " 1 2 3" && p.ranksLeftOut.empty(),
             "the front ranks with the rear perspective: the front loads:" + ids(p.onlyRanks));

    p = mp::planLoad(m, {}, {10}, {});
    MP_CHECK(ids(p.onlyRanks) == " 2 3 5 6 7" && p.unloadedStops.count(10) == 1,
             "a stop left out:" + ids(p.onlyRanks));

    MP_CHECK(!p.readsNothing, "and the rest is read");

    p = mp::planLoad(bare, {"rear"}, {10, 11, 12}, {});
    MP_CHECK(p.readsNothing && p.onlyRanks.empty() && ids(p.unloadedStops) == " 10 11 12",
             "every stop left out on an organ of nothing else: they stay out, and no rank is read:" +
                 ids(p.unloadedStops));

    p = mp::planLoad(bare, {}, {10, 11, 12}, {1});
    MP_CHECK(p.readsNothing && p.ranksLeftOut.empty(),
             "and the single ranks give way first, as ever");

    p = mp::planLoad(m, {}, {}, {99});
    MP_CHECK(p.onlyRanks.empty() && p.ranksLeftOut.empty(), "a rank this organ lacks counts for nothing");
  }
};
static LoadPlanTest g_loadPlan;

}  // namespace
