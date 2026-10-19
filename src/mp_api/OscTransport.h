// The control protocol over OSC (protocol/control.yaml), for control
// surfaces and apps that speak it: TouchOSC, Open Stage Control, a
// microcontroller on Wi-Fi.
//
//   /mp/identify                      -> /mp/identify s protocol s version s program i session s organ
//   /mp/<plural>/count                -> /mp/<plural>/count i n
//   /mp/<plural>/<index>              -> /mp/<plural>/<index> i value
//   /mp/<plural>/<index> i|f value    -> sets it, then the same reply
//   /mp/<plural>/<index>/press        -> /mp/<plural>/<index>/press
//   /mp/<plural>/<index>/describe     -> the address, then name and value pairs
//   /mp/<plural>/dump [i first [i n]] -> /mp/<plural>/dump i first i value ...
//   /mp/replyto [s host] i port       where replies and events go
//   /mp/subscribe [s plural ...]      events, as /mp/<plural>/<index> i value
//   /mp/unsubscribe [s plural ...]
//
// The program's properties go by name, /mp/program/masterGain, and so may
// commands, /mp/commands/panic/press. An error goes to /mp/error s address
// i code s name. Everything runs on the message thread.
#pragma once

#include "ControlModel.h"

#include <juce_events/juce_events.h>
#include <juce_osc/juce_osc.h>

#include <functional>

namespace mp::api {

class OscTransport : private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                     private juce::Timer {
public:
  explicit OscTransport(MasterpieceProcessor& p);
  ~OscTransport() override;

  // Listens on a UDP port; false with the reason when it cannot.
  bool start(int port, juce::String& error);
  void stop();
  bool running() const { return port_ != 0; }
  int port() const { return port_; }
  // Off, a change from the network is refused as it is over HTTP.
  void setAllowChanges(bool on) { allowChanges_ = on; }

  // One message, as if received; and where replies go instead of the
  // network, for tests.
  void handle(const juce::OSCMessage& m);
  std::function<void(const juce::OSCMessage&)> onSend;
  void pollNow() { timerCallback(); }

private:
  void oscMessageReceived(const juce::OSCMessage& m) override;
  void oscBundleReceived(const juce::OSCBundle& b) override;
  void timerCallback() override;
  void send(const juce::OSCMessage& m);
  void error(const juce::String& address, Error e);
  // The index in `part` for this type: a number, or a property or command
  // by name. -1 when it is neither.
  static int indexOf(Type type, const juce::String& part);
  static uint32_t maskOf(const juce::OSCMessage& m);

  ControlModel model_;
  juce::OSCReceiver receiver_;
  juce::OSCSender sender_;
  bool hasTarget_ = false;
  int port_ = 0;
  bool allowChanges_ = true;
  uint32_t mask_ = 0;  // subscribed types, bit n for type n
  int ticks_ = 0;
};

}  // namespace mp::api
