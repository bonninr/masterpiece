#include "MidiPorts.h"

#include <cstdlib>

#if JUCE_LINUX
 #include <alsa/asoundlib.h>
#endif

namespace mp {

namespace {

#if JUCE_LINUX
// "usb-0000:00:1d.0-1.2.3/0" for port 0 of the USB device in socket 1.2.3,
// from the ALSA sequencer port JUCE names "client-port". Two keyboards of
// the same model, which have the same name and often the same serial
// number, are told apart by the socket each is plugged into, and keep it
// when they are plugged in again in another order. The socket is read from
// the card's long name, which the USB audio driver writes as
// "<product> at usb-<socket>, <speed>". A device that is not on USB, or a
// port that is not a card's, has none and is known by its name.
juce::String alsaUsbSocket(const juce::String& juceId) {
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

// "1.2.3" from "usb-0000:00:1d.0-1.2.3/0": the socket as the USB tree numbers
// it, which is what a person can follow to the plug. Empty for any other
// identifier.
juce::String usbSocketOf(const juce::String& lasting) {
  if (!lasting.startsWith("usb-")) return {};
  const auto path = lasting.substring(4).upToFirstOccurrenceOf("/", false, false);
  const int dash = path.lastIndexOfChar('-');
  return dash >= 0 ? path.substring(dash + 1) : juce::String();
}

} // namespace

juce::String lastingMidiIdentifier(const juce::MidiDeviceInfo& port) {
 #if JUCE_WINDOWS || JUCE_MAC || JUCE_IOS
  return port.identifier;
 #elif JUCE_LINUX
  return alsaUsbSocket(port.identifier);
 #else
  juce::ignoreUnused(port);
  return {};
 #endif
}

juce::StringArray midiPortLabels(const juce::Array<juce::MidiDeviceInfo>& ports,
                                 const std::function<juce::String(const juce::MidiDeviceInfo&)>& lasting) {
  juce::StringArray labels;
  for (int i = 0; i < ports.size(); ++i) {
    const auto& name = ports[i].name;
    int twins = 0, position = 0;
    for (int j = 0; j < ports.size(); ++j)
      if (ports[j].name == name) {
        ++twins;
        if (j == i) position = twins;
      }
    if (twins < 2) {
      labels.add(name);
      continue;
    }
    // The socket labels the port only where no twin is in the same socket.
    const auto socket = usbSocketOf(lasting(ports[i]));
    bool alone = socket.isNotEmpty();
    for (int j = 0; j < ports.size() && alone; ++j)
      if (j != i && ports[j].name == name && usbSocketOf(lasting(ports[j])) == socket) alone = false;
    labels.add(alone ? name + " (USB " + socket + ")" : name + " #" + juce::String(position));
  }
  // A device whose own name reads like another's label keeps its place by
  // number: two rows never read alike.
  for (int i = 1; i < labels.size(); ++i) {
    const auto base = labels[i];
    for (int n = 2; labels.indexOf(labels[i]) < i; ++n) labels.set(i, base + " #" + juce::String(n));
  }
  return labels;
}

juce::Array<int> midiPortsForLabels(const juce::StringArray& wanted,
                                    const juce::Array<juce::MidiDeviceInfo>& ports,
                                    const juce::StringArray& labels) {
  juce::Array<int> chosen;
  juce::Array<int> taken;
  // Labels first, so a bare name never takes the port a label names.
  for (const auto& w : wanted) {
    int i = labels.indexOf(w);
    if (taken.contains(i)) i = -1;
    if (i >= 0) taken.add(i);
    chosen.add(i);
  }
  for (int k = 0; k < wanted.size(); ++k) {
    if (chosen[k] >= 0) continue;
    for (int i = 0; i < ports.size(); ++i)
      if (ports[i].name == wanted[k] && !taken.contains(i)) {
        chosen.set(k, i);
        taken.add(i);
        break;
      }
  }
  return chosen;
}

} // namespace mp
