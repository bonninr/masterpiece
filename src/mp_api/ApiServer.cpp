// cpp-httplib first: on Windows its winsock2.h must come before anything that
// brings in windows.h.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <httplib.h>

#include "ApiServer.h"

#include "../mp_audio/MasterpieceProcessor.h"
#include "MpWeb.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <future>

namespace mp::api {

struct ApiServer::Subscriber {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::string> queue;
  std::vector<bool> types;  // by type code; empty for all
  bool closed = false;
  bool wants(Type t) const {
    return types.empty() || (static_cast<size_t>(t) < types.size() && types[static_cast<size_t>(t)]);
  }
};

namespace {

std::string json(const juce::var& v) { return juce::JSON::toString(v, true).toStdString(); }

juce::var errorBody(Error e) {
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  const ErrorInfo* info = errorInfo(e);
  o->setProperty("error", info != nullptr ? juce::String(std::string(info->name)) : juce::String("error"));
  return v;
}

void reply(httplib::Response& res, const juce::var& body) {
  res.set_content(json(body), "application/json; charset=utf-8");
}

void fail(httplib::Response& res, Error e) {
  const ErrorInfo* info = errorInfo(e);
  res.status = info != nullptr ? info->httpStatus : 500;
  reply(res, errorBody(e));
}

juce::var valueBody(int value) {
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("value", value);
  return v;
}

std::string eventText(const ControlModel::Change& c, int session) {
  const TypeInfo* info = typeInfo(c.type);
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("type", info != nullptr ? juce::String(std::string(info->name)) : juce::String());
  if (c.type == Type::Program && c.index >= 0 && c.index < kProgramCount)
    o->setProperty("property", juce::String(std::string(kProgram[c.index].name)));
  o->setProperty("index", c.index);
  o->setProperty("value", c.value);
  o->setProperty("session", session);
  return "event: change\ndata: " + json(v) + "\n\n";
}

// The web console's files, built into the program.
bool webFile(const std::string& path, std::string& body, std::string& type) {
  std::string name = path == "/" ? "index.html" : path.substr(1);
  // MASTERPIECE_WEB_DIR serves the files from a folder, for working on the
  // page without building the program again.
  const juce::String dir = juce::SystemStats::getEnvironmentVariable("MASTERPIECE_WEB_DIR", {});
  bool found = false;
  if (dir.isNotEmpty() && name.find("..") == std::string::npos) {
    const juce::File f = juce::File(dir).getChildFile(juce::String(name));
    juce::MemoryBlock mb;
    if (f.existsAsFile() && f.loadFileAsData(mb)) {
      body.assign(static_cast<const char*>(mb.getData()), mb.getSize());
      found = true;
    }
  }
  for (int i = 0; !found && i < MpWeb::namedResourceListSize; ++i) {
    if (name != MpWeb::originalFilenames[i]) continue;
    int size = 0;
    const char* data = MpWeb::getNamedResource(MpWeb::namedResourceList[i], size);
    if (data == nullptr) return false;
    body.assign(data, static_cast<size_t>(size));
    found = true;
  }
  if (!found) return false;
  {
    const auto ends = [&name](const char* ext) {
      const std::string e(ext);
      return name.size() >= e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0;
    };
    type = ends(".html")  ? "text/html; charset=utf-8"
           : ends(".js")  ? "text/javascript; charset=utf-8"
           : ends(".css") ? "text/css; charset=utf-8"
           : ends(".json") ? "application/json"
           : ends(".svg") ? "image/svg+xml"
                          : "application/octet-stream";
    return true;
  }
  return false;
}

}  // namespace

ApiServer::ApiServer(MasterpieceProcessor& p) : proc_(p), model_(p) {}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::onMessageThread(std::function<void()> fn) {
  if (executor_) {
    const std::lock_guard<std::mutex> lock(executorMutex_);
    executor_(std::move(fn));
    return true;
  }
  if (juce::MessageManager::getInstanceWithoutCreating() == nullptr) return false;
  return runAndWait([](std::function<void()> call) { return juce::MessageManager::callAsync(std::move(call)); },
                    std::move(fn), 3000, stopping_);
}

bool ApiServer::runAndWait(const std::function<bool(std::function<void()>)>& post,
                           std::function<void()> fn, int waitMs,
                           const std::atomic<bool>& stopping) {
  // Shared by the waiting request and the queued call, which can outlive it.
  struct Handoff {
    std::mutex mutex;
    std::condition_variable changed;
    enum class State { Queued, Running, Done, Dropped } state = State::Queued;
  };
  auto handoff = std::make_shared<Handoff>();
  const bool posted = post([fn = std::move(fn), handoff] {
    {
      const std::lock_guard<std::mutex> lock(handoff->mutex);
      if (handoff->state == Handoff::State::Dropped) return;
      handoff->state = Handoff::State::Running;
    }
    // Done on every way out, so the request never waits on a call that threw.
    struct Finish {
      Handoff& h;
      ~Finish() {
        {
          const std::lock_guard<std::mutex> lock(h.mutex);
          h.state = Handoff::State::Done;
        }
        h.changed.notify_all();
      }
    } finish{*handoff};
    fn();
  });
  if (!posted) return false;

  std::unique_lock<std::mutex> lock(handoff->mutex);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
  // In short waits, so a stop is never held up by a request in flight.
  while (handoff->state != Handoff::State::Done) {
    const bool late = std::chrono::steady_clock::now() >= deadline || stopping.load();
    if (late && handoff->state == Handoff::State::Queued) {
      handoff->state = Handoff::State::Dropped;
      return false;
    }
    // Running: it reads and writes the request's variables, so it is seen out.
    handoff->changed.wait_for(lock, std::chrono::milliseconds(30));
  }
  return true;
}

bool ApiServer::apply(const ServerSettings& s, juce::String& error) {
  // The port as asked for: 0 (any) stays whatever port it got.
  const bool same = running() && s.enabled && s.port == requestedPort_ && s.loopbackOnly == settings_.loopbackOnly;
  requestedPort_ = s.port;
  {
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    const int bound = settings_.port;
    settings_ = s;
    if (same) settings_.port = bound;
  }
  if (same) return true;  // a change of permission or token needs no restart
  stop();
  if (!s.enabled) return true;

  server_ = std::make_unique<httplib::Server>();
  // Each open event stream holds a thread of the pool; the pool is sized so
  // that even kMaxEventStreams of them leave threads free for other requests.
  server_->new_task_queue = [] { return new httplib::ThreadPool(kThreadPoolSize); };
  routes();
  const char* host = s.loopbackOnly ? "127.0.0.1" : "0.0.0.0";
  if (s.port == 0) {
    const int any = server_->bind_to_any_port(host);
    const std::lock_guard<std::mutex> lock(settingsMutex_);
    settings_.port = any;
  } else if (!server_->bind_to_port(host, s.port)) {
    error = "port " + juce::String(s.port) + " is in use or not allowed";
    server_.reset();
    return false;
  }
  stopping_.store(false);
  running_.store(true);
  thread_ = std::thread([this] { server_->listen_after_bind(); });
  if (!executor_) startTimerHz(20);
  juce::Logger::writeToLog("control API: http://" +
                           (s.loopbackOnly ? "127.0.0.1:" + juce::String(settings_.port)
                                           : addresses(settings_.port).joinIntoString(", http://")) +
                           (s.allowChanges ? "" : " (read only)"));
  return true;
}

void ApiServer::stop() {
  stopTimer();
  if (!running_.exchange(false)) return;
  stopping_.store(true);
  {
    const std::lock_guard<std::mutex> lock(subscribersMutex_);
    for (auto& s : subscribers_) {
      const std::lock_guard<std::mutex> l(s->mutex);
      s->closed = true;
      s->wake.notify_all();
    }
    subscribers_.clear();
  }
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
  server_.reset();
  juce::Logger::writeToLog("control API: stopped");
}

juce::StringArray ApiServer::addresses(int port) {
  juce::StringArray out;
  for (const auto& ip : juce::IPAddress::getAllAddresses(false))
    if (!ip.isNull() && ip.toString() != "127.0.0.1") out.add(ip.toString() + ":" + juce::String(port));
  if (out.isEmpty()) out.add("localhost:" + juce::String(port));
  return out;
}

void ApiServer::timerCallback() {
  // Every change since the last tick, whatever made it: the window, MIDI,
  // pistons, the sequencer, another client.
  std::vector<ControlModel::Change> changes;
  if (executor_) {
    const std::lock_guard<std::mutex> lock(executorMutex_);
    model_.poll(changes, ++ticks_ % 10 == 0);
  } else {
    model_.poll(changes, ++ticks_ % 10 == 0);
  }
  if (changes.empty()) return;
  const int session = model_.session();
  const std::lock_guard<std::mutex> lock(subscribersMutex_);
  for (auto& s : subscribers_) {
    const std::lock_guard<std::mutex> l(s->mutex);
    for (const auto& c : changes)
      if (s->wants(c.type)) s->queue.push_back(eventText(c, session));
    // A client that stopped reading is cut off before it fills the memory.
    if (s->queue.size() > 4096) s->closed = true;
    s->wake.notify_all();
  }
}

void ApiServer::routes() {
  auto& srv = *server_;

  // A change: allowed, the token right, and sent as JSON (which a page of
  // another site cannot do without asking first, and this server never says
  // yes), and for the session it was built for.
  auto mayChange = [this](const httplib::Request& req, httplib::Response& res) {
    ServerSettings now;
    {
      const std::lock_guard<std::mutex> lock(settingsMutex_);
      now = settings_;
    }
    if (!now.allowChanges) return fail(res, Error::NotAllowed), false;
    if (!now.token.empty() && req.get_header_value("Authorization") != "Bearer " + now.token)
      return fail(res, Error::NotAllowed), false;
    if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0)
      return fail(res, Error::BadMessage), false;
    if (req.has_header("X-Session")) {
      int session = 0;
      onMessageThread([&] { session = model_.session(); });
      if (std::atoi(req.get_header_value("X-Session").c_str()) != session)
        return fail(res, Error::StaleSession), false;
    }
    return true;
  };
  auto typeOf = [](const std::string& plural) -> const TypeInfo* { return typeByPlural(plural); };

