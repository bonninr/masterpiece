// What reaches a manual, in full.
//
// A channel number is nowhere near enough for a real rig. This window is the
// one place a manual's MIDI is set, reached by right-clicking the manual or
// from Organ settings' MIDI tab (#850). It follows GrandOrgue's manual
// receiver — the one place any of this is written down — with a field per
// thing a console actually does wrong:
//
//   device + channel   Two keyboards both send note 60 on channel 1.
//   key range          Splitting ONE keyboard across two manuals, or taking a
//                      bottom octave as a pedal.
//   transpose          The same split moved back into range.
//   velocity window    A console that bottoms out at 100, or sends a fixed 64.
//                      Inverted (low above high) reverses the sense, which is
//                      how a normally-closed contact is read.
//   no velocity        Tracker action: down or not down, and a velocity-
//                      switched sample set would pick the wrong layer.
//   short octave       Historic keyboards whose bottom octave omits the
//                      accidentals and puts other notes on those keys.
//   debounce           Old contacts chatter and retrigger the pipe.
//
// A manual can have SEVERAL of these. That is the point of the list: one
// physical keyboard split across two manuals is two bindings, and so is a
// manual played from two consoles.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"

namespace mp::ui {

class ManualDialog : public juce::Component, private juce::Timer {
public:
  ManualDialog(MasterpieceProcessor& p, Id keyboardId);
  ~ManualDialog() override;

  void resized() override;
  void paint(juce::Graphics& g) override;

  // Open it in its own window, sized to fit. `tab` is the tab shown first:
  // Keys, Pistons or Send.
  enum class Tab { Keys, Pistons, Send };
  static void show(MasterpieceProcessor& p, Id keyboardId, Tab tab = Tab::Keys);

  // The one way a manual's MIDI is written out, wherever it is shown (#850):
  // the Keys tab's rows, and Organ settings' MIDI tab, which lists every
  // manual and opens this window, as GrandOrgue's list of MIDI objects does.
  //   describeKeys   one row: "Any device  ch 2  keys C4 (60) to C7 (96)  transposed -24"
  //   describeManual every row of the manual, one per line, or the organ's
  //                  default channel when the player has set none
  //   describePistons every piston that brings the manual to a keyboard, one
  //                  per line, in the Pistons tab's words
  static juce::String describeKeys(const MasterpieceProcessor& p, const MidiMap::KeyboardBinding& b);
  static juce::StringArray describeManual(const MasterpieceProcessor& p, Id keyboardId);
  static juce::StringArray describePistons(const MasterpieceProcessor& p, Id keyboardId);
  // A box of the MIDI devices, "Any device" first, with `selected` (a name the
  // MIDI map gives) chosen; a saved device not connected now stays listed.
  // deviceInBox gives back the chosen name, empty for any device.
  static void fillDeviceBox(juce::ComboBox& box, const MasterpieceProcessor& p, const std::string& selected);
  static std::string deviceInBox(const juce::ComboBox& box, const MasterpieceProcessor& p);

private:
  void timerCallback() override;
  void rebuildList();
  void addBinding();
  void applyEdits();
  // The binding being edited, or -1 when the list is empty.
  int selected() const;

  MasterpieceProcessor& proc_;
  Id keyboardId_;

  juce::Label title_;
  juce::ListBox list_;
  std::unique_ptr<juce::ListBoxModel> model_;
  // In the empty list: the organ's channel that plays the manual meanwhile,
  // as Organ settings says it.
  juce::Label empty_;
  juce::TextButton add_{TRANS("Add")}, remove_{TRANS("Remove")}, learn_{TRANS("Learn from a key")};

  juce::Label deviceLabel_, channelLabel_, rangeLabel_, transposeLabel_,
      velocityLabel_, debounceLabel_;
  juce::ComboBox device_, channel_;
  juce::Slider lowKey_, highKey_, transpose_, lowVel_, highVel_, debounce_;
  juce::ToggleButton ignoreVel_{TRANS("Ignore velocity (tracker action)")};
  juce::ToggleButton shortOctave_{TRANS("Short octave")};
  juce::Label note_;
  // Set while the controls are being filled in, so writing to them does not
  // read straight back as an edit.
  bool loading_ = false;
  bool learning_ = false;  // a learn for this manual was running at the last tick
};

} // namespace mp::ui
