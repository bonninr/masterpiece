#include "JackMidi.h"

#include "../../src/mp_audio/MasterpieceProcessor.h"

#if JUCE_LINUX
 #include <dlfcn.h>
 #include <jack/jack.h>
 #include <jack/midiport.h>

struct JackMidiInput::Impl {
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

  void* lib = nullptr;
  ClientClose clientClose = nullptr;
  Deactivate deactivate = nullptr;
  PortBuffer portBuffer = nullptr;
  EventCount eventCount = nullptr;
  EventGet eventGet = nullptr;
  jack_client_t* client = nullptr;
  jack_port_t* port = nullptr;
  mp::MasterpieceProcessor* proc = nullptr;
  int deviceId = 0;

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
    if (client != nullptr) {
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
  juce::Logger::writeToLog("JACK MIDI input: " +
                           juce::String(portName != nullptr ? portName(impl->port) : "midi_in"));
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