  srv.Get("/api/v1", [this](const httplib::Request&, httplib::Response& res) {
    juce::var v;
    if (!onMessageThread([&] { v = model_.identity(); })) return fail(res, Error::Busy);
    reply(res, v);
  });

  srv.Get("/api/v1/openapi.json", [](const httplib::Request& req, httplib::Response& res) {
    std::string body, type;
    if (!webFile("/openapi.json", body, type)) return fail(res, Error::UnknownOperation);
    (void)req;
    res.set_content(body, type);
  });

  srv.Get("/api/v1/program", [this](const httplib::Request&, httplib::Response& res) {
    juce::var v;
    if (!onMessageThread([&] { v = model_.program(); })) return fail(res, Error::Busy);
    reply(res, v);
  });

  srv.Put(R"(/api/v1/program/(\w+))", [this, mayChange](const httplib::Request& req, httplib::Response& res) {
    if (!mayChange(req, res)) return;
    const std::string name = req.matches[1];
    int index = -1;
    for (const auto& p : kProgram)
      if (p.name == name) index = static_cast<int>(p.prop);
    if (index < 0) return fail(res, Error::OutOfRange);
    const juce::var body = juce::JSON::parse(juce::String(req.body));
    if (!body.hasProperty("value")) return fail(res, Error::BadMessage);
    const juce::var raw = body["value"];
    if (!(raw.isInt() || raw.isInt64() || raw.isDouble() || raw.isBool())) return fail(res, Error::BadValue);
    int value = static_cast<int>(raw);
    Error e = Error::None;
    if (!onMessageThread([&] { e = model_.set(Type::Program, index, value); })) return fail(res, Error::Busy);
    if (e != Error::None) return fail(res, e);
    reply(res, valueBody(value));
  });

