#include "Wizard.h"

namespace mp::ui {
namespace {
constexpr int kSteps = 3;
constexpr int kRow = 28;
}  // namespace

bool WizardPanel::isFirstRun(const MasterpieceProcessor& proc) {
  // No global settings file means the program has never been configured here.
  // Deliberately not a "seen the wizard" flag of its own: one more piece of
  // state to get out of step with reality, for no extra information.
  return !proc.globalSettingsFile().existsAsFile();
}

WizardPanel::WizardPanel(MasterpieceProcessor& proc,
                         juce::AudioDeviceManager& devices)
    : proc_(proc), devices_(devices) {
  auto style = [](juce::Label& l, float size, juce::Colour colour) {
    l.setColour(juce::Label::textColourId, colour);
    l.setFont(juce::Font(juce::FontOptions(size)));
    l.setJustificationType(juce::Justification::topLeft);
  };
  addAndMakeVisible(title_);
  style(title_, 20.0f, juce::Colours::white);
  addAndMakeVisible(body_);
  style(body_, 13.0f, juce::Colours::lightgrey);
  addAndMakeVisible(progress_);
  style(progress_, 12.0f, juce::Colours::grey);

  // 0 inputs, 2 outputs: an organ plays, it does not record, and offering an
  // input channel list here would be three more choices that change nothing.
  audio_ = std::make_unique<juce::AudioDeviceSelectorComponent>(
      devices_, 0, 0, 1, 8, /*showMidiInputOptions*/ false,
      /*showMidiOutputSelector*/ false, /*showChannelsAsStereoPairs*/ true,
      /*hideAdvancedOptions*/ false);
  addChildComponent(*audio_);

  addChildComponent(midiHeading_);
  style(midiHeading_, 13.0f, juce::Colours::lightgrey);
  midiHeading_.setText("Tick the keyboards you play from:",
                       juce::dontSendNotification);
  addChildComponent(midiNone_);
  style(midiNone_, 13.0f, juce::Colours::grey);
  midiNone_.setText(
      "No MIDI inputs found. You can still play with the mouse on the "
      "on-screen keyboard, and plug a console in later.",
      juce::dontSendNotification);

  for (const auto& in : juce::MidiInput::getAvailableDevices()) {
    auto b = std::make_unique<juce::ToggleButton>(in.name);
    b->setToggleState(devices_.isMidiInputDeviceEnabled(in.identifier),
                      juce::dontSendNotification);
    const auto id = in.identifier;
    auto* raw = b.get();
    b->onClick = [this, id, raw] {
      devices_.setMidiInputDeviceEnabled(id, raw->getToggleState());
    };
    addChildComponent(*b);
    midiInputs_.push_back(std::move(b));
  }

  addChildComponent(openOrgan_);
  openOrgan_.onClick = [this] {
    if (onOpenOrgan) onOpenOrgan();
  };
  addChildComponent(organStatus_);
  style(organStatus_, 12.0f, juce::Colours::grey);

  addAndMakeVisible(back_);
  back_.onClick = [this] { showStep(step_ - 1); };
  addAndMakeVisible(next_);
  next_.onClick = [this] {
    if (step_ + 1 >= kSteps) finish();
    else showStep(step_ + 1);
  };
  addAndMakeVisible(skip_);
  // Skipping is a first-class answer. A player who knows their rig should not
  // have to click through three screens to reach an organ.
  skip_.onClick = [this] { finish(); };

  showStep(0);
}

void WizardPanel::showStep(int step) {
  step_ = juce::jlimit(0, kSteps - 1, step);

  audio_->setVisible(step_ == 0);
  midiHeading_.setVisible(step_ == 1 && !midiInputs_.empty());
  midiNone_.setVisible(step_ == 1 && midiInputs_.empty());
  for (auto& b : midiInputs_) b->setVisible(step_ == 1);
  openOrgan_.setVisible(step_ == 2);
  organStatus_.setVisible(step_ == 2);

  switch (step_) {
    case 0:
      title_.setText("Sound", juce::dontSendNotification);
      body_.setText(
          "Pick the device you listen through, and a buffer size.\n\n"
          "A smaller buffer responds faster to a key; too small and the audio "
          "breaks up. 256 or 512 samples suits most machines. You can change "
          "this at any time.",
          juce::dontSendNotification);
      break;
    case 1:
      title_.setText("Your keyboards", juce::dontSendNotification);
      body_.setText(
          "Which manual a key plays is decided by its MIDI channel, and no "
          "organ file can guess how your console is wired.\n\n"
          "Turn your inputs on here; the channel for each manual is on the "
          "MIDI page in settings, where you can also right-click a drawstop "
          "and move the real one to learn it.",
          juce::dontSendNotification);
      break;
    default:
      title_.setText("An organ", juce::dontSendNotification);
      body_.setText(
          "Choose the organ definition file of a sample set you have "
          "installed. A large set takes a while to load the first time; the "
          "console appears as soon as it is ready.\n\n"
          "Afterwards you can put it on a numbered slot from the Favourites "
          "page, so getting back to it never means finding it again.",
          juce::dontSendNotification);
      organStatus_.setText(
          proc_.organModel().organName.empty()
              ? "No organ loaded yet"
              : "Loaded: " + juce::String(proc_.organModel().organName),
          juce::dontSendNotification);
      break;
  }

  back_.setEnabled(step_ > 0);
  next_.setButtonText(step_ + 1 >= kSteps ? "Finish" : "Next");
  progress_.setText("Step " + juce::String(step_ + 1) + " of " +
                        juce::String(kSteps),
                    juce::dontSendNotification);
  resized();
}

void WizardPanel::finish() {
  // Writing the defaults is what makes this a first run that has happened.
  proc_.saveGlobalDefaults();
  if (onFinished) onFinished();
  if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
    dw->closeButtonPressed();
}

void WizardPanel::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff15171c));
}

void WizardPanel::resized() {
  auto r = getLocalBounds().reduced(16);

  auto footer = r.removeFromBottom(kRow + 8);
  progress_.setBounds(footer.removeFromLeft(110));
  next_.setBounds(footer.removeFromRight(110).reduced(2, 2));
  back_.setBounds(footer.removeFromRight(110).reduced(2, 2));
  skip_.setBounds(footer.removeFromRight(120).reduced(2, 2));

  title_.setBounds(r.removeFromTop(32));
  body_.setBounds(r.removeFromTop(96));
  r.removeFromTop(8);

  if (step_ == 0) {
    audio_->setBounds(r);
    return;
  }
  if (step_ == 1) {
    if (midiInputs_.empty()) {
      midiNone_.setBounds(r.removeFromTop(60));
      return;
    }
    midiHeading_.setBounds(r.removeFromTop(kRow));
    for (auto& b : midiInputs_) {
      b->setBounds(r.removeFromTop(kRow).withTrimmedLeft(12));
      r.removeFromTop(2);
    }
    return;
  }
  openOrgan_.setBounds(r.removeFromTop(kRow + 4).withWidth(200));
  r.removeFromTop(8);
  organStatus_.setBounds(r.removeFromTop(kRow));
}

}  // namespace mp::ui
