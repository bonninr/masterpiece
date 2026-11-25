// The organ network behind OrganNetwork: the event network's reading of
// Hauptwerk's object model, the legacy network wrapped unchanged, the trace
// simulator, and the A/B harness that plays one script through both and
// reports where they part.
//
// The A/B tests do not fail on a difference: the event network is not at
// parity yet, and the differences are the report. Set MP_AB_REPORT to a file
// to get them written out as well as printed.
#include "MpTest.h"
#include "../src/mp_control/EventNetwork.h"
#include "../src/mp_control/LegacyNetwork.h"
#include "../src/mp_control/TraceSimulator.h"
#include "../src/mp_core/OdfLoader.h"

#ifdef MP_TEST_HAS_AUDIO
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/ProcessorTrace.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

using mp::test::Category;
using Kind = mp::OrganNetwork::Kind;

// ---- definitions, written as Hauptwerk writes them ----

std::string list(const char* type, const std::string& rows) {
  return std::string("<ObjectList ObjectType=\"") + type + "\">" + rows + "</ObjectList>";
}

std::string organXml(const std::string& name, const std::string& tables) {
  return "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
         "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>" +
         name + "</Identification_Name><Identification_UniqueOrganID>990</Identification_UniqueOrganID>"
                "</_General></ObjectList>" +
         tables + "</Hauptwerk>";
}

std::string sw(int id, const std::string& name, bool latching = true, bool engaged = false) {
  return "<Switch><SwitchID>" + std::to_string(id) + "</SwitchID><Name>" + name + "</Name><Latching>" +
         (latching ? "Y" : "N") + "</Latching><DefaultToEngaged>" + (engaged ? "Y" : "N") +
         "</DefaultToEngaged></Switch>";
}

std::string link(int from, int to, int engage = 1, int disengage = 2, int condition = 0, bool condEngaged = true,
                 bool sourceEngaged = true) {
  return "<SwitchLinkage><SourceSwitchID>" + std::to_string(from) + "</SourceSwitchID><DestSwitchID>" +
         std::to_string(to) + "</DestSwitchID>" +
         (condition != 0 ? "<ConditionSwitchID>" + std::to_string(condition) +
                               "</ConditionSwitchID><ConditionSwitchLinkIfEngaged>" + (condEngaged ? "Y" : "N") +
                               "</ConditionSwitchLinkIfEngaged>"
                         : std::string()) +
         "<SourceSwitchLinkIfEngaged>" + (sourceEngaged ? "Y" : "N") +
         "</SourceSwitchLinkIfEngaged><EngageLinkActionCode>" + std::to_string(engage) +
         "</EngageLinkActionCode><DisengageLinkActionCode>" + std::to_string(disengage) +
         "</DisengageLinkActionCode></SwitchLinkage>";
}

std::string control(int id, const std::string& name, int value = 0) {
  return "<ContinuousControl><ControlID>" + std::to_string(id) + "</ControlID><Name>" + name +
         "</Name><DefaultValue>" + std::to_string(value) + "</DefaultValue></ContinuousControl>";
}

std::string controlLink(int from, int to, int type = 1, double ms = 0, double accel = 0, double damping = 0,
                        int condition = 0) {
  return "<ContinuousControlLinkage><SourceControlID>" + std::to_string(from) + "</SourceControlID><DestControlID>" +
         std::to_string(to) + "</DestControlID>" +
         (condition != 0 ? "<ConditionSwitchID>" + std::to_string(condition) +
                               "</ConditionSwitchID><ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged>"
                         : std::string()) +
         "<LinkTypeCode>" + std::to_string(type) +
         "</LinkTypeCode><WillisTypeIncSpeedInMillisecondsPerStepWithOneStepDiff>" + std::to_string(ms) +
         "</WillisTypeIncSpeedInMillisecondsPerStepWithOneStepDiff><InertiaModelTypePositiveAcceleratingCoeff>" +
         std::to_string(accel) + "</InertiaModelTypePositiveAcceleratingCoeff><InertiaModelTypePositiveDampingCoeff>" +
         std::to_string(damping) + "</InertiaModelTypePositiveDampingCoeff></ContinuousControlLinkage>";
}

std::string stage(int controlId, int value, int switchId, bool engUp, bool engDown, bool disUp, bool disDown) {
  auto yn = [](bool b) { return b ? "Y" : "N"; };
  return "<ContinuousControlStageSwitch><ContinuousControlID>" + std::to_string(controlId) +
         "</ContinuousControlID><ContinuousControlValue>" + std::to_string(value) +
         "</ContinuousControlValue><ControlledSwitchID>" + std::to_string(switchId) +
         "</ControlledSwitchID><EngageWhenValueIncreasing>" + yn(engUp) +
         "</EngageWhenValueIncreasing><EngageWhenValueDecreasing>" + yn(engDown) +
         "</EngageWhenValueDecreasing><DisengageWhenValueIncreasing>" + yn(disUp) +
         "</DisengageWhenValueIncreasing><DisengageWhenValueDecreasing>" + yn(disDown) +
         "</DisengageWhenValueDecreasing></ContinuousControlStageSwitch>";
}

