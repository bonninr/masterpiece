// jOrgan dispositions, read as jOrgan writes them.
//
// A disposition (*.disposition) is an XML list of elements -- keyboards,
// stops, couplers, ranks, sounds, filters, consoles and more -- each with a
// numeric id, a name, the ids of the elements it references, its properties,
// and MIDI messages written in jOrgan's message language (MPL). This reads
// the list as it stands and evaluates MPL messages; what the elements mean
// for an organ is JOrganImport's business.
//
// MPL, as jOrgan's jorgan.midi.mpl package defines it: a message is a tuple
// of commands, one per MIDI byte, separated by commas; a command is a chain
// of steps separated by "|". "set 176" gives a byte, "set pitch" the value
// of a variable, "set program 0" the variable or 0 when it is not set;
// "add", "sub", "mult", "div", "mod", "min", "max" work on the value the same
// way; "get pitch" stores the value; "equal", "greater" and the other
// conditions pass the value on or stop the chain.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace mp {

struct JOrganReference {
  int id = 0;
  int x = 0, y = 0;  // a console's location of the element, when it is one
  bool located = false;
};

struct JOrganElement {
  int id = 0;
  std::string type;         // the tag: "stop", "rank", "fluidsynth.fluidsynthSound"...
  std::string name, description, style;
  double zoom = 1.0;
  std::map<std::string, std::string> props;  // the element's other child elements, as text
  std::vector<JOrganReference> references;
  std::vector<std::pair<std::string, std::string>> messages;  // (kind, MPL text)

  std::string prop(const std::string& key, const std::string& fallback = "") const;
  int num(const std::string& key, int fallback) const;
  double real(const std::string& key, double fallback) const;
  // The messages of one kind, as "rank-engaged", in file order.
  std::vector<std::string> messagesOf(const std::string& kind) const;
};

struct JOrganDisposition {
  std::string version;
  std::vector<JOrganElement> elements;  // in file order
  std::map<int, size_t> byId;

  const JOrganElement* find(int id) const;
};

// Read a disposition from its text. False, with the reason, when it is not one.
bool readJOrganDisposition(const std::string& text, JOrganDisposition& out, std::string& error);

// MPL variables by name.
using MplVariables = std::map<std::string, double>;

// Evaluate one command chain on `value`, as jOrgan does. NaN when a
// condition stops it, or when it needs a variable that is not set.
double evaluateMpl(const std::string& chain, double value, MplVariables& vars);

// An output message's bytes, every command evaluated from nothing. A byte
// that does not evaluate is -1.
std::vector<int> mplBytes(const std::string& message, const MplVariables& vars = {});

// An input message's first command when it is a plain "equal N": the status
// byte it matches, or -1.
int mplMatchedStatus(const std::string& message);

}  // namespace mp
