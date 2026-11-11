#include "MixerStrip.h"

#include "../mp_audio/LiveLevels.h"

#include <algorithm>
#include <cmath>

namespace mp {

namespace {
// The cap's line and the strip's stripe, by what the strip moves: a desk's
// convention of grey channels, coloured groups and a red master.
juce::Colour kindColour(ChannelStrip::Kind k) {
  switch (k) {
    case ChannelStrip::Kind::Rank: return fader::kChannel;
    case ChannelStrip::Kind::Division: return fader::kDivision;
    case ChannelStrip::Kind::Group: return fader::kGroup;
    case ChannelStrip::Kind::Pair: return fader::kMaster;
  }
  return juce::Colours::white;
}

juce::String minus() { return juce::String(juce::CharPointer_UTF8("\xe2\x88\x92")); }

// A level in decibels as the scale and the readout write it.
juce::String dbText(float db, int decimals) {
  if (std::fabs(db) < 0.05f) return decimals > 0 ? juce::String(0.0, decimals) : "0";
  const juce::String n = juce::String(std::fabs(db), decimals);
  return (db > 0.0f ? juce::String("+") : minus()) + n;
}

// A tick's number: whole numbers plain, others to one place, a true minus.
juce::String tickText(double v) {
  if (std::abs(v) < 1.0e-9) return "0";
  const bool whole = std::abs(v - std::round(v)) < 1.0e-6;
  const juce::String n = whole ? juce::String(juce::roundToInt(std::abs(v))) : juce::String(std::abs(v), 1);
  return (v < 0.0 ? minus() : juce::String()) + n;
}

// The live levels' full and top, as the fader's range takes them.
constexpr double kFullValue = LiveLevels::kFull, kTopValue = LiveLevels::kTop;

const juce::Colour kTick{0xff7d8088}, kUnityTick{0xffeeeeee}, kTickText{0xff9a9da5};
}  // namespace

// ------------------------------------------------------------------ the look

FaderLook::FaderLook() = default;

FaderLook& FaderLook::shared() {
  static FaderLook look;
  return look;
}

int FaderLook::capLength(const juce::Slider& s) {
  if (s.isHorizontal()) return kCapLengthHorizontal;
  if (s.getProperties().contains("longCap")) return kCapLength;
  return s.getHeight() < kShortTravel ? kCapLengthShort : kCapLength;
}

int FaderLook::getSliderThumbRadius(juce::Slider& s) { return capLength(s) / 2; }

void FaderLook::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float startAngle,
                                 float endAngle, juce::Slider& s) {
  const float alpha = s.isEnabled() ? 1.0f : 0.45f;
  const auto box = juce::Rectangle<float>(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                          static_cast<float>(h));
  const float radius = juce::jmin(box.getWidth(), box.getHeight()) / 2.0f - 1.0f;
  const auto centre = box.getCentre();
  const float angle = startAngle + pos * (endAngle - startAngle);

  // The travel, and the part of it up to the value.
  const float arcR = radius - 1.0f;
  juce::Path track;
  track.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, startAngle, endAngle, true);
  g.setColour(juce::Colour(0xff050506).withMultipliedAlpha(alpha));
  g.strokePath(track, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  juce::Path lit;
  lit.addCentredArc(centre.x, centre.y, arcR, arcR, 0.0f, startAngle, angle, true);
  g.setColour(s.findColour(juce::Slider::thumbColourId).withMultipliedAlpha(0.8f * alpha));
  g.strokePath(lit, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

  // The knob, lit from above, with its shadow and a dark rim.
  const float knobR = radius - 4.5f;
  const auto knob = juce::Rectangle<float>(knobR * 2.0f, knobR * 2.0f).withCentre(centre);
  g.setColour(juce::Colours::black.withAlpha(0.5f * alpha));
  g.fillEllipse(knob.translated(0.0f, 1.5f));
  juce::ColourGradient body(juce::Colour(0xff6c6f76), knob.getX(), knob.getY(), juce::Colour(0xff232428),
                            knob.getX(), knob.getBottom(), false);
  body.multiplyOpacity(alpha);
  g.setGradientFill(body);
  g.fillEllipse(knob);
  g.setColour(juce::Colour(0xff0c0c0e).withMultipliedAlpha(alpha));
  g.drawEllipse(knob, 1.0f);

  // The pointer, from near the middle to the rim.
  juce::Path pointer;
  pointer.addRoundedRectangle(-1.0f, -knobR + 1.5f, 2.0f, knobR * 0.6f, 1.0f);
  g.setColour(juce::Colour(0xffeeeeee).withMultipliedAlpha(alpha));
  g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
}

juce::Label* FaderLook::createSliderTextBox(juce::Slider& s) {
  auto* l = LookAndFeel_V4::createSliderTextBox(s);
  l->setColour(juce::Label::textColourId, juce::Colour(0xff9fe0ff));
  l->setColour(juce::Label::backgroundColourId, juce::Colour(0xff0d0e10));
  l->setColour(juce::Label::outlineColourId, juce::Colour(0xff2c2e33));
  l->setColour(juce::Label::textWhenEditingColourId, juce::Colours::white);
  l->setColour(juce::Label::backgroundWhenEditingColourId, juce::Colour(0xff0d0e10));
  l->setColour(juce::TextEditor::highlightColourId, juce::Colour(0xff2f5d7a));
  l->setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
  return l;
}

void FaderLook::drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float minPos,
                                 float maxPos, juce::Slider::SliderStyle style, juce::Slider& s) {
  if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal) {
    LookAndFeel_V4::drawLinearSlider(g, x, y, w, h, sliderPos, minPos, maxPos, style, s);
    return;
  }
  const bool vertical = style == juce::Slider::LinearVertical;
  const juce::Rectangle<float> area(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                    static_cast<float>(h));
  const float alpha = s.isEnabled() ? 1.0f : 0.45f;
  const float length = static_cast<float>(capLength(s));
  const float across = vertical ? juce::jmin(static_cast<float>(kCapWidth), area.getWidth() - 2.0f)
                                : juce::jmin(static_cast<float>(kCapWidth), area.getHeight() - 2.0f);

  // The slot: a narrow cut in the panel, dark inside, lit on its lower lip.
  const float slotW = 4.0f;
  juce::Rectangle<float> slot =
      vertical ? juce::Rectangle<float>(area.getCentreX() - slotW / 2.0f, maxPos - 3.0f, slotW, minPos - maxPos + 6.0f)
               : juce::Rectangle<float>(minPos - 3.0f, area.getCentreY() - slotW / 2.0f, maxPos - minPos + 6.0f, slotW);
  g.setColour(juce::Colour(0xff050506).withMultipliedAlpha(alpha));
  g.fillRoundedRectangle(slot, 2.5f);
  g.setColour(juce::Colour(0xff45474d).withMultipliedAlpha(alpha));
  g.drawRoundedRectangle(slot.translated(0.0f, 0.5f), 2.5f, 1.0f);
  g.setColour(juce::Colour(0xff000000).withMultipliedAlpha(alpha));
  g.drawRoundedRectangle(slot, 2.5f, 1.0f);

  // The marks from the "ticks" property, the "unity" one longer and brighter.
  const auto& props = s.getProperties();
  if (const auto* ticks = props["ticks"].getArray()) {
    const double unity = props.getWithDefault("unity", 0.0);
    g.setFont(juce::Font(juce::FontOptions(9.5f)));
    if (vertical) {
      // Either side of the cap; the numbers on the left where they fit.
      const float capLeft = area.getCentreX() - across / 2.0f, capRight = area.getCentreX() + across / 2.0f;
      const float textRight = capLeft - 9.0f;
      const bool numbers = textRight - area.getX() >= 16.0f;
      // The unity number first, so a crowded neighbour gives way to it.
      std::vector<double> order;
      for (const auto& t : *ticks) order.push_back(static_cast<double>(t));
      std::stable_partition(order.begin(), order.end(), [unity](double v) { return std::abs(v - unity) < 1.0e-6; });
      std::vector<float> written;
      for (double v : order) {
        const float py = static_cast<float>(s.getPositionOfValue(v));
        const bool isUnity = std::abs(v - unity) < 1.0e-6;
        const float len = isUnity ? 6.0f : 4.0f;
        const float thick = isUnity ? 1.5f : 1.0f;
        g.setColour((isUnity ? kUnityTick : kTick).withMultipliedAlpha(alpha));
        g.fillRect(capLeft - 2.0f - len, py - thick / 2.0f, len, thick);
        g.fillRect(capRight + 2.0f, py - thick / 2.0f, len, thick);
        if (!numbers) continue;
        bool clear = true;
        for (float wy : written) clear = clear && std::abs(wy - py) >= 10.0f;
        if (!clear) continue;
        written.push_back(py);
        g.setColour((isUnity ? kUnityTick : kTickText).withMultipliedAlpha(alpha));
        g.drawText(tickText(v), juce::Rectangle<float>(area.getX(), py - 5.5f, textRight - area.getX(), 11.0f),
                   juce::Justification::centredRight, false);
      }
    } else {
      // Above and below the slot; the numbers under the cap where the slider
      // is tall enough to leave them a line.
      const float capBottom = area.getCentreY() + across / 2.0f;
      const bool numbers = area.getBottom() - capBottom >= 10.0f;
      std::vector<juce::Rectangle<float>> written;
      for (const auto& t : *ticks) {
        const double v = t;
        const float px = static_cast<float>(s.getPositionOfValue(v));
        const bool isUnity = std::abs(v - unity) < 1.0e-6;
        const float len = isUnity ? 5.0f : 3.0f;
        g.setColour((isUnity ? kUnityTick : kTick).withMultipliedAlpha(alpha));
        g.fillRect(px - 0.5f, slot.getY() - len - 1.0f, isUnity ? 1.5f : 1.0f, len);
        g.fillRect(px - 0.5f, slot.getBottom() + 1.0f, isUnity ? 1.5f : 1.0f, len);
        if (!numbers) continue;
        const juce::String text = tickText(v);
        const float tw = static_cast<float>(text.length()) * 5.5f + 4.0f;
        juce::Rectangle<float> box(px - tw / 2.0f, capBottom, tw, 10.0f);
        box.setX(juce::jlimit(area.getX(), juce::jmax(area.getX(), area.getRight() - tw), box.getX()));
        bool clear = true;
        for (const auto& w : written) clear = clear && !w.expanded(2.0f, 0.0f).intersects(box);
        if (!clear) continue;
        written.push_back(box);
        g.setColour((isUnity ? kUnityTick : kTickText).withMultipliedAlpha(alpha));
        g.drawText(text, box, juce::Justification::centred, false);
      }
    }
  }

  // The cap, centred on the value.
  const juce::Rectangle<float> cap =
      vertical ? juce::Rectangle<float>(area.getCentreX() - across / 2.0f, sliderPos - length / 2.0f, across, length)
               : juce::Rectangle<float>(sliderPos - length / 2.0f, area.getCentreY() - across / 2.0f, length, across);

  // Its shadow on the panel below it.
  g.setColour(juce::Colours::black.withAlpha(0.5f * alpha));
  g.fillRoundedRectangle(cap.translated(0.0f, 2.0f).expanded(0.5f), 2.5f);

  // The body, lit from above: a lighter top face falling to a dark lower edge.
  juce::ColourGradient body(juce::Colour(0xff6c6f76), cap.getX(), cap.getY(), juce::Colour(0xff232428), cap.getX(),
                            cap.getBottom(), false);
  body.addColour(0.12, juce::Colour(0xff4d5056));
  body.addColour(0.5, juce::Colour(0xff35373c));
  body.multiplyOpacity(alpha);
  g.setGradientFill(body);
  g.fillRoundedRectangle(cap, 2.0f);

  // The ribs a finger grips, either side of the line.
  const auto ribs = [&](float from, float to) {
    for (float p = from; p + 1.0f < to; p += 3.0f) {
      if (vertical) {
        g.setColour(juce::Colour(0xff17181a).withMultipliedAlpha(alpha));
        g.fillRect(cap.getX() + 2.0f, p, cap.getWidth() - 4.0f, 1.0f);
        g.setColour(juce::Colour(0xff6a6d73).withMultipliedAlpha(alpha));
        g.fillRect(cap.getX() + 2.0f, p + 1.0f, cap.getWidth() - 4.0f, 1.0f);
      } else {
        g.setColour(juce::Colour(0xff17181a).withMultipliedAlpha(alpha));
        g.fillRect(p, cap.getY() + 3.0f, 1.0f, cap.getHeight() - 6.0f);
        g.setColour(juce::Colour(0xff6a6d73).withMultipliedAlpha(alpha));
        g.fillRect(p + 1.0f, cap.getY() + 3.0f, 1.0f, cap.getHeight() - 6.0f);
      }
    }
  };
  if (vertical) {
    ribs(cap.getY() + 3.0f, sliderPos - 3.0f);
    ribs(sliderPos + 3.0f, cap.getBottom() - 2.0f);
  } else {
    ribs(cap.getX() + 3.0f, sliderPos - 3.0f);
    ribs(sliderPos + 3.0f, cap.getRight() - 2.0f);
  }

  // The line across the middle that reads against the scale.
  g.setColour(s.findColour(juce::Slider::thumbColourId).withMultipliedAlpha(alpha));
  if (vertical) g.fillRect(cap.getX() + 1.0f, sliderPos - 1.0f, cap.getWidth() - 2.0f, 2.0f);
  else g.fillRect(sliderPos - 1.0f, cap.getY() + 1.0f, 2.0f, cap.getHeight() - 2.0f);

  // A bevel: light along the top, dark around.
  g.setColour(juce::Colour(0xff8d9097).withMultipliedAlpha(alpha));
  g.drawLine(cap.getX() + 2.5f, cap.getY() + 0.75f, cap.getRight() - 2.5f, cap.getY() + 0.75f, 1.0f);
  g.setColour(juce::Colour(0xff0c0c0e).withMultipliedAlpha(alpha));
  g.drawRoundedRectangle(cap, 2.0f, 1.0f);
}