std::string rank(int id, const std::string& name) {
  return "<Rank><RankID>" + std::to_string(id) + "</RankID><Name>" + name + "</Name></Rank>";
}

std::string pipe(int id, int rankId, int note, int pallet = 0) {
  return "<Pipe_SoundEngine01><PipeID>" + std::to_string(id) + "</PipeID><RankID>" + std::to_string(rankId) +
         "</RankID><NormalMIDINoteNumber>" + std::to_string(note) + "</NormalMIDINoteNumber>" +
         (pallet != 0 ? "<ControllingPalletSwitchID>" + std::to_string(pallet) + "</ControllingPalletSwitchID>"
                      : std::string()) +
         "</Pipe_SoundEngine01>";
}

std::string keyboard(int id, const std::string& name, int code = 0) {
  return "<Keyboard><KeyboardID>" + std::to_string(id) + "</KeyboardID><Name>" + name +
         "</Name><DefaultInputOutputKeyboardAsgnCode>" + std::to_string(code) +
         "</DefaultInputOutputKeyboardAsgnCode><KeyGen_NumberOfKeys>61</KeyGen_NumberOfKeys>"
         "<KeyGen_MIDINoteNumberOfFirstKey>36</KeyGen_MIDINoteNumberOfFirstKey></Keyboard>";
}

std::string division(int id, const std::string& name) {
  return "<Division><DivisionID>" + std::to_string(id) + "</DivisionID><Name>" + name + "</Name></Division>";
}

std::string keyAction(int fromKb, int to, bool toKeyboard, int condition = 0) {
  return "<KeyAction><SourceKeyboardID>" + std::to_string(fromKb) + "</SourceKeyboardID>" +
         (toKeyboard ? "<DestIsKeyboardNotDivision>Y</DestIsKeyboardNotDivision><DestKeyboardID>" +
                           std::to_string(to) + "</DestKeyboardID>"
                     : "<DestIsKeyboardNotDivision>N</DestIsKeyboardNotDivision><DestDivisionID>" +
                           std::to_string(to) + "</DestDivisionID>") +
         (condition != 0 ? "<ConditionSwitchID>" + std::to_string(condition) +
                               "</ConditionSwitchID><ConditionSwitchLinkIfEngaged>Y</ConditionSwitchLinkIfEngaged>"
                         : std::string()) +
         "<ActionTypeCode>1</ActionTypeCode><ActionEffectCode>1</ActionEffectCode>"
         "<MIDINoteNumOfFirstSourceKey>36</MIDINoteNumOfFirstSourceKey><NumberOfKeys>61</NumberOfKeys></KeyAction>";
}

std::string keyboardKey(int kb, int note, int switchId) {
  return "<KeyboardKey><KeyboardID>" + std::to_string(kb) + "</KeyboardID><SwitchID>" + std::to_string(switchId) +
         "</SwitchID><NormalMIDINoteNumber>" + std::to_string(note) + "</NormalMIDINoteNumber></KeyboardKey>";
}

std::string divisionInput(int div, int note, int switchId) {
  return "<DivisionInput><DivisionID>" + std::to_string(div) + "</DivisionID><SwitchID>" + std::to_string(switchId) +
         "</SwitchID><NormalMIDINoteNumber>" + std::to_string(note) + "</NormalMIDINoteNumber></DivisionInput>";
}

std::string stop(int id, const std::string& name, int div, int switchId) {
  return "<Stop><StopID>" + std::to_string(id) + "</StopID><Name>" + name + "</Name><DivisionID>" +
         std::to_string(div) + "</DivisionID><ControllingSwitchID>" + std::to_string(switchId) +
         "</ControllingSwitchID></Stop>";
}

std::string stopRank(int stopId, int rankId) {
  return "<StopRank><StopID>" + std::to_string(stopId) + "</StopID><RankID>" + std::to_string(rankId) +
         "</RankID><MIDINoteNumOfFirstMappedDivisionInputNode>36</MIDINoteNumOfFirstMappedDivisionInputNode>"
         "<NumberOfMappedDivisionInputNodes>61</NumberOfMappedDivisionInputNodes></StopRank>";
}

