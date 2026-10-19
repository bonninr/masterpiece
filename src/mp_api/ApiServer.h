// The control protocol over HTTP (protocol/control.yaml): a small server on
// the local network, the web console it serves, and the stream of changes.
//
// The server runs on threads of its own; every request is carried out on the
// message thread through ControlModel, as a click in the window would be.
#pragma once

#include "ControlModel.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
}

namespace mp::api {

struct ServerSettings {
  bool enabled = false;
  int port = 8080;  // 0: any free port, which port() then gives
  // Off: everything can be read, nothing changed.
  bool allowChanges = true;
  // When set, a change needs "Authorization: Bearer <token>".
  std::string token;
};

class ApiServer : private juce::Timer {
public:
  explicit ApiServer(MasterpieceProcessor& p);
  ~ApiServer() override;

  // Starts with these settings, or stops when they say off. False with the
  // reason when the port cannot be had.
  bool apply(const ServerSettings& s, juce::String& error);
  void stop();
  bool running() const { return running_.load(); }
  int port() const { return settings_.port; }
  // The addresses a browser on the network can open, for the settings.
  static juce::StringArray addresses(int port);

  // For tests, which have no message loop: requests run through `run`,
  // one at a time, and changes are looked for when pollNow() is called.
  void setExecutor(std::function<void(std::function<void()>)> run) { executor_ = std::move(run); }
  void pollNow() { timerCallback(); }

  // One client of the event stream.
  struct Subscriber;

private:
  void timerCallback() override;
  void routes();
  // Runs `fn` on the message thread and waits for it; false when the program
  // is stopping or the message thread did not answer in time.
  bool onMessageThread(std::function<void()> fn);

  MasterpieceProcessor& proc_;
  ControlModel model_;
  std::mutex settingsMutex_;  // the server threads read it, the window writes it
  ServerSettings settings_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::mutex subscribersMutex_;
  std::vector<std::shared_ptr<Subscriber>> subscribers_;
  int ticks_ = 0;
  int requestedPort_ = -1;
  std::function<void(std::function<void()>)> executor_;
  std::mutex executorMutex_;
};

}  // namespace mp::api
