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

#include <csignal>
#include <functional>
#include <memory>
#include <vector>

#include "../../src/mp_audio/MasterpieceProcessor.h"
#include "../../src/mp_ui/Ui.h"
#include "../../src/mp_ui/OrganSettings.h"
#include "../../src/mp_ui/Settings.h"
#include "../../src/mp_ui/Wizard.h"
#include "JackMidi.h"
#include "../../src/mp_ui/Mobile.h"
#include "../../src/mp_control/Registration.h"

#if JUCE_LINUX
 #include <alsa/asoundlib.h>
#endif

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
// Set from a signal handler: SIGTERM or SIGHUP, as a system shutdown, a
// logout or a service manager stopping the program sends them. Only a flag
// may be touched there; the message thread acts on it (QuitWatch).
volatile std::sig_atomic_t quitSignalled = 0;
#if JUCE_LINUX || JUCE_MAC
extern "C" void onQuitSignal(int) { quitSignalled = 1; }
#endif
} // namespace

class MasterpieceApp : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return "Masterpiece"; }
  const juce::String getApplicationVersion() override { return MP_VERSION; }
  bool moreThanOneInstanceAllowed() override { return true; }

  // The first argument that names an organ definition or package that exists.
  static juce::File organFileIn(const juce::StringArray& args) {
    for (const auto& a : args) {
      const auto path = a.unquoted();
      if (path.startsWithChar('-')) continue;
      const auto f = juce::File::getCurrentWorkingDirectory().getChildFile(path);
      const auto ext = f.getFileExtension().toLowerCase();
      if (f.existsAsFile() && (ext == ".organ_hauptwerk_xml" || ext == ".customorgan_hauptwerk_xml" ||
                               ext == ".organ" || ext == ".orgue" || ext == ".rar"))
        return f;
    }
    return {};
  }

  // macOS hands a file chosen in Finder's "Open With" to the running app this
  // way, and so does a second launch where only one instance runs.
  void anotherInstanceStarted(const juce::String& commandLine) override {
    const auto f = organFileIn(juce::StringArray::fromTokens(commandLine, true));
    if (f != juce::File() && win_ != nullptr) win_->editor().loadOrgan(f, false);
  }

  // A MIDI input's identifier where it names the same device in the next run:
  // Windows' interface path (which includes the USB socket) and macOS's
  // CoreMIDI id. Linux numbers its ports in the order the devices appear, so
  // there the identifier is the USB socket of the card behind the port, with
  // the port's number on that card. Android numbers its devices afresh.
  static juce::String lastingIdentifier(const juce::MidiDeviceInfo& in) {
   #if JUCE_WINDOWS || JUCE_MAC || JUCE_IOS
    return in.identifier;
   #elif JUCE_LINUX
    return alsaUsbSocket(in.identifier);
   #else
    juce::ignoreUnused(in);
    return {};
   #endif
  }

 #if JUCE_LINUX
  // "usb-0000:00:1d.0-1.2.3/0" for port 0 of the USB device in socket 1.2.3,
  // from the ALSA sequencer port JUCE names "client-port". Two keyboards of
  // the same model, which have the same name and often the same serial
  // number, are told apart by the socket each is plugged into, and keep it
  // when they are plugged in again in another order. The socket is read from
  // the card's long name, which the USB audio driver writes as
  // "<product> at usb-<socket>, <speed>". A device that is not on USB, or a
  // port that is not a card's, has none and is known by its name.
  static juce::String alsaUsbSocket(const juce::String& juceId) {
    const auto numbers = juce::StringArray::fromTokens(juceId, "-:", "");
    if (numbers.size() < 2 || !numbers[0].containsOnly("0123456789") ||
        !numbers[1].containsOnly("0123456789"))
      return {};
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) return {};
    snd_seq_client_info_t* info = nullptr;
    snd_seq_client_info_alloca(&info);
    int card = -1;
    if (snd_seq_get_any_client_info(seq, numbers[0].getIntValue(), info) == 0)
      card = snd_seq_client_info_get_card(info);
    snd_seq_close(seq);
    if (card < 0) return {};
    char* longName = nullptr;
    if (snd_card_get_longname(card, &longName) < 0 || longName == nullptr) return {};
    const juce::String name(longName);
    std::free(longName);
    if (!name.contains(" at usb-")) return {};
    const auto socket = name.fromFirstOccurrenceOf(" at usb-", false, false)
                            .upToFirstOccurrenceOf(",", false, false)
                            .trim();
    return socket.isEmpty() ? juce::String() : "usb-" + socket + "/" + numbers[1];
  }
 #endif

  void initialise(const juce::String& commandLine) override {
    // A shutdown or reboot ends the program with SIGTERM. Left to its default
    // the signal ends it at once, without shutdown(), and at the next start
    // that looked like a crash. Handled, it is an ordinary quit.
   #if JUCE_LINUX || JUCE_MAC
    struct sigaction quitAction {};
    quitAction.sa_handler = onQuitSignal;
    sigemptyset(&quitAction.sa_mask);
    sigaction(SIGTERM, &quitAction, nullptr);
    sigaction(SIGHUP, &quitAction, nullptr);
   #endif
    quitWatch_ = std::make_unique<QuitWatch>();
    proc_ = std::make_unique<mp::MasterpieceProcessor>();
    // Remember which organ is loaded until a clean exit, so a crash is not
    // repeated by reopening the organ it happened with.
    proc_->setCrashGuard(true);

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
    // What every option above and below does, for a player at a terminal
    // (#90). Kept beside the parser it describes.
    if (args.contains("--help") || args.contains("-h") || args.contains("/?")) {
      std::cout <<
          "Masterpiece " MP_VERSION " - pipe organ sample player\n"
          "\n"
          "Usage: Masterpiece [options] [organ file]\n"
          "\n"
          "  <organ file>             an organ definition (.Organ_Hauptwerk_xml, .organ) or\n"
          "                           package (.rar, .orgue) to open; the same as --odf\n"
          "  --odf <file>             open this organ\n"
          "  --gui-only               draw its console without reading any audio\n"
          "  --console-page <n>       show this console page once the organ is up (from 1)\n"
          "  --storage <format>       int16, int24 or float32 samples in memory\n"
          "  --load-mono <on|off>     load samples as mono\n"
          "  --stream-releases <on|off>  stream release tails from disk\n"
          "  --preload-head <frames>  minimum head of every sample (0 = whole file)\n"
          "  --load-rate <Hz>         convert samples to this rate as they load\n"
          "  --cache <off|single|per-organ>  the sample cache\n"
          "  --log <file>             write the log to this file\n"
          "  --log-midi               log every MIDI message and what became of it\n"
          "  --log-releases           log every key release\n"
          "  --record-midi <file>     record everything played, saved on quit\n"
          "  --virtual-midi [name]    publish a MIDI input port of Masterpiece's own\n"
          "  --play-midi <file>       play a MIDI file through the organ once it is up\n"
          "  --record-audio <file>    record the output while --play-midi plays\n"
          "  --draw-stops <all|n|ids> draw these stops for --play-midi\n"
          "  --registration <name>    draw a registration chosen from the stop names\n"
          "  --preload-drawn          load only the ranks the drawn stops use\n"
          "  --draw-only              draw the stops, then hand over the console\n"
          "  --stay-open              stay open after --play-midi has played\n"
          "  --version                print the version and quit\n"
          "  -h, --help               print this and quit\n";
      std::cout.flush();
      juce::JUCEApplication::getInstance()->setApplicationReturnValue(0);
      quit();
      return;
    }
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
    // Describe every key release: which of the pipe's releases played, its
    // length, where it started and how it joined the attack. For a click or
    // a noise heard when keys let go.
    const bool logReleases = args.contains("--log-releases");
    // Record everything played, from start to quit, as a MIDI file:
    //   --record-midi performance.mid
    // For a fault that cannot be reproduced on demand: play until it happens,
    // quit, and the moment is in the last seconds of the file, which replays
    // it exactly.
    for (int i = 0; i + 1 < args.size(); ++i)
      if (args[i] == "--record-midi")
        recordMidiTo_ = juce::File::getCurrentWorkingDirectory().getChildFile(
            args[i + 1].unquoted());
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
    // Without --log, a log anyway: when an organ will not open on someone
    // else's machine, this file is what they can send. This session's, and
    // the one before it, so a crash does not lose the run that crashed.
    if (logger_ == nullptr) {
      const auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("Masterpiece");
      dir.createDirectory();
      const auto current = dir.getChildFile("masterpiece.log");
      if (current.existsAsFile()) current.moveFileTo(dir.getChildFile("masterpiece.previous.log"));
      logger_ = std::make_unique<juce::FileLogger>(current, "Masterpiece " MP_VERSION " log");
      juce::Logger::setCurrentLogger(logger_.get());
    }

    // --log-touches: every press written down with the component that took
    // it, for finding what a tap that does nothing landed on (#197).
    if (args.contains("--log-touches")) {
      touchLog_ = std::make_unique<TouchLog>();
      juce::Desktop::getInstance().addGlobalMouseListener(touchLog_.get());
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
    // Whether the setup is the player's or one Masterpiece picked: only the
    // second gets the organ-worthy defaults below.
    bool pickedHere = saved == nullptr;
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
      pickedHere = true;
      if (audioError.isNotEmpty())
        juce::Logger::writeToLog("audio device: " + audioError);
    }
    // A device can open at a rate no organ survives: some default to 8 kHz
    // (a virtual "Steam Streaming" output did), where nothing above 4 kHz can
    // sound and every top octave comes out wrong (#90). Nobody chooses that
    // for an organ, so a rate below 44.1 kHz moves to 48 kHz, or 44.1 kHz,
    // whatever was saved. And a setup Masterpiece picked itself -- a first
    // start, or the defaults after a saved one failed -- gets a buffer of
    // about 10 ms: small enough to play, large enough not to crackle.
    if (auto* dev = devices_->getCurrentAudioDevice()) {
      auto setup = devices_->getAudioDeviceSetup();
      bool change = false;
      if (dev->getCurrentSampleRate() < 44100.0) {
        const auto rates = dev->getAvailableSampleRates();
        const double better = rates.contains(48000.0) ? 48000.0 : rates.contains(44100.0) ? 44100.0 : 0.0;
        if (better > 0.0) {
          juce::Logger::writeToLog("audio device: " + juce::String(dev->getCurrentSampleRate(), 0) +
                                   " Hz is too low for an organ; using " + juce::String(better, 0) + " Hz");
          setup.sampleRate = better;
          change = pickedHere = true;
        }
      }
      if (pickedHere) {
        const double rate = setup.sampleRate > 0.0 ? setup.sampleRate : dev->getCurrentSampleRate();
        const int wanted = juce::roundToInt(rate * 0.010);
        int best = 0;
        for (int size : dev->getAvailableBufferSizes())
          if (best == 0 || std::abs(size - wanted) < std::abs(best - wanted)) best = size;
        if (best > 0 && best != dev->getCurrentBufferSizeSamples()) {
          setup.bufferSize = best;
          change = true;
        }
      }
      if (change) devices_->setAudioDeviceSetup(setup, true);
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
    // Every one except those the player switched off (#230): a console
    // plugged in for the first time plays at once.
    proc_->readMidiInputSwitches();
    for (const auto& in : juce::MidiInput::getAvailableDevices()) {
      devices_->setMidiInputDeviceEnabled(in.identifier, proc_->midiInputEnabled(in.name));
      auto route = std::make_unique<DeviceRoute>(
          *proc_, proc_->registerMidiDevice(in.name, lastingIdentifier(in)));
      devices_->addMidiInputDeviceCallback(in.identifier, route.get());
      routes_.push_back({in.identifier, std::move(route)});
    }
    // And JACK's MIDI, which the inputs above do not include (#198).
    jackMidi_ = JackMidiInput::open(*proc_);

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
            *proc_, proc_->registerOwnMidiInput(name));
        virtualInput_ = juce::MidiInput::createNewDevice(name, virtualRoute_.get());
        if (virtualInput_ != nullptr) {
          virtualInput_->start();
          juce::Logger::writeToLog("virtual MIDI input: " + name);
        } else {
          juce::Logger::writeToLog("could not create a virtual MIDI input");
        }
      }
