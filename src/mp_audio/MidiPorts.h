// What the program calls each MIDI port: a name a person can read and choose
// by, and an identifier that names the same device in the next run.
//
// Linux names two devices of one model alike, so a list of names showed two
// identical rows, and a choice saved by name could reach only one of them
// (#980). Each port in a list gets a label of its own here.
#pragma once
#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>

namespace mp {

// A MIDI port's identifier where it names the same device in the next run:
// Windows' interface path (which includes the USB socket) and macOS's
// CoreMIDI id. Linux numbers its ports in the order the devices appear, so
// there the identifier is the USB socket of the card behind the port, with
// the port's number on that card: "usb-0000:00:1d.0-1.2.3/0". Android numbers
// its devices afresh, and gives none.
juce::String lastingMidiIdentifier(const juce::MidiDeviceInfo& port);

// The label of each port in `ports`, in order. A port whose name no other
// port in the list has is labelled with its name. Ports that share a name are
// labelled with the USB socket each is plugged into, "Launchpad Mini (USB
// 1.2)", or numbered "Launchpad Mini #2" in the list's order where the system
// gives no socket. No two labels in the list are the same.
juce::StringArray midiPortLabels(
    const juce::Array<juce::MidiDeviceInfo>& ports,
    const std::function<juce::String(const juce::MidiDeviceInfo&)>& lasting = lastingMidiIdentifier);

// The port among `ports` that each label in `wanted` names, in order, or -1
// where none does. `labels` are the ports' labels from midiPortLabels. A
// label saved before ports were labelled is a bare name: it names the first
// port of that name that no other wanted label names. No port is named twice.
juce::Array<int> midiPortsForLabels(const juce::StringArray& wanted,
                                    const juce::Array<juce::MidiDeviceInfo>& ports,
                                    const juce::StringArray& labels);

} // namespace mp
