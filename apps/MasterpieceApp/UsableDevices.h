// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

// The audio device manager the standalone app uses. On Linux its ALSA list
// holds only the devices that open (#851): ALSA advertises every PCM plugin
// and surround layout of every card, and on a desktop most of them fail.
// Elsewhere it is JUCE's own manager.
class UsableDeviceManager final : public juce::AudioDeviceManager {
public:
  void createAudioDeviceTypes(juce::OwnedArray<juce::AudioIODeviceType>& types) override;
};