void styleFader(juce::Slider& s, const std::vector<double>& ticks, double unity, juce::Colour colour) {
  s.setLookAndFeel(&FaderLook::shared());
  s.setColour(juce::Slider::thumbColourId, colour);
  s.setSliderSnapsToMousePosition(false);
  juce::Array<juce::var> marks;
  for (double t : ticks) marks.add(t);
  s.getProperties().set("ticks", marks);
  s.getProperties().set("unity", unity);
  s.repaint();
}

// ----------------------------------------------------------------- the meter

StripMeter::StripMeter() {
  setOpaque(false);
}

int StripMeter::lampsFor(float level) {
  if (level <= 0.0f) return 0;
  const float db = 20.0f * std::log10(level);
  if (db < kFloorDb) return 0;
  return juce::jlimit(0, kLamps, static_cast<int>(std::floor((db - kFloorDb) / kStepDb)) + 1);
}

void StripMeter::setLevel(float level) {
  const int lit = lampsFor(level);
  int held = held_;
  if (lit >= held) {
    held = lit;
    holdTicks_ = 6;  // a second and a half at the mixer's 4 looks a second
  } else if (holdTicks_ > 0) {
    --holdTicks_;
  } else {
    held = lit;
  }
  if (lit == lit_ && held == held_) return;
  lit_ = lit;
  held_ = held;
  repaint();
}

