// The recorder's buttons are sized from their own text (#429). The labels
// are longer in some languages than in others, and the fixed widths the
// buttons had squeezed the French ones ("Enregistrer l'audio...",
// "Charger un MIDI...") into a strip of unreadable letters.
//
// The test builds the recorder's window in every language the program
// ships, measures each label with the font the buttons are drawn with, and
// checks that the button is wide enough for its text and that every button
// stays inside the window.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_audio/Translations.h"
#include "../src/mp_ui/Settings.h"

#include <cstdio>
#include <string>

using mp::test::Category;

namespace {

// Every language the program speaks, English first.
const char* const kLanguages[] = {"en", "cs", "de", "es", "fr", "it", "nl", "pl"};

std::string plain(const juce::String& s) { return s.toStdString(); }

// The language is a global one: whatever the test does, the program goes
// back to English for the tests that follow it.
struct EnglishAgain {
  ~EnglishAgain() { mp::applyLanguage("en"); }
};

}  // namespace

class RecorderLayoutTest final : public mp::test::Test {
public:
  RecorderLayoutTest() : Test("functional.ui.recorder-layout", Category::Functional) {}

  void run() override {
    const EnglishAgain backToEnglish;
    // The panel's timer wants a message manager, and the test runs no message
    // loop: the timer never fires, which is what the test wants.
    juce::MessageManager::getInstance();
    mp::MasterpieceProcessor proc;
    int english = 0, french = 0, widestText = 0;

    for (const char* code : kLanguages) {
      mp::applyLanguage(code);
      mp::ui::RecorderPanel panel(proc);
      // The width the panel asks for: the window around it is at least this
      // wide, so a button that does not fit here does not fit there either.
      const int needed = panel.preferredWidth();
      panel.setSize(needed, 420);

      int buttons = 0;
      for (auto* child : panel.getChildren()) {
        auto* button = dynamic_cast<juce::TextButton*>(child);
        if (button == nullptr) continue;
        ++buttons;
        const auto font = button->getLookAndFeel().getTextButtonFont(*button, button->getHeight());
        const int letters =
            juce::GlyphArrangement::getStringWidthInt(font, button->getButtonText());
        widestText = juce::jmax(widestText, letters);
        MP_CHECK(button->getWidth() >= letters,
                 std::string(code) + ": \"" + plain(button->getButtonText()) + "\" needs " +
                     std::to_string(letters) + " px and the button has " +
                     std::to_string(button->getWidth()));
        MP_CHECK(button->getRight() <= panel.getWidth(),
                 std::string(code) + ": \"" + plain(button->getButtonText()) +
                     "\" falls outside the window");
        std::printf("        %s  %-24s %4d px of text, %4d px of button\n", code,
                    plain(button->getButtonText()).c_str(), letters, button->getWidth());
      }
      MP_CHECK(buttons == 10, std::string(code) + ": the recorder has ten buttons");
      if (std::string(code) == "en") english = needed;
      if (std::string(code) == "fr") french = needed;

      // A panel narrower than the row needs, as on a phone in portrait: the
      // buttons wrap to a second line and every one of them stays on the
      // screen.
      if (std::string(code) == "fr") {
        mp::ui::RecorderPanel narrow(proc);
        narrow.setSize(300, 420);
        for (auto* child : narrow.getChildren()) {
          auto* button = dynamic_cast<juce::TextButton*>(child);
          if (button == nullptr) continue;
          MP_CHECK(button->getRight() <= narrow.getWidth(),
                   "fr: \"" + plain(button->getButtonText()) + "\" reaches past the panel");
          MP_CHECK(button->getBottom() <= narrow.getHeight(),
                   "fr: \"" + plain(button->getButtonText()) + "\" reaches below the panel");
        }
      }
    }

    // The widths come from the text, so the language with the longer words
    // needs the wider window. A machine with no font to measure with has
    // nothing to compare.
    if (widestText > 0)
      MP_CHECK(french > english,
               "the French window (" + std::to_string(french) + " px) is wider than the English one (" +
                   std::to_string(english) + " px)");
  }
};
static RecorderLayoutTest g_recorderLayout;

#endif  // MP_TEST_HAS_AUDIO