mp::OrganModel load(const std::string& xml) {
  mp::OrganModel m;
  mp::OdfDiagnostics d;
  mp::OdfLoader l;
  mp::OdfLoader::Options o;
  MP_CHECK(l.loadFromXmlString(xml, "fixture.Organ_Hauptwerk_xml", o, m, d), "the fixture loads");
  return m;
}

// Records what a network reports.
struct Pipes final : mp::NetworkListener {
  std::vector<std::string> moved;
  void pipeMoved(mp::Id rankId, mp::Id pipeId, bool on, double, const mp::Pipe*) override {
    moved.push_back(std::to_string(rankId) + ":" + std::to_string(pipeId) + (on ? " on" : " off"));
  }
};

void loadNetwork(mp::OrganNetwork& n, const mp::OrganModel& m) {
  n.resetSwitches(m);
  n.resetControls(m, nullptr);
  n.propagate(0, &n.engagedSwitches());
  n.setTimed(true);
}

// ---- the event network's semantics ----

class EventSwitchActionsTest final : public mp::test::Test {
public:
  EventSwitchActionsTest() : Test("functional.network.event-switch-actions", Category::Functional) {}
  void run() override {
    const auto m = load(organXml(
        "switch actions",
        list("Switch", sw(1, "follow src") + sw(2, "follow dst") + sw(3, "reversible", false) + sw(4, "reversed") +
                           sw(5, "inverting src") + sw(6, "inverting dst") + sw(7, "gated src") + sw(8, "gated dst") +
                           sw(9, "gate")) +
            list("SwitchLinkage", link(1, 2) + link(3, 4, 3, 7) + link(5, 6, 7, 4) + link(7, 8, 1, 2, 9))));
    mp::EventNetwork n;
    loadNetwork(n, m);
    MP_CHECK(n.linkageCount() == 4, "four wires");

    n.set(1, true);
    MP_CHECK(n.engaged(2), "1/2: the destination follows the source in");
    MP_CHECK(n.lastChanges().size() == 2, "and the change lists both switches");
    n.set(1, false);
    MP_CHECK(!n.engaged(2), "1/2: and out");

    n.set(3, true);
    n.set(3, false);
    MP_CHECK(n.engaged(4), "3/7: a press engages, the release leaves it");
    n.set(3, true);
    n.set(3, false);
    MP_CHECK(!n.engaged(4), "3/7: the next press disengages");

    MP_CHECK(n.engaged(6), "7/4: an inverting wire holds its destination on at load");
    n.set(5, true);
    MP_CHECK(!n.engaged(6), "7/4: the source in pushes the destination out");
    n.set(5, false);
    MP_CHECK(n.engaged(6), "7/4: and back");

    n.set(7, true);
    MP_CHECK(!n.engaged(8), "gated: nothing through a closed gate");
    n.set(9, true);
    MP_CHECK(n.engaged(8), "gated: opening the gate fires the wire");
    n.set(9, false);
    MP_CHECK(!n.engaged(8), "gated: closing it stops the wire, which disengages");
  }
};
EventSwitchActionsTest g_eventSwitchActions;

class EventTimedLinkagesTest final : public mp::test::Test {
public:
  EventTimedLinkagesTest() : Test("functional.network.event-timed-linkages", Category::Functional) {}
  void run() override {
    const auto m = load(organXml(
        "timed linkages",
        list("Switch", sw(10, "gate")) +
            list("ContinuousControl",
                 control(1, "source") + control(2, "willis") + control(3, "inertia") + control(4, "gated")) +
            list("ContinuousControlLinkage", controlLink(1, 2, 2, 10.0) + controlLink(1, 3, 3, 0, 44.7, 4.6) +
                                                 controlLink(1, 4, 1, 0, 0, 0, 10))));
    mp::EventNetwork n;
    loadNetwork(n, m);
    n.setValue(1, 5);
    MP_CHECK(n.value(2) == 0, "Willis: nothing moves at once");
    MP_CHECK(n.moving(), "Willis: on its way");
    n.advance(0.005);
    MP_CHECK(n.value(2) == 2, "Willis: faster with more to go, two steps in 5 ms (2 + 2.5 ms): " + std::to_string(n.value(2)));
    n.advance(0.020);
    MP_CHECK(n.value(2) == 5, "Willis: the last step takes the full 10 ms, arrived by 25 ms: " + std::to_string(n.value(2)));
    for (int i = 0; i < 500; ++i) n.advance(0.01);
    MP_CHECK(n.value(3) == 5, "inertia: settled on the source in 5 s: " + std::to_string(n.value(3)));
    MP_CHECK(!n.moving(), "and nothing is moving");

    MP_CHECK(n.value(4) == 0, "gated: a linkage behind a closed gate does nothing");
    n.set(10, true);
    MP_CHECK(n.value(4) == 5, "gated: opening it carries the source across");
  }
};
EventTimedLinkagesTest g_eventTimedLinkages;