void StripMeter::paint(juce::Graphics& g) {
  const auto r = getLocalBounds().toFloat();
  g.setColour(juce::Colour(0xff08080a));
  g.fillRoundedRectangle(r, 2.0f);
  const float gap = 1.0f;
  const float lampH = (r.getHeight() - 2.0f - gap * (kLamps - 1)) / static_cast<float>(kLamps);
  if (lampH <= 0.5f) return;
  for (int i = 0; i < kLamps; ++i) {
    const float db = kFloorDb + kStepDb * static_cast<float>(i);
    const juce::Colour on = showsClipping_ && db >= -6.0f ? juce::Colour(0xffff4433)
                            : db >= -18.0f               ? juce::Colour(0xffffc233)
                                                         : juce::Colour(0xff44dd55);
    const bool lit = i < lit_ || i == held_ - 1;
    const float bottom = r.getBottom() - 1.0f - static_cast<float>(i) * (lampH + gap);
    const juce::Rectangle<float> lamp(r.getX() + 1.0f, bottom - lampH, r.getWidth() - 2.0f, lampH);
    g.setColour(lit ? on : on.withAlpha(0.13f));
    g.fillRect(lamp);
  }
}

// ----------------------------------------------------------------- the strip

ChannelStrip::ChannelStrip(Kind kind, const juce::String& name, const juce::String& tooltip)
    : kind_(kind), name_(name) {
  fader.setLookAndFeel(&FaderLook::shared());
  fader.getProperties().set("longCap", true);
  fader.setColour(juce::Slider::thumbColourId, kindColour(kind));
  // A desk's travel: 0 dB three quarters up, the 24 dB of boost in the top
  // quarter, the cut below. Only where the cap stands changes; the values,
  // what MIDI sends and what is saved, are the live levels' own.
  fader.setNormalisableRange(juce::NormalisableRange<double>(
      0.0, kTopValue,
      [](double, double, double p) {
        return p <= kUnityAt ? kFullValue * p / kUnityAt
                             : kFullValue + (kTopValue - kFullValue) * (p - kUnityAt) / (1.0 - kUnityAt);
      },
      [](double, double, double v) {
        return v <= kFullValue ? kUnityAt * v / kFullValue
                               : kUnityAt + (1.0 - kUnityAt) * (v - kFullValue) / (kTopValue - kFullValue);
      },
      [](double, double, double v) { return std::round(juce::jlimit(0.0, kTopValue, v)); }));
  fader.setDoubleClickReturnValue(true, LiveLevels::kFull);
  fader.setSliderSnapsToMousePosition(false);
  fader.setScrollWheelEnabled(true);
  fader.setTooltip(TRANS("A live level, heard at once: 0 dB is the level as voiced"));
  fader.textFromValueFunction = [this](double v) {
    const int n = juce::roundToInt(v);
    if (n <= 0) return TRANS("Off");
    return dbText(LiveLevels::dbOf(n), 1) + (narrow_ ? juce::String() : juce::String(" dB"));
  };
  // A level typed in dB, "+6" or "-12 dB"; "Off" is silence. Anything else
  // leaves the level where it was.
  fader.valueFromTextFunction = [this](const juce::String& text) {
    const auto t = text.trim().replace(minus(), "-");
    if (t.equalsIgnoreCase(TRANS("Off"))) return 0.0;
    if (!t.containsAnyOf("0123456789")) return fader.getValue();
    return static_cast<double>(LiveLevels::valueOf(static_cast<float>(t.getDoubleValue())));
  };
  fader.onValueChange = [this] {
    showValue();
    if (onLevel) onLevel(level());
  };
  addAndMakeVisible(fader);

  value_.setJustificationType(juce::Justification::centred);
  value_.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
  value_.setColour(juce::Label::textColourId, juce::Colour(0xff9fe0ff));
  value_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff0d0e10));
  value_.setColour(juce::Label::outlineColourId, juce::Colour(0xff2c2e33));
  value_.setMinimumHorizontalScale(0.7f);
  value_.setEditable(false, true, false);
  value_.setTooltip(TRANS("Double-click to type a level in dB"));
  value_.onTextChange = [this] { fader.setValue(fader.getValueFromText(value_.getText()), juce::sendNotificationSync); showValue(); };
  addAndMakeVisible(value_);

  const auto styleSwitch = [](juce::TextButton& b, juce::Colour on, const juce::String& tip) {
    b.setClickingTogglesState(true);
    b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff34363b));
    b.setColour(juce::TextButton::buttonOnColourId, on);
    b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffb8bac0));
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    b.setTooltip(tip);
  };
  styleSwitch(mute, juce::Colour(0xffe2503a), TRANS("Mute: silences this strip, the fader stays where it is"));
  styleSwitch(solo, juce::Colour(0xffe8c53a),
              kind == Kind::Pair ? TRANS("Solo: only the soloed output pairs sound")
                                 : TRANS("Solo: only the ranks under a soloed strip sound"));
  mute.onClick = [this] { if (onMute) onMute(mute.getToggleState()); };
  solo.onClick = [this] { if (onSolo) onSolo(solo.getToggleState()); };
  addAndMakeVisible(mute);
  addAndMakeVisible(solo);

  midi.setButtonText(TRANS("MIDI"));
  midi.setTooltip(TRANS("Learn a knob or a pedal for the level, and buttons for mute and solo"));
  addAndMakeVisible(midi);

  label_.setText(name, juce::dontSendNotification);
  label_.setJustificationType(juce::Justification::centred);
  label_.setFont(juce::Font(juce::FontOptions(12.0f)));
  label_.setMinimumHorizontalScale(0.7f);
  label_.setColour(juce::Label::textColourId, juce::Colour(0xffe9e4cf));
  label_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff15161a));
  label_.setColour(juce::Label::outlineColourId, juce::Colour(0xff2c2e33));
  label_.setTooltip(tooltip.isNotEmpty() ? tooltip : name);
  addAndMakeVisible(label_);

  // An output pair's meter reads its own signal and shows where it nears
  // full scale; the others read a balance.
  meter_.setShowsClipping(kind == Kind::Pair);
  meter_.setTooltip(kind == Kind::Pair
                        ? TRANS("The output pair's level after its swell boxes and its room, before the master "
                                "volume. Red: near full scale.")
                        : TRANS("The balance of the pipes on this strip, after the faders, before the swell boxes "
                                "and the room."));
  addAndMakeVisible(meter_);
  showValue();
}

