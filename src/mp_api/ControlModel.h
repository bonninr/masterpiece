// The control protocol's model of a running Masterpiece (see
// protocol/control.yaml): every object a client can read or change, by type
// and by an index from 0 that holds for one organ load. Every transport -- HTTP,
// SysEx, OSC -- reads and changes the organ through this and nothing else.
//
// Message thread only, as the window is. While an organ loads the model is
// the loader's, and every call answers busy.
#pragma once

#include "ControlProtocol.h"

#include <juce_core/juce_core.h>

#include "../mp_control/PlayerCombinations.h"

#include <unordered_map>

#include <cstdint>
#include <string>
#include <vector>

namespace mp {
class MasterpieceProcessor;
}

namespace mp::api {

class ControlModel {
public:
  explicit ControlModel(MasterpieceProcessor& p);

  // The session: moves on at each organ load. 0 before the first.
  int session();
  // Every object of a type, with its properties and value, as JSON. Switches
  // and controls as the console shows them, unless `all`: the organ's
  // internal ones too.
  juce::var list(Type type, Error& error, bool all = false);
  juce::var describe(Type type, int index, Error& error);
  // A value; false with the error when there is none.
  bool get(Type type, int index, int& value, Error& error);
  // A Set; `value` comes back as the value now. For a switch, 2 toggles.
  Error set(Type type, int index, int& value);
  Error press(Type type, int index);
  // Every value of a type in index order.
  juce::var dump(Type type, Error& error);
  // The program object, every property with its value.
  juce::var program();
  Error setProgram(ProgramProp prop, int value);
  juce::var identity();

  // What changed since the last call: compared with the values seen then.
  // The first call after a load reports the session and the organ state
  // only, and clients count and dump again. The readings that move all the
  // time (sounding voices, load progress, late blocks) only when `readings`.
  struct Change {
    Type type;
    int index;
    int value;
  };
  void poll(std::vector<Change>& out, bool readings);

  int count(Type type);

private:
  struct SwitchEntry {
    int64_t id = 0;
    std::string name;
    Role role = Role::Other;
    int division = -1;
    bool latching = true;
    bool available = true;
    bool console = true;
  };
  struct NamedEntry {
    int64_t id = 0;
    std::string name;
    int division = -1;
    bool console = true;
  };
  bool consoleObject(Type type, int index) const;
  // The switches a stored piston would draw, as indices into switches_.
  juce::var holds(const std::vector<PlayerCombinations::Change>& changes) const;
  std::unordered_map<int64_t, int> switchIndex_;  // switch id -> index
  Error quitOrPowerOff(bool powerOff);
  // Builds the lists for the organ now loaded; nothing while one loads.
  bool ensure(Error& error);
  void rebuild();
  int divisionIndex(int64_t divisionId) const;
  juce::var object(Type type, int index);
  int programValue(ProgramProp prop);
  int valueOf(Type type, int index);

  MasterpieceProcessor& proc_;
  int builtFor_ = -1;  // organLoads() the lists were built for
  // When this model was made, which is when the program started: quitting and
  // shutting down are refused for the first two minutes.
  const juce::uint32 startedMs_ = juce::Time::getMillisecondCounter();
  std::vector<SwitchEntry> switches_;
  std::vector<NamedEntry> controls_, divisions_, keyboards_, ranks_, windchests_;
  struct PipeEntry {
    int64_t id = 0;
    int rank = 0;  // index into ranks_
    int key = 0;
  };
  std::vector<PipeEntry> pipes_;
  struct EnclosureEntry {
    int64_t id = 0, controlId = 0;
    std::string name;
  };
  std::vector<EnclosureEntry> enclosures_;
  struct TremulantEntry {
    int64_t id = 0, switchId = 0;  // the switch the player moves for it
    std::string name;
    int rate = 0, depth = 0;
  };
  std::vector<TremulantEntry> tremulants_;
  int controlIndex(int64_t controlId) const;
  std::vector<NamedEntry> divisionals_;  // id = the division's id, division = piston number
  std::vector<std::string> temperaments_;  // "" is the organ's own

  // For poll(): the values last reported, per type.
  int polledFor_ = -1;
  std::vector<std::vector<int>> seen_;
  std::vector<int> seenProgram_;
};

}  // namespace mp::api
