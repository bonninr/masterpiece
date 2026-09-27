// Temperament, pitch and transposer: the tuning a player changes between
// pieces, over the organ's own. Opened from the top bar's Tuning button, whose
// label always says what is in force, so the panel itself can stay closed.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../mp_audio/MasterpieceProcessor.h"

#include <memory>
#include <string>
#include <vector>

namespace mp::ui {

class TuningPanel : public juce::Component, private juce::Timer {
public:
  explicit TuningPanel(MasterpieceProcessor& p);
  ~TuningPanel() override;
  void resized() override;
  void paint(juce::Graphics& g) override;

  // One line for the top bar: "Werckmeister III · A 415 · +2".
  static juce::String summary(const MasterpieceProcessor& p);

private:
  void timerCallback() override;
  void refresh();
  void fillTemperaments();

  MasterpieceProcessor& proc_;
  juce::Label temperamentLabel_, pitchLabel_, transposeLabel_;
  juce::ComboBox temperament_;
  std::vector<std::string> choices_;  // by item id - 1
  juce::TextButton scala_{"Scala file..."};
  std::unique_ptr<juce::FileChooser> chooser_;

  juce::Slider pitch_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::Label pitchNote_;
  juce::TextButton native_, a415_{"415"}, a440_{"440"}, a442_{"442"};

  juce::TextButton down_{"-"}, up_{"+"}, zero_{"0"};
  juce::Label transposeValue_;

  juce::Label note_;
};

} // namespace mp::ui
