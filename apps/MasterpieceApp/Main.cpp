// Masterpiece — standalone application.
//
// This is the product (see the standalone-first decision): the plugin wrappers
// are secondary. It owns the audio device and MIDI input and drives the same
// MasterpieceProcessor the plugin and the headless renderer use, so there is
// one engine and three front ends rather than three engines.
#include <iostream>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>
#include <vector>

#include "../../src/mp_audio/MasterpieceProcessor.h"
#include "../../src/mp_ui/Ui.h"
#include "../../src/mp_ui/Settings.h"
#include "../../src/mp_ui/Wizard.h"
#include "../../src/mp_control/Registration.h"

namespace {
// True when the open device can actually produce sound. A saved setup can
// name hardware that is gone — or select a type with no output at all while
// still reporting success — and either way the organ would run silent with
// flat meters and no MIDI processed. Checked after every open, so a dead
// setup falls back to defaults rather than running mute.
bool audioOutputAlive(juce::AudioDeviceManager& dm) {
  auto* dev = dm.getCurrentAudioDevice();
  return dev != nullptr &&
         dev->getActiveOutputChannels().countNumberOfSetBits() > 0;
}
} // namespace

class MasterpieceApp : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return "Masterpiece"; }
  const juce::String getApplicationVersion() override { return MP_VERSION; }
  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String& commandLine) override {
    proc_ = std::make_unique<mp::MasterpieceProcessor>();

    // Automation surface (docs/automation/gui-automation.md), read before
    // anything is built, because some of it decides how things are built:
    //   --odf <path>       load an organ at startup
    //   --draw-only        draw the stops, then hand over the console
    //   --gui-only         draw its console without reading any audio
    //   --virtual-midi [n] publish a MIDI input port of our own
    //   --log <path>       write the load phase timings to a file
    //
    // fromTokens(..., true) PRESERVES the quotes it split on, so every value
    // taken from here is unquoted before use. Organ paths almost always
    // contain spaces.
    const auto args = juce::StringArray::fromTokens(commandLine, true);
    // Say which build this is and stop. The window title carries it too, but
    // a player on a forum needs something they can copy.
    if (args.contains("--version")) {
      std::cout << "Masterpiece " << MP_VERSION << std::endl;
      juce::JUCEApplication::getInstance()->setApplicationReturnValue(0);
      quit();
      return;
    }

    const bool guiOnly = args.contains("--gui-only");
    // Report every MIDI message and what became of it. The one question a
    // player with a silent console cannot answer from outside.
    const bool logMidi = args.contains("--log-midi");
    // Which console page to show once the organ is up, counting from 1. A set
    // that puts its jambs on their own pages needs this to be photographed,
    // and clicking the tab from a script is not reliable across display
    // scalings.
    int consolePage = 0;
    for (int i = 0; i + 1 < args.size(); ++i)
      if (args[i] == "--console-page") consolePage = args[i + 1].getIntValue();

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
    auto audioError =
        devices_->initialise(0, 2, saved.get(), /*selectDefaultDeviceOnFailure*/ true);
    if (audioError.isNotEmpty() || !audioOutputAlive(*devices_)) {
      // Not merely a failed open: a setup that opens with no live output is
      // the same silence. Fall back to factory defaults, which is what a
      // fresh install gets and what the user expects on relaunch.
      if (audioError.isNotEmpty())
        juce::Logger::writeToLog("audio device: " + audioError);
      else
        juce::Logger::writeToLog("audio device: saved setup has no live output,"
                                 " falling back to defaults");
      devices_->closeAudioDevice();
      audioError = devices_->initialise(0, 2, nullptr, true);
      if (audioError.isNotEmpty())
        juce::Logger::writeToLog("audio device: " + audioError);
    }
    if (auto* dev = devices_->getCurrentAudioDevice())
      juce::Logger::writeToLog(
          "audio device: " + dev->getName() + ", " +
          juce::String(dev->getCurrentSampleRate(), 0) + " Hz, " +
          juce::String(dev->getCurrentBufferSizeSamples()) + " samples");

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

    // Play MIDI through the organ as soon as it is up, with the stops drawn.
    // Together these turn "show me this organ playing" into one command --
    // what makes it repeatable across a shelf of them, and what lets the
    // console be filmed while it plays.
    //
    // A RECITAL rather than a single piece: every one of these switches may
    // be given more than once, and each --play-midi begins a new take that
    // the --draw-stops and --record-audio around it belong to.
    //
    //   --draw-stops 1,2,11 --play-midi toccata.mid --record-audio t.wav
    //   --draw-stops 4,9    --play-midi chorale.mid --record-audio c.wav
    //
    // This exists because loading is the expensive part. A large set takes
    // fifteen minutes off this disk, and recording three pieces used to mean
    // paying that three times over for the same organ.
    struct Take {
      juce::File midi;
      juce::File audio;
      juce::Array<int> stopIds;   // explicit ids
      juce::String registration;  // or a recipe read off the stop names
      int firstN = 0;             // or the first N stops
      bool all = false;           // or everything
      bool wants() const {
        return midi != juce::File() || audio != juce::File() || all ||
               firstN > 0 || registration.isNotEmpty() || !stopIds.isEmpty();
      }
    };
    std::vector<Take> takes(1);
    // Draw the stops and hand the console over, rather than treating the
    // request as a recital. --draw-stops exists to set up a take: it draws,
    // plays, and quits, which is right for rendering and wrong for a player
    // who only wanted a few stops out without waiting for the whole organ to
    // preload. Asked for interactively it looks exactly like a crash, and was
    // reported as one. --draw-only says what it does and keeps the window.
    const bool drawOnly = args.contains("--draw-only");
    bool stayOpen = args.contains("--stay-open") || drawOnly;

    auto cwdFile = [](const juce::String& s) {
      return juce::File::getCurrentWorkingDirectory().getChildFile(s.unquoted());
    };

    for (int i = 0; i < args.size(); ++i) {
      if (args[i] == "--play-midi" && i + 1 < args.size()) {
        // A second piece starts a new take rather than replacing the first.
        if (takes.back().midi != juce::File()) takes.emplace_back();
        takes.back().midi = cwdFile(args[++i]);
      } else if (args[i] == "--record-audio" && i + 1 < args.size()) {
        takes.back().audio = cwdFile(args[++i]);
      } else if (args[i] == "--draw-stops" && i + 1 < args.size()) {
        const auto v = args[++i].unquoted();
        // "all", a count, or the stops themselves by id. The last is how a
        // registration chosen by ear gets played: --registration is a guess
        // from the stop names, and a named list is the answer.
        //
        // Applied to the NEXT take when the current one already has its
        // music, so the switches may be written either side of --play-midi.
        Take& t = (takes.back().midi != juce::File()) ? takes.emplace_back()
                                                      : takes.back();
        if (v == "all") {
          t.all = true;
        } else if (v.containsChar(',')) {
          for (const auto& tok : juce::StringArray::fromTokens(v, ",", ""))
            if (tok.trim().isNotEmpty()) t.stopIds.add(tok.trim().getIntValue());
        } else {
          t.firstN = v.getIntValue();
        }
      }
      // Register by ear rather than by index. "--draw-stops 4" means the
      // first four stops in the list, and stop lists are ordered by division
      // -- so on most organs that is four pedal stops and silent manuals.
      // --registration reads the stop NAMES and picks a combination that
      // means something, on an organ nobody has written a preset for.
      else if (args[i] == "--registration" && i + 1 < args.size()) {
        Take& t = (takes.back().midi != juce::File()) ? takes.emplace_back()
                                                      : takes.back();
        t.registration = args[++i].unquoted().toLowerCase();
      }
    }
    while (takes.size() > 1 && !takes.back().wants()) takes.pop_back();

    if (takes.front().wants()) {
      win_->onLoaded = [this, takes, stayOpen, drawOnly] {
        // Held by the chain of callbacks below rather than by the lambda, so
        // that each take can hand the next one on without copying the list.
        auto list = std::make_shared<std::vector<Take>>(takes);
        auto playFrom = std::make_shared<std::function<void(size_t)>>();

        *playFrom = [this, list, playFrom, stayOpen, drawOnly](size_t index) {
          if (index >= list->size()) {
            // Deliberately still running unless told otherwise. Closing would
            // throw away a sample set that cost a quarter of an hour to read,
            // and the usual reason to script this is to record more than one
            // thing on the same organ.
            juce::Logger::writeToLog("recital finished");
            if (!stayOpen) juce::JUCEApplication::getInstance()->systemRequestedQuit();
            return;
          }
          const Take& t = (*list)[index];

          // Let go of anything the previous piece left holding. A file that
          // ends on a held chord leaves those pipes speaking, and an organ has
          // no decay to cover it: the note sounds through the gap and into the
          // next take.
          proc_->releaseAllKeys();

          // Each take registers from scratch: leaving the previous one drawn
          // would make take two the sum of both, which is the sort of thing
          // nobody notices until the recording is listened to.
          for (const auto& e : proc_->stopList())
            proc_->setStopEngaged(e.stopId, false);

          juce::String drawn;
          auto note = [&drawn, this](mp::Id id) {
            const auto it = proc_->organModel().stops.find(id);
            if (it != proc_->organModel().stops.end())
              drawn += (drawn.isEmpty() ? "" : ", ") + juce::String(it->second.name);
          };
          if (!t.stopIds.isEmpty()) {
            for (int id : t.stopIds) {
              if (proc_->organModel().stops.count(id) == 0) {
                juce::Logger::writeToLog("no stop " + juce::String(id) +
                                         " on this organ");
                continue;
              }
              proc_->setStopEngaged(id, true);
              note(id);
            }
          } else if (t.registration.isNotEmpty()) {
            const auto style =
                mp::registrationFromName(t.registration.toStdString());
            for (mp::Id id : mp::chooseRegistration(proc_->organModel(), style)) {
              proc_->setStopEngaged(id, true);
              note(id);
            }
          } else if (t.all) {
            proc_->engageAllStops();
            drawn = "everything";
          } else if (t.firstN > 0) {
            int n = 0;
            for (const auto& e : proc_->stopList()) {
              if (n++ >= t.firstN) break;
              proc_->setStopEngaged(e.stopId, true);
              note(e.stopId);
            }
          }
          juce::Logger::writeToLog("take " + juce::String((int)index + 1) + "/" +
                                   juce::String((int)list->size()) +
                                   " drawn: " + drawn);

          // Stop here under --draw-only: no piece to play, nothing to advance
          // to, and above all no quit. The organ is loaded and the stops are
          // out; the console belongs to whoever is sitting at it.
          if (drawOnly) {
            juce::Logger::writeToLog("draw-only: console ready, stops drawn");
            return;
          }

          if (!t.midi.existsAsFile()) {
            if (t.midi != juce::File())
              juce::Logger::writeToLog("no such MIDI: " + t.midi.getFullPathName());
            (*playFrom)(index + 1);
            return;
          }
          if (!proc_->recorder().loadFromFile(t.midi)) {
            juce::Logger::writeToLog("could not read MIDI: " +
                                     t.midi.getFullPathName());
            (*playFrom)(index + 1);
            return;
          }

          // A beat of silence first: a file that starts the instant the
          // console appears is cut off at the head by every recorder. The
          // same beat also lets the previous take's release tails die away
          // rather than bleeding into the next one.
          juce::Timer::callAfterDelay(1500, [this, list, playFrom, index] {
            const Take& take = (*list)[index];
            // Capture from inside the program rather than off the sound card.
            // What the engine produced is what gets written -- no loopback
            // device to find, no other application's sounds, and nothing lost
            // if the machine stutters. Started in the SAME callback as
            // playback, so the file begins where the music does.
            if (take.audio != juce::File()) {
              take.audio.getParentDirectory().createDirectory();
              if (!proc_->audioRecorder().start(take.audio,
                                                proc_->getSampleRate(), 2))
                juce::Logger::writeToLog("could not record to " +
                                         take.audio.getFullPathName());
            }
            // Wall-clock, to the millisecond, at the instant the music
            // starts. A screen recording of a whole recital is one long file,
            // and this is what lets it be cut back into takes afterwards
            // without lining anything up by eye.
            juce::Logger::writeToLog(
                "take " + juce::String((int)index + 1) + " starts at " +
                juce::String(juce::Time::getCurrentTime().toMilliseconds()));
            proc_->recorder().startPlayback();

            // Poll for the end of the piece. The recorder reports whether it
            // is playing but announces nothing when it stops, and a poll at
            // this rate costs nothing next to rendering the organ.
            struct Watch : public juce::Timer {
              MasterpieceApp* app;
              std::shared_ptr<std::vector<Take>> list;
              std::shared_ptr<std::function<void(size_t)>> playFrom;
              size_t index;
              void timerCallback() override {
                if (app->proc_->recorder().isPlaying()) return;
                stopTimer();
                app->proc_->audioRecorder().stop();
                auto next = playFrom;
                const size_t i = index;
                // Deleted from outside its own callback.
                juce::MessageManager::callAsync([next, i] { (*next)(i + 1); });
                delete this;
              }
            };
            auto* w = new Watch{};
            w->app = this;
            w->list = list;
            w->playFrom = playFrom;
            w->index = index;
            w->startTimer(250);
          });
        };
        (*playFrom)(0);
      };
    }

    // Memory knobs, so a study of what an organ costs to hold can be driven
    // from a script rather than from the settings page. All three apply to
    // the NEXT load, which is why they are read before loadOrgan below.
    //
    //   --storage int24|int16 what a resident frame costs
    //   --load-mono on           fold a stereo set to one channel
    //   --load-rate 48000        convert as it loads (0 = as recorded)
    //   --cache single|off       keep the decoded samples for the next load
    //   --stream-releases on     hold only the head of each release tail
    //   --preload-head <frames>  minimum head of every sample (0 = whole file)
    for (int i = 0; i < args.size(); ++i) {
      if (args[i] == "--storage" && i + 1 < args.size()) {
        const auto v = args[++i].unquoted().trim().toLowerCase();
        if (v == "int24") {
          proc_->setSampleStorage(mp::SampleStorage::Int24);
        } else if (v == "int16") {
          proc_->setSampleStorage(mp::SampleStorage::Int16);
        } else if (v == "float32") {
          proc_->setSampleStorage(mp::SampleStorage::Float32);
        } else {
          juce::Logger::writeToLog(
              "--storage: expected int24, int16 or float32, got " + v);
        }
        proc_->overrideSetting("storage");
      } else if (args[i] == "--load-mono" && i + 1 < args.size()) {
        const auto v = args[++i].unquoted().trim().toLowerCase();
        proc_->setLoadMono(v == "on" || v == "1" || v == "true" || v == "yes");
        proc_->overrideSetting("mono");
      } else if (args[i] == "--cache" && i + 1 < args.size()) {
        const auto v = args[++i].unquoted().trim().toLowerCase();
        proc_->setCacheMode(v == "off"        ? mp::SampleLibrary::CacheMode::Off
                            : v == "per-organ" ? mp::SampleLibrary::CacheMode::PerOrgan
                                               : mp::SampleLibrary::CacheMode::Single);
      } else if (args[i] == "--load-rate" && i + 1 < args.size()) {
        proc_->setLoadSampleRate(args[++i].unquoted().getDoubleValue());
        proc_->overrideSetting("rate");
      } else if (args[i] == "--stream-releases" && i + 1 < args.size()) {
        const auto v = args[++i].unquoted().trim().toLowerCase();
        proc_->setStreamReleases(v == "on" || v == "1" || v == "true" || v == "yes");
        proc_->overrideSetting("stream");
      } else if (args[i] == "--preload-head" && i + 1 < args.size()) {
        proc_->setPreloadHeadFrames((int64_t)args[++i].unquoted().getLargeIntValue());
        proc_->overrideSetting("preload");
      }
    }
    juce::Logger::writeToLog(
        juce::String("memory config: storage=") +
        (proc_->sampleStorage() == mp::SampleStorage::Int16   ? "int16"
         : proc_->sampleStorage() == mp::SampleStorage::Int24 ? "int24"
                                                              : "float32") +
        ", mono=" + (proc_->loadMono() ? "on" : "off") +
        ", rate=" + (proc_->loadSampleRate() > 0.0
                         ? juce::String(proc_->loadSampleRate(), 0)
                         : juce::String("as recorded")) +
        ", streamReleases=" + (proc_->streamReleases() ? "on" : "off") +
        ", preloadHead=" + juce::String(proc_->preloadHeadFrames()) + " frames");

    // --preload-drawn reads only the ranks the recital will actually draw.
    // On a large set that is the difference between a minute and a few
    // seconds, which is the whole cost of trying a registration. Everything
    // NOT in the list is silent afterwards, so it is opt-in and says so in
    // the log.
    if (args.contains("--preload-drawn")) {
      std::vector<mp::Id> wanted;
      for (const auto& t : takes)
        for (int id : t.stopIds) wanted.push_back(id);
      if (wanted.empty())
        juce::Logger::writeToLog(
            "--preload-drawn ignored: no --draw-stops id list to narrow to");
      else
        proc_->setPreloadStops(std::move(wanted));
    }

    // --organ-root <dir>: where this organ's OrganInstallationPackages is,
    // for a layout the definition's own path cannot reveal.
    {
      const int at = args.indexOf("--organ-root");
      if (at >= 0 && at + 1 < args.size())
        proc_->setOrganRootOverride(juce::File(args[at + 1]));
    }

    // --preload-ranks 2,4,14: load exactly these ranks. For organs whose
    // stops reach their pipes through pallets, where the drawn stops name no
    // ranks and --preload-drawn cannot narrow the load.
    {
      const int at = args.indexOf("--preload-ranks");
      if (at >= 0 && at + 1 < args.size()) {
        std::vector<mp::Id> ranks;
        for (const auto& s : juce::StringArray::fromTokens(args[at + 1], ",", ""))
          if (s.getIntValue() > 0) ranks.push_back(s.getIntValue());
        proc_->setPreloadRanks(std::move(ranks));
      }
    }

    if (consolePage > 0) {
      auto* win = win_.get();
      // Chained rather than assigned: a take list may already have claimed
      // this hook, and choosing a page must not cancel the performance.
      auto previous = std::move(win->onLoaded);
      win->onLoaded = [win, consolePage, previous] {
        win->editor().showConsolePage(consolePage);
        if (previous) previous();
      };
    }

    if (logMidi) {
      proc_->setMidiLogging(true);
      juce::Logger::writeToLog(
          "midi: logging on. Every message and its outcome follows.");
      for (const auto& in : juce::MidiInput::getAvailableDevices())
        juce::Logger::writeToLog("midi: input device present: " + in.name);
      // The manual selector is built from this list, keyed by channel. Two
      // keyboards answering to the same channel collide in that menu, so the
      // list is worth seeing outright. Chained onto the load hook rather than
      // assigned, so it cannot cancel a performance that claimed it first.
      auto* win = win_.get();
      auto previous = std::move(win->onLoaded);
      win->onLoaded = [this, win, previous] {
        juce::Logger::writeToLog("midi: playable keyboards (id, channel, name):");
        for (mp::Id kb : proc_->playableKeyboards())
          juce::Logger::writeToLog(
              "midi:   keyboard " + juce::String(static_cast<int>(kb)) +
              " -> channel " + juce::String(proc_->channelForKeyboard(kb)) +
              "  code(model)=" +
              juce::String(proc_->organModel().keyboards.count(kb)
                               ? proc_->organModel().keyboards.at(kb).assignmentCode
                               : -1) +
              "  code(resolved)=" + juce::String(proc_->assignmentCodeOf(kb)) +
              "  \"" + juce::String(proc_->keyboardName(kb)) + "\"");
        (void)win;
        if (previous) previous();
      };
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
        : DocumentWindow("Masterpiece " MP_VERSION, juce::Colour(0xff15171c),
                         allButtons),
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
        // The version stays in the title with the organ's name. Asked for:
        // a player who has downloaded a build has no other way to tell which
        // one they are running.
        setName("Masterpiece " MP_VERSION " - " + name);
        if (onLoaded) onLoaded();
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
    // Run once the organ is up. Used to start a demonstration performance
    // without a human having to click through a file dialog first.
    std::function<void()> onLoaded;

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
      panel->onOpenOrgan = [this] { editor_->showOrganDialog(); };
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
