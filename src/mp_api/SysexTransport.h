// The control protocol over MIDI System Exclusive (protocol/control.yaml,
// encodings.sysex), for a console with a MIDI cable and no network: requests
// read on the MIDI inputs, replies and events sent on the MIDI output.
//
// handle() runs on the message thread; isOurs() on any, so a MIDI callback
// can keep these messages away from the organ.
#pragma once

#include "ControlModel.h"
#include "SysexCodec.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

namespace mp::api {

class SysexTransport : private juce::Timer {
public:
  using Send = std::function<void(const juce::MidiMessage&)>;
  SysexTransport(MasterpieceProcessor& p, Send send);
  ~SysexTransport() override;

  // This program's unit number (0-126); a message to 127 is for every unit.
  void setUnit(int unit) { unit_ = unit; }
  int unit() const { return unit_; }

  // A message with this protocol's header, whatever its unit.
  static bool isOurs(const juce::MidiMessage& m);
  // Carries out one request and sends the reply.
  void handle(const juce::MidiMessage& m);
  // Looks for changes and sends them to a subscriber; on a timer, or by hand
  // in tests.
  void pollNow() { timerCallback(); }

private:
  void timerCallback() override;
  void reply(Op op, int sequence, const std::vector<uint8_t>& payload);
  void error(Op op, int sequence, Error e);

  MasterpieceProcessor& proc_;
  ControlModel model_;
  Send send_;
  int unit_ = 0;
  uint32_t mask_ = 0;  // subscribed types, bit n for type n
  int ticks_ = 0;
};

}  // namespace mp::api