ChannelStrip::~ChannelStrip() { fader.setLookAndFeel(nullptr); }

void ChannelStrip::setLevel(int value) {
  if (juce::roundToInt(fader.getValue()) == value) return;
  fader.setValue(value, juce::dontSendNotification);
  showValue();
}

int ChannelStrip::level() const { return juce::roundToInt(fader.getValue()); }

void ChannelStrip::setTopControl(juce::Component* c) {
  top_ = c;
  if (c != nullptr) addAndMakeVisible(c);
  resized();
}

void ChannelStrip::setNarrow(bool on) {
  if (narrow_ == on) return;
  narrow_ = on;
  label_.setFont(juce::Font(juce::FontOptions(on ? 10.5f : 12.0f)));
  value_.setFont(juce::Font(juce::FontOptions(on ? 11.0f : 12.0f, juce::Font::bold)));
  showValue();
  resized();
  repaint();
}

void ChannelStrip::showValue() { value_.setText(fader.getTextFromValue(fader.getValue()), juce::dontSendNotification); }

void ChannelStrip::resized() {
  auto r = getLocalBounds().reduced(narrow_ ? 3 : 4, 0);
  r.removeFromTop(5);  // the stripe
  // The top control takes the strip's whole width, so "Pair 1" reads in full.
  auto topRow = r.removeFromTop(22);
  if (top_ != nullptr) top_->setBounds(topRow.withX(1).withWidth(getWidth() - 2));
  r.removeFromTop(3);
  value_.setBounds(r.removeFromTop(18));
  r.removeFromTop(4);

  label_.setBounds(r.removeFromBottom(32));
  r.removeFromBottom(4);
  midi.setBounds(r.removeFromBottom(20));
  r.removeFromBottom(3);
  auto buttons = r.removeFromBottom(20);
  mute.setBounds(buttons.removeFromLeft(buttons.getWidth() / 2 - 1));
  buttons.removeFromLeft(2);
  solo.setBounds(buttons);
  r.removeFromBottom(6);

  // The fader's column: the scale, the slot and cap, the meter.
  scale_ = r.removeFromLeft(narrow_ ? 5 : 26);
  auto meterColumn = r.removeFromRight(narrow_ ? 6 : 8);
  r.removeFromRight(2);
  fader.setBounds(r);
  const int top = fader.getY() + juce::roundToInt(fader.getPositionOfValue(LiveLevels::kTop));
  const int bottom = fader.getY() + juce::roundToInt(fader.getPositionOfValue(0.0));
  meter_.setBounds(meterColumn.getX(), top - 2, meterColumn.getWidth(), bottom - top + 4);
}

