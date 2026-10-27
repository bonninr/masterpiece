// The control API (protocol/control.yaml): the model on a small organ, then
// the same over HTTP, with the event stream and the permissions.
#ifdef MP_TEST_HAS_API
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <httplib.h>

#include "MpTest.h"
#include "../src/mp_api/ApiServer.h"
#include "../src/mp_api/ControlModel.h"
#include "../src/mp_audio/MasterpieceProcessor.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using mp::test::Category;
using namespace mp::api;

juce::var body(const httplib::Result& r) { return r ? juce::JSON::parse(juce::String(r->body)) : juce::var(); }

juce::File fixture() { return juce::File(juce::String(MP_TEST_FIXTURES_DIR) + "/m24.expression.Organ_Hauptwerk_xml"); }

int findSwitch(ControlModel& m, const char* name) {
  Error e;
  const juce::var list = m.list(Type::Switch, e);
  for (const auto& item : *list["items"].getArray())
    if (item["name"].toString() == name) return static_cast<int>(item["index"]);
  return -1;
}

class ControlModelTest final : public mp::test::Test {
public:
  ControlModelTest() : Test("functional.api.model", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    ControlModel m(proc);
    MP_CHECK(m.session() == 0 && m.count(Type::Switch) == 0, "before an organ, nothing to list");
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    MP_CHECK(m.session() == 1, "the load is a new session");

    MP_CHECK(m.count(Type::Switch) == 3, "the organ's three switches");
    const int trem = findSwitch(m, "Swell Tremulant");
    const int blower = findSwitch(m, "Blower");
    MP_CHECK(trem >= 0 && blower >= 0, "found by name");
    Error e;
    MP_CHECK(m.describe(Type::Switch, trem, e)["role"].toString() == "tremulant", "a tremulant's switch says so");
    MP_CHECK(m.describe(Type::Switch, blower, e)["role"].toString() == "other", "the blower is another switch");

    int v = 1;
    MP_CHECK(m.set(Type::Switch, trem, v) == Error::None && v == 1, "drawn");
    int read = 0;
    MP_CHECK(m.get(Type::Switch, trem, read, e) && read == 1, "and read back");
    v = 2;
    MP_CHECK(m.set(Type::Switch, trem, v) == Error::None && v == 0, "2 toggles it off again");
    v = 5;
    MP_CHECK(m.set(Type::Switch, trem, v) == Error::BadValue, "a switch has no 5");
    v = 1;
    MP_CHECK(m.set(Type::Switch, 99, v) == Error::OutOfRange, "no switch 99");

    MP_CHECK(m.count(Type::Control) > 0, "the controls are listed");
    v = 64;
    MP_CHECK(m.set(Type::Control, 0, v) == Error::None && v == 64, "a shoe moves");
    v = 200;
    MP_CHECK(m.set(Type::Control, 0, v) == Error::BadValue, "past 127 is refused");

    MP_CHECK(m.setProgram(ProgramProp::Transpose, 2) == Error::None && proc.transpose() == 2, "transposed by the program");
    MP_CHECK(m.setProgram(ProgramProp::Transpose, 20) == Error::BadValue, "within the range only");
    MP_CHECK(m.setProgram(ProgramProp::SoundingVoices, 1) == Error::ReadOnly, "a reading cannot be set");
    MP_CHECK(m.press(Type::Eqpreset, 2) == Error::None && proc.currentVoicingEqPreset() == 3,
             "the third preset, Chiff, is recalled");
    int eqPreset = -2;
    MP_CHECK(m.get(Type::Program, static_cast<int>(ProgramProp::EqPreset), eqPreset, e) && eqPreset == 2,
             "and the program says which");
    MP_CHECK(m.press(Type::Command, static_cast<int>(Command::Panic)) == Error::None, "panic");
    MP_CHECK(m.press(Type::Command, static_cast<int>(Command::PowerOff)) == Error::NotAllowed &&
                 m.press(Type::Command, static_cast<int>(Command::Quit)) == Error::NotAllowed,
             "quitting and shutting down are refused unless the player allowed them");
    MP_CHECK(static_cast<int>(m.list(Type::Switch, e, true)["listed"]) == 3 &&
                 static_cast<int>(m.list(Type::Switch, e)["listed"]) == 3,
             "an organ with no console pictures shows every switch either way");
    MP_CHECK(m.count(Type::Temperament) > 1, "the organ's own temperament and the library's");
    MP_CHECK(m.count(Type::Rank) == static_cast<int>(proc.organModel().ranks.size()), "every rank");
    if (m.count(Type::Rank) > 0) {
      v = -35;
      MP_CHECK(m.set(Type::Rank, 0, v) == Error::None && v == -35, "a rank's level set");
      const auto rankId = static_cast<mp::Id>(static_cast<juce::int64>(m.describe(Type::Rank, 0, e)["odfId"]));
      MP_CHECK(std::fabs(proc.voicing().live().rank(rankId).gainDb + 3.5f) < 0.01f, "and the voicing has it");
      v = 500;
      MP_CHECK(m.set(Type::Rank, 0, v) == Error::BadValue, "within -24 and +12 dB");
    }
    MP_CHECK(m.count(Type::Note) == m.count(Type::Keyboard) * 128, "every key of every keyboard");
    size_t pipes = 0;
    for (const auto& [id, rank] : proc.organModel().ranks) pipes += rank.pipes.size();
    MP_CHECK(m.count(Type::Pipe) == static_cast<int>(pipes), "every pipe of every rank");
    MP_CHECK(m.count(Type::Enclosure) == static_cast<int>(proc.organModel().enclosures.size()) &&
                 m.count(Type::Enclosure) == 2,
             "the swell box and the empty box");
    MP_CHECK(m.count(Type::Tremulant) == static_cast<int>(proc.organModel().tremulants.size()),
             "every tremulant");
    if (m.count(Type::Tremulant) > 0) {
      v = 1;
      int now = 0;
      MP_CHECK(m.set(Type::Tremulant, 0, v) == Error::None && m.get(Type::Tremulant, 0, now, e) && now == 1,
               "a tremulant started from its own object");
      v = 0;
      m.set(Type::Tremulant, 0, v);
    }
    MP_CHECK(m.count(Type::Windchest) == static_cast<int>(proc.organModel().wind.size()), "every windchest");
    MP_CHECK(m.count(Type::Route) == m.count(Type::Rank), "a route per rank");
    if (m.count(Type::Route) > 0) {
      int pair = 0;
      MP_CHECK(m.get(Type::Route, 0, pair, e) && pair >= 1, "a rank plays through a pair, from 1");
      v = proc.mixBusCount() + 1;
      MP_CHECK(m.set(Type::Route, 0, v) == Error::BadValue, "only to a pair the mixer has");
      v = juce::jmax(1, proc.mixBusCount());
      int now = 0;
      MP_CHECK(m.set(Type::Route, 0, v) == Error::None && m.get(Type::Route, 0, now, e) && now == v,
               "routed to the last pair and read back");
      MP_CHECK(m.describe(Type::Route, 0, e)["rank"].toString() == "0", "the route names its rank");
    }
    MP_CHECK(m.setProgram(ProgramProp::RoomWet, 40) == Error::None, "the room's wet share");
    int wet = 0;
    MP_CHECK(m.get(Type::Program, static_cast<int>(ProgramProp::RoomWet), wet, e) && wet == 40, "read back");

    // What a general holds: the tremulant drawn, stored in general 1.
    if (m.count(Type::General) > 0) {
      v = 1;
      m.set(Type::Switch, trem, v);
      proc.setCaptureMode(true);
      proc.pressGeneral(1);
      proc.setCaptureMode(false);
      const juce::var general = m.describe(Type::General, 0, e);
      bool holdsTrem = false;
      if (const auto* list = general["holds"].getArray())
        for (const auto& i : *list) holdsTrem = holdsTrem || static_cast<int>(i) == trem;
      MP_CHECK(static_cast<bool>(general["set"]) && holdsTrem, "a stored general says which switches it draws");
      v = 0;
      m.set(Type::Switch, trem, v);
    }

    // Changes, whatever made them.
    std::vector<ControlModel::Change> changes;
    m.poll(changes, true);  // the first look after a load
    changes.clear();
    proc.setSwitchEngaged(3, true);  // the window or MIDI, not the API
    m.poll(changes, true);
    bool seen = false;
    for (const auto& c : changes) seen = seen || (c.type == Type::Switch && c.index == blower && c.value == 1);
    MP_CHECK(seen, "a switch moved elsewhere is reported");
    changes.clear();
    m.poll(changes, true);
    MP_CHECK(changes.empty(), "and only once");
  }
};
static ControlModelTest g_controlModel;

