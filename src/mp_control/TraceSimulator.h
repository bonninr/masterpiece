// A headless trace of an organ's network: load a definition, play a script
// of keys, switches, stops, pistons, controls and waits through one
// OrganNetwork, and write down every switch and control that moved, with the
// time on the simulator's clock. The same script through both networks gives
// two traces whose difference is exactly where the event network departs
// from the engine's behaviour (diffTraces).
//
// Script, one command per line, '#' starts a comment:
//   key <keyboard> <note> down|up [velocity]
//   switch <id> on|off
//   stop <stopId> on|off          the stop's controlling switch
//   piston <switchId>             pressed and let go
//   control <id> <value>
//   wait <ms>                     time passes, in ticks
//   tick <ms>                     the tick, 256 frames at 48 kHz by default
//
// Trace lines: "<ms> <kind> <id> <value>  | <name>", kind one of switch,
// control, division, pipe. Lines at the same time are sorted. The first lines,
// at "init", are the state after load: switches engaged and controls away
// from their declared default.
#pragma once
#include "OrganNetwork.h"

#include <string>
#include <vector>

namespace mp {

struct TraceStep {
  enum class Kind { Key, Switch, Stop, Piston, Control, Wait, Tick };
  Kind kind = Kind::Wait;
  Id id = 0;        // keyboard, switch, stop or control
  int note = 0;     // key
  int value = 0;    // on/off, key down/up, control value, velocity
  double ms = 0.0;  // wait, tick
  std::string text; // the line, for errors
};

struct TraceScript {
  std::vector<TraceStep> steps;
  // False with a message naming the line it could not read.
  static bool parse(const std::string& text, TraceScript& out, std::string& error);
};

struct TraceOptions {
  double tickMs = 256.0 * 1000.0 / 48000.0;
  // Division and pipe lines as well as switches and controls.
  bool sounding = true;
};

std::vector<std::string> runTrace(const OrganModel& model, OrganNetwork::Kind kind, const TraceScript& script,
                                  const TraceOptions& options = {});

struct TraceDiff {
  size_t onlyA = 0, onlyB = 0;
  // "- " for lines only in A, "+ " for lines only in B, time by time.
  std::vector<std::string> lines;
  bool same() const { return onlyA == 0 && onlyB == 0; }
};
// Compare two traces time by time. With `kinds` non-empty, only lines whose
// kind is listed ("switch", "control", ...) take part.
TraceDiff diffTraces(const std::vector<std::string>& a, const std::vector<std::string>& b,
                     const std::vector<std::string>& kinds = {});

} // namespace mp
