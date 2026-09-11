// Masterpiece — standalone application.
//
// This is the product (see the standalone-first decision): the plugin wrappers
// are secondary. It owns the audio device and MIDI input and drives the same
// MasterpieceProcessor the plugin and the headless renderer use, so there is
// one engine and three front ends rather than three engines.
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>

#include "../../src/mp_audio/MasterpieceProcessor.h"
#include "../../src/mp_ui/Ui.h"
#include "../../src/mp_ui/Settings.h"
#include "../../src/mp_ui/Wizard.h"

class MasterpieceApp : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return "Masterpiece"; }
  const juce::String getApplicationVersion() override { return "0.2.0"; }
  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String& commandLine) override {
    proc_ = std::make_unique<mp::MasterpieceProcessor>();

    // Automation surface (docs/automation/gui-automation.md), read before
    // anything is built, because some of it decides how things are built:
    //   --odf <path>       load an organ at startup
    //   --gui-only         draw its console without reading any audio
    //   --virtual-midi [n] publish a MIDI input port of our own
    //   --log <path>       write the load phase timings to a file
    //
    // fromTokens(..., true) PRESERVES the quotes it split on, so every value
    // taken from here is unquoted before use. Organ paths almost always
    // contain spaces.
    const auto args = juce::StringArray::fromTokens(commandLine, true);
    const bool guiOnly = args.contains("--gui-only");

    // Installed before anything is loaded, because the load is what it is
    // there to time. Nothing logs until this exists.
    for (int i = 0; i < args.size(); ++i)
      if (args[i] == "--log" && i + 1 < args.size()) {
        logger_ = std::make_unique<juce::FileLogger>(
            juce::File::getCurrentWorkingDirectory().getChildFile(
                args[i + 1].unquoted()),
            "Masterpiece load log");
        juce::Logger::setCurrentLogger(logger_.get());
      }

    // Audio first: the device's real rate and block size are what the engine
    // must be prepared for, and asking for them before the organ loads means
    // the voice pool and DSP are sized once rather than twice.
    player_ = std::make_unique<juce::AudioProcessorPlayer>();
    devices_ = std::make_unique<juce::AudioDeviceManager>();
    // The driver, rate and buffer a player chose are properties of the
    // machine, not of any organ, and re-choosing them every launch is the
    // kind of thing that makes a program feel unfinished. JUCE serialises
    // the lot; a saved state that no longer matches the hardware is ignored
    // and the defaults come back.
    std::unique_ptr<juce::XmlElement> saved(
        juce::XmlDocument::parse(audioSettingsFile()));
    const auto audioError =
        devices_->initialise(0, 2, saved.get(), /*selectDefaultDeviceOnFailure*/ true);
    if (audioError.isNotEmpty())
      juce::Logger::writeToLog("audio device: " + audioError);

    player_->setProcessor(proc_.get());
    devices_->addAudioCallback(player_.get());

    // Every MIDI input, enabled by default: an organist plugging in a console
    // expects it to play, not to hunt through a settings dialog first.
    //
    // Each device gets its OWN callback rather than all of them feeding the
    // player's merged buffer, because the merged buffer has no device in it —
    // and a rig with two manuals plugged in sends the same note on the same
    // channel from both. Telling them apart is the whole point of "this
    // keyboard plays the Great and that one plays the Swell".
    for (const auto& in : juce::MidiInput::getAvailableDevices()) {
      devices_->setMidiInputDeviceEnabled(in.identifier, true);
      auto route = std::make_unique<DeviceRoute>(
          *proc_, proc_->registerMidiDevice(in.name));
      devices_->addMidiInputDeviceCallback(in.identifier, route.get());
      routes_.push_back({in.identifier, std::move(route)});
    }

    // A port of our own, so anything that can send MIDI can play this organ
    // without a console plugged in. macOS and Linux let a process publish one;
    // Windows has no such API, and needs a helper such as loopMIDI, whose port
    // the loop above picks up like any other device.
#if JUCE_MAC || JUCE_LINUX
    for (int i = 0; i < args.size(); ++i)
      if (args[i] == "--virtual-midi") {
        const auto name = (i + 1 < args.size() && !args[i + 1].startsWith("--"))
                              ? args[i + 1].unquoted()
                              : juce::String("Masterpiece");
        virtualRoute_ = std::make_unique<DeviceRoute>(
            *proc_, proc_->registerMidiDevice(name.toStdString()));
        virtualInput_ = juce::MidiInput::createNewDevice(name, virtualRoute_.get());
        if (virtualInput_ != nullptr) {
          virtualInput_->start();
          juce::Logger::writeToLog("virtual MIDI input: " + name);
        } else {
          juce::Logger::writeToLog("could not create a virtual MIDI input");
        }
      }
#endif

    win_ = std::make_unique<DocWindow>(*proc_, *devices_);
    win_->setVisible(true);

    juce::File odf;
    for (int i = 0; i < args.size(); ++i)
      if (args[i] == "--odf" && i + 1 < args.size()) {
        // fromTokens(..., true) PRESERVES the quotes it split on, so a path
        // with spaces arrives as "C:\...xml" including the quote characters
        // and resolves to nothing. Organ paths almost always contain spaces.
        const auto path = args[++i].unquoted();
        odf = juce::File::getCurrentWorkingDirectory().getChildFile(path);
      }

    // Nothing named on the command line: pick up where the player left off.
    // loadGlobalDefaults is what knows which organ that was, and it answers
    // with nothing if the file has since moved or the drive is unplugged.
    if (odf == juce::File()) {
      proc_->loadGlobalDefaults();
      if (proc_->reopenLastOrgan()) odf = proc_->lastOrgan();
    }

    if (odf != juce::File()) win_->editor().loadOrgan(odf, guiOnly);

    // A fresh installation has no audio device chosen, no MIDI input enabled
    // and no organ. Offering the three in order beats three separate ways of
    // discovering that nothing happens when you press a key.
    //
    // Not in --gui-only: that mode exists for screenshots and smoke tests,
    // and a modal dialog over the console would defeat both.
    if (!guiOnly && mp::ui::WizardPanel::isFirstRun(*proc_))
      win_->showWizard(*proc_);
  }

  // Beside the player's own data, with the organ settings and the MIDI maps.
  static juce::File audioSettingsFile() {
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Masterpiece")
        .getChildFile("audio.xml");
  }

  void shutdown() override {
    // The device state is written on the way out rather than on every change:
    // a player dragging a buffer-size slider would otherwise rewrite the file
    // once per pixel.
    if (devices_)
      if (auto state = devices_->createStateXml()) {
        const auto f = audioSettingsFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText(state->toString());
      }

    // Before the logger is destroyed: JUCE asserts on a dangling current
    // logger, and shutdown is the one place that is guaranteed to run.
    juce::Logger::setCurrentLogger(nullptr);

    // Stop the port before its callback can be destroyed under it.
    if (virtualInput_) virtualInput_->stop();
    virtualInput_.reset();
    virtualRoute_.reset();

    if (devices_ && player_) {
      devices_->removeAudioCallback(player_.get());
      for (auto& r : routes_)
        devices_->removeMidiInputDeviceCallback(r.identifier, r.route.get());
      routes_.clear();
    }
    if (player_) player_->setProcessor(nullptr);
    win_.reset();
    player_.reset();
    devices_.reset();
    proc_.reset();
  }

  void systemRequestedQuit() override { quit(); }