class ApiHttpTest final : public mp::test::Test {
public:
  ApiHttpTest() : Test("functional.api.http", Category::Functional) {}
  void run() override {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(fixture(), 0, true).ok, "the organ loads");
    ApiServer server(proc);
    server.setExecutor([](std::function<void()> fn) { fn(); });
    ServerSettings s;
    s.enabled = true;
    s.port = 0;
    s.loopbackOnly = true;
    juce::String error;
    MP_CHECK(server.apply(s, error) && server.running() && server.port() > 0, "the server starts on a free port");

    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(5, 0);
    auto r = cli.Get("/api/v1");
    MP_CHECK(r && r->status == 200 && static_cast<int>(body(r)["session"]) == 1, "identify, with the session");
    r = cli.Get("/api/v1/switches");
    MP_CHECK(r && r->status == 200 && r->body.find("Swell Tremulant") != std::string::npos, "the switches with their names");
    r = cli.Get("/api/v1/nonsense");
    MP_CHECK(r && r->status == 404 && body(r)["error"].toString() == "unknown-type", "an unknown type is named");

    // The event stream, read on a thread of its own.
    std::atomic<bool> gotHello{false}, gotChange{false};
    std::thread reader([&] {
      httplib::Client ev("127.0.0.1", server.port());
      ev.set_read_timeout(5, 0);
      std::string seen;
      ev.Get("/api/v1/events?types=switches", [&](const char* data, size_t n) {
        seen.append(data, n);
        if (seen.find("event: hello") != std::string::npos) gotHello = true;
        if (seen.find("event: change") != std::string::npos && seen.find("switch") != std::string::npos) gotChange = true;
        return !gotChange.load();
      });
    });
    // A failed check throws: the reader is joined on every way out.
    struct Join {
      std::thread& t;
      ~Join() {
        if (t.joinable()) t.join();
      }
    } join{reader};
    for (int i = 0; i < 100 && !gotHello; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MP_CHECK(gotHello.load(), "the stream says hello");
    server.pollNow();  // the first look after a load

    r = cli.Put("/api/v1/switches/0/value", "{\"value\": 1}", "text/plain");
    MP_CHECK(r && r->status == 400, "a change not sent as JSON is refused");
    r = cli.Put("/api/v1/switches/0/value", "{\"value\": \"toggle\"}", "application/json");
    MP_CHECK(r && r->status == 200 && static_cast<int>(body(r)["value"]) == 1, "toggled on over HTTP");
    server.pollNow();
    for (int i = 0; i < 100 && !gotChange; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MP_CHECK(gotChange.load(), "and every subscriber hears of it");

    r = cli.Post("/api/v1/commands/0/press", "{}", "application/json");
    MP_CHECK(r && r->status == 200, "a command is pressed");
    r = cli.Get("/api/v1/switches/values");
    MP_CHECK(r && r->status == 200 && body(r)["values"].isArray(), "a dump");
    r = cli.Get("/");
    MP_CHECK(r && r->status == 200 && r->body.find("<title>Masterpiece</title>") != std::string::npos,
             "the web console is served");
    r = cli.Get("/api/v1/openapi.json");
    MP_CHECK(r && r->status == 200 && r->body.find("\"openapi\"") != std::string::npos, "and the API description");

    // A token, then read only.
    s.token = "organist";
    server.apply(s, error);
    r = cli.Put("/api/v1/switches/0/value", "{\"value\": 0}", "application/json");
    MP_CHECK(r && r->status == 403, "a change without the token is refused");
    httplib::Headers auth{{"Authorization", "Bearer organist"}};
    r = cli.Put("/api/v1/switches/0/value", auth, "{\"value\": 0}", "application/json");
    MP_CHECK(r && r->status == 200, "and allowed with it");
    s.allowChanges = false;
    server.apply(s, error);
    r = cli.Put("/api/v1/switches/0/value", auth, "{\"value\": 1}", "application/json");
    MP_CHECK(r && r->status == 403, "read only refuses every change");
    r = cli.Get("/api/v1/switches/0/value");
    MP_CHECK(r && r->status == 200, "and still answers reads");

    server.stop();
    MP_CHECK(!server.running(), "stopped");
  }
};
static ApiHttpTest g_apiHttp;

// A request hands its work to the message thread and answers busy when that
// thread is held up, as it is at the end of a long organ load. The work still
// queued must then never run: it writes into the request's own variables,
// which are gone, and the program crashed when the thread came free.
class ApiLateAnswerTest final : public mp::test::Test {
public:
  ApiLateAnswerTest() : Test("functional.api.late-answer", Category::Functional) {}
  void run() override {
    const std::atomic<bool> running{false};
    std::vector<std::function<void()>> queue;
    const auto toQueue = [&queue](std::function<void()> call) {
      queue.push_back(std::move(call));
      return true;
    };

    // Held up past the wait: busy, and the late call does nothing.
    auto ran = std::make_shared<int>(0);
    MP_CHECK(!ApiServer::runAndWait(toQueue, [ran] { ++*ran; }, 50, running),
             "a request the message thread does not reach in time answers busy");
    MP_CHECK(queue.size() == 1, "its work is still queued");
    queue.front()();
    MP_CHECK(*ran == 0, "and does not run when the thread comes free");
    queue.clear();

    // On time: the work runs and the request sees what it wrote.
    int value = 0;
    std::thread thread;
    const auto onThread = [&thread](std::function<void()> call) {
      thread = std::thread([call = std::move(call)] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        call();
      });
      return true;
    };
    MP_CHECK(ApiServer::runAndWait(onThread, [&value] { value = 7; }, 2000, running) && value == 7,
             "work done in time is answered");
    thread.join();

