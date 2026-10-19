#include "TuningPanel.h"
#include "Mobile.h"

#include "../mp_core/Temperament.h"

#include <cmath>

namespace mp::ui {

namespace {

const juce::Colour kBackground{0xff1b1e24};
const juce::Colour kText{0xffe6e9ef};
const juce::Colour kTextDim{0xff7d8594};

void styleHeading(juce::Label& l, const juce::String& text) {
  l.setText(text, juce::dontSendNotification);
  l.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
  l.setColour(juce::Label::textColourId, juce::Colours::orange);
}

juce::String signedSemitones(int n) {
  return n > 0 ? "+" + juce::String(n) : juce::String(n);
}

} // namespace

juce::String TuningPanel::summary(const MasterpieceProcessor& p) {
  juce::String s(p.temperamentName());
  s << "  A " << juce::String(p.masterPitchHz(), 1);
  if (p.transpose() != 0) s << "  " << signedSemitones(p.transpose());
  return s;
}

TuningPanel::TuningPanel(MasterpieceProcessor& p) : proc_(p) {
  styleHeading(temperamentLabel_, TRANS("Temperament"));
  styleHeading(pitchLabel_, TRANS("Pitch (A)"));
  styleHeading(transposeLabel_, TRANS("Transpose"));
  for (auto* l : {&temperamentLabel_, &pitchLabel_, &transposeLabel_}) addAndMakeVisible(*l);

  addAndMakeVisible(temperament_);
  fillTemperaments();
  temperament_.onChange = [this] {
    const int i = temperament_.getSelectedId() - 1;
    if (i < 0 || i >= static_cast<int>(choices_.size())) return;
    if (choices_[static_cast<size_t>(i)] != proc_.temperamentChoice())
      proc_.setTemperament(choices_[static_cast<size_t>(i)]);
  };
  addAndMakeVisible(scala_);
  scala_.setTooltip(TRANS("A temperament from a Scala .scl file: twelve notes, repeating at the octave"));
  scala_.onClick = [this] {
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("A Scala temperament"), proc_.dialogFolder("scala"), "*.scl");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode |
                              juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                            const auto f = pickedFile(fc);
                            if (!f.existsAsFile()) return;
                            proc_.rememberDialogFolder("scala", f);
                            std::string why;
                            if (!proc_.setTemperament("scala:" + f.getFullPathName().toStdString(), &why))
                              note_.setText(TRANS("%1 was not loaded: %2")
                                                .replace("%1", f.getFileName())
                                                .replace("%2", juce::String(why)),
                                            juce::dontSendNotification);
                            fillTemperaments();
                          });
  };

  addAndMakeVisible(pitch_);
  const double native = proc_.nativePitchHz();
  pitch_.setRange(native * 0.75, native * 1.34, 0.1);
  pitch_.setTextValueSuffix(" Hz");
  pitch_.setNumDecimalPlacesToDisplay(1);
  pitch_.setDoubleClickReturnValue(true, native);
  pitch_.setValue(proc_.masterPitchHz(), juce::dontSendNotification);
  pitch_.onValueChange = [this] {
    const double v = pitch_.getValue();
    // The organ's own pitch is "no change", not a setting that happens to
    // equal it: a later organ with another pitch must not inherit a number.
    proc_.setMasterPitchHz(std::abs(v - proc_.nativePitchHz()) < 0.05 ? 0.0 : v);
  };
  addAndMakeVisible(pitchNote_);
  pitchNote_.setColour(juce::Label::textColourId, kTextDim);
  native_.setButtonText(TRANS("Organ's own (%1)").replace("%1", juce::String(native, 1)));
  native_.onClick = [this] { proc_.setMasterPitchHz(0.0); };
  a415_.onClick = [this] { proc_.setMasterPitchHz(415.0); };
  a440_.onClick = [this] { proc_.setMasterPitchHz(440.0); };
  a442_.onClick = [this] { proc_.setMasterPitchHz(442.0); };
  for (auto* b : {&native_, &a415_, &a440_, &a442_}) addAndMakeVisible(*b);

  down_.onClick = [this] { proc_.setTranspose(proc_.transpose() - 1); };
  up_.onClick = [this] { proc_.setTranspose(proc_.transpose() + 1); };
  zero_.onClick = [this] { proc_.setTranspose(0); };
  for (auto* b : {&down_, &up_, &zero_}) addAndMakeVisible(*b);
  addAndMakeVisible(transposeValue_);
  transposeValue_.setJustificationType(juce::Justification::centred);
  transposeValue_.setColour(juce::Label::textColourId, kText);

  styleHeading(unevennessLabel_, TRANS("Unevenness"));
  addAndMakeVisible(unevennessLabel_);
  for (auto* l : {&detuneLabel_, &swayLabel_}) {
    addAndMakeVisible(*l);
    l->setColour(juce::Label::textColourId, kText);
  }
  detuneLabel_.setText(TRANS("Pipe detune"), juce::dontSendNotification);
  swayLabel_.setText(TRANS("Wind sway"), juce::dontSendNotification);
  for (auto* s : {&detune_, &sway_}) {
    addAndMakeVisible(*s);
    s->setRange(0.0, MasterpieceProcessor::kMaxUnevennessCents, 0.1);
    s->setDoubleClickReturnValue(true, 0.0);
    s->textFromValueFunction = [](double v) {
      return v <= 0.0 ? TRANS("Off") : TRANS("%1 cents").replace("%1", juce::String(v, 1));
    };
  }
  detune_.setTooltip(TRANS("Each pipe a little out of tune, by up to this much: the same pipe by the "
                           "same amount at every note. Heard from the next note; off, it costs nothing."));
  sway_.setTooltip(TRANS("A slow, small movement in the pitch of each division, as an organ's wind gives "
                         "it. Heard on the notes started while it is on; off, it costs nothing."));
  detune_.onValueChange = [this] { proc_.setPipeDetuneCents(static_cast<float>(detune_.getValue())); };
  sway_.onValueChange = [this] { proc_.setWindSwayCents(static_cast<float>(sway_.getValue())); };

  addAndMakeVisible(note_);
  note_.setColour(juce::Label::textColourId, kTextDim);
  note_.setJustificationType(juce::Justification::topLeft);
  note_.setText(TRANS("Notes played from now on take the change; held notes keep the pitch "
                      "they started with. Saved for this organ. Transposing moves the keys, "
                      "not the samples. Map any of these to your console in Settings, MIDI."),
                juce::dontSendNotification);

  setSize(460, 340);
  refresh();
  startTimerHz(4);
}

