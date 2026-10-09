#include "JackMidi.h"

#include "../../src/mp_audio/MasterpieceProcessor.h"

#if JUCE_LINUX
 #include <dlfcn.h>
 #include <jack/jack.h>
 #include <jack/midiport.h>

namespace {
// Where the port's connections are kept between runs (#249): one source port
// name a line, as JACK names it ("MuseScore:mscore-midi-1").
juce::File connectionsFile() {
  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
      .getChildFile("Masterpiece")
      .getChildFile("jack-connections.txt");
}
}  // namespace

struct JackMidiInput::Impl : private juce::Timer {
  // The few libjack functions this needs, looked up at run time.
  using ClientOpen = jack_client_t* (*)(const char*, jack_options_t, jack_status_t*, ...);
  using ClientClose = int (*)(jack_client_t*);
  using PortRegister = jack_port_t* (*)(jack_client_t*, const char*, const char*, unsigned long, unsigned long);
  using SetProcess = int (*)(jack_client_t*, JackProcessCallback, void*);
  using Activate = int (*)(jack_client_t*);
  using Deactivate = int (*)(jack_client_t*);
  using PortBuffer = void* (*)(jack_port_t*, jack_nframes_t);
  using EventCount = uint32_t (*)(void*);
  using EventGet = int (*)(jack_midi_event_t*, void*, uint32_t);
  using PortName = const char* (*)(const jack_port_t*);
  using PortConnections = const char** (*)(const jack_port_t*);
  using Free = void (*)(void*);
  using Connect = int (*)(jack_client_t*, const char*, const char*);
  using PortByName = jack_port_t* (*)(jack_client_t*, const char*);
  using ConnectedTo = int (*)(const jack_port_t*, const char*);

  void* lib = nullptr;
  ClientClose clientClose = nullptr;
  Deactivate deactivate = nullptr;
  PortBuffer portBuffer = nullptr;
  EventCount eventCount = nullptr;
  EventGet eventGet = nullptr;
  PortConnections portConnections = nullptr;
  Free jackFree = nullptr;
  Connect connect = nullptr;
  PortByName portByName = nullptr;
  ConnectedTo connectedTo = nullptr;
  jack_client_t* client = nullptr;
  jack_port_t* port = nullptr;
  std::string portFullName;
  mp::MasterpieceProcessor* proc = nullptr;
  int deviceId = 0;
  // Sources saved from the last run, still to be connected in this one. A
  // program started later is connected when its port appears; once connected,
  // a source is left to the player, who may take the link away.
  juce::StringArray pending;

  void restoreConnections() {
    pending = juce::StringArray::fromLines(connectionsFile().loadFileAsString());
    pending.trim();
    pending.removeEmptyStrings();
    if (pending.isEmpty() || connect == nullptr || portByName == nullptr) return;
    timerCallback();
    if (!pending.isEmpty()) startTimer(2000);
  }
  void timerCallback() override {
    for (int i = pending.size(); --i >= 0;) {
      const auto source = pending[i].toStdString();
      if (portByName(client, source.c_str()) == nullptr) continue;  // not running yet
      if (connectedTo == nullptr || connectedTo(port, source.c_str()) == 0) {
        if (connect(client, source.c_str(), portFullName.c_str()) != 0) continue;
        juce::Logger::writeToLog("JACK MIDI input: connected from " + pending[i] + " as last time");
      }
      pending.remove(i);
    }
    if (pending.isEmpty()) stopTimer();
  }
  void saveConnections() {
    if (portConnections == nullptr) return;
    juce::StringArray sources;
    if (const char** list = portConnections(port)) {
      for (const char** c = list; *c != nullptr; ++c) sources.add(*c);
      if (jackFree != nullptr) jackFree(list);
    }
    // Sources not running now were still wanted: kept for the next run.
    for (const auto& p : pending) sources.addIfNotAlreadyThere(p);
    const auto f = connectionsFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(sources.joinIntoString("\n") + (sources.isEmpty() ? "" : "\n"));
  }

