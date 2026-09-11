// First-run wizard: the three things that stop a new installation making a
// sound, in the order they stop it.
//
// This is not a tour. Every step is a decision the program genuinely cannot
// make for the player, and each one is skippable because a player who already
// knows their rig should not be made to click through it:
//
//   1. the audio device        nothing is audible without one
//   2. the MIDI input          and which channel plays which manual, which no
//                              organ file can guess
//   3. an organ                the sample set lives wherever the player put it
//
// Shown once, when there is no global settings file yet, and reachable again
// afterwards — a rig changes, and "run it again" should not mean deleting
// configuration to trick the program into offering it.
#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"

#include <functional>
#include <memory>

namespace mp::ui {

class WizardPanel : public juce::Component {
public:
  WizardPanel(MasterpieceProcessor& proc, juce::AudioDeviceManager& devices);

  void resized() override;
  void paint(juce::Graphics& g) override;

  // Called when the wizard finishes or is skipped, so the caller can record
  // that it has been offered and need not appear again unasked.
  std::function<void()> onFinished;
  // Asks the host to open an organ, because the file dialog and what happens
  // after a load belong to the application, not to this panel.
  std::function<void()> onOpenOrgan;

  // Whether this installation has been through the wizard. A file the player
  // has never had is the only honest signal of a first run.
  static bool isFirstRun(const MasterpieceProcessor& proc);

private:
  void showStep(int step);
  void finish();

  MasterpieceProcessor& proc_;
  juce::AudioDeviceManager& devices_;

  int step_ = 0;
  juce::Label title_;
  juce::Label body_;
  // Step 1 embeds JUCE's own device selector rather than reimplementing it:
  // the list of drivers, rates and buffer sizes is the operating system's
  // answer, and a second opinion about it would only ever be wrong.
  std::unique_ptr<juce::AudioDeviceSelectorComponent> audio_;
  juce::Label midiHeading_;
  std::vector<std::unique_ptr<juce::ToggleButton>> midiInputs_;
  juce::Label midiNone_;
  juce::TextButton openOrgan_{"Choose an organ..."};
  juce::Label organStatus_;

  juce::TextButton back_{"Back"};
  juce::TextButton next_{"Next"};
  juce::TextButton skip_{"Skip setup"};
  juce::Label progress_;
};

}  // namespace mp::ui
