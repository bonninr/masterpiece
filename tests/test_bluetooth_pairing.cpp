// The Bluetooth MIDI pairing window opens once (#565): on macOS each press of
// "Bluetooth MIDI" in Audio and MIDI opened another window. A press while the
// window is open does nothing; once it closes, the next press opens it again.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/Ui.h"

using mp::test::Category;

namespace {

class BluetoothPairingOnceTest final : public mp::test::Test {
public:
  BluetoothPairingOnceTest() : Test("functional.ui.bluetooth-pairing-once", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();

    // A stand-in for the system window: it keeps the callback and calls it
    // when told to close, as the real one does.
    int opened = 0;
    std::unique_ptr<juce::ModalComponentManager::Callback> held;
    auto open = [&](juce::ModalComponentManager::Callback* closed) {
      ++opened;
      held.reset(closed);
      return true;
    };
    auto close = [&] {
      if (held) held->modalStateFinished(0);
      held.reset();
    };

    MP_CHECK(mp::ui::openPairingOnce(open) && opened == 1, "the first press opens the window");
    MP_CHECK(!mp::ui::openPairingOnce(open) && opened == 1,
             "a second press while it is open opens no second window");
    MP_CHECK(!mp::ui::openPairingOnce(open) && opened == 1, "nor does a third");
    close();
    MP_CHECK(mp::ui::openPairingOnce(open) && opened == 2, "once closed, the next press opens it");
    close();

    // A window that cannot open leaves the button working.
    auto fails = [&](juce::ModalComponentManager::Callback* closed) {
      ++opened;
      delete closed;
      return false;
    };
    MP_CHECK(!mp::ui::openPairingOnce(fails) && opened == 3, "a failed open is tried");
    MP_CHECK(mp::ui::openPairingOnce(open) && opened == 4, "and the next press still opens");
    close();

    // The selector in Audio and MIDI: its button goes through the guard
    // wherever the platform has Bluetooth MIDI (macOS, iOS, Android).
    juce::AudioDeviceManager devices;
    juce::AudioDeviceSelectorComponent selector(devices, 0, 0, 1, 8, true, true, true, false);
    const bool available = juce::BluetoothMidiDevicePairingDialogue::isAvailable();
    MP_CHECK(mp::ui::guardPairingButton(selector) == available,
             available ? "the selector's Bluetooth MIDI button is found and guarded"
                       : "no Bluetooth MIDI button on this platform, nothing to guard");
  }
};
static BluetoothPairingOnceTest g_bluetoothPairingOnce;

}  // namespace

#endif  // MP_TEST_HAS_AUDIO