  // JACK's process thread: hand every short message to the processor's queue,
  // which copies it into a fixed slot. Nothing here allocates or locks.
  static int process(jack_nframes_t frames, void* arg) {
    auto* self = static_cast<Impl*>(arg);
    void* buffer = self->portBuffer(self->port, frames);
    if (buffer == nullptr) return 0;
    const uint32_t n = self->eventCount(buffer);
    for (uint32_t i = 0; i < n; ++i) {
      jack_midi_event_t ev;
      if (self->eventGet(&ev, buffer, i) != 0 || ev.size == 0 || ev.size > 3) continue;
      self->proc->pushMidi(self->deviceId, juce::MidiMessage(ev.buffer, static_cast<int>(ev.size)));
    }
    return 0;
  }

  ~Impl() {
    stopTimer();
    if (client != nullptr) {
      saveConnections();
      deactivate(client);
      clientClose(client);
    }
    if (lib != nullptr) dlclose(lib);
  }
};

std::unique_ptr<JackMidiInput> JackMidiInput::open(mp::MasterpieceProcessor& proc) {
  auto impl = std::make_unique<Impl>();
  impl->lib = dlopen("libjack.so.0", RTLD_NOW);
  if (impl->lib == nullptr) return nullptr;
  auto sym = [&](const char* name) { return dlsym(impl->lib, name); };
  const auto clientOpen = reinterpret_cast<Impl::ClientOpen>(sym("jack_client_open"));
  const auto portRegister = reinterpret_cast<Impl::PortRegister>(sym("jack_port_register"));
  const auto setProcess = reinterpret_cast<Impl::SetProcess>(sym("jack_set_process_callback"));
  const auto activate = reinterpret_cast<Impl::Activate>(sym("jack_activate"));
  const auto portName = reinterpret_cast<Impl::PortName>(sym("jack_port_name"));
  impl->clientClose = reinterpret_cast<Impl::ClientClose>(sym("jack_client_close"));
  impl->deactivate = reinterpret_cast<Impl::Deactivate>(sym("jack_deactivate"));
  impl->portBuffer = reinterpret_cast<Impl::PortBuffer>(sym("jack_port_get_buffer"));
  impl->eventCount = reinterpret_cast<Impl::EventCount>(sym("jack_midi_get_event_count"));
  impl->eventGet = reinterpret_cast<Impl::EventGet>(sym("jack_midi_event_get"));
  impl->portConnections = reinterpret_cast<Impl::PortConnections>(sym("jack_port_get_connections"));
  impl->jackFree = reinterpret_cast<Impl::Free>(sym("jack_free"));
  impl->connect = reinterpret_cast<Impl::Connect>(sym("jack_connect"));
  impl->portByName = reinterpret_cast<Impl::PortByName>(sym("jack_port_by_name"));
  impl->connectedTo = reinterpret_cast<Impl::ConnectedTo>(sym("jack_port_connected_to"));
  if (clientOpen == nullptr || portRegister == nullptr || setProcess == nullptr || activate == nullptr ||
      impl->clientClose == nullptr || impl->deactivate == nullptr || impl->portBuffer == nullptr ||
      impl->eventCount == nullptr || impl->eventGet == nullptr)
    return nullptr;

  jack_status_t status{};
  // JackNoStartServer: a player who chose ALSA gets no JACK server started
  // behind their back just because Masterpiece opened.
  impl->client = clientOpen("Masterpiece MIDI", JackNoStartServer, &status);
  if (impl->client == nullptr) return nullptr;
  impl->port = portRegister(impl->client, "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
  if (impl->port == nullptr) return nullptr;
  impl->proc = &proc;
  impl->deviceId = proc.registerOwnMidiInput("JACK MIDI");
  if (setProcess(impl->client, &Impl::process, impl.get()) != 0 || activate(impl->client) != 0)
    return nullptr;
  impl->portFullName = portName != nullptr ? portName(impl->port) : "Masterpiece MIDI:midi_in";
  juce::Logger::writeToLog("JACK MIDI input: " + juce::String(impl->portFullName));
  // The links the player made last time, back as they were (#249).
  impl->restoreConnections();
  std::unique_ptr<JackMidiInput> out(new JackMidiInput());
  out->impl_ = std::move(impl);
  return out;
}

JackMidiInput::~JackMidiInput() = default;

#else

struct JackMidiInput::Impl {};
std::unique_ptr<JackMidiInput> JackMidiInput::open(mp::MasterpieceProcessor&) { return nullptr; }
JackMidiInput::~JackMidiInput() = default;

#endif