#endif

    mp::ui::installTouchLook();
    mp::ui::restoreLinkedDocuments();
    win_ = std::make_unique<DocWindow>(*proc_, *devices_);
    win_->setVisible(true);
    // A phone or tablet plays full screen, with the system's bars hidden
    // until a swipe from the edge brings them back. Not on iOS: kiosk mode
    // sizes the window to JUCE's display, which stays upright on a turned
    // iPad, and there the window follows the scene instead (DocWindow).
   #if !JUCE_IOS
    if (mp::ui::kMobile) juce::Desktop::getInstance().setKioskModeComponent(win_.get(), false);
   #endif

    juce::File odf;
    for (int i = 0; i < args.size(); ++i)
      if (args[i] == "--odf" && i + 1 < args.size()) {
        // fromTokens(..., true) PRESERVES the quotes it split on, so a path
        // with spaces arrives as "C:\...xml" including the quote characters
        // and resolves to nothing. Organ paths almost always contain spaces.
        const auto path = args[++i].unquoted();
        odf = juce::File::getCurrentWorkingDirectory().getChildFile(path);
      }
    // An organ file on its own, as "Open with Masterpiece" passes it (#90).
    if (odf == juce::File())
      odf = organFileIn(args);

    // Nothing named on the command line: pick up where the player left off.
    // loadGlobalDefaults is what knows which organ that was, and it answers
    // with nothing if the file has since moved or the drive is unplugged.
    // Read the global file first in any case: it is also what says whether
    // the last session ended cleanly.
    proc_->loadGlobalDefaults();
    const juce::File crashed = proc_->crashedOrgan();
    proc_->forgetCrash();
    if (odf == juce::File() && proc_->reopenLastOrgan()) {
      odf = proc_->lastOrgan();
      // Reopened after an unclean end too. That is usually a power cut or a
      // killed process, and a console with no screen has nobody to open the
      // organ by hand. The program is started once per login, so an organ
      // that did crash it is not reopened in a loop.
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
      // A script plays this organ, and nobody is there to press Keep changes:
      // an organ opened for the first time would otherwise wait in Organ
      // settings with its samples unloaded, and the piece play in silence.
      win_->editor().onBeforeFirstLoad = nullptr;
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
    //   --storage int24|int16    what a resident frame costs
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

    if (recordMidiTo_ != juce::File()) {
      proc_->recorder().startRecording();
      juce::Logger::writeToLog("midi: recording to " + recordMidiTo_.getFullPathName() +
                               ", saved on quit");
    }

    if (logReleases) proc_->setReleaseLogging(true);

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

    // An unclean end is written to the log. Nothing waits for a click: on a
    // console that starts by itself there is nobody to give one.
    if (crashed != juce::File())
      juce::Logger::writeToLog("start: the last session did not end cleanly while " +
                               crashed.getFileName() + " was loaded" +
                               (odf == crashed ? "; reopened" : ""));

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
  // Where the main window was, and whether maximised, as JUCE writes it.
  static juce::File windowStateFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Masterpiece")
        .getChildFile("window.txt");
  }

  static juce::File audioSettingsFile() {
    return juce::File::getSpecialLocation(
               juce::File::userApplicationDataDirectory)
        .getChildFile("Masterpiece")
        .getChildFile("audio.xml");
  }

  void shutdown() override {
    if (touchLog_ != nullptr) juce::Desktop::getInstance().removeGlobalMouseListener(touchLog_.get());
    // The device state is written on the way out rather than on every change:
    // a player dragging a buffer-size slider would otherwise rewrite the file
    // once per pixel.
    if (devices_)
      if (auto state = devices_->createStateXml()) {
        const auto f = audioSettingsFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText(state->toString());
      }

    // The main window's place, for the next start (#53).
    if (win_) windowStateFile().replaceWithText(win_->getWindowStateAsString());

    // Before the logger is destroyed: JUCE asserts on a dangling current
    // logger, and shutdown is the one place that is guaranteed to run.
    juce::Logger::setCurrentLogger(nullptr);

    // Stop the port before its callback can be destroyed under it.
    if (virtualInput_) virtualInput_->stop();
    virtualInput_.reset();
    virtualRoute_.reset();
    jackMidi_.reset();  // its process thread hands messages to the processor

    if (devices_ && player_) {
      devices_->removeAudioCallback(player_.get());
      for (auto& r : routes_)
        devices_->removeMidiInputDeviceCallback(r.identifier, r.route.get());
      routes_.clear();
    }
    if (player_) player_->setProcessor(nullptr);
    // Audio has stopped, so the recording is complete and safe to write.
    if (proc_ && recordMidiTo_ != juce::File()) {
      proc_->recorder().stopRecording();
      proc_->recorder().saveToFile(recordMidiTo_);
    }
    // A clean exit: the organ that was loaded did not crash anything.
    if (proc_) proc_->clearRunningOrgan();
    win_.reset();
    player_.reset();
    devices_.reset();
    if (proc_) proc_->setReleaseLogging(false);
    proc_.reset();
    // Every window is gone by now, so nothing still points at the look.
    mp::ui::removeTouchLook();
  }

  void systemRequestedQuit() override { quit(); }

  // A phone or tablet ends a backgrounded app whenever it wants its memory,
  // without calling shutdown(): to the crash guard that looked like a crash,
  // and the organ was not reopened. In the background the session counts as
  // ended cleanly; in front again, as running.
  void suspended() override {
    if (mp::ui::kMobile && proc_) proc_->clearRunningOrgan();
  }
  void resumed() override {
    if (mp::ui::kMobile && proc_) proc_->markRunningOrgan();
  }

  // Android's back button closes the panel in front, as Escape does on a
  // desktop. With none open, the system's own back applies.
  bool backButtonPressed() override { return mp::ui::closeFrontWindow(win_.get()); }