  srv.Get("/api/v1/events", [this](const httplib::Request& req, httplib::Response& res) {
    // A stream would hold a server thread until the client goes away, so past
    // the cap a new one is refused rather than starving ordinary requests.
    if (eventStreams_.fetch_add(1) >= kMaxEventStreams) {
      eventStreams_.fetch_sub(1);
      return fail(res, Error::Busy);
    }
    auto sub = std::make_shared<Subscriber>();
    if (req.has_param("types")) {
      sub->types.assign(kTypeCount, false);
      for (const auto& t : juce::StringArray::fromTokens(juce::String(req.get_param_value("types")), ",", "")) {
        if (const TypeInfo* info = typeByPlural(t.trim().toStdString()))
          sub->types[static_cast<size_t>(info->type)] = true;
      }
      sub->types[static_cast<size_t>(Type::Program)] = true;  // the session, always
    }
    int session = 0;
    onMessageThread([&] { session = model_.session(); });
    sub->queue.push_back("event: hello\ndata: {\"session\": " + std::to_string(session) + "}\n\n");
    {
      const std::lock_guard<std::mutex> lock(subscribersMutex_);
      subscribers_.push_back(sub);
    }
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, sub](size_t, httplib::DataSink& sink) {
          std::unique_lock<std::mutex> l(sub->mutex);
          sub->wake.wait_for(l, std::chrono::seconds(15),
                             [&] { return !sub->queue.empty() || sub->closed || stopping_.load(); });
          if (sub->closed || stopping_.load()) return false;
          if (sub->queue.empty()) {
            const char* ping = ": ping\n\n";  // keeps proxies and sleepy Wi-Fi from closing it
            return sink.write(ping, std::strlen(ping));
          }
          while (!sub->queue.empty()) {
            const std::string text = std::move(sub->queue.front());
            sub->queue.pop_front();
            if (!sink.write(text.data(), text.size())) return false;
          }
          return true;
        },
        [this, sub](bool) {
          eventStreams_.fetch_sub(1);
          const std::lock_guard<std::mutex> lock(subscribersMutex_);
          subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), sub), subscribers_.end());
        });
  });

  srv.Get(R"(/api/v1/(\w+)/values)", [this, typeOf](const httplib::Request& req, httplib::Response& res) {
    const TypeInfo* info = typeOf(req.matches[1]);
    if (info == nullptr) return fail(res, Error::UnknownType);
    juce::var v;
    Error e = Error::None;
    if (!onMessageThread([&] { v = model_.dump(info->type, e); })) return fail(res, Error::Busy);
    if (e != Error::None) return fail(res, e);
    reply(res, v);
  });

  srv.Get(R"(/api/v1/(\w+)/(\d+)/value)", [this, typeOf](const httplib::Request& req, httplib::Response& res) {
    const TypeInfo* info = typeOf(req.matches[1]);
    if (info == nullptr) return fail(res, Error::UnknownType);
    const int index = std::atoi(req.matches[2].str().c_str());
    int value = 0;
    Error e = Error::None;
    bool ok = false;
    if (!onMessageThread([&] { ok = model_.get(info->type, index, value, e); })) return fail(res, Error::Busy);
    if (!ok) return fail(res, e);
    reply(res, valueBody(value));
  });

  srv.Put(R"(/api/v1/(\w+)/(\d+)/value)", [this, typeOf, mayChange](const httplib::Request& req, httplib::Response& res) {
    if (!mayChange(req, res)) return;
    const TypeInfo* info = typeOf(req.matches[1]);
    if (info == nullptr) return fail(res, Error::UnknownType);
    const int index = std::atoi(req.matches[2].str().c_str());
    const juce::var body = juce::JSON::parse(juce::String(req.body));
    if (!body.hasProperty("value")) return fail(res, Error::BadMessage);
    const juce::var raw = body["value"];
    const bool toggle = raw.isString() && raw.toString() == "toggle";
    if (!(toggle || raw.isInt() || raw.isInt64() || raw.isDouble() || raw.isBool())) return fail(res, Error::BadValue);
    int value = toggle ? 2 : static_cast<int>(raw);
    Error e = Error::None;
    if (!onMessageThread([&] { e = model_.set(info->type, index, value); })) return fail(res, Error::Busy);
    if (e != Error::None) return fail(res, e);
    reply(res, valueBody(value));
  });

  srv.Post(R"(/api/v1/(\w+)/(\d+)/press)", [this, typeOf, mayChange](const httplib::Request& req, httplib::Response& res) {
    if (!mayChange(req, res)) return;
    const TypeInfo* info = typeOf(req.matches[1]);
    if (info == nullptr) return fail(res, Error::UnknownType);
    const int index = std::atoi(req.matches[2].str().c_str());
    Error e = Error::None;
    if (!onMessageThread([&] { e = model_.press(info->type, index); })) return fail(res, Error::Busy);
    if (e != Error::None) return fail(res, e);
    reply(res, juce::var(new juce::DynamicObject()));
  });

  srv.Get(R"(/api/v1/(\w+)/(\d+))", [this, typeOf](const httplib::Request& req, httplib::Response& res) {
    const TypeInfo* info = typeOf(req.matches[1]);
    if (info == nullptr) return fail(res, Error::UnknownType);
    const int index = std::atoi(req.matches[2].str().c_str());
    juce::var v;
    Error e = Error::None;
    if (!onMessageThread([&] { v = model_.describe(info->type, index, e); })) return fail(res, Error::Busy);
    if (e != Error::None) return fail(res, e);
    reply(res, v);
  });

  srv.Get(R"(/api/v1/(\w+))", [this, typeOf](const httplib::Request& req, httplib::Response& res) {
    const TypeInfo* info = typeOf(req.matches[1]);
    if (info == nullptr) return fail(res, Error::UnknownType);
    juce::var v;
    Error e = Error::None;
    const bool all = req.get_param_value("all") == "1";
    if (!onMessageThread([&] { v = model_.list(info->type, e, all); })) return fail(res, Error::Busy);
    if (e != Error::None) return fail(res, e);
    reply(res, v);
  });

  // The web console.
  srv.Get(R"(/([\w.-]*))", [](const httplib::Request& req, httplib::Response& res) {
    std::string body, type;
    if (!webFile(req.path, body, type)) {
      res.status = 404;
      return;
    }
    res.set_content(body, type);
  });
}

}  // namespace mp::api