TuningPanel::~TuningPanel() { stopTimer(); }

void TuningPanel::fillTemperaments() {
  choices_.clear();
  temperament_.clear(juce::dontSendNotification);
  choices_.push_back("");
  temperament_.addItem(TRANS("The organ's own"), 1);
  for (const auto& t : temperamentLibrary()) {
    choices_.push_back(t.name);
    temperament_.addItem(juce::String(t.name), static_cast<int>(choices_.size()));
  }
  const std::string& now = proc_.temperamentChoice();
  if (now.rfind("scala:", 0) == 0) {
    choices_.push_back(now);
    temperament_.addItem(TRANS("Scala: %1").replace("%1", juce::String(proc_.temperamentName())),
                         static_cast<int>(choices_.size()));
  }
  refresh();
}

void TuningPanel::refresh() {
  const std::string& now = proc_.temperamentChoice();
  for (size_t i = 0; i < choices_.size(); ++i)
    if (choices_[i] == now) temperament_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
  temperament_.changeItemText(1, now.empty() ? TRANS("The organ's own (%1)").replace("%1", juce::String(proc_.temperamentName()))
                                             : TRANS("The organ's own"));
  if (!pitch_.isMouseButtonDown())
    pitch_.setValue(proc_.masterPitchHz(), juce::dontSendNotification);
  const double cents = 1200.0 * std::log2(proc_.masterPitchHz() / proc_.nativePitchHz());
  pitchNote_.setText(std::abs(cents) < 0.05 ? TRANS("The organ's own pitch")
                                            : TRANS("%1 cents from the organ's own").replace("%1", juce::String(cents, 1)),
                     juce::dontSendNotification);
  transposeValue_.setText(signedSemitones(proc_.transpose()), juce::dontSendNotification);
  if (!detune_.isMouseButtonDown())
    detune_.setValue(proc_.pipeDetuneCents(), juce::dontSendNotification);
  if (!sway_.isMouseButtonDown())
    sway_.setValue(proc_.windSwayCents(), juce::dontSendNotification);
  down_.setEnabled(proc_.transpose() > -12);
  up_.setEnabled(proc_.transpose() < 12);
}

void TuningPanel::timerCallback() {
  // Nothing to keep up to date while nobody can see it.
  if (!isShowing()) return;
  // A MIDI piston may have moved any of these while the panel is open.
  if (proc_.temperamentChoice().rfind("scala:", 0) == 0 &&
      temperament_.getNumItems() == static_cast<int>(temperamentLibrary().size()) + 1)
    fillTemperaments();
  refresh();
}

void TuningPanel::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void TuningPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  constexpr int kLabel = 110, kRow = 28, kGap = 8;

  auto row = r.removeFromTop(kRow);
  temperamentLabel_.setBounds(row.removeFromLeft(kLabel));
  scala_.setBounds(row.removeFromRight(100).reduced(0, 2));
  row.removeFromRight(6);
  temperament_.setBounds(row.reduced(0, 2));
  r.removeFromTop(kGap);

  row = r.removeFromTop(kRow);
  pitchLabel_.setBounds(row.removeFromLeft(kLabel));
  pitch_.setBounds(row);
  row = r.removeFromTop(kRow);
  row.removeFromLeft(kLabel);
  native_.setBounds(row.removeFromLeft(140).reduced(0, 2));
  row.removeFromLeft(4);
  for (auto* b : {&a415_, &a440_, &a442_}) {
    b->setBounds(row.removeFromLeft(48).reduced(0, 2));
    row.removeFromLeft(4);
  }
  row = r.removeFromTop(22);
  row.removeFromLeft(kLabel);
  pitchNote_.setBounds(row);
  r.removeFromTop(kGap);

  row = r.removeFromTop(kRow);
  transposeLabel_.setBounds(row.removeFromLeft(kLabel));
  down_.setBounds(row.removeFromLeft(32).reduced(0, 2));
  transposeValue_.setBounds(row.removeFromLeft(48));
  up_.setBounds(row.removeFromLeft(32).reduced(0, 2));
  row.removeFromLeft(8);
  zero_.setBounds(row.removeFromLeft(32).reduced(0, 2));
  r.removeFromTop(kGap);

  unevennessLabel_.setBounds(r.removeFromTop(kRow));
  for (auto [label, slider] : {std::pair{&detuneLabel_, &detune_}, std::pair{&swayLabel_, &sway_}}) {
    row = r.removeFromTop(kRow);
    label->setBounds(row.removeFromLeft(kLabel));
    slider->setBounds(row.reduced(0, 2));
  }
  r.removeFromTop(kGap);

  note_.setBounds(r);
}

} // namespace mp::ui
