// The window an organ loads behind.
//
// A large set takes minutes off a slow disk, and until now the message thread
// did that work itself: the window went white, Windows offered to close the
// unresponsive program, and there was no way to say "wrong organ, stop".
//
// So the load runs on its own thread and this watches it. It is modal in the
// sense that matters — you cannot play an organ that is not loaded yet — but
// the message loop keeps running underneath, so the window paints, drags and
// answers.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

#include "../mp_audio/MasterpieceProcessor.h"

#include <functional>

namespace mp::ui {

class LoadingDialog : public juce::Component, private juce::Timer {
public:
  LoadingDialog(MasterpieceProcessor& proc, const juce::String& organName);
  ~LoadingDialog() override;

  void resized() override;
  void paint(juce::Graphics& g) override;

  // Called on the message thread when the player presses Cancel. The load
  // itself stops at the next file boundary; this is only the request.
  std::function<void()> onCancel;

private:
  void timerCallback() override;

  // How long is left, from the rate this load is actually achieving. Not a
  // guess from file counts or disk specs: those are wrong by more than the
  // thing they are estimating.
  juce::String estimateRemaining();

  MasterpieceProcessor& proc_;
  LoadProgress::Phase lastPhase_ = LoadProgress::Phase::Idle;
  double phaseStartMs_ = 0.0;
  // Smoothed, because a raw estimate recomputed ten times a second jitters by
  // whole minutes and reads as nonsense even when the average is right.
  double etaSeconds_ = -1.0;
  juce::Label title_;
  juce::Label phase_;
  juce::Label detail_;
  juce::ProgressBar bar_;
  double barValue_ = 0.0;
  juce::TextButton cancel_{"Cancel"};
  bool cancelling_ = false;
};

}  // namespace mp::ui
