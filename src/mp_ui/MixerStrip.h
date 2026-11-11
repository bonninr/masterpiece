// The Mixer tab's channel strips: a mixing desk's fader with a scale in
// decibels, a lamp meter beside it, mute and solo, the level as a number and
// the strip's name below. One strip per rank, division, group and output pair.
// And the desk fader's look for every other fader that changes the sound.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace mp {

// A mixing desk's fader: a dark slot and a ribbed cap with a line across its
// middle, in the slider's thumb colour. Ticks from the slider's "ticks"
// property, its "unity" tick longer and brighter, with their numbers where
// there is room. The value box at the fader's end shows the exact value, light
// on black, and takes a typed one.
class FaderLook : public juce::LookAndFeel_V4 {
public:
  FaderLook();
  void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float minPos,
                        float maxPos, juce::Slider::SliderStyle style, juce::Slider& s) override;
  // A small potentiometer: a dark knob with a light pointer, its travel as an
  // arc around it, lit up to the value.
  void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle,
                        float endAngle, juce::Slider& s) override;
  int getSliderThumbRadius(juce::Slider& s) override;
  juce::Label* createSliderTextBox(juce::Slider& s) override;

  // The cap's size along the travel and across it. A vertical fader shorter
  // than kShortTravel takes the short cap.
  static constexpr int kCapLength = 40, kCapWidth = 26;
  static constexpr int kCapLengthShort = 24, kShortTravel = 220;
  static constexpr int kCapLengthHorizontal = 16;
  // One look for every fader, kept for the program's life.
  static FaderLook& shared();
  // The length of a slider's cap along its travel.
  static int capLength(const juce::Slider& s);
};

// The cap lines' colours: grey for a channel, amber for a division, cyan for a
// group, red for an output and the master.
namespace fader {
inline const juce::Colour kChannel{0xffe6e6e6}, kDivision{0xfff0b030}, kGroup{0xff4fc3d9}, kMaster{0xffe0483c};
// For a fader with no level that stands for "as it was".
inline constexpr double kNoUnity = 1.0e300;
// Five ticks from lo to hi, evenly.
inline std::vector<double> evenTicks(double lo, double hi) {
  return {lo, lo + (hi - lo) * 0.25, lo + (hi - lo) * 0.5, lo + (hi - lo) * 0.75, hi};
}
}  // namespace fader

// Gives a slider the desk fader's look: ticks at `ticks`, in the slider's own
// values, `unity` the long one, and the cap's line in `colour`.
void styleFader(juce::Slider& s, const std::vector<double>& ticks, double unity,
                juce::Colour colour = fader::kChannel);

// A column of lamps lit to a level. 2 dB a lamp from -50 dBFS to full scale:
// green to -18, amber above. An output's meter shows red from -6, where its
// signal comes near full scale; a balance meter stops at amber. The highest
// lamp of the last moment is held a little while.
class StripMeter : public juce::Component, public juce::SettableTooltipClient {
public:
  StripMeter();
  // A linear level, 1 is full scale. Repaints only when a lamp changes.
  void setLevel(float level);
  void setShowsClipping(bool on) { showsClipping_ = on; }
  void paint(juce::Graphics& g) override;

  static constexpr int kLamps = 26;
  static constexpr float kFloorDb = -50.0f, kStepDb = 2.0f;
  // How many lamps a level lights.
  static int lampsFor(float level);

private:
  int lit_ = 0, held_ = 0, holdTicks_ = 0;
  bool showsClipping_ = false;
};

class ChannelStrip : public juce::Component {
public:
  enum class Kind { Rank, Division, Group, Pair };
  ChannelStrip(Kind kind, const juce::String& name, const juce::String& tooltip);
  ~ChannelStrip() override;

  // The level's value, 0..kTop of the live levels; the readout follows.
  void setLevel(int value);
  int level() const;
  void setMeter(float level) { meter_.setLevel(level); }
  void setMuted(bool on) { mute.setToggleState(on, juce::dontSendNotification); }
  void setSoloed(bool on) { solo.setToggleState(on, juce::dontSendNotification); }
  // A control above the readout, such as a rank's pair; the strip shows it
  // and lays it out, the caller keeps it.
  void setTopControl(juce::Component* c);
  // Narrow: the scale's ticks without numbers, the readout without its unit.
  void setNarrow(bool on);
  const juce::String& name() const { return name_; }

  std::function<void(int)> onLevel;
  std::function<void(bool)> onMute, onSolo;

  juce::Slider fader{juce::Slider::LinearVertical, juce::Slider::NoTextBox};
  juce::TextButton mute{"M"}, solo{"S"};
  juce::TextButton midi;

  void paint(juce::Graphics& g) override;
  void resized() override;

  static constexpr int kWidth = 78, kNarrowWidth = 50;
  static constexpr int kMinHeight = 300;
  static int widthFor(bool narrow) { return narrow ? kNarrowWidth : kWidth; }
  // Where 0 dB stands on the travel, from the bottom: three quarters up, as on
  // a desk, the boost above it and the cut below.
  static constexpr double kUnityAt = 0.75;

private:
  void showValue();
  Kind kind_;
  juce::String name_;
  juce::Label value_, label_;
  StripMeter meter_;
  juce::Component* top_ = nullptr;
  juce::Rectangle<int> scale_;
  bool narrow_ = false;
};

}  // namespace mp
