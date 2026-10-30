// The first-run wizard closes when the player chooses an organ from it (#591).
// It stayed open over the console, still saying "No organ loaded yet", while
// the organ loaded behind it.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Wizard.h"

using mp::test::Category;

namespace {
juce::TextButton* findButton(juce::Component& root, const juce::String& text) {
  for (auto* c : root.getChildren())
    if (auto* b = dynamic_cast<juce::TextButton*>(c); b != nullptr && b->getButtonText() == text)
      return b;
  return nullptr;
}

// Closes the way the dialog the application launches does.
struct HidingDialog final : juce::DialogWindow {
  HidingDialog() : DialogWindow("Welcome", juce::Colours::black, true, false) {}
  void closeButtonPressed() override { setVisible(false); }
};
}  // namespace

class WizardOrganTest final : public mp::test::Test {
public:
  WizardOrganTest() : Test("functional.ui.wizard-closes-on-organ", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    const juce::File global = proc.globalSettingsFile();
    const bool had = global.existsAsFile();
    const juce::String before = had ? global.loadFileAsString() : juce::String();

    juce::AudioDeviceManager devices;
    mp::ui::WizardPanel panel(proc, devices);
    panel.setSize(560, 520);
    HidingDialog window;
    window.setContentNonOwned(&panel, true);
    window.setVisible(true);

    int opened = 0, finished = 0;
    panel.onOpenOrgan = [&] {
      ++opened;
      MP_CHECK(!window.isVisible(), "the wizard is closed before the organ chooser opens");
    };
    panel.onFinished = [&] { ++finished; };

    auto* next = findButton(panel, TRANS("Next"));
    MP_CHECK(next != nullptr, "the wizard has a Next button");
    if (next == nullptr) return;
    // Simple or Advanced, sound, keyboards, then the organ.
    next->onClick();
    next->onClick();
    next->onClick();
    auto* choose = findButton(panel, TRANS("Choose an organ..."));
    MP_CHECK(choose != nullptr && choose->isVisible(), "the last step offers an organ");
    if (choose == nullptr) return;
    choose->onClick();

    MP_CHECK(opened == 1, "choosing an organ asks the application to open one");
    MP_CHECK(finished == 1, "choosing an organ finishes the wizard");
    MP_CHECK(!window.isVisible(), "the wizard is closed once an organ is chosen");

    window.clearContentComponent();
    if (had) global.replaceWithText(before);
    else global.deleteFile();
  }
};
static WizardOrganTest g_wizardOrgan;

#endif  // MP_TEST_HAS_AUDIO