private:
  struct DocWindow : juce::DocumentWindow {
    DocWindow(mp::MasterpieceProcessor& p, juce::AudioDeviceManager& dm)
        : DocumentWindow("Masterpiece", juce::Colour(0xff15171c), allButtons),
          devices_(dm) {
      auto* ed = new mp::ui::MasterpieceEditor(p);
      // The editor must not reach for hardware itself; the application owns
      // the device manager and hands the panel down.
      ed->onAudioSettings = [this] { showAudioSettings(); };
      ed->onSettings = [this, &p] { showSettings(p); };
      // A document window should name its document. It also lets anything
      // driving the app from outside wait for the organ rather than guess at
      // a duration, which on a slow disk is the difference between a console
      // and a blank panel.
      ed->onOrganLoaded = [this](const juce::String& name) {
        setName("Masterpiece - " + name);
      };
      editor_ = ed;
      setUsingNativeTitleBar(true);
      setContentOwned(ed, true);
      setResizable(true, true);
      centreWithSize(getWidth(), getHeight());
    }

    void closeButtonPressed() override {
      juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    mp::ui::MasterpieceEditor& editor() { return *editor_; }

    void showSettings(mp::MasterpieceProcessor& proc) {
      auto panel = std::make_unique<mp::ui::SettingsWindow>(proc, devices_);
      juce::DialogWindow::LaunchOptions opts;
      opts.content.setOwned(panel.release());
      opts.dialogTitle = "Masterpiece settings";
      opts.dialogBackgroundColour = juce::Colour(0xff15171c);
      opts.escapeKeyTriggersCloseButton = true;
      opts.useNativeTitleBar = true;
      opts.resizable = true;
      opts.launchAsync();
    }

    void showWizard(mp::MasterpieceProcessor& proc) {
      auto panel = std::make_unique<mp::ui::WizardPanel>(proc, devices_);
      panel->setSize(560, 520);
      // Opening the organ is the application's business: the file dialog and
      // what happens after a load both live out here.
      panel->onOpenOrgan = [this] { editor_->chooseAndLoadOrgan(); };
      juce::DialogWindow::LaunchOptions opts;
      opts.content.setOwned(panel.release());
      opts.dialogTitle = "Welcome to Masterpiece";
      opts.dialogBackgroundColour = juce::Colour(0xff15171c);
      opts.escapeKeyTriggersCloseButton = true;
      opts.useNativeTitleBar = true;
      opts.resizable = true;
      opts.launchAsync();
    }

    void showAudioSettings() {
      auto panel = std::make_unique<juce::AudioDeviceSelectorComponent>(
          devices_, 0, 0, 1, 8, true, true, true, false);
      panel->setSize(500, 450);
      juce::DialogWindow::LaunchOptions opts;
      opts.content.setOwned(panel.release());
      opts.dialogTitle = "Audio and MIDI";
      opts.dialogBackgroundColour = juce::Colour(0xff15171c);
      opts.escapeKeyTriggersCloseButton = true;
      opts.useNativeTitleBar = true;
      opts.resizable = true;
      opts.launchAsync();
    }

   private:
    juce::AudioDeviceManager& devices_;
    mp::ui::MasterpieceEditor* editor_ = nullptr;
  };

  std::unique_ptr<mp::MasterpieceProcessor> proc_;
  std::unique_ptr<juce::AudioDeviceManager> devices_;
  // One per physical input, so every message arrives knowing where it came
  // from. Runs on the driver's MIDI thread: it does nothing but tag and hand
  // over, and the processor's queue is allocation-free for the same reason.
  struct DeviceRoute : juce::MidiInputCallback {
    DeviceRoute(mp::MasterpieceProcessor& p, int id) : proc(p), deviceId(id) {}
    void handleIncomingMidiMessage(juce::MidiInput*,
                                   const juce::MidiMessage& msg) override {
      proc.pushMidi(deviceId, msg);
    }
    mp::MasterpieceProcessor& proc;
    int deviceId;
  };
  struct Routed {
    juce::String identifier;
    std::unique_ptr<DeviceRoute> route;
  };
  std::vector<Routed> routes_;

  std::unique_ptr<juce::AudioProcessorPlayer> player_;
  std::unique_ptr<DocWindow> win_;
  std::unique_ptr<juce::FileLogger> logger_;
  // Our own published port, where the platform allows one. The input holds a
  // pointer to its route, so the route is declared FIRST and therefore
  // destroyed last — the input goes away while its callback is still valid.
  std::unique_ptr<DeviceRoute> virtualRoute_;
  std::unique_ptr<juce::MidiInput> virtualInput_;
};

START_JUCE_APPLICATION(MasterpieceApp)
