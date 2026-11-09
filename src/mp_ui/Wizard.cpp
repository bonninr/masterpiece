#include "Wizard.h"

#include "ManualDialog.h"
#include "Mobile.h"

#include "../mp_audio/DataFolder.h"
#include "../mp_core/TestSound.h"

namespace mp::ui {
namespace {
constexpr int kSteps = 4;
constexpr int kRow = 28;

// The room's share of the sound when the Test Sound organ first loads.
constexpr float kTestSoundReverbMix = 0.5f;

class TestSoundThread : public juce::ThreadWithProgressWindow {
public:
  TestSoundThread(MasterpieceProcessor& proc, juce::File folder, std::function<void(const juce::File&)> done)
      : juce::ThreadWithProgressWindow(TRANS("Creating the Test Sound organ"), true, true),
        proc_(proc), folder_(std::move(folder)), done_(std::move(done)) {}

  void run() override {
    result_ = mp::writeTestSound(folder_.getFullPathName().toStdString(), {}, [this](double p) {
      setProgress(p);
      return !threadShouldExit();
    });
  }

  void threadComplete(bool cancelled) override {
    juce::File organ;
    if (result_.ok) {
      organ = juce::File(juce::String::fromUTF8(result_.organFile.u8string().c_str()));
      // Its own room as the reverb, the first time only: after that the
      // organ's settings are the player's. Named once the definition is
      // there: the name comes from the organ's own name and id.
      const juce::File settings = proc_.settingsFileFor(organ);
      if (!settings.existsAsFile()) {
        const juce::File room(juce::String::fromUTF8(result_.roomFile.u8string().c_str()));
        settings.getParentDirectory().createDirectory();
        settings.replaceWithText("reverb 1 " + juce::String(kTestSoundReverbMix) + " " +
                                 room.getFullPathName() + "\n");
      }
    } else if (!cancelled && !threadShouldExit()) {
      juce::Logger::writeToLog("Test Sound organ not created: " + juce::String(result_.error));
      juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                             TRANS("Test Sound organ not created"),
                                             juce::String::fromUTF8(result_.error.c_str()));
    }
    if (done_) done_(organ);
    delete this;
  }

private:
  MasterpieceProcessor& proc_;
  juce::File folder_;
  std::function<void(const juce::File&)> done_;
  mp::TestSoundResult result_;
};
}  // namespace

