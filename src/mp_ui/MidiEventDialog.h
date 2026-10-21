// One object's MIDI, in its own window: GrandOrgue's MIDI event dialog.
//
// Right-click a drawstop, a coupler, a piston or a swell shoe on the console
// and choose "MIDI window...". Three tabs:
//
//   Receive   every message that works it. Several are normal: a rocker
//             tab sends one note to draw a stop and another to cancel it,
//             and a stop can be worked from two consoles. Each row has its
//             own behaviour (toggle, held, draws, cancels) or, for a shoe,
//             its own range, and a Listen button that fills it from the
//             console.
//   Send      what the console is sent when the object changes: the lamp in
//             a drawstop, an LED by a piston, a motorised fader.
//   Shortcut  a key on the computer's keyboard.
//
// Manuals keep their own window (ManualDialog), which takes the Send tab
// from here: the keys played on a manual can be sent on to a sound module.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"

namespace mp::ui {

class MidiEventDialog {
public:
  // `kind` is Switch, ContinuousControl, or one of the program's own buttons
  // (a player general, a cancel, the setter, the stepper: #226), which the
  // dialog treats like a switch and titles with `name`.
  static void show(MasterpieceProcessor& p, MidiTargetKind kind, Id id,
                   const juce::String& name = {});

  // What a player calls the object (#346): for a switch, the stop or
  // tremulant it works, followed through the organ's switch linkages, since
  // the switch behind a drawn knob is often named only by a number. Shown at
  // the top of the right-click menu and in the MIDI window's title.
  static juce::String targetName(const MasterpieceProcessor& p, MidiTargetKind kind, Id id);
  static juce::String switchName(const OrganModel& m, Id switchId);
  // A mapping's message, for a MIDI menu's header: "CC 7 ch 1".
  static juce::String describeBinding(const MidiBinding& b);

  // The Send tab on its own, for the manual window.
  static std::unique_ptr<juce::Component> makeSendPanel(MasterpieceProcessor& p,
                                                        MidiTargetKind kind, Id id);
};

} // namespace mp::ui
