// The organ's wind and tremulants as the Diagnostics report gives them: each
// compartment with what its pipes draw, each linkage with what works its
// valve, each tremulant with the pipes it reaches, and each rank a switch
// swaps in, by id and name.
#include "MpTest.h"
#include "../src/mp_core/OrganStructure.h"

#include <cstdio>
#include <string>
#include <utility>

namespace {

using mp::test::Category;

mp::OrganModel reportOrgan() {
  mp::OrganModel m;
  mp::WindCompartment blower;
  blower.compartmentId = 1;
  blower.name = "Blower";
  blower.infiniteVolume = true;
  blower.defaultPressureInches = 4.0;
  m.wind[1] = blower;
  mp::WindCompartment reservoir;
  reservoir.compartmentId = 2;
  reservoir.name = "Reservoir";
  reservoir.infiniteVolume = false;
  reservoir.volumeM3 = 0.5;
  reservoir.defaultPressureInches = 3.0;
  reservoir.hasBellows = true;
  reservoir.bellowsMassKg = 80.0;
  reservoir.bellowsDamping = 12.0;
  reservoir.bellowsWidthM = 1.2;
  reservoir.bellowsLengthM = 2.0;
  reservoir.bellowsExtensionM = 0.3;
  reservoir.bellowsExtensionOutputControlId = 7;
  m.wind[2] = reservoir;

  mp::Switch motor;
  motor.switchId = 43;
  motor.name = "Blower motor";
  m.switches[43] = motor;
  mp::Switch tremSwitch;
  tremSwitch.switchId = 44;
  tremSwitch.name = "Tremulant";
  tremSwitch.isTremulant = true;
  m.switches[44] = tremSwitch;
  mp::ContinuousControl height;
  height.controlId = 7;
  height.name = "Reservoir height";
  m.continuousControls[7] = height;

  mp::WindCompartmentLink feed;
  feed.firstCompartmentId = 1;
  feed.secondCompartmentId = 2;
  feed.name = "Feed";
  feed.valveSwitchId = 43;
  feed.massFlowKgPerSec = 0.25;
  feed.flowRandom.on = true;
  feed.flowRandom.accel = 1.0;
  feed.flowRandom.damping = 2.0;
  feed.flowRandom.maxPct = 1.5;
  m.windLinks.push_back(feed);
  mp::WindCompartmentLink regulated;
  regulated.firstCompartmentId = 1;
  regulated.secondCompartmentId = 2;
  regulated.valveControlId = 7;
  regulated.valveControlTypeCode = 2;
  m.windLinks.push_back(regulated);

  mp::Rank principal;
  principal.rankId = 10;
  principal.name = "Prinzipal 8";
  for (int i = 0; i < 3; ++i) {
    mp::Pipe pipe;
    pipe.pipeId = 100 + i;
    pipe.windSourceCompartmentId = 2;
    pipe.windMassFlowKgPerSec = 0.01;
    principal.pipes.push_back(pipe);
  }
  m.ranks[10] = principal;
  mp::Rank tremmed;
  tremmed.rankId = 11;
  tremmed.name = "Prinzipal 8 trem";
  tremmed.pipes.push_back(mp::Pipe{});
  m.ranks[11] = tremmed;

  mp::Tremulant trem;
  trem.tremulantId = 3;
  trem.name = "Tremulant";
  trem.controllingSwitchId = 44;
  trem.engagedHz = 6.0;
  trem.disengagedHz = 5.0;
  trem.depthPercent = 12.0;
  m.tremulants[3] = trem;
  mp::TremulantPipeMod mod;
  mod.tremulantId = 3;
  mod.ampDepthDb = -6.0;
  mod.pitchDepthPct = 80.0;
  m.tremulantPipes[100] = mod;
  mod.ampDepthDb = -2.0;
  m.tremulantPipes[101] = mod;

  mp::Stop stop;
  stop.stopId = 5;
  stop.name = "Prinzipal 8'";
  mp::StopRankEntry entry;
  entry.rankId = 10;
  entry.alternateRankId = 11;
  entry.alternateSwitchId = 44;
  stop.ranks.push_back(entry);
  m.stops[5] = stop;
  return m;
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

class OrganStructureReportTest final : public mp::test::Test {
public:
  OrganStructureReportTest() : Test("functional.diagnostics.organ-structure-report", Category::Functional) {}
  void run() override {
    const std::string text = mp::windAndTremulantReport(reportOrgan());
    std::printf("%s", text.c_str());
    MP_CHECK(has(text, "Wind: 2 compartments, 2 linkages; 3 of 4 pipes draw wind"), "the wind's heading");
    MP_CHECK(has(text, "compartment 1 \"Blower\": fixed pressure 4.00 in"), "the blower");
    MP_CHECK(has(text, "compartment 2 \"Reservoir\": 0.500 m3, 3.00 in; bellows 80.0 kg"), "the reservoir");
    MP_CHECK(has(text, "extension to control 7 \"Reservoir height\""), "the board's control");
    MP_CHECK(has(text, "3 pipes draw 0.0300 kg/s"), "what the pipes draw");
    MP_CHECK(has(text, "linkage 1, compartment 1 to 2 \"Feed\": valve switch 43"), "the switched linkage");
    MP_CHECK(has(text, "(open when engaged); 0.2500 kg/s at 1.00 in; drift 1.50 %"), "its flow and drift");
    MP_CHECK(has(text, "linkage 2, compartment 1 to 2: valve control 7 \"Reservoir height\", type 2"),
             "the regulated linkage");
    MP_CHECK(has(text, "tremulant 3 \"Tremulant\": switch 44"), "the tremulant and its switch");
    MP_CHECK(has(text, "6.00 Hz drawn, 5.00 Hz off; depth 12.0 %"), "its rate and depth");
    MP_CHECK(has(text, "2 pipes, -6.0 to -2.0 dB, pitch 80 to 80 %"), "the pipes it reaches");
    MP_CHECK(has(text, "Alternate ranks: 1"), "the alternates' heading");
    MP_CHECK(has(text, "rank 11 \"Prinzipal 8 trem\" for rank 10 \"Prinzipal 8\" in stop 5 \"Prinzipal 8'\", by switch 44"),
             "the tremmed rank and its switch");

    const std::string solved = mp::windAndTremulantReport(
        reportOrgan(), [](mp::Id id) { return std::make_pair(id == 2 ? 3.0 : 4.0, id == 2 ? 2.875 : 4.0); });
    MP_CHECK(has(solved, "; working 3.000 in, now 2.875 in"), "the reservoir's pressure from the solver");

    const std::string cut = mp::windAndTremulantReport(reportOrgan(), {}, 1);
    MP_CHECK(has(cut, "  and 1 more\n"), "a long section is cut with the number left out");
  }
};
static OrganStructureReportTest g_organStructureReport;

}  // namespace