void createTestSound(MasterpieceProcessor& proc, std::function<void(const juce::File&)> done) {
  // A folder of its own: "Organs" is the same folder as the per-organ
  // settings ("organs") wherever names ignore case.
  const juce::File folder = mp::dataFolder().getChildFile("Test Sound");
  folder.createDirectory();
  (new TestSoundThread(proc, folder, std::move(done)))->launchThread();
}

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
      devices_, 0, 0, 1, MasterpieceProcessor::kMaxOutputChannels, /*showMidiInputOptions*/ false,
      /*showMidiOutputSelector*/ false, /*showChannelsAsStereoPairs*/ true,
      /*hideAdvancedOptions*/ false);
  addChildComponent(*audio_);

  // The display mode comes first: it decides what every later screen and
  // menu shows.
  for (auto* b : {&simple_, &advanced_}) {
    b->setRadioGroupId(1);
    addChildComponent(*b);
  }
  (proc_.simpleMode() ? simple_ : advanced_).setToggleState(true, juce::dontSendNotification);
  simple_.onClick = [this] { if (simple_.getToggleState()) proc_.setSimpleMode(true); };
  advanced_.onClick = [this] { if (advanced_.getToggleState()) proc_.setSimpleMode(false); };

  addChildComponent(midiHeading_);
  style(midiHeading_, 13.0f, juce::Colours::lightgrey);
  midiHeading_.setText(TRANS("MIDI inputs"),
                       juce::dontSendNotification);
  addChildComponent(midiNone_);
  style(midiNone_, 13.0f, juce::Colours::grey);
  midiNone_.setText(
      TRANS("No MIDI inputs found. You can still play with the mouse on the "
            "on-screen keyboard, and plug a console in later."),
      juce::dontSendNotification);

  addChildComponent(channelsHeading_);
  style(channelsHeading_, 13.0f, juce::Colours::lightgrey);
  channelsHeading_.setText(TRANS("Default channels"), juce::dontSendNotification);
  const auto& def = proc_.defaultConsole();
  for (int role = 0; role < kRoles; ++role) {
    auto& label = roleLabels_[static_cast<size_t>(role)];
    auto& box = roleChannels_[static_cast<size_t>(role)];
    style(label, 13.0f, juce::Colours::lightgrey);
    label.setText(role == 0 ? TRANS("Pedal") : TRANS("Manual %1").replace("%1", juce::String(role)),
                  juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centredRight);
    addChildComponent(label);
    box.addItem("-", 1);
    for (int c = 1; c <= 16; ++c) box.addItem(juce::String(c), c + 1);
    const auto it = def.find(role);
    box.setSelectedId(it == def.end() ? 1 : it->second + 1, juce::dontSendNotification);
    box.onChange = [this, role, &box] {
      proc_.setDefaultConsoleChannel(role, box.getSelectedId() - 1);
      showRoleDevices();
    };
    addChildComponent(box);
    auto& device = roleDevices_[static_cast<size_t>(role)];
    device.setTooltip(TRANS("The MIDI device this keyboard comes from. Any device: every device sending on "
                            "the channel plays it."));
    device.onChange = [this, role, &device] {
      proc_.setDefaultConsoleDevice(role, ManualDialog::deviceInBox(device, proc_));
    };
    addChildComponent(device);
  }
  showRoleDevices();

  for (const auto& in : juce::MidiInput::getAvailableDevices()) {
    auto b = std::make_unique<juce::ToggleButton>(in.name);
    b->setToggleState(devices_.isMidiInputDeviceEnabled(in.identifier),
                      juce::dontSendNotification);
    const auto id = in.identifier;
    const auto name = in.name;
    auto* raw = b.get();
    // The choice is the player's, kept as Settings > MIDI keeps it (#637):
    // closing the device alone lasted until the retry opened it again.
    b->onClick = [this, id, name, raw] {
      devices_.setMidiInputDeviceEnabled(id, raw->getToggleState());
      proc_.setMidiInputEnabled(name, raw->getToggleState());
    };
    addChildComponent(*b);
    midiInputs_.push_back(std::move(b));
  }

  addChildComponent(openOrgan_);
  // Choosing an organ is the wizard's last question: the wizard closes and the
  // organ chooser opens, so the console is in view while the organ loads (#591).
  openOrgan_.onClick = [this] {
    auto open = onOpenOrgan;  // finish() may delete this panel
    finish(false);
    if (open) open();
  };
  addChildComponent(organStatus_);
  style(organStatus_, 12.0f, juce::Colours::grey);
  addChildComponent(testSound_);
  testSound_.setToggleState(kMobile, juce::dontSendNotification);
  addChildComponent(testSoundNote_);
  style(testSoundNote_, 13.0f, juce::Colours::grey);
  testSoundNote_.setText(TRANS("A small organ generated on this device in a few seconds, to check the audio and "
                               "MIDI setup before a sample set is installed. One manual, one stop and a swell "
                               "pedal, in a large stone room. Created and loaded on Finish."),
                         juce::dontSendNotification);

  addAndMakeVisible(back_);
  back_.onClick = [this] { showStep(step_ - 1); };
  addAndMakeVisible(next_);
  next_.onClick = [this] {
    if (step_ + 1 >= kSteps) finish(testSound_.getToggleState());
    else showStep(step_ + 1);
  };
  addAndMakeVisible(skip_);
  // Skipping is a first-class answer. A player who knows their rig should not
  // have to click through three screens to reach an organ. The Test Sound
  // box keeps its say: ticked on a phone or tablet, the organ is made anyway.
  skip_.onClick = [this] { finish(testSound_.getToggleState()); };

  showStep(0);
}