    // Started before the wait ends and still running after: seen out, so the
    // request's variables outlive it.
    value = 0;
    const auto atOnce = [&thread](std::function<void()> call) {
      thread = std::thread([call = std::move(call)] { call(); });
      return true;
    };
    const auto start = std::chrono::steady_clock::now();
    const bool done = ApiServer::runAndWait(
        atOnce,
        [&value] {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          value = 9;
        },
        20, running);
    const auto waited = std::chrono::steady_clock::now() - start;
    thread.join();
    MP_CHECK(done && value == 9, "work already running is waited for");
    MP_CHECK(waited >= std::chrono::milliseconds(190), "to its end");

    // Stopping: busy at once, and the queued work is dropped.
    const std::atomic<bool> stopping{true};
    *ran = 0;
    MP_CHECK(!ApiServer::runAndWait(toQueue, [ran] { ++*ran; }, 5000, stopping), "a stop answers busy");
    queue.front()();
    MP_CHECK(*ran == 0, "and drops the queued work");
    queue.clear();

    // Refused by the queue: busy.
    MP_CHECK(!ApiServer::runAndWait([](std::function<void()>) { return false; }, [] {}, 50, running),
             "work the queue refuses answers busy");
  }
};
static ApiLateAnswerTest g_apiLateAnswer;

