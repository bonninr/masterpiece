// A console profile (#866): the tabs, couplers, tremulants and pistons of a
// player's own console, with their printed names and the MIDI each sends,
// saved once and mapped onto every organ.
//
// A tab is matched to an organ's stop by what it is: its family, read from
// its name through a dictionary of stop names in several languages (Bourdon,
// Gedackt, Bordone and Stopped Diapason are one family), its footage, and
// the keyboard it sits on, counted from the pedal (0) up. A coupler is
// matched by the keyboards it joins and its pitch, a tremulant by its
// keyboard, a piston by its name. Each tab goes to one control of the organ
// at most, best matches first, and each match carries a score: a strong one
// can be used as it is, a weak one is for the player to confirm.
//
// JUCE-free: the matching is plain functions over names and numbers, tested
// in the fast loop.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../mp_core/OrganModel.h"
#include "MidiMap.h"

namespace mp {

enum class ToneFamily {
  Unknown,
  Principal,
  OpenFlute,
  StoppedFlute,
  String,
  Celeste,
  ChorusReed,
  SoloReed,
  Regal,
  Mixture,
  Mutation,
  Cornet,
  Percussion,
};

// What a stop's name says about it.
struct StopTraits {
  ToneFamily family = ToneFamily::Unknown;
  double footage = 0.0;  // 8.0, 2.667; 0 when the name states none
  int ranks = 0;         // a mixture's ranks, 0 when the name states none
};

// Lower case, accents off, everything but letters, digits, '/' and spaces
// made a space; the fraction signs written out ("2⅔" is "2 2/3").
std::string foldName(const std::string& name);
StopTraits stopTraits(const std::string& name);
// How alike two families sound, 0 to 1: 1 for the same family.
double familyCloseness(ToneFamily a, ToneFamily b);
const char* toneFamilyName(ToneFamily f);

enum class ConsoleTabKind { Stop, Coupler, Tremulant, Piston };
const char* tabKindName(ConsoleTabKind k);
bool tabKindFromName(const std::string& s, ConsoleTabKind& out);

// One control: a tab of the player's console in a profile, or a control of
// the loaded organ to match it against.
struct ConsoleTab {
  ConsoleTabKind kind = ConsoleTabKind::Stop;
  std::string label;  // the printed name: "Bourdon 8"
  // The keyboard it sits on: 0 the pedal, 1 the first manual from the
  // bottom, -1 when not known. A coupler: the keyboard it couples onto.
  int keyboard = -1;
  int fromKeyboard = -1;  // a coupler: the keyboard it brings in, -1 not known
  int shift = 0;          // a coupler: -12 for a 16' coupler, 12 for a 4'
  Id switchId = 0;        // on an organ: the switch it works
  bool operator==(const ConsoleTab& o) const {
    return kind == o.kind && label == o.label && keyboard == o.keyboard && fromKeyboard == o.fromKeyboard &&
           shift == o.shift && switchId == o.switchId;
  }
};

// The profile: its tabs, and what each sends as switch bindings in a MIDI map
// of its own, the target number being the tab's index plus one. The map
// numbers its devices itself; MidiMap::bindFrom names them again in the
// organ's map.
struct ConsoleProfile {
  std::string name;
  std::vector<ConsoleTab> tabs;
  MidiMap messages;

  static Id targetOf(size_t tab) { return static_cast<Id>(tab + 1); }
  std::vector<MidiBinding> bindingsOf(size_t tab) const;
  void setBindingsOf(size_t tab, const std::vector<MidiBinding>& bindings);
  // A tab taken out: the later tabs' messages move down one.
  void removeTab(size_t tab);

  std::string toText() const;
  bool fromText(const std::string& text);
  // Changes whenever the tabs or the messages do: an organ remembers the
  // stamp it was last mapped from, and is offered the profile again only
  // after a change.
  std::string stamp() const;
};

// 0 to 1: how well a tab fits a control of the organ. 0 when they cannot
// be the same thing: another kind, another keyboard, another footage.
double matchScore(const ConsoleTab& tab, const ConsoleTab& control);

struct ProfileMatch {
  size_t tab = 0;      // in the profile
  size_t control = 0;  // in the organ's controls
  double score = 0.0;
  bool strong = false;
  // Found in the last pass: a stop left over, given the stop left on its
  // keyboard and at its pitch whose family sounds nearest. Always for the
  // player to confirm.
  bool related = false;
};
constexpr double kWeakestMatch = 0.3;
constexpr double kStrongMatch = 0.8;
// How near two families must sound for the last pass: a principal and a
// stopped flute (0.25) yes, a principal and a trumpet (0.1) never.
constexpr double kRelatedSound = 0.25;
// Each tab to one control at most and each control to one tab, best scores
// first; pairs under kWeakestMatch stay unmatched. Then the stops still
// unmatched, by sound: each to the nearest family left on its keyboard and
// at its footage, at kRelatedSound or nearer. familyCloseness is the one
// measure of sound, so a measure learned from recordings can replace it.
// In profile order.
std::vector<ProfileMatch> matchProfile(const std::vector<ConsoleTab>& tabs,
                                       const std::vector<ConsoleTab>& controls);

// The organ's side, from its model.
// The keyboard a division is played from: 0 the pedal, 1.. the manuals,
// -1 not known. The organ's own channel convention first (Hauptwerk's
// default assignment, which a GrandOrgue organ carries too), then a CODM
// organ's manual number, then a name that says Pedal or a roman numeral.
int divisionKeyboard(const OrganModel& m, Id divisionId);
// The same for a keyboard of the organ.
int keyboardNumber(const OrganModel& m, Id keyboardId);
// A coupler's keyboards and pitch, from the key action its switch enables,
// followed through a few switch linkages. False when none is found.
bool couplerKeyboards(const OrganModel& m, Id switchId, int& onto, int& from, int& shift);
// The same read from a name: "II/I", "I/P", "Swell to Great", "Sw-Gt 16".
// `divisions` gives the organ's own division names and their keyboards.
bool couplerKeyboardsFromName(const std::string& name,
                              const std::vector<std::pair<std::string, int>>& divisions, int& onto,
                              int& from, int& shift);
// A coupler's pitch read from its name: -12 for "16" or "Sub", 12 for "4",
// "Super" or "Octave", 0 for the unison.
int couplerShiftFromName(const std::string& name);
// A keyboard named in a label: Pedal, a roman numeral, or one of `divisions`.
int keyboardFromName(const std::string& name, const std::vector<std::pair<std::string, int>>& divisions);

}  // namespace mp