class EventStageSwitchesTest final : public mp::test::Test {
public:
  EventStageSwitchesTest() : Test("functional.network.event-stage-switches", Category::Functional) {}
  void run() override {
    const auto m = load(organXml("stage switches", list("Switch", sw(20, "blower")) +
                                                       list("ContinuousControl", control(5, "start")) +
                                                       list("ContinuousControlStageSwitch",
                                                            stage(5, 64, 20, true, false, false, true))));
    mp::EventNetwork n;
    n.setDrivesStages(true);
    loadNetwork(n, m);
    n.setValue(5, 100);
    MP_CHECK(n.engaged(20), "crossing 64 upwards engages");
    n.setValue(5, 70);
    MP_CHECK(n.engaged(20), "staying above leaves it");
    n.setValue(5, 10);
    MP_CHECK(!n.engaged(20), "crossing downwards disengages");
    n.setDrivesStages(false);
    n.setValue(5, 127);
    MP_CHECK(!n.engaged(20), "with the host driving stages, the network leaves them");
  }
};
EventStageSwitchesTest g_eventStageSwitches;

// Keyboard 1 plays division 1 and, through coupler 30, keyboard 2, which plays
// division 2. Keyboard 2's C4 has key switch 40, wired to pallet 41 of a pipe
// in rank 3.
std::string keysOrgan() {
  return organXml(
      "keys",
      list("Keyboard", keyboard(1, "Great", 2) + keyboard(2, "Swell", 3)) +
          list("Division", division(1, "Great") + division(2, "Swell")) +
          list("KeyAction", keyAction(1, 1, false) + keyAction(1, 2, true, 30) + keyAction(2, 2, false)) +
          list("KeyboardKey", keyboardKey(2, 60, 40)) +
          list("Switch", sw(30, "Sw/Gt") + sw(31, "Open Diapason") + sw(32, "Gamba") + sw(40, "Sw key 60", false) +
                             sw(41, "pallet", false)) +
          list("SwitchLinkage", link(40, 41)) + list("Rank", rank(1, "Diapason") + rank(2, "Gamba") + rank(3, "Pallet")) +
          list("Pipe_SoundEngine01", pipe(101, 1, 60) + pipe(201, 2, 60) + pipe(301, 3, 60, 41)) +
          list("Stop", stop(11, "Open Diapason", 1, 31) + stop(12, "Gamba", 2, 32)) +
          list("StopRank", stopRank(11, 1) + stopRank(12, 2)));
}

class EventKeysToPipesTest final : public mp::test::Test {
public:
  EventKeysToPipesTest() : Test("functional.network.event-keys-to-pipes", Category::Functional) {}
  void run() override {
    const auto m = load(keysOrgan());
    mp::EventNetwork n;
    Pipes p;
    n.setListener(&p);
    loadNetwork(n, m);
    n.keyDown(1, 60, 100);
    MP_CHECK(n.soundingPipes() == 0, "nothing drawn, nothing sounds");
    n.set(31, true);
    MP_CHECK(n.soundingPipes() == 1 && p.moved.back() == "1:101 on", "drawing the Diapason under a held key sounds it");
    n.set(32, true);
    MP_CHECK(n.soundingPipes() == 1, "the Gamba is on the Swell, not coupled yet");
    n.set(30, true);
    MP_CHECK(n.engaged(40), "the coupler puts the Swell's key down, and its key switch with it");
    MP_CHECK(n.soundingPipes() == 3, "Diapason, coupled Gamba and the pallet pipe: " +
                                         std::to_string(n.soundingPipes()));
    n.keyUp(1, 60);
    MP_CHECK(n.soundingPipes() == 0 && !n.engaged(40), "letting go silences everything");
    n.keyDown(2, 60, 100);
    MP_CHECK(n.soundingPipes() == 2, "the Swell's own key: Gamba and pallet");
    n.keyUp(2, 60);
  }
};
EventKeysToPipesTest g_eventKeysToPipes;

class EventDivisionInputsTest final : public mp::test::Test {
public:
  EventDivisionInputsTest() : Test("functional.network.event-division-inputs", Category::Functional) {}
  void run() override {
    const auto m = load(organXml(
        "inputs", list("Keyboard", keyboard(2, "GO", 2)) + list("Division", division(3, "GO")) +
                      list("KeyboardKey", keyboardKey(2, 60, 5060)) + list("DivisionInput", divisionInput(3, 60, 33060)) +
                      list("Switch", sw(1302, "Montre") + sw(5060, "key", false) + sw(33060, "input", false)) +
                      list("SwitchLinkage", link(5060, 33060)) + list("Rank", rank(201, "Montre")) +
                      list("Pipe_SoundEngine01", pipe(301, 201, 60)) + list("Stop", stop(901, "Montre", 3, 1302)) +
                      list("StopRank", stopRank(901, 201))));
    mp::EventNetwork n;
    loadNetwork(n, m);
    n.set(1302, true);
    n.keyDown(2, 60, 100);
    MP_CHECK(n.engaged(5060) && n.engaged(33060), "the key switch closes the division input");
    MP_CHECK(n.soundingPipes() == 1, "and the input plays the division's note");
    n.keyUp(2, 60);
    MP_CHECK(n.soundingPipes() == 0 && !n.engaged(33060), "and lets go");
  }
};
EventDivisionInputsTest g_eventDivisionInputs;

