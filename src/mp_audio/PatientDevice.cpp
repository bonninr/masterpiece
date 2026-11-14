// SPDX-License-Identifier: GPL-3.0-only
#include "PatientDevice.h"

namespace mp {

PatientDevice::PatientDevice(std::unique_ptr<juce::AudioIODevice> inner, std::vector<int> waitsMs,
                             int quickFailMs)
    : juce::AudioIODevice(inner->getName(), inner->getTypeName()),
      inner_(std::move(inner)),
      waits_(std::move(waitsMs)),
      quickFailMs_(quickFailMs) {}

PatientDevice::~PatientDevice() = default;

juce::String PatientDevice::open(const juce::BigInteger& inputChannels,
                                 const juce::BigInteger& outputChannels, double sampleRate,
                                 int bufferSizeSamples) {
  // One attempt, and whether it failed quickly enough to try again.
  bool quick = false;
  double tookMs = 0.0;
  const auto attempt = [&] {
    const auto began = juce::Time::getMillisecondCounterHiRes();
    auto error = inner_->open(inputChannels, outputChannels, sampleRate, bufferSizeSamples);
    tookMs = juce::Time::getMillisecondCounterHiRes() - began;
    quick = tookMs < quickFailMs_;
    return error;
  };
  // What was asked for, so a failure in the log can be read on its own.
  const juce::String asked = juce::String(sampleRate, 0) + " Hz, " + juce::String(bufferSizeSamples) +
                             " samples, " + juce::String(outputChannels.countNumberOfSetBits()) +
                             " outputs";
  auto error = attempt();
  lastAttempts_ = 1;
  for (const int wait : waits_) {
    if (error.isEmpty() || !quick) break;
    juce::Logger::writeToLog("audio device: " + getName() + " at " + asked + ": " + error +
                             "; opening again in " + juce::String(wait) + " ms");
    inner_->close();
    juce::Thread::sleep(wait);
    error = attempt();
    ++lastAttempts_;
  }
  if (error.isNotEmpty())
    juce::Logger::writeToLog("audio device: " + getName() + " did not open at " + asked + " after " +
                             juce::String(lastAttempts_) + (lastAttempts_ == 1 ? " attempt" : " attempts") +
                             ", the last one " + juce::String(juce::roundToInt(tookMs)) + " ms: " + error);
  else if (lastAttempts_ > 1)
    juce::Logger::writeToLog("audio device: " + getName() + " opened at " + asked + " at attempt " +
                             juce::String(lastAttempts_));
  return error;
}

PatientDeviceType::PatientDeviceType(std::unique_ptr<juce::AudioIODeviceType> inner,
                                     std::vector<int> waitsMs)
    : juce::AudioIODeviceType(inner->getTypeName()),
      inner_(std::move(inner)),
      waits_(std::move(waitsMs)) {
  inner_->addListener(this);
}

PatientDeviceType::~PatientDeviceType() { inner_->removeListener(this); }

int PatientDeviceType::getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const {
  if (auto* patient = dynamic_cast<PatientDevice*>(device)) device = &patient->inner();
  return inner_->getIndexOfDevice(device, asInput);
}

juce::AudioIODevice* PatientDeviceType::createDevice(const juce::String& outputName,
                                                     const juce::String& inputName) {
  std::unique_ptr<juce::AudioIODevice> device(inner_->createDevice(outputName, inputName));
  if (device == nullptr) return nullptr;
  return new PatientDevice(std::move(device), waits_);
}

}  // namespace mp
