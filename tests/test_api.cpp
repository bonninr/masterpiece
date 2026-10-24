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
#include <functional>
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

}  // namespace
#endif  // MP_TEST_HAS_API