// ---- the legacy network, wrapped ----

class LegacyAdapterTest final : public mp::test::Test {
public:
  LegacyAdapterTest() : Test("functional.network.legacy-adapter", Category::Functional) {}
  void run() override {
    const auto m = load(organXml(
        "adapter", list("Switch", sw(1, "a") + sw(2, "b") + sw(3, "c", false) + sw(4, "d") + sw(5, "e", true, true) +
                                      sw(6, "f")) +
                       list("SwitchLinkage", link(1, 2) + link(3, 4, 3, 7) + link(5, 6, 7, 4))));
    mp::SwitchNetwork plain;
    plain.reset(m);
    mp::LegacyNetwork wrapped;
    wrapped.resetSwitches(m);
    const std::vector<std::pair<mp::Id, bool>> moves{{1, true}, {3, true}, {3, false}, {5, false},
                                                     {1, false}, {3, true}, {5, true}};
    for (const auto& [id, on] : moves) {
      plain.set(id, on);
      wrapped.set(id, on);
      MP_CHECK(plain.engagedSwitches() == wrapped.engagedSwitches(), "the adapter's switches are SwitchNetwork's");
      MP_CHECK(plain.lastChanges() == wrapped.lastChanges(), "and so are its changes");
    }
    MP_CHECK(mp::makeOrganNetwork(Kind::Legacy)->kind() == Kind::Legacy, "the factory makes a legacy network");
    MP_CHECK(mp::makeOrganNetwork(Kind::Event)->kind() == Kind::Event, "and an event network");
  }
};
LegacyAdapterTest g_legacyAdapter;

// ---- the trace simulator ----

class TraceScriptTest final : public mp::test::Test {
public:
  TraceScriptTest() : Test("functional.network.trace-script", Category::Functional) {}
  void run() override {
    mp::TraceScript s;
    std::string error;
    MP_CHECK(mp::TraceScript::parse("# a script\nkey 1 60 down\nswitch 30 on\nstop 11 off\npiston 7\n"
                                    "control 1 64\nwait 20\ntick 1\nkey 1 60 up 0\n",
                                    s, error),
             "a script reads: " + error);
    MP_CHECK(s.steps.size() == 8, "eight steps");
    MP_CHECK(!mp::TraceScript::parse("key 1 sixty down\n", s, error) && error.find("line 1") == 0,
             "a bad line is named: " + error);

    const auto m = load(keysOrgan());
    mp::TraceScript play;
    MP_CHECK(mp::TraceScript::parse("stop 11 on\nkey 1 60 down\nwait 10\nswitch 30 on\nkey 1 60 up\n", play, error),
             "the play script reads");
    const auto trace = mp::runTrace(m, Kind::Event, play);
    auto has = [&](const std::string& line) {
      for (const auto& l : trace)
        if (l.rfind(line, 0) == 0) return true;
      return false;
    };
    MP_CHECK(has("0.000 switch 31 on"), "the stop's switch, at 0 ms");
    MP_CHECK(has("0.000 pipe 1:101 on"), "its pipe, at 0 ms");
    MP_CHECK(has("10.667 switch 40 on"), "the coupled key switch after the wait, at two ticks");
    MP_CHECK(has("10.667 pipe 1:101 off"), "and the key let go");
  }
};
TraceScriptTest g_traceScript;

// ---- A/B: one script, both networks ----

struct Fixture {
  std::string name, xml, script;
};

