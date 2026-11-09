// PatientDevice (#1007): a driver that fails its first opens, as an ASIO
// driver still locking its clock after a rate change does, opens on a later
// attempt with the same channels, rate and buffer. A driver that opens at once
// is opened once. One that never opens reports its own error after every
// attempt; one whose failure is slow reports it after the first. The device type lists, finds and announces the wrapped type's
// devices.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"
#include "../src/mp_audio/PatientDevice.h"

#include <string>
#include <vector>

namespace {

using mp::test::Category;

struct Opens {
  int failFirst = 0;
  int takesMs = 0;
  int calls = 0;
  int closes = 0;
  std::vector<double> rates;
  std::vector<int> buffers;
  std::vector<int> outputs;
};

class FakeDevice final : public juce::AudioIODevice {
public:
  FakeDevice(const juce::String& name, Opens& opens) : AudioIODevice(name, "Fake"), opens_(opens) {}
  juce::String open(const juce::BigInteger&, const juce::BigInteger& out, double rate, int buffer) override {
    ++opens_.calls;
    opens_.rates.push_back(rate);
    opens_.buffers.push_back(buffer);
    opens_.outputs.push_back(out.countNumberOfSetBits());
    if (opens_.takesMs > 0) juce::Thread::sleep(opens_.takesMs);
    if (opens_.calls <= opens_.failFirst) return "Can't start device";
    open_ = true;
    return {};
  }
  void close() override { ++opens_.closes; open_ = false; }
  bool isOpen() override { return open_; }
  void start(juce::AudioIODeviceCallback*) override {}
  void stop() override {}
  bool isPlaying() override { return false; }
  juce::String getLastError() override { return {}; }
  juce::StringArray getOutputChannelNames() override { return {"1", "2", "3", "4"}; }
  juce::StringArray getInputChannelNames() override { return {}; }
  juce::Array<double> getAvailableSampleRates() override { return {48000.0, 96000.0}; }
  juce::Array<int> getAvailableBufferSizes() override { return {256, 1024}; }
  int getDefaultBufferSize() override { return 256; }
  int getCurrentBufferSizeSamples() override { return 1024; }
  double getCurrentSampleRate() override { return 96000.0; }
  int getCurrentBitDepth() override { return 24; }
  juce::BigInteger getActiveOutputChannels() const override { return {}; }
  juce::BigInteger getActiveInputChannels() const override { return {}; }
  int getOutputLatencyInSamples() override { return 0; }
  int getInputLatencyInSamples() override { return 0; }

private:
  Opens& opens_;
  bool open_ = false;
};

juce::String openAt96k(mp::PatientDevice& device) {
  juce::BigInteger out;
  out.setRange(0, 4, true);
  return device.open({}, out, 96000.0, 1024);
}

class PatientDeviceOpenTest final : public mp::test::Test {
public:
  PatientDeviceOpenTest() : Test("functional.audio.patient-open", Category::Functional) {}
  void run() override {
    const std::vector<int> waits{1, 1, 1};
    {
      Opens opens;
      mp::PatientDevice device(std::make_unique<FakeDevice>("Ready", opens), waits);
      MP_CHECK(openAt96k(device).isEmpty() && device.isOpen(), "a ready driver opens");
      MP_CHECK(opens.calls == 1 && opens.closes == 0 && device.lastAttempts() == 1,
               "and is opened once: " + std::to_string(opens.calls));
    }
    {
      Opens opens;
      opens.failFirst = 2;
      mp::PatientDevice device(std::make_unique<FakeDevice>("Slow clock", opens), waits);
      MP_CHECK(openAt96k(device).isEmpty() && device.isOpen(), "a driver failing twice opens at the third attempt");
      MP_CHECK(opens.calls == 3 && device.lastAttempts() == 3 && opens.closes == 2,
               "closed after each failure: " + std::to_string(opens.closes));
      bool same = true;
      for (size_t i = 0; i < opens.rates.size(); ++i)
        same = same && opens.rates[i] == 96000.0 && opens.buffers[i] == 1024 && opens.outputs[i] == 4;
      MP_CHECK(same, "every attempt asks for the same rate, buffer and channels");
      MP_CHECK(device.getName() == "Slow clock" && device.getTypeName() == "Fake" &&
                   device.getCurrentSampleRate() == 96000.0 && device.getOutputChannelNames().size() == 4,
               "the device reads as the driver's own");
    }
    {
      Opens opens;
      opens.failFirst = 100;
      mp::PatientDevice device(std::make_unique<FakeDevice>("Dead", opens), waits);
      MP_CHECK(openAt96k(device) == "Can't start device" && !device.isOpen(),
               "a driver that never opens reports its error");
      MP_CHECK(opens.calls == 4 && device.lastAttempts() == 4, "after one attempt per wait and the first");
    }
    {
      // A slow failure, as JUCE's 3 s wait for a first callback: one attempt.
      Opens opens;
      opens.failFirst = 100;
      opens.takesMs = 80;
      mp::PatientDevice device(std::make_unique<FakeDevice>("No callbacks", opens), waits, 40);
      MP_CHECK(openAt96k(device) == "Can't start device" && opens.calls == 1 && device.lastAttempts() == 1,
               "a slow failure is reported at once: " + std::to_string(opens.calls));
    }
    MP_CHECK(mp::PatientDevice::kQuickFailMs < 3000, "JUCE's 3 s callback wait counts as slow");
    const auto defaults = mp::PatientDevice::defaultWaits();
    int total = 0;
    for (size_t i = 0; i < defaults.size(); ++i) {
      total += defaults[i];
      MP_CHECK(i == 0 || defaults[i] > defaults[i - 1], "each default wait longer than the last");
    }
    MP_CHECK(!defaults.empty() && defaults.front() <= 20, "the first retry comes within 20 ms");
    MP_CHECK(total >= 1000 && total <= 2000, "the default waits add up to 1 to 2 s: " + std::to_string(total));
  }
};
static PatientDeviceOpenTest g_open;

class FakeType final : public juce::AudioIODeviceType {
public:
  explicit FakeType(Opens& opens) : AudioIODeviceType("Fake"), opens_(opens) {}
  void scanForDevices() override { ++scans; }
  juce::StringArray getDeviceNames(bool input) const override {
    return input ? juce::StringArray() : juce::StringArray{"A", "B"};
  }
  int getDefaultDeviceIndex(bool) const override { return 1; }
  int getIndexOfDevice(juce::AudioIODevice* d, bool) const override {
    return d == nullptr ? -1 : getDeviceNames(false).indexOf(d->getName());
  }
  bool hasSeparateInputsAndOutputs() const override { return false; }
  juce::AudioIODevice* createDevice(const juce::String& out, const juce::String&) override {
    return getDeviceNames(false).contains(out) ? new FakeDevice(out, opens_) : nullptr;
  }
  void announce() { callDeviceChangeListeners(); }
  int scans = 0;

private:
  Opens& opens_;
};

struct Heard final : juce::AudioIODeviceType::Listener {
  int changes = 0;
  void audioDeviceListChanged() override { ++changes; }
};

class PatientDeviceTypeTest final : public mp::test::Test {
public:
  PatientDeviceTypeTest() : Test("functional.audio.patient-type", Category::Functional) {}
  void run() override {
    Opens opens;
    opens.failFirst = 1;
    auto fake = std::make_unique<FakeType>(opens);
    auto* inner = fake.get();
    mp::PatientDeviceType type(std::move(fake), {1});
    type.scanForDevices();
    MP_CHECK(type.getTypeName() == "Fake" && inner->scans == 1, "the scan reaches the wrapped type");
    MP_CHECK(type.getDeviceNames(false) == juce::StringArray({"A", "B"}) && type.getDefaultDeviceIndex(false) == 1,
             "its list and default are the wrapped type's");
    std::unique_ptr<juce::AudioIODevice> device(type.createDevice("B", {}));
    auto* patient = dynamic_cast<mp::PatientDevice*>(device.get());
    MP_CHECK(patient != nullptr, "a created device is patient");
    MP_CHECK(type.getIndexOfDevice(device.get(), false) == 1, "and is found at its place in the list");
    juce::BigInteger out;
    out.setRange(0, 2, true);
    MP_CHECK(device->open({}, out, 48000.0, 256).isEmpty() && opens.calls == 2,
             "and opens at its second attempt");
    MP_CHECK(type.createDevice("Missing", {}) == nullptr, "a device the type lacks is not created");
    Heard heard;
    type.addListener(&heard);
    inner->announce();
    MP_CHECK(heard.changes == 1, "a change in the wrapped type's list is announced");
    type.removeListener(&heard);
  }
};
static PatientDeviceTypeTest g_type;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