private:
  // Turns a quit signal into the same quit a closed window makes.
  struct QuitWatch : juce::Timer {
    QuitWatch() { startTimer(200); }
    void timerCallback() override {
      if (quitSignalled == 0) return;
      stopTimer();
      juce::Logger::writeToLog("quit: asked by the system (signal)");
      if (auto* app = juce::JUCEApplication::getInstance()) app->systemRequestedQuit();
    }
  };
  std::unique_ptr<QuitWatch> quitWatch_;
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
      ed->onOrganSettings = [this, &p] { showOrganSettings(p); };
      ed->onBeforeFirstLoad = [this, &p](std::function<void()> load) {
        showOrganSettings(p, std::move(load));
      };
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
      // On a desktop the window takes the console's size. On a phone or
      // tablet the screen gives the size, and the console sits inside the
      // safe area: a window that also followed its content shrank to fit it
      // and was placed smaller again, down to 2 by 2 pixels, a black screen.
      setContentOwned(ed, !mp::ui::kMobile);
      // A phone or tablet gives the console the whole screen: no title bar,
      // nothing to resize, and no place to remember.
      if (mp::ui::kMobile) {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(0);
        setResizable(false, false);
       #if JUCE_IOS
        // The whole scene, the console placed inside its safe area by
        // resized(), and the scene watched: JUCE resizes nothing when an
        // iPad turns or its window is resized.
        setBounds(mp::ui::screenBounds());
        sceneWatch_.startTimer(500);
        // Every panel, dialog, menu and alert in a layer over the console.
        addAndMakeVisible(panelHost_);
        mp::ui::setPanelHost(&panelHost_);
       #else
        setBounds(mp::ui::screenArea());
       #endif
        return;
      }
      setUsingNativeTitleBar(true);
      setResizable(true, true);
      centreWithSize(getWidth(), getHeight());
      // Where it was last time (#53), unless no screen shows it any more: a
      // second monitor unplugged must not leave the window off every screen.
      const auto saved = windowStateFile().loadFileAsString();
      if (saved.isNotEmpty() && restoreWindowStateFromString(saved) &&
          juce::Desktop::getInstance().getDisplays().getDisplayForPoint(getBounds().getCentre()) == nullptr)
        centreWithSize(getWidth(), getHeight());
      if (!isFullScreen()) mp::ui::keepOnScreen(*this);
    }

    void closeButtonPressed() override {
      juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

   #if JUCE_IOS
    // The window covers the whole scene, and is sized again when
    // the iPad turns or its window changes. The console sits inside the
    // display's safe area, clear of the window controls, the rounded corners
    // and the home indicator, where every tap lands (#197).
    void resized() override {
      juce::DocumentWindow::resized();
      if (auto* content = getContentComponent()) {
        const auto safe = getLocalArea(nullptr, mp::ui::screenArea()).getIntersection(getLocalBounds());
        content->setBounds(safe);
        // The layer the panels open in, inside the safe area itself: a
        // dialog's title bar under the status bar takes no taps, and its
        // close button with it (#233).
        panelHost_.setBounds(safe);
        panelHost_.toFront(false);
      }
    }

    // The layer the panels open in: clear, and passing touches through where
    // it holds nothing; a panel in it fills it.
    struct PanelHost final : juce::Component {
      PanelHost() { setInterceptsMouseClicks(false, true); }
      void resized() override {
        for (auto* c : getChildren())
          if (auto* w = dynamic_cast<juce::ResizableWindow*>(c)) w->setBounds(getLocalBounds());
          else if (auto* a = dynamic_cast<juce::AlertWindow*>(c)) a->setCentrePosition(getLocalBounds().getCentre());
      }
    };
    PanelHost panelHost_;

    struct SceneWatch final : juce::Timer {
      juce::DocumentWindow& window;
      juce::Rectangle<int> area;
      explicit SceneWatch(juce::DocumentWindow& w) : window(w) {}
      void timerCallback() override {
        // A modal window behind the console takes every touch while nobody
        // can see it or close it (#197): it is kept in front.
        if (auto* m = juce::ModalComponentManager::getInstance()->getModalComponent(0))
          if (m->isOnDesktop() && m != &window && !m->isAlwaysOnTop()) {
            auto& desktop = juce::Desktop::getInstance();
            if (desktop.getComponent(desktop.getNumComponents() - 1) != m) m->toFront(true);
          }
        const auto whole = mp::ui::screenBounds();
        const auto safe = mp::ui::screenArea();
        if (whole == window.getBounds() && safe == area) return;
        // Written down at every change: what the scene, the window and JUCE's
        // display say is the record a report from an iPad needs.
        const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        juce::Logger::writeToLog("screen: scene " + whole.toString() + ", safe " + safe.toString() +
                                 ", window " + window.getBounds().toString() + ", display " +
                                 (d != nullptr ? d->totalArea.toString() : juce::String("none")));
        area = safe;
        // The whole layout again, the panel layer with the content: placing
        // the content alone left the layer the size of the first, unsafe
        // area, and a dialog's close button under the status bar (#233).
        if (whole != window.getBounds()) window.setBounds(whole);
        else window.resized();
      }
    };
    SceneWatch sceneWatch_{*this};
   #endif

    mp::ui::MasterpieceEditor& editor() { return *editor_; }
    // Run once the organ is up. Used to start a demonstration performance
    // without a human having to click through a file dialog first.
    std::function<void()> onLoaded;

    void showSettings(mp::MasterpieceProcessor& proc, std::function<void()> onClosed = {}) {
      auto panel = std::make_unique<mp::ui::SettingsWindow>(proc, devices_);
      panel->onClosed = std::move(onClosed);
      panel->setOrganLoader([this](const juce::File& f) { editor().loadOrgan(f, false); });
      juce::DialogWindow::LaunchOptions opts;
      opts.content.setOwned(panel.release());
      opts.dialogTitle = "General settings";
      opts.dialogBackgroundColour = juce::Colour(0xff15171c);
      opts.escapeKeyTriggersCloseButton = true;
      opts.useNativeTitleBar = true;
      opts.resizable = true;
      mp::ui::launchDialog(opts);
    }

    void showOrganSettings(mp::MasterpieceProcessor& proc, std::function<void()> onClosed = {}) {
      auto panel = std::make_unique<mp::ui::OrganSettingsWindow>(
          proc, [this] { editor_->reloadOrgan(); }, &devices_);
      panel->onClosed = std::move(onClosed);
      juce::DialogWindow::LaunchOptions opts;
      opts.content.setOwned(panel.release());
      opts.dialogTitle = "Organ settings";
      opts.dialogBackgroundColour = juce::Colour(0xff15171c);
      opts.escapeKeyTriggersCloseButton = true;
      opts.useNativeTitleBar = true;
      opts.resizable = true;
      mp::ui::launchDialog(opts);
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
      mp::ui::launchDialog(opts);
    }

    void showAudioSettings() {
      auto panel = std::make_unique<mp::ui::AudioSettingsPanel>(
          devices_, editor_->organProcessor());
      juce::DialogWindow::LaunchOptions opts;
      opts.content.setOwned(panel.release());
      opts.dialogTitle = "Audio and MIDI";
      opts.dialogBackgroundColour = juce::Colour(0xff15171c);
      opts.escapeKeyTriggersCloseButton = true;
      opts.useNativeTitleBar = true;
      opts.resizable = true;
      mp::ui::launchDialog(opts);
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
  // Where --record-midi saves the session on quit; empty when not asked.
  juce::File recordMidiTo_;
  std::unique_ptr<DocWindow> win_;
  std::unique_ptr<juce::FileLogger> logger_;
  // Also writes down the modal component whenever it changes: a modal window
  // the player cannot see takes every touch, and no listener hears them.
  struct TouchLog final : juce::MouseListener, juce::Timer {
    TouchLog() { startTimer(500); }
    juce::String lastModal, lastWindows, lastDesktop;
    void timerCallback() override {
      auto* mcm = juce::ModalComponentManager::getInstance();
      juce::String now = juce::String(mcm->getNumModalComponents()) + " modal";
      if (auto* m = mcm->getModalComponent(0))
        now << ": '" << m->getName() << "' " << typeid(*m).name() << " at "
            << m->getScreenBounds().toString() << (m->isShowing() ? " showing" : " NOT showing")
            << (m->isOnDesktop() ? "" : " off the desktop");
      // JUCE's own windows, back to front, named: which one a UIKit window
      // in the list below is.
      juce::String desk;
      auto& desktop = juce::Desktop::getInstance();
      for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* c = desktop.getComponent(i)) {
          bool self = true, children = true;
          c->getInterceptsMouseClicks(self, children);
          desk << "\n  '" << c->getName() << "' " << typeid(*c).name() << " "
               << c->getScreenBounds().toString() << (c->isVisible() ? " visible" : " hidden")
               << (self ? "" : " clicks-through") << (children ? "" : " children-deaf");
        }
      if (desk != lastDesktop) {
        lastDesktop = desk;
        juce::Logger::writeToLog("desktop:" + desk);
      }
     #if JUCE_IOS
      const auto windows = mp::ui::describeWindows();
      if (windows != lastWindows) {
        lastWindows = windows;
        juce::Logger::writeToLog("windows: " + windows);
      }
     #endif
      if (now == lastModal) return;
      lastModal = now;
      juce::Logger::writeToLog("modal: " + now);
    }
    void mouseDown(const juce::MouseEvent& e) override {
      juce::String chain;
      for (auto* c = e.eventComponent; c != nullptr; c = c->getParentComponent())
        chain << (chain.isEmpty() ? "" : " < ") << "'" << c->getName() << "' "
              << typeid(*c).name();
      juce::Logger::writeToLog("touch: at " + e.getScreenPosition().toString() + " on " + chain);
    }
  };
  std::unique_ptr<TouchLog> touchLog_;
  // Our own published port, where the platform allows one. The input holds a
  // pointer to its route, so the route is declared FIRST and therefore
  // destroyed last — the input goes away while its callback is still valid.
  std::unique_ptr<DeviceRoute> virtualRoute_;
  std::unique_ptr<juce::MidiInput> virtualInput_;
  // Linux: a JACK MIDI port, when a JACK server is running (#198).
  std::unique_ptr<JackMidiInput> jackMidi_;
};

START_JUCE_APPLICATION(MasterpieceApp)