std::vector<Fixture> fixtures() {
  std::vector<Fixture> f;
  // Caen (#1067): the key's switch closes the flue division's input, and the
  // reeds' input while the Anches is drawn (test_floating_division_inputs).
  f.push_back({"floating-division-inputs",
               organXml("Caen", list("Keyboard", keyboard(2, "Grand Orgue", 2)) +
                                    list("Division", division(3, "GO") + division(4, "GO Anches")) +
                                    list("KeyboardKey", keyboardKey(2, 60, 5060)) +
                                    list("DivisionInput", divisionInput(3, 60, 33060) + divisionInput(4, 60, 34060)) +
                                    list("Switch", sw(1302, "GO: Montre 8") + sw(1314, "GO: Trompette 8") +
                                                       sw(1368, "Cp: Anches GO", true, true) +
                                                       sw(5060, "GO key 60", false) + sw(33060, "GO DivInp 60", false) +
                                                       sw(34060, "GO Anches DivInp 60", false)) +
                                    list("SwitchLinkage", link(5060, 33060) + link(5060, 34060, 1, 2, 1368)) +
                                    list("Rank", rank(201, "Montre") + rank(202, "Trompette")) +
                                    list("Pipe_SoundEngine01", pipe(301, 201, 60) + pipe(302, 202, 60)) +
                                    list("Stop", stop(901, "GO: Montre 8", 3, 1302) + stop(902, "GO: Trompette 8", 4, 1314)) +
                                    list("StopRank", stopRank(901, 201) + stopRank(902, 202))),
               "stop 901 on\nstop 902 on\nkey 2 60 down\nwait 10\nswitch 1368 off\nwait 10\nswitch 1368 on\n"
               "key 2 60 up\nwait 10\n"});
  // Friesach (#967): keys straight into the division, key-action noises on
  // the division inputs as pallets (test_key_noise_inputs).
  f.push_back({"key-noise-inputs",
               organXml("Friesach", list("Keyboard", keyboard(701, "I", 1)) + list("Division", division(801, "G")) +
                                        list("KeyAction", keyAction(701, 801, false)) +
                                        list("DivisionInput", divisionInput(801, 60, 1260) +
                                                                  divisionInput(801, 61, 1261) +
                                                                  divisionInput(801, 62, 1262)) +
                                        list("Switch", sw(1260, "Keys Div:1 MIDI:60", false) +
                                                           sw(1261, "Keys Div:1 MIDI:61", false) +
                                                           sw(1262, "Keys Div:1 MIDI:62", false)) +
                                        list("Rank", rank(202, "Key Action")) +
                                        list("Pipe_SoundEngine01", pipe(302, 202, 60, 1260) + pipe(304, 202, 62, 1262))),
               "key 701 60 down\nwait 10\nkey 701 60 up\nkey 701 62 down\nwait 10\nkey 701 62 up\nwait 10\n"});
  // Alessandria-shaped pallets: the key switch opens one pallet wire per
  // tremulant state, each gated on the tremulant.
  f.push_back({"pallet-tremulant-pair",
               organXml("Pallets", list("Keyboard", keyboard(1, "Manual", 2)) +
                                       list("KeyboardKey", keyboardKey(1, 60, 100)) +
                                       list("Switch", sw(100, "key 60", false) + sw(200, "pallet 60", false) +
                                                          sw(300, "Tremulant") + sw(400, "Principale")) +
                                       list("SwitchLinkage", link(100, 200, 1, 2, 300, false) +
                                                                 link(100, 200, 1, 2, 300, true)) +
                                       list("Rank", rank(10, "Principale")) +
                                       list("Pipe_SoundEngine01", pipe(1000, 10, 60, 200))),
               "key 1 60 down\nwait 10\nswitch 300 on\nwait 10\nkey 1 60 up\nwait 10\nkey 1 60 down\nwait 10\n"
               "switch 300 off\nkey 1 60 up\nwait 10\n"});
  // A start-up ramp: the blower's switch reached through a Willis linkage
  // and a stage switch at the top, and a swell shoe with inertia.
  f.push_back({"timed-blower-and-swell",
               organXml("Blower", list("Switch", sw(1, "Blower", true, false) + sw(2, "Wind")) +
                                      list("ContinuousControl", control(10, "Blower switch value") +
                                                                    control(11, "Blower ramp") +
                                                                    control(20, "Swell pedal") +
                                                                    control(21, "Swell shutters")) +
                                      list("ContinuousControlLinkage",
                                           controlLink(10, 11, 2, 5.0) + controlLink(20, 21, 3, 0, 44.7, 4.6)) +
                                      list("ContinuousControlStageSwitch", stage(11, 127, 2, true, false, false, true))),
               "control 10 127\nwait 700\ncontrol 20 127\nwait 200\ncontrol 10 0\nwait 700\n"});
  // A general cancel built from wires (#1218 shape): the cancel pushes the
  // stops in, and the blower beside them must stay on.
  f.push_back({"wired-cancel",
               organXml("Cancel", list("Switch", sw(1, "GC", false) + sw(2, "Bourdon") + sw(3, "Flute") +
                                                     sw(4, "Blower", true, true) + sw(5, "Bourdon node")) +
                                      list("SwitchLinkage", link(1, 2, 2, 7) + link(1, 3, 2, 7) + link(2, 5) +
                                                                link(5, 2))),
               "switch 2 on\nswitch 3 on\npiston 1\nwait 10\n"});
  // Sherborne (#1228): a plain key action into the division; the division
  // note closes its input switch, which opens the pallet while the stop's
  // knob is drawn. The stop names no rank (test_input_only_division).
  f.push_back({"input-only-division",
               organXml("Sherborne", list("Keyboard", keyboard(2, "Great", 2)) + list("Division", division(3, "Great")) +
                                         list("KeyAction", keyAction(2, 3, false)) +
                                         list("DivisionInput", divisionInput(3, 60, 33060)) +
                                         list("Switch", sw(1302, "Gt. Open Diapason 8") + sw(33060, "Gt input 60", false) +
                                                            sw(7060, "Pallet 60", false)) +
                                         list("SwitchLinkage", link(33060, 7060, 1, 2, 1302)) +
                                         list("Rank", rank(201, "Open Diapason")) +
                                         list("Pipe_SoundEngine01", pipe(301, 201, 60, 7060)) +
                                         list("Stop", stop(901, "Gt. Open Diapason 8", 3, 1302))),
               "key 2 60 down\nwait 10\nkey 2 60 up\nwait 10\nswitch 1302 on\nkey 2 60 down\nwait 10\nkey 2 60 up\n"
               "wait 10\nswitch 1302 off\nkey 2 60 down\nwait 10\nkey 2 60 up\nwait 10\n"});
  return f;
}