void WizardPanel::showStep(int step) {
  step_ = juce::jlimit(0, kSteps - 1, step);

  simple_.setVisible(step_ == 0);
  advanced_.setVisible(step_ == 0);
  audio_->setVisible(step_ == 1);
  midiHeading_.setVisible(step_ == 2 && !midiInputs_.empty());
  midiNone_.setVisible(step_ == 2 && midiInputs_.empty());
  for (auto& b : midiInputs_) b->setVisible(step_ == 2);
  channelsHeading_.setVisible(step_ == 2);
  for (auto& c : roleLabels_) c.setVisible(step_ == 2);
  for (auto& c : roleChannels_) c.setVisible(step_ == 2);
  for (auto& c : roleDevices_) c.setVisible(step_ == 2);
  openOrgan_.setVisible(step_ == 3);
  organStatus_.setVisible(step_ == 3);
  testSound_.setVisible(step_ == 3);
  testSoundNote_.setVisible(step_ == 3);

  switch (step_) {
    case 0:
      title_.setText(TRANS("Display mode"), juce::dontSendNotification);
      body_.setText(
          TRANS("Simple: the main settings, with presets.\nAdvanced: every setting.\n\n"
                "Also in Settings > General."),
          juce::dontSendNotification);
      break;
    case 1:
      title_.setText(TRANS("Audio device"), juce::dontSendNotification);
      body_.setText(
          TRANS("Output device and buffer size. 256 or 512 samples suit most computers.\n\n"
                "Also in Settings > Audio."),
          juce::dontSendNotification);
      break;
    case 2:
      // The devices there are now: one ticked a moment ago is listed too.
      showRoleDevices();
      title_.setText(TRANS("MIDI configuration"), juce::dontSendNotification);
      body_.setText(
          TRANS("MIDI channel and device of the pedal and each manual, for every organ without "
                "its own. A keyboard with no channel plays from no MIDI device.\n\n"
                "Also in Settings > MIDI."),
          juce::dontSendNotification);
      break;
    default:
      title_.setText(TRANS("Organ"), juce::dontSendNotification);
      body_.setText(
          TRANS("Definition file or package. The first load of a large set takes longer."),
          juce::dontSendNotification);
      organStatus_.setText(
          proc_.organModel().organName.empty()
              ? TRANS("No organ loaded yet")
              : TRANS("Loaded: %1").replace("%1", juce::String(proc_.organModel().organName)),
          juce::dontSendNotification);
      break;
  }

  back_.setEnabled(step_ > 0);
  next_.setButtonText(step_ + 1 >= kSteps ? TRANS("Finish") : TRANS("Next"));
  progress_.setText(TRANS("Step %1 of %2")
                        .replace("%1", juce::String(step_ + 1))
                        .replace("%2", juce::String(kSteps)),
                    juce::dontSendNotification);
  resized();
}

void WizardPanel::showRoleDevices() {
  const auto& def = proc_.defaultConsole();
  const auto& devices = proc_.defaultConsoleDevices();
  for (int role = 0; role < kRoles; ++role) {
    auto& device = roleDevices_[static_cast<size_t>(role)];
    const auto dev = devices.find(role);
    ManualDialog::fillDeviceBox(device, proc_, dev == devices.end() ? std::string() : dev->second);
    // The box opens once the keyboard has a channel.
    device.setEnabled(def.count(role) != 0);
  }
}

void WizardPanel::finish(bool testSound) {
  // Writing the defaults is what makes this a first run that has happened.
  proc_.saveGlobalDefaults();
  if (onFinished) onFinished();
  // Held here: closing the dialog deletes this panel.
  auto load = onLoadOrgan;
  auto& proc = proc_;
  if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
    dw->closeButtonPressed();
  if (testSound)
    createTestSound(proc, [load](const juce::File& organ) {
      if (organ != juce::File() && load) load(organ);
    });
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
    simple_.setBounds(r.removeFromTop(kRow).withTrimmedLeft(12).withWidth(300));
    r.removeFromTop(4);
    advanced_.setBounds(r.removeFromTop(kRow).withTrimmedLeft(12).withWidth(300));
    return;
  }
  if (step_ == 1) {
    audio_->setBounds(r);
    return;
  }
  if (step_ == 2) {
    // The channels first, two to a row with their devices, then the inputs,
    // which can be many.
    channelsHeading_.setBounds(r.removeFromTop(kRow));
    const int half = (r.getWidth() - 12) / 2;
    for (int role = 0; role < kRoles; role += 2) {
      auto line = r.removeFromTop(kRow).withTrimmedLeft(12);
      for (int k = role; k < role + 2 && k < kRoles; ++k) {
        auto cell = line.removeFromLeft(half);
        roleLabels_[static_cast<size_t>(k)].setBounds(cell.removeFromLeft(72));
        cell.removeFromLeft(4);
        roleChannels_[static_cast<size_t>(k)].setBounds(cell.removeFromLeft(56).reduced(0, 1));
        cell.removeFromLeft(4);
        roleDevices_[static_cast<size_t>(k)].setBounds(cell.withTrimmedRight(12).reduced(0, 1));
      }
      r.removeFromTop(2);
    }
    r.removeFromTop(8);
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
  r.removeFromTop(16);
  testSound_.setBounds(r.removeFromTop(kRow).withWidth(320));
  testSoundNote_.setBounds(r.removeFromTop(72).withTrimmedLeft(24));
}

}  // namespace mp::ui
