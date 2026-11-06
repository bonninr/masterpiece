// SPDX-License-Identifier: GPL-3.0-only
// The tuner: the note that sounds, its distance in cents from the temperament
// in force, and the levels of its first harmonics. It reads the organ's own
// output through the spectrum tap, which runs only while the window shows.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../mp_audio/MasterpieceProcessor.h"

#include <array>
#include <vector>

namespace mp::ui {

class TunerPanel : public juce::Component, private juce::Timer {
public:
  explicit TunerPanel(MasterpieceProcessor& p);
  ~TunerPanel() override;
  void paint(juce::Graphics& g) override;

private:
  void timerCallback() override;
  // The lowest key held on any keyboard, as it sounds after the transposer;
  // -1 when none is.
  int lowestHeldKey() const;

  MasterpieceProcessor& proc_;
  bool tapOpen_ = false;
  long long openedAt_ = 0;
  std::vector<float> samples_;
  // The reading shown, held for a few frames after the sound stops.
  bool showing_ = false;
  int missed_ = 0;
  double hz_ = 0.0;
  std::array<float, SpectrumTap::Pitch::kHarmonics> harmonics_{};
};

class TunerWindow : public juce::DocumentWindow {
public:
  explicit TunerWindow(MasterpieceProcessor& p);
  void closeButtonPressed() override;
};

}  // namespace mp::ui