class AbFixturesTest final : public mp::test::Test {
public:
  AbFixturesTest() : Test("functional.network.ab-fixtures", Category::Functional) {}
  void run() override {
    std::string report;
    for (const Fixture& f : fixtures()) {
      const auto m = load(f.xml);
      mp::TraceScript s;
      std::string error;
      MP_CHECK(mp::TraceScript::parse(f.script, s, error), f.name + ": " + error);
      const auto a = mp::runTrace(m, Kind::Legacy, s);
      const auto b = mp::runTrace(m, Kind::Event, s);
      MP_CHECK(!b.empty(), f.name + ": the event network writes a trace");
      const auto d = mp::diffTraces(a, b, {"switch", "control"});
      report += "== " + f.name + ": legacy " + std::to_string(a.size()) + " lines, event " +
                std::to_string(b.size()) + "; " + std::to_string(d.onlyA) + " only in legacy, " +
                std::to_string(d.onlyB) + " only in event\n";
      for (const auto& l : d.lines) report += l + "\n";
    }
    std::fputs(report.c_str(), stdout);
    if (const char* path = std::getenv("MP_AB_REPORT")) std::ofstream(path) << report;
  }
};
AbFixturesTest g_abFixtures;

#ifdef MP_TEST_HAS_AUDIO
void setNetworkEnv(bool on) {
#ifdef _WIN32
  _putenv_s("MP_EVENT_NETWORK", on ? "1" : "");
#else
  if (on) setenv("MP_EVENT_NETWORK", "1", 1);
  else unsetenv("MP_EVENT_NETWORK");
#endif
}

// The processor with the flag on: the organ plays through the event network,
// and the flag off leaves it on the legacy one.
class ProcessorEventNetworkTest final : public mp::test::Test {
public:
  ProcessorEventNetworkTest() : Test("functional.network.processor-switch", Category::Functional) {}
  void run() override {
    const juce::File root =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-event-network");
    root.deleteRecursively();
    const auto defs = root.getChildFile("OrganDefinitions");
    defs.createDirectory();
    const auto odf = defs.getChildFile("network.Organ_Hauptwerk_xml");
    odf.replaceWithText(fixtures().front().xml);
    for (const bool on : {false, true}) {
      setNetworkEnv(on);
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(odf);
      proc.setRateAndBufferSizeDetails(48000.0, 256);
      proc.prepareToPlay(48000.0, 256);
      MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
      MP_CHECK(proc.networkKind() == (on ? Kind::Event : Kind::Legacy),
               std::string("the network follows MP_EVENT_NETWORK=") + (on ? "1" : "unset"));
      proc.setSwitchEngaged(1368, false);
      MP_CHECK(!proc.switchEngaged(1368), "a switch moves through it");
      proc.setSwitchEngaged(5060, true);
      MP_CHECK(proc.switchEngaged(33060), "and its wiring follows");
      proc.setSwitchEngaged(5060, false);
      settings.deleteFile();
    }
    setNetworkEnv(false);
    root.deleteRecursively();
  }
};
ProcessorEventNetworkTest g_processorEventNetwork;

