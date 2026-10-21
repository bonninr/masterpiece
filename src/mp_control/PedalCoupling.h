// "Pedal combinations coupled": the French combinaison coupler, as English
// consoles name it. While it is on, a divisional of the coupled manual
// (normally the Great) also recalls the Pedal divisional of the same number,
// the setter stores both, and the manual's cancel cancels the Pedal too.
//
// This works out which two of the player's divisions it ties: the Pedal, and
// the manual whose pistons carry it, the first manual. Hauptwerk-format organs
// say which keyboard is the pedal by its default assignment code (1 is the
// pedal, 2 the first manual) more often than by name; a division is linked to
// its keyboard by the keyboard's division hint or the division's own list.
//
// JUCE-free and engine-free, so the fast tests can check it.
#pragma once
#include "../mp_core/OrganModel.h"
#include "PlayerCombinations.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace mp {

struct CoupledDivisions {
  Id pedal = 0;
  Id manual = 0;
  bool valid() const { return pedal != 0 && manual != 0 && pedal != manual; }
};

namespace pedal_coupling {

// Pedal in the languages organs are labelled in: pedal, pedale, pedaal,
// p\xc3\xa9dale, pedalier, and the short "Ped." of a jamb.
inline bool namedPedal(const std::string& name) {
  std::string n;
  for (char c : name) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (const char* word : {"pedal", "pedaal", "p\xc3\xa9" "dal", "ped."})
    if (n.find(word) != std::string::npos) return true;
  return n == "ped" || n == "p";
}

}  // namespace pedal_coupling

inline CoupledDivisions coupledDivisions(const OrganModel& model,
                                         const std::vector<PlayerCombinations::Division>& divisions) {
  struct Row {
    Id id;
    int code;  // the lowest assignment code of its keyboards; 1000 for none
    bool pedal;
  };
  std::vector<Row> rows;
  for (const auto& d : divisions) {
    int code = 1000;
    bool pedal = pedal_coupling::namedPedal(d.name);
    const auto consider = [&](const Keyboard& kb) {
      if (kb.assignmentCode > 0) code = std::min(code, kb.assignmentCode);
      if (kb.assignmentCode == 1 || pedal_coupling::namedPedal(kb.name)) pedal = true;
    };
    for (const auto& [kid, kb] : model.keyboards)
      if (kb.primaryDivisionHint == d.divisionId) consider(kb);
    if (const auto it = model.divisions.find(d.divisionId); it != model.divisions.end())
      for (Id kid : it->second.keyboardIds)
        if (const auto kb = model.keyboards.find(kid); kb != model.keyboards.end()) consider(kb->second);
    rows.push_back({d.divisionId, code, pedal});
  }
  CoupledDivisions out;
  for (const auto& r : rows)
    if (r.pedal) {
      out.pedal = r.id;
      break;
    }
  // The first manual: the lowest assignment code that is not the pedal's, then
  // the player's own order of divisions.
  const Row* best = nullptr;
  for (const auto& r : rows)
    if (!r.pedal && r.id != out.pedal && (best == nullptr || r.code < best->code)) best = &r;
  if (best != nullptr) out.manual = best->id;
  return out;
}

}  // namespace mp
