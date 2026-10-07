#pragma once
// A JACK MIDI input port, for programs that send MIDI only through JACK
// (#198: MuseScore 3 lists its JACK ports, and JUCE's JACK backend carries
// audio only, taking MIDI from ALSA). Linux only.
//
// A client of its own, "Masterpiece MIDI", with one port, midi_in, opened
// only when a JACK server is already running: it never starts one. libjack
// is loaded at run time, as JUCE's own JACK backend loads it, so a machine
// without JACK runs without it and the package needs no JACK library.
//
// What arrives is handed to the processor's device queue under its own
// device, "JACK MIDI", from JACK's process thread: the same allocation-free
// path every other MIDI input takes.

#include <memory>

namespace mp {
class MasterpieceProcessor;
}

class JackMidiInput {
public:
  // Nothing when there is no JACK library or no running server.
  static std::unique_ptr<JackMidiInput> open(mp::MasterpieceProcessor& proc);
  ~JackMidiInput();

  JackMidiInput(const JackMidiInput&) = delete;
  JackMidiInput& operator=(const JackMidiInput&) = delete;

private:
  JackMidiInput() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