// The console's switches on a GrandOrgue set: a stop drawn on two panels is
// listed once, and a stop worked through an "And" lists its drawstop, with
// the undrawn switch named first (#570).
class ApiHelperSwitchesTest final : public mp::test::Test {
public:
  ApiHelperSwitchesTest() : Test("functional.api.helper-switches", Category::Functional) {}
  void run() override {
    const std::string organ =
        "[Organ]\r\nChurchName=Helpers\r\nHasPedals=N\r\nNumberOfManuals=1\r\nNumberOfRanks=1\r\n"
        "NumberOfSwitches=2\r\nNumberOfWindchestGroups=1\r\nNumberOfPanels=1\r\n"
        "[WindchestGroup001]\r\nName=W\r\n"
        "[Switch001]\r\nName=Blower\r\nDefaultToEngaged=Y\r\nDisplayed=N\r\n"
        "[Switch002]\r\nName=Principal\r\nDisplayed=Y\r\n"
        "[Rank001]\r\nName=P\r\nFirstMidiNoteNumber=36\r\nNumberOfLogicalPipes=1\r\nPipe001=p.wav\r\n"
        "[Manual001]\r\nName=Great\r\nNumberOfLogicalKeys=1\r\nNumberOfAccessibleKeys=1\r\n"
        "FirstAccessibleKeyMIDINoteNumber=36\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Stop001]\r\nName=Principal\r\nNumberOfRanks=1\r\nRank001=1\r\nNumberOfAccessiblePipes=1\r\n"
        "Function=And\r\nSwitchCount=2\r\nSwitch001=001\r\nSwitch002=002\r\n"
        "[Panel001]\r\nName=Jamb\r\nHasPedals=N\r\nNumberOfManuals=0\r\n"
        "NumberOfSwitches=1\r\nSwitch001=002\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-api-helper-switches");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("helpers.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      ControlModel m(proc);
      Error e;
      const juce::var list = m.list(Type::Switch, e);
      int principals = 0;
      juce::String names, principalRole;
      for (const auto& item : *list["items"].getArray()) {
        const juce::String name = item["name"].toString();
        names << " [" << name << "]";
        if (name == "Principal") {
          ++principals;
          principalRole = item["role"].toString();
        }
      }
      MP_CHECK(principals == 1, "the drawstop is listed once:" + names.toStdString());
      MP_CHECK(principalRole == "stop", "as the stop it draws: " + principalRole.toStdString());
      MP_CHECK(!names.contains("mirror") && !names.contains("logic"),
               "no copy and no gate is listed:" + names.toStdString());
      const auto& model = proc.organModel();
      const auto stop = model.stops.find(2001);
      MP_CHECK(stop != model.stops.end(), "the stop is there");
      if (stop != model.stops.end())
        MP_CHECK(proc.playerSwitchFor(stop->second.controllingSwitchId) == 8002,
                 "the stop's knob is its drawstop, the blower named first notwithstanding");
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static ApiHelperSwitchesTest g_apiHelperSwitches;

// A divisional piston names its division, so a console can group pistons
// that share a name across divisions, as Wildervank's 1 to 5 do (#572).
class ApiPistonDivisionsTest final : public mp::test::Test {
public:
  ApiPistonDivisionsTest() : Test("functional.api.piston-divisions", Category::Functional) {}
  void run() override {
    const std::string organ =
        "[Organ]\r\nChurchName=Divisionals\r\nHasPedals=N\r\nNumberOfManuals=2\r\n"
        "NumberOfGenerals=1\r\nNumberOfDivisionals=2\r\n"
        "[General001]\r\nName=G1\r\nNumberOfStops=1\r\nStopManual001=1\r\nStopNumber001=1\r\n"
        "[Divisional001]\r\nName=1\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Divisional002]\r\nName=1\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "[Manual001]\r\nName=Hauptwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=1\r\n"
        "NumberOfDivisionals=1\r\nDivisional001=1\r\n"
        "[Manual002]\r\nName=Schwellwerk\r\nNumberOfLogicalKeys=3\r\nFirstAccessibleKeyMIDINoteNumber=36\r\n"
        "NumberOfAccessibleKeys=3\r\nNumberOfStops=1\r\nStop001=2\r\n"
        "NumberOfDivisionals=1\r\nDivisional001=2\r\n"
        "[Stop001]\r\nName=Principal 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=P8\\036.wav\r\n"
        "[Stop002]\r\nName=Viola 8\r\nNumberOfLogicalPipes=1\r\nNumberOfAccessiblePipes=1\r\n"
        "FirstAccessiblePipeLogicalKeyNumber=1\r\nPipe001=V8\\036.wav\r\n";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-api-piston-divisions");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("divisionals.organ");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      ControlModel m(proc);
      Error e;
      std::vector<std::string> divisionNames;
      const juce::var divisions = m.list(Type::Division, e);
      for (const auto& d : *divisions["items"].getArray())
        divisionNames.push_back(d["name"].toString().toStdString());
      std::vector<std::string> ones;
      int generalDivision = -2;
      std::string listed;
      const juce::var switches = m.list(Type::Switch, e, true);
      for (const auto& item : *switches["items"].getArray()) {
        listed += " [" + item["name"].toString().toStdString() + " " + item["role"].toString().toStdString() + " " +
                  item["division"].toString().toStdString() + "]";
        if (item["role"].toString() != "piston") continue;
        const int d = static_cast<int>(item["division"]);
        if (item["name"].toString() == "1")
          ones.push_back(d >= 0 && d < static_cast<int>(divisionNames.size()) ? divisionNames[static_cast<size_t>(d)]
                                                                              : "none");
        if (item["name"].toString() == "G1") generalDivision = d;
      }
      std::string seen;
      for (const auto& n : ones) seen += " [" + n + "]";
      MP_CHECK(ones.size() == 2 && ones[0] != ones[1] && ones[0] != "none" && ones[1] != "none",
               "each piston 1 names its own division:" + seen + " of" + listed);
      MP_CHECK(generalDivision == -1, "a general names none: " + std::to_string(generalDivision));
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static ApiPistonDivisionsTest g_apiPistonDivisions;

// The same on a definition whose panel pistons work the combinations' own
// switches through the wiring and whose combinations name their division by
// code alone, as Wildervank's do: "CustPg1_Comb piston: PED 01" engages
// switch 201, which fires "Div1 combination 01" (#572).
class ApiLinkedPistonDivisionsTest final : public mp::test::Test {
public:
  ApiLinkedPistonDivisionsTest() : Test("functional.api.linked-piston-divisions", Category::Functional) {}
  void run() override {
    const std::string organ =
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><o><a>1</a></o></ObjectList>"
        "<ObjectList ObjectType=\"Division\"><o><a>1</a><b>PED</b></o><o><a>2</a><b>HW</b></o></ObjectList>"
        "<ObjectList ObjectType=\"Switch\">"
        "<o><a>101</a><b>Comb piston: General 01</b><c>101</c><d>N</d></o>"
        "<o><a>201</a><b>Comb piston: PED 01</b><c>201</c><d>N</d></o>"
        "<o><a>10075</a><b>CustPg1_Comb piston: General 01</b><d>N</d></o>"
        "<o><a>10076</a><b>CustPg1_Comb piston: PED 01</b><d>N</d></o></ObjectList>"
        "<ObjectList ObjectType=\"SwitchLinkage\"><o><a>10075</a><b>101</b></o><o><a>10076</a><b>201</b></o>"
        "</ObjectList>"
        "<ObjectList ObjectType=\"Combination\">"
        "<o><a>3073</a><b>General 01</b><c>101</c></o>"
        "<o><a>55681</a><b>Div1 combination 01</b><c>201</c></o></ObjectList></Hauptwerk>";
    const juce::File dir =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-api-linked-pistons");
    dir.deleteRecursively();
    dir.createDirectory();
    const juce::File file = dir.getChildFile("linked.Organ_Hauptwerk_xml");
    file.replaceWithText(juce::String::fromUTF8(organ.c_str()));
    {
      mp::MasterpieceProcessor proc;
      const juce::File settings = proc.settingsFileFor(file);
      MP_CHECK(proc.loadOrgan(file, 0, true).ok, "the organ loads");
      ControlModel m(proc);
      Error e;
      std::vector<std::string> divisionNames;
      const juce::var divisions = m.list(Type::Division, e);
      for (const auto& d : *divisions["items"].getArray())
        divisionNames.push_back(d["name"].toString().toStdString());
      std::map<juce::int64, std::string> divisionOf;
      std::string listed;
      const juce::var switches = m.list(Type::Switch, e, true);
      for (const auto& item : *switches["items"].getArray()) {
        const int d = static_cast<int>(item["division"]);
        const auto id = static_cast<juce::int64>(item["odfId"]);
        listed += " [" + std::to_string(id) + " " + item["role"].toString().toStdString() + " " + std::to_string(d) + "]";
        if (item["role"].toString() == "piston")
          divisionOf[id] = d >= 0 && d < static_cast<int>(divisionNames.size()) ? divisionNames[static_cast<size_t>(d)]
                                                                                 : "none";
      }
      MP_CHECK(divisionOf.count(10076) && divisionOf[10076] == "PED",
               "the panel's PED 01 piston, wired to the combination's switch, names the PED:" + listed);
      MP_CHECK(divisionOf.count(10075) && divisionOf[10075] == "none", "the panel's general names none:" + listed);
      settings.deleteFile();
    }
    dir.deleteRecursively();
  }
};
static ApiLinkedPistonDivisionsTest g_apiLinkedPistonDivisions;

// The organ's own console pages over HTTP: a page with one drawstop, drawn
// red when in and green when drawn. The picture shows the knob as it stands,
// a tap draws the stop, and the change comes back as the knob's rectangle
// alone, drawn green.
class ApiConsolePagesTest final : public mp::test::Test {
public:
  ApiConsolePagesTest() : Test("functional.api.console-pages", Category::Functional) {}

  static void png(const juce::File& f, int w, int h, juce::Colour c) {
    juce::Image img(juce::Image::RGB, w, h, false);
    img.clear(img.getBounds(), c);
    f.deleteFile();
    juce::FileOutputStream out(f);
    juce::PNGImageFormat().writeImageToStream(img, out);
  }

  void run() override {
    // Components belong to the message thread: this one. The server's work is
    // handed here and run while the requests wait on threads of their own.
    juce::MessageManager::getInstance();
    std::mutex m;
    std::deque<std::function<void()>> queue;
    const auto pump = [&] {
      for (;;) {
        std::function<void()> fn;
        {
          const std::lock_guard<std::mutex> lock(m);
          if (queue.empty()) return;
          fn = std::move(queue.front());
          queue.pop_front();
        }
        fn();
      }
    };
    const auto call = [&](std::function<httplib::Result()> request) {
      auto f = std::async(std::launch::async, std::move(request));
      while (f.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready) pump();
      return f.get();
    };

    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-console-pages");
    root.deleteRecursively();
    const auto pkg = root.getChildFile("OrganInstallationPackages").getChildFile("000001");
    pkg.createDirectory();
    root.getChildFile("OrganDefinitions").createDirectory();
    png(pkg.getChildFile("back.png"), 300, 400, juce::Colours::grey);
    png(pkg.getChildFile("in.png"), 40, 60, juce::Colours::red);
    png(pkg.getChildFile("out.png"), 40, 60, juce::Colours::lime);
    const juce::File odf = root.getChildFile("OrganDefinitions").getChildFile("page.Organ_Hauptwerk_xml");
    odf.replaceWithText(
        "<?xml version=\"1.0\"?><Hauptwerk FileFormat=\"Organ\">"
        "<ObjectList ObjectType=\"_General\"><_General><Identification_Name>Page</Identification_Name>"
        "<Identification_UniqueOrganID>78</Identification_UniqueOrganID></_General></ObjectList>"
        "<ObjectList ObjectType=\"RequiredInstallationPackage\"><RequiredInstallationPackage>"
        "<InstallationPackageID>1</InstallationPackageID><Name>P</Name></RequiredInstallationPackage></ObjectList>"
        "<ObjectList ObjectType=\"Switch\">"
        "<Switch><SwitchID>1101</SwitchID><Name>Principal 8</Name><Latching>Y</Latching>"
        "<Disp_ImageSetInstanceID>20</Disp_ImageSetInstanceID>"
        "<Disp_ImageSetIndexEngaged>2</Disp_ImageSetIndexEngaged>"
        "<Disp_ImageSetIndexDisengaged>1</Disp_ImageSetIndexDisengaged></Switch></ObjectList>"
        "<ObjectList ObjectType=\"DisplayPage\">"
        "<DisplayPage><PageID>1</PageID><Name>Console</Name></DisplayPage></ObjectList>"
        "<ObjectList ObjectType=\"ImageSet\">"
        "<ImageSet><ImageSetID>10</ImageSetID><Name>Knob</Name><InstallationPackageID>1</InstallationPackageID>"
        "<ImageWidthPixels>40</ImageWidthPixels><ImageHeightPixels>60</ImageHeightPixels></ImageSet>"
        "<ImageSet><ImageSetID>11</ImageSetID><Name>Back</Name><InstallationPackageID>1</InstallationPackageID>"
        "<ImageWidthPixels>300</ImageWidthPixels><ImageHeightPixels>400</ImageHeightPixels></ImageSet></ObjectList>"
        "<ObjectList ObjectType=\"ImageSetElement\">"
        "<ImageSetElement><ImageSetID>10</ImageSetID><ImageIndexWithinSet>1</ImageIndexWithinSet><BitmapFilename>in.png</BitmapFilename></ImageSetElement>"
        "<ImageSetElement><ImageSetID>10</ImageSetID><ImageIndexWithinSet>2</ImageIndexWithinSet><BitmapFilename>out.png</BitmapFilename></ImageSetElement>"
        "<ImageSetElement><ImageSetID>11</ImageSetID><ImageIndexWithinSet>1</ImageIndexWithinSet><BitmapFilename>back.png</BitmapFilename></ImageSetElement>"
        "</ObjectList>"
        "<ObjectList ObjectType=\"ImageSetInstance\">"
        "<ImageSetInstance><ImageSetInstanceID>21</ImageSetInstanceID><Name>Back</Name>"
        "<ImageSetID>11</ImageSetID><DefaultImageIndexWithinSet>1</DefaultImageIndexWithinSet>"
        "<DisplayPageID>1</DisplayPageID><ScreenLayerNumber>1</ScreenLayerNumber>"
        "<LeftXPosPixels>0</LeftXPosPixels><TopYPosPixels>0</TopYPosPixels></ImageSetInstance>"
        "<ImageSetInstance><ImageSetInstanceID>20</ImageSetInstanceID><Name>Principal knob</Name>"
        "<ImageSetID>10</ImageSetID><DefaultImageIndexWithinSet>1</DefaultImageIndexWithinSet>"
        "<DisplayPageID>1</DisplayPageID><ScreenLayerNumber>5</ScreenLayerNumber>"
        "<LeftXPosPixels>100</LeftXPosPixels><TopYPosPixels>200</TopYPosPixels></ImageSetInstance>"
        "</ObjectList></Hauptwerk>");
    struct Clean {
      juce::File dir;
      ~Clean() { dir.deleteRecursively(); }
    } clean{root};

    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, true).ok, "the organ loads");
    const auto block = [&proc] {
      juce::AudioBuffer<float> buf(2, 256);
      juce::MidiBuffer none;
      proc.processBlock(buf, none);
    };
    block();

    ApiServer server(proc);
    server.setExecutor([&](std::function<void()> fn) {
      std::promise<void> done;
      {
        const std::lock_guard<std::mutex> lock(m);
        queue.push_back([&] {
          fn();
          done.set_value();
        });
      }
      done.get_future().wait();
    });
    ServerSettings s;
    s.enabled = true;
    s.port = 0;
    s.loopbackOnly = true;
    juce::String error;
    MP_CHECK(server.apply(s, error) && server.running(), "the server starts");
    httplib::Client cli("127.0.0.1", server.port());
    cli.set_read_timeout(10, 0);
    const auto get = [&](const std::string& path) { return call([&cli, path] { return cli.Get(path); }); };
    const auto tap = [&](const std::string& phase, int x, int y, const httplib::Headers& h = {}) {
      const std::string b = "{\"phase\": \"" + phase + "\", \"x\": " + std::to_string(x) + ", \"y\": " + std::to_string(y) + "}";
      return call([&cli, b, h] { return cli.Post("/api/v1/pages/0/pointer", h, b, "application/json"); });
    };
    const auto image = [](const httplib::Result& r) {
      return r ? juce::ImageFileFormat::loadFrom(r->body.data(), r->body.size()) : juce::Image();
    };

    auto r = get("/api/v1/pages");
    const juce::var list = body(r);
    MP_CHECK(r && r->status == 200 && list["pages"].size() == 1, "one page is listed");
    const juce::var p0 = list["pages"][0];
    MP_CHECK(p0["name"].toString() == "Console" && static_cast<int>(p0["width"]) == 300 &&
                 static_cast<int>(p0["height"]) == 400,
             "with its name and size, got " + juce::JSON::toString(p0, true).toStdString());

    r = get("/api/v1/pages/0/picture");
    juce::Image whole = image(r);
    MP_CHECK(r && r->status == 200 && r->get_header_value("Content-Type") == "image/png" && whole.getWidth() == 300 &&
                 whole.getHeight() == 400,
             "the page's picture, at its own size");
    MP_CHECK(whole.getPixelAt(120, 230).getRed() > 200 && whole.getPixelAt(120, 230).getGreen() < 50,
             "the knob is drawn in, red");
    MP_CHECK(whole.getPixelAt(10, 10).getRed() == whole.getPixelAt(10, 10).getGreen(), "on the grey background");
    const std::string serial = r->get_header_value("X-Serial");

    r = get("/api/v1/pages/0/changes?since=" + serial);
    MP_CHECK(r && r->status == 200 && body(r)["areas"].size() == 0, "nothing changed yet");

    // A tap on the knob draws the stop, as a click does.
    r = tap("down", 120, 230);
    MP_CHECK(r && r->status == 200, "the press is taken");
    r = tap("up", 120, 230);
    MP_CHECK(r && r->status == 200, "and the release");
    block();
    MP_CHECK(proc.shownSwitchEngaged(1101), "the stop is drawn");

    r = get("/api/v1/pages/0/changes?since=" + serial);
    const juce::var ch = body(r);
    MP_CHECK(r && r->status == 200 && ch["areas"].size() == 1, "one rectangle changed, got " + r->body);
    const juce::var a = ch["areas"][0];
    const int ax = a["x"], ay = a["y"], aw = a["w"], ah = a["h"];
    MP_CHECK(ax <= 100 && ay <= 200 && ax + aw >= 140 && ay + ah >= 260 && aw < 60 && ah < 80,
             "the knob's, got " + r->body);
    r = get("/api/v1/pages/0/picture?x=" + std::to_string(ax) + "&y=" + std::to_string(ay) + "&w=" +
            std::to_string(aw) + "&h=" + std::to_string(ah));
    const juce::Image part = image(r);
    MP_CHECK(part.getWidth() == aw && part.getHeight() == ah, "that rectangle alone is sent");
    const auto c = part.getPixelAt(120 - ax, 230 - ay);
    MP_CHECK(c.getGreen() > 200 && c.getRed() < 50, "with the knob drawn out, green");

    r = get("/api/v1/pages/0/changes?since=" + juce::String(static_cast<juce::int64>(ch["serial"])).toStdString());
    MP_CHECK(r && body(r)["areas"].size() == 0, "and nothing since");

    // A tap on the background changes nothing.
    tap("down", 10, 10);
    tap("up", 10, 10);
    block();
    MP_CHECK(proc.shownSwitchEngaged(1101), "a tap beside the knob leaves the stop");

    r = tap("sideways", 120, 230);
    MP_CHECK(r && r->status == 400, "an unknown phase is refused");
    r = get("/api/v1/pages/3/picture");
    MP_CHECK(r && r->status == 404, "a page the organ lacks is out of range");

    s.token = "organist";
    server.apply(s, error);
    r = tap("down", 120, 230);
    MP_CHECK(r && r->status == 403, "a tap without the token is refused");
    r = get("/api/v1/pages/0/picture?w=10&h=10");
    MP_CHECK(r && r->status == 200, "and the picture is still there to read");
    block();
    MP_CHECK(proc.shownSwitchEngaged(1101), "the refused tap left the stop drawn");

    server.stop();
  }
};
static ApiConsolePagesTest g_apiConsolePages;

}  // namespace
#endif  // MP_TEST_HAS_API