void ChannelStrip::paint(juce::Graphics& g) {
  const auto r = getLocalBounds().toFloat().reduced(1.0f, 0.0f);
  juce::ColourGradient panel(juce::Colour(0xff2d2f34), r.getX(), r.getY(), juce::Colour(0xff1d1e22), r.getX(),
                             r.getBottom(), false);
  g.setGradientFill(panel);
  g.fillRoundedRectangle(r, 4.0f);
  g.setColour(juce::Colour(0xff3c3e44));
  g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);
  g.setColour(kindColour(kind_).withAlpha(0.85f));
  g.fillRoundedRectangle(r.getX() + 3.0f, r.getY() + 1.0f, r.getWidth() - 6.0f, 3.0f, 1.5f);

  // The scale: every 2 dB a short tick, the marks labelled; 0 dB, the level
  // as voiced, longer and brighter. The ticks stand where the fader's line
  // stands at that level.
  if (scale_.isEmpty()) return;
  const float tickRight = static_cast<float>(scale_.getRight()) + 3.0f;
  const auto yOf = [this](double value) {
    return static_cast<float>(fader.getY()) + static_cast<float>(fader.getPositionOfValue(value));
  };
  // The minor ticks where at least 4 px apart.
  g.setColour(juce::Colour(0xff5c5f66));
  float lastY = 1.0e9f;
  for (int db = -38; db <= 24; db += 2) {
    const float y = yOf(LiveLevels::valueOf(static_cast<float>(db)));
    if (std::abs(lastY - y) < 4.0f && db != 0) continue;
    lastY = y;
    g.fillRect(tickRight - 3.0f, y - 0.5f, 3.0f, 1.0f);
  }
  // The marks, the most useful first; one that would crowd a mark already
  // written keeps its tick and loses its number.
  g.setFont(juce::Font(juce::FontOptions(10.0f)));
  std::vector<float> written;
  const auto mark = [&](double value, const juce::String& text) {
    const float y = yOf(value);
    const bool unity = juce::roundToInt(value) == LiveLevels::kFull;
    g.setColour(unity ? kUnityTick : kTickText);
    g.fillRect(tickRight - (unity ? 8.0f : 5.0f), y - (unity ? 1.0f : 0.5f), unity ? 8.0f : 5.0f, unity ? 2.0f : 1.0f);
    if (narrow_) return;
    for (float wy : written)
      if (std::abs(wy - y) < 11.0f) return;
    written.push_back(y);
    g.drawText(text, juce::Rectangle<float>(static_cast<float>(scale_.getX()) - 2.0f, y - 6.0f, static_cast<float>(scale_.getWidth()) - 5.0f, 12.0f),
               juce::Justification::centredRight, false);
  };
  mark(LiveLevels::kFull, "0");
  mark(0.0, minus() + juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e")));
  for (int db : {24, -10, 12, -20, -30, 6, -5, -15})
    mark(static_cast<double>(LiveLevels::valueOf(static_cast<float>(db))), dbText(static_cast<float>(db), 0));
}

}  // namespace mp
