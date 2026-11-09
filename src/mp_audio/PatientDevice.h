// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>
#include <vector>

namespace mp {

// An audio device that gives its driver time to settle (#1007).
//
// Some ASIO drivers take a second or more to lock their clock after a sample
// rate change. JUCE starts the device 10 ms after it sets the rate, so the first
// open fails with "Can't start device"; a second open, at the rate the driver
// now holds, starts. PatientDevice closes the device, waits, and opens it again
// with the same channels, rate and buffer, each wait longer than the last. It
// reports the error only when every attempt fails. A device that opens at once
// costs nothing: the first attempt is the only one.
//
// Only a quick failure gets another attempt. A slow one has had its patience
// already: JUCE waits 3 s for the first callback before "Device didn't start
// correctly", and a dead driver would otherwise hold the program for about 14 s.
class PatientDevice final : public juce::AudioIODevice {
public:
  // The waits before each further attempt, in milliseconds: 20 ms settles a
  // professional card, and the longer ones cover a driver still locking.
  static std::vector<int> defaultWaits() { return {20, 80, 320, 1280}; }
  // A failed open that took longer than this ends the attempts.
  static constexpr int kQuickFailMs = 1000;

  PatientDevice(std::unique_ptr<juce::AudioIODevice> inner, std::vector<int> waitsMs,
                int quickFailMs = kQuickFailMs);
  ~PatientDevice() override;

  juce::AudioIODevice& inner() noexcept { return *inner_; }
  // Attempts the last open() made, 1 when the first one succeeded.
  int lastAttempts() const noexcept { return lastAttempts_; }

  juce::String open(const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                    double sampleRate, int bufferSizeSamples) override;
  void close() override { inner_->close(); }
  bool isOpen() override { return inner_->isOpen(); }
  void start(juce::AudioIODeviceCallback* callback) override { inner_->start(callback); }
  void stop() override { inner_->stop(); }
  bool isPlaying() override { return inner_->isPlaying(); }
  juce::String getLastError() override { return inner_->getLastError(); }

  juce::StringArray getOutputChannelNames() override { return inner_->getOutputChannelNames(); }
  juce::StringArray getInputChannelNames() override { return inner_->getInputChannelNames(); }
  std::optional<juce::BigInteger> getDefaultOutputChannels() const override {
    return inner_->getDefaultOutputChannels();
  }
  std::optional<juce::BigInteger> getDefaultInputChannels() const override {
    return inner_->getDefaultInputChannels();
  }
  juce::Array<double> getAvailableSampleRates() override { return inner_->getAvailableSampleRates(); }
  juce::Array<int> getAvailableBufferSizes() override { return inner_->getAvailableBufferSizes(); }
  int getDefaultBufferSize() override { return inner_->getDefaultBufferSize(); }
  int getCurrentBufferSizeSamples() override { return inner_->getCurrentBufferSizeSamples(); }
  double getCurrentSampleRate() override { return inner_->getCurrentSampleRate(); }
  int getCurrentBitDepth() override { return inner_->getCurrentBitDepth(); }
  juce::BigInteger getActiveOutputChannels() const override { return inner_->getActiveOutputChannels(); }
  juce::BigInteger getActiveInputChannels() const override { return inner_->getActiveInputChannels(); }
  int getOutputLatencyInSamples() override { return inner_->getOutputLatencyInSamples(); }
  int getInputLatencyInSamples() override { return inner_->getInputLatencyInSamples(); }
  juce::AudioWorkgroup getWorkgroup() const override { return inner_->getWorkgroup(); }
  bool hasControlPanel() const override { return inner_->hasControlPanel(); }
  bool showControlPanel() override { return inner_->showControlPanel(); }
  bool setAudioPreprocessingEnabled(bool enabled) override {
    return inner_->setAudioPreprocessingEnabled(enabled);
  }
  int getXRunCount() const noexcept override { return inner_->getXRunCount(); }

private:
  std::unique_ptr<juce::AudioIODevice> inner_;
  std::vector<int> waits_;
  int quickFailMs_;
  int lastAttempts_ = 0;
};

// A device type whose devices are PatientDevices: the list, the defaults and
// the change notices are the wrapped type's own.
class PatientDeviceType final : public juce::AudioIODeviceType,
                                private juce::AudioIODeviceType::Listener {
public:
  PatientDeviceType(std::unique_ptr<juce::AudioIODeviceType> inner, std::vector<int> waitsMs);
  ~PatientDeviceType() override;

  void scanForDevices() override { inner_->scanForDevices(); }
  juce::StringArray getDeviceNames(bool wantInputNames) const override {
    return inner_->getDeviceNames(wantInputNames);
  }
  int getDefaultDeviceIndex(bool forInput) const override { return inner_->getDefaultDeviceIndex(forInput); }
  int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override;
  bool hasSeparateInputsAndOutputs() const override { return inner_->hasSeparateInputsAndOutputs(); }
  juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String& inputName) override;

private:
  void audioDeviceListChanged() override { callDeviceChangeListeners(); }

  std::unique_ptr<juce::AudioIODeviceType> inner_;
  std::vector<int> waits_;
};

}  // namespace mp
