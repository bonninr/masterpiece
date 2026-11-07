// SPDX-License-Identifier: GPL-3.0-only
#include "TunerWindow.h"

#include <algorithm>
#include <cmath>

#include "Mobile.h"

namespace mp::ui {

namespace {

const juce::Colour kBackground(0xff15171c);
const juce::Colour kTrack(0xff2a2f3a);
const juce::Colour kDim(0xff8a93a3);
const juce::Colour kText(0xffe6e8ec);
const juce::Colour kInTune(0xff5cc77a);
const juce::Colour kNear(0xffe0a050);
const juce::Colour kOff(0xffe06050);

// The scale reaches half a semitone either way.
constexpr double kScaleCents = 50.0;
// The harmonics' bars run from the fundamental's level down this far.
constexpr float kBarRangeDb = 60.0f;

juce::Colour colourFor(double cents) {
  const double off = std::fabs(cents);
  return off <= 2.0 ? kInTune : (off <= 10.0 ? kNear : kOff);
}

}  // namespace

TunerPanel::TunerPanel(MasterpieceProcessor& p) : proc_(p) {
  setOpaque(true);
  setSize(440, 330);
  startTimerHz(8);
}

TunerPanel::~TunerPanel() {
  if (tapOpen_) proc_.spectrum().close();
}

void TunerPanel::timerCallback() {
  const bool want = isShowing();
  if (want != tapOpen_) {
    tapOpen_ = want;
    if (want) proc_.spectrum().open();
    else proc_.spectrum().close();
    // The ring still holds what it took the last time it was open.
    openedAt_ = proc_.spectrum().samplesPushed();
    showing_ = false;
    missed_ = 0;
    repaint();
  }
  if (!tapOpen_) return;
  const double rate = proc_.getSampleRate();
  if (rate <= 0.0 || proc_.spectrum().samplesPushed() - openedAt_ < SpectrumTap::kPitchSpan) return;
  // A held key is the note to read. The blower and the reverb sound on with
  // no key down, and a 32' stop sounds two octaves under the key.
  const int key = lowestHeldKey();
  SpectrumTap::Pitch pitch;
  if (key >= 0) {
    const double keyHz = proc_.masterPitchHz() * std::pow(2.0, (key - 69) / 12.0);
    samples_.resize(static_cast<size_t>(SpectrumTap::kPitchSpan));
    proc_.spectrum().latest(samples_.data(), SpectrumTap::kPitchSpan);
    pitch = SpectrumTap::measurePitch(samples_.data(), SpectrumTap::kPitchSpan, rate, 0.9 * keyHz / 4.0);
  }
  if (pitch.found) {
    // The same note a moment later settles the needle; another note moves it at once.
    if (showing_ && std::fabs(1200.0 * std::log2(pitch.hz / hz_)) < 20.0) {
      hz_ *= std::pow(pitch.hz / hz_, 0.4);
      for (size_t h = 0; h < harmonics_.size(); ++h)
        harmonics_[h] += 0.4f * (pitch.harmonicsDb[h] - harmonics_[h]);
    } else {
      hz_ = pitch.hz;
      harmonics_ = pitch.harmonicsDb;
    }
    showing_ = true;
    missed_ = 0;
  } else if (showing_ && ++missed_ > 4) {
    showing_ = false;
  }
  repaint();
}

int TunerPanel::lowestHeldKey() const {
  int lowest = 128;
  for (const auto& [id, keyboard] : proc_.organModel().keyboards) {
    (void)keyboard;
    for (int note = 0; note < lowest; ++note) {
      if (proc_.keyDownOnKeyboard(id, note)) {
        lowest = note;
        break;
      }
    }
  }
  return lowest < 128 ? lowest + proc_.transpose() : -1;
}

void TunerPanel::paint(juce::Graphics& g) {
  g.fillAll(kBackground);
  auto r = getLocalBounds().reduced(16, 12);
  auto head = r.removeFromTop(60);
  auto info = r.removeFromTop(22);
  r.removeFromTop(10);
  auto scale = r.removeFromTop(40);
  r.removeFromTop(14);
  auto bars = r;

  const double aHz = proc_.masterPitchHz();
  const auto temperament = proc_.soundingTemperament();
  const auto note = nearestTemperedNote(hz_, aHz, temperament);
  const juce::String dot = juce::String::fromUTF8("   \xc2\xb7   ");
  juce::String line = juce::String::fromUTF8(proc_.temperamentName().c_str()) + dot + "A " + juce::String(aHz, 1) + " Hz";
  if (showing_) line = juce::String(hz_, 2) + " Hz" + dot + line;
  g.setColour(kDim);
  g.setFont(juce::FontOptions(14.0f));
  g.drawText(line, info, juce::Justification::centred);

  // The note and its distance from the temperament.
  if (showing_) {
    const auto colour = colourFor(note.cents);
    g.setColour(kText);
    g.setFont(juce::FontOptions(46.0f, juce::Font::bold));
    g.drawText(juce::MidiMessage::getMidiNoteName(note.midiNote, true, true, 4), head.removeFromLeft(head.getWidth() / 2),
               juce::Justification::centred);
    g.setColour(colour);
    g.setFont(juce::FontOptions(30.0f));
    g.drawText((note.cents >= 0.0 ? "+" : "") + juce::String(note.cents, 1) + " " + TRANS("cents"), head,
               juce::Justification::centred);
  } else {
    g.setColour(kDim);
    g.setFont(juce::FontOptions(18.0f));
    g.drawText(TRANS("Hold a key to measure its pitch."), head, juce::Justification::centred);
  }

  // The scale: half a semitone flat to half a semitone sharp.
  const auto track = scale.removeFromTop(22).toFloat().reduced(10.0f, 0.0f);
  const auto xFor = [&track](double cents) {
    return track.getX() + track.getWidth() * static_cast<float>((cents + kScaleCents) / (2.0 * kScaleCents));
  };
  g.setColour(kTrack);
  g.fillRoundedRectangle(track.withSizeKeepingCentre(track.getWidth(), 6.0f), 3.0f);
  g.setColour(kInTune.withAlpha(0.35f));
  g.fillRect(juce::Rectangle<float>(xFor(-2.0), track.getY(), xFor(2.0) - xFor(-2.0), track.getHeight()));
  g.setFont(juce::FontOptions(11.0f));
  for (const double c : {-50.0, -25.0, -10.0, 0.0, 10.0, 25.0, 50.0}) {
    const float x = xFor(c);
    g.setColour(kDim);
    g.drawVerticalLine(juce::roundToInt(x), track.getY() + (c == 0.0 ? 0.0f : 6.0f), track.getBottom() - (c == 0.0 ? 0.0f : 6.0f));
    g.drawText((c > 0.0 ? "+" : "") + juce::String(static_cast<int>(c)),
               juce::Rectangle<float>(x - 20.0f, track.getBottom() + 2.0f, 40.0f, 14.0f), juce::Justification::centred);
  }
  if (showing_) {
    const float x = xFor(juce::jlimit(-kScaleCents, kScaleCents, note.cents));
    g.setColour(colourFor(note.cents));
    g.fillRoundedRectangle(x - 2.0f, track.getY() - 2.0f, 4.0f, track.getHeight() + 4.0f, 2.0f);
  }

  // The harmonics, each against the fundamental.
  g.setColour(kDim);
  g.setFont(juce::FontOptions(13.0f));
  g.drawText(TRANS("Harmonics"), bars.removeFromTop(18), juce::Justification::centredLeft);
  auto labels = bars.removeFromBottom(16);
  const int n = SpectrumTap::Pitch::kHarmonics;
  const float column = static_cast<float>(bars.getWidth()) / n;
  for (int h = 0; h < n; ++h) {
    const auto col = juce::Rectangle<float>(bars.getX() + h * column, static_cast<float>(bars.getY()), column,
                                            static_cast<float>(bars.getHeight()))
                         .reduced(column * 0.2f, 0.0f);
    g.setColour(kTrack);
    g.fillRect(col.withTrimmedTop(14.0f));
    g.setColour(kDim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(h + 1), juce::Rectangle<float>(col.getX(), static_cast<float>(labels.getY()), col.getWidth(), 16.0f),
               juce::Justification::centred);
    if (!showing_) continue;
    const float db = harmonics_[static_cast<size_t>(h)];
    const float fill = juce::jlimit(0.0f, 1.0f, (db + kBarRangeDb) / kBarRangeDb);
    const auto area = col.withTrimmedTop(14.0f);
    g.setColour(juce::Colour(0xff50a0e0));
    g.fillRect(area.withTop(area.getBottom() - area.getHeight() * fill));
    if (db > -kBarRangeDb) {
      g.setColour(kText);
      g.drawText(juce::String(juce::roundToInt(db)), col.withHeight(14.0f).translated(0.0f, area.getHeight() * (1.0f - fill)),
                 juce::Justification::centred);
    }
  }
}

TunerWindow::TunerWindow(MasterpieceProcessor& p)
    : juce::DocumentWindow(TRANS("Tuner"), kBackground, juce::DocumentWindow::closeButton) {
  setUsingNativeTitleBar(true);
  setContentOwned(new TunerPanel(p), true);
  setResizable(false, false);
  fitToScreen(*this);
}

void TunerWindow::closeButtonPressed() { showFloating(*this, false, false); }

}  // namespace mp::ui
