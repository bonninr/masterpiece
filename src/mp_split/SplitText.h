// The split keyboard in files: an organ's layout, the shared presets and the
// learned controls. Plain lines a person can read, every one optional, so a
// file from a later version loses only what this one does not know.
//
// An organ's file:
//   splits 2 53 72 84          how many, then the three points
//   zone 0 pedal none 0 0      zone, role, layered role, octave, transpose
//   transpose 0
//   follow 0
//   bass 1 59 1 off 1500       on, top key, also on the manual, sustain, ms
//   preset 2                   the preset it was recalled from, if any
//
// The shared file holds the player's presets, each "[preset N] name" followed
// by layout lines, and the split's own lines:
//   on 1
//   source <input name>        absent: any input
//   control next note 1 96 <input name>
#pragma once
#include <array>
#include <functional>
#include <string>

#include "SplitKeyboard.h"

namespace mp::split {

std::string layoutToText(const Layout& l);
// Lines this does not know are skipped; what is missing keeps `l`'s value.
void layoutFromText(const std::string& text, Layout& l);

const char* roleName(Role r);
Role roleFromName(const std::string& name);
const char* controlName(int control);

struct Shared {
  SplitKeyboard::Presets presets;
  std::string names[kPresetCount];  // the player's own; factory names come from factoryName()
  std::array<Trigger, kControlCount> triggers{};
  bool on = false;
  std::string source;  // empty: any input

  Shared();
};

// Input names for the controls' devices: an id is meaningful only in one run.
using NameOf = std::function<std::string(int device)>;
using IdOf = std::function<int(const std::string& name)>;

std::string sharedToText(const Shared& s, const NameOf& nameOf);
Shared sharedFromText(const std::string& text, const IdOf& idOf);

// The organ's file: its layout and the preset it came from.
std::string organToText(const Layout& l, int preset);
void organFromText(const std::string& text, Layout& l, int& preset);

}  // namespace mp::split