// Each pipe of a fixture given a layer and an attack on one stand-in sample,
// so the processor starts voices for it.
std::string withSamples(const std::string& xml) {
  std::string layers, attacks;
  const std::string tag = "<Pipe_SoundEngine01><PipeID>";
  for (size_t at = xml.find(tag); at != std::string::npos; at = xml.find(tag, at + 1)) {
    const size_t from = at + tag.size();
    const std::string id = xml.substr(from, xml.find('<', from) - from);
    layers += "<Pipe_SoundEngine01_Layer><LayerID>" + id + "</LayerID><PipeID>" + id +
              "</PipeID></Pipe_SoundEngine01_Layer>";
    attacks += "<Pipe_SoundEngine01_AttackSample><UniqueID>" + id + "</UniqueID><LayerID>" + id +
               "</LayerID><SampleID>1</SampleID></Pipe_SoundEngine01_AttackSample>";
  }
  const std::string tables =
      "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
      "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
      "<ObjectList ObjectType=\"Sample\"><Sample><SampleID>1</SampleID><InstallationPackageID>1</InstallationPackageID>"
      "<SampleFilename>tone.wav</SampleFilename><Pitch_SpecificationMethodCode>1</Pitch_SpecificationMethodCode>"
      "<Pitch_ExactSamplePitch>261.63</Pitch_ExactSamplePitch></Sample></ObjectList>" +
      list("Pipe_SoundEngine01_Layer", layers) + list("Pipe_SoundEngine01_AttackSample", attacks);
  std::string out = xml;
  out.insert(out.rfind("</Hauptwerk>"), tables);
  return out;
}

juce::File writeFixture(const juce::File& root, const Fixture& f) {
  root.deleteRecursively();
  const auto defs = root.getChildFile("OrganDefinitions");
  const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
  defs.createDirectory();
  pkg.createDirectory();
  juce::WavAudioFormat fmt;
  std::unique_ptr<juce::FileOutputStream> os(pkg.getChildFile("tone.wav").createOutputStream());
  juce::StringPairArray meta;
  meta.set("NumSampleLoops", "1");
  meta.set("Loop0Start", "1000");
  meta.set("Loop0End", "47000");
  std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(os.release(), 48000.0, 1, 16, meta, 0));
  juce::AudioBuffer<float> tone(1, 48000);
  for (int i = 0; i < 48000; ++i)
    tone.setSample(0, i, static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 261.63 * i / 48000.0)));
  w->writeFromAudioSampleBuffer(tone, 0, 48000);
  w.reset();
  const auto odf = defs.getChildFile(f.name + ".Organ_Hauptwerk_xml");
  odf.replaceWithText(withSamples(f.xml));
  return odf;
}

// The A/B harness at the engine's level: every fixture's script through a
// whole processor on each network, the traces diffed: switches, controls and
// the pipes speaking. Two outcomes are required of the event network, the
// rest is reported.
class ProcessorAbTest final : public mp::test::Test {
public:
  ProcessorAbTest() : Test("functional.network.ab-processor", Category::Functional) {}
  void run() override {
    std::string report;
    const juce::File base = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-ab-processor");
    for (const Fixture& f : fixtures()) {
      const juce::File odf = writeFixture(base.getChildFile(f.name), f);
      mp::TraceScript s;
      std::string error;
      MP_CHECK(mp::TraceScript::parse(f.script, s, error), f.name + ": " + error);
      const auto a = mp::runProcessorTrace(odf, Kind::Legacy, s);
      const auto b = mp::runProcessorTrace(odf, Kind::Event, s);
      MP_CHECK(a.loaded && b.loaded, f.name + ": loads on both networks: " + a.error + b.error);
      const auto d = mp::diffTraces(a.lines, b.lines, {"switch", "control", "pipe"});
      report += "== processor " + f.name + ": legacy " + std::to_string(a.lines.size()) + " lines, event " +
                std::to_string(b.lines.size()) + "; " + std::to_string(d.onlyA) + " only in legacy, " +
                std::to_string(d.onlyB) + " only in event\n";
      for (const auto& l : d.lines) report += l + "\n";
      auto has = [](const std::vector<std::string>& t, const std::string& what) {
        for (const auto& l : t)
          if (l.find(what) != std::string::npos) return true;
        return false;
      };
      if (f.name == "input-only-division")
        MP_CHECK(has(b.lines, "pipe 201:301 on"), "Sherborne on the event network: the drawn stop's pipe speaks");
      if (f.name == "floating-division-inputs")
        MP_CHECK(has(b.lines, "pipe 202:302 on"), "Caen on the event network: the reeds speak with the Anches drawn");
    }
    base.deleteRecursively();
    std::fputs(report.c_str(), stdout);
    if (const char* path = std::getenv("MP_AB_REPORT")) std::ofstream(path, std::ios::app) << report;
  }
};
ProcessorAbTest g_processorAb;
#endif

}  // namespace
