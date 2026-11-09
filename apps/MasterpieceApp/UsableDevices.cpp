// SPDX-License-Identifier: GPL-3.0-only
#include "UsableDevices.h"

#if JUCE_LINUX
 #include <alsa/asoundlib.h>
 #include <cstdlib>
 #include <map>
 #include <set>

namespace {

// ALSA's own name for each hinted PCM device, under the name JUCE shows for
// it, with whether it opens for this direction. We open the way JUCE tests
// its own extra entries: non-blocking, closed at once.
std::map<juce::String, bool> probe(bool input) {
  std::map<juce::String, bool> result;
  void** hints = nullptr;
  if (snd_device_name_hint(-1, "pcm", &hints) != 0) return result;
  const auto field = [](void* hint, const char* key) {
    char* raw = snd_device_name_get_hint(hint, key);
    const juce::String s = raw != nullptr ? juce::String::fromUTF8(raw) : juce::String();
    free(raw);
    return s;
  };
  for (void** h = hints; *h != nullptr; ++h) {
    const auto id = field(*h, "NAME");
    if (id.isEmpty()) continue;
    auto name = field(*h, "DESC").replace("\n", "; ");
    if (name.isEmpty()) name = id;
    snd_pcm_t* pcm = nullptr;
    const bool opens = snd_pcm_open(&pcm, id.toRawUTF8(),
                                    input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK,
                                    SND_PCM_NONBLOCK) == 0;
    if (pcm != nullptr) snd_pcm_close(pcm);
    // Two devices can share a description; one that opens keeps the name.
    result[name] = result[name] || opens;
  }
  snd_device_name_free_hint(hints);
  return result;
}

// JUCE numbers repeated names "Name (2)": the hint's description is the part
// before that.
juce::String hintName(const juce::String& shown) {
  if (shown.endsWithChar(')')) {
    const int open = shown.lastIndexOf(" (");
    if (open > 0 && shown.substring(open + 2, shown.length() - 1).containsOnly("0123456789"))
      return shown.substring(0, open);
  }
  return shown;
}

class UsableAlsaType final : public juce::AudioIODeviceType,
                             private juce::AudioIODeviceType::Listener {
public:
  explicit UsableAlsaType(juce::AudioIODeviceType* inner)
      : juce::AudioIODeviceType(inner->getTypeName()), inner_(inner) {
    inner_->addListener(this);
  }
  ~UsableAlsaType() override { inner_->removeListener(this); }

  void scanForDevices() override {
    inner_->scanForDevices();
    outputs_ = usable(false);
    inputs_ = usable(true);
  }
  juce::StringArray getDeviceNames(bool wantInputNames) const override {
    return wantInputNames ? inputs_ : outputs_;
  }
  int getDefaultDeviceIndex(bool forInput) const override {
    const auto all = inner_->getDeviceNames(forInput);
    const auto name = all[inner_->getDefaultDeviceIndex(forInput)];
    return juce::jmax(0, (forInput ? inputs_ : outputs_).indexOf(name));
  }
  int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
    const auto all = inner_->getDeviceNames(asInput);
    const int at = inner_->getIndexOfDevice(device, asInput);
    return at < 0 ? -1 : (asInput ? inputs_ : outputs_).indexOf(all[at]);
  }
  bool hasSeparateInputsAndOutputs() const override { return true; }
  juce::AudioIODevice* createDevice(const juce::String& output, const juce::String& input) override {
    // The device in use stays listed: open, a card tests busy.
    if (output.isNotEmpty()) kept_.insert(output);
    if (input.isNotEmpty()) kept_.insert(input);
    return inner_->createDevice(output, input);
  }

private:
  void audioDeviceListChanged() override {
    scanForDevices();
    callDeviceChangeListeners();
  }

  juce::StringArray usable(bool input) const {
    const auto all = inner_->getDeviceNames(input);
    const auto opens = probe(input);
    juce::StringArray kept;
    for (const auto& name : all) {
      const auto found = opens.find(hintName(name));
      // Not a hint (JUCE's own "default" and "pulse" entries, already tested
      // by JUCE), opening, or in use.
      if (found == opens.end() || found->second || kept_.count(name) != 0) kept.add(name);
    }
    // Every probe failing says more about the probe than the devices.
    return kept.isEmpty() ? all : kept;
  }

  std::unique_ptr<juce::AudioIODeviceType> inner_;
  juce::StringArray outputs_, inputs_;
  std::set<juce::String> kept_;
};

}  // namespace

void UsableDeviceManager::createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) {
  juce::AudioDeviceManager::createAudioDeviceTypes(types);
  for (int i = 0; i < types.size(); ++i)
    if (types[i]->getTypeName() == "ALSA") {
      auto* inner = types.removeAndReturn(i);
      types.insert(i, new UsableAlsaType(inner));
      break;
    }
}

#elif JUCE_WINDOWS

 #include "../../src/mp_audio/PatientDevice.h"

// ASIO devices open with patience: a driver still locking its clock after a
// rate change gets more attempts (#1007).
void UsableDeviceManager::createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) {
  juce::AudioDeviceManager::createAudioDeviceTypes(types);
  for (int i = 0; i < types.size(); ++i)
    if (types[i]->getTypeName() == "ASIO") {
      std::unique_ptr<juce::AudioIODeviceType> inner(types.removeAndReturn(i));
      types.insert(i, new mp::PatientDeviceType(std::move(inner), mp::PatientDevice::defaultWaits()));
      break;
    }
}

#else

void UsableDeviceManager::createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) {
  juce::AudioDeviceManager::createAudioDeviceTypes(types);
}

#endif
