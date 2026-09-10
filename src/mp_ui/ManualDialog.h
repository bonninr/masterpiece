// What reaches a manual, in full.
//
// A channel number is nowhere near enough for a real rig, and the settings tab
// only offers that. This is the rest of it, following GrandOrgue's manual
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

  // Open it in its own window, sized to fit.
  static void show(MasterpieceProcessor& p, Id keyboardId);

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
  juce::TextButton add_{"Add"}, remove_{"Remove"}, learn_{"Learn from a key"};

  juce::Label deviceLabel_, channelLabel_, rangeLabel_, transposeLabel_,
      velocityLabel_, debounceLabel_;
  juce::ComboBox device_, channel_;
  juce::Slider lowKey_, highKey_, transpose_, lowVel_, highVel_, debounce_;
  juce::ToggleButton ignoreVel_{"Ignore velocity (tracker action)"};
  juce::ToggleButton shortOctave_{"Short octave"};
  juce::Label note_;
  // Set while the controls are being filled in, so writing to them does not
  // read straight back as an edit.
  bool loading_ = false;
};

} // namespace mp::ui
