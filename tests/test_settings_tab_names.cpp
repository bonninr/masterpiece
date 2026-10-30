// The Settings tabs keep their names whole in a narrow window ("Performanc",
// "Metronom" and "Languag" before): every word of a name fits its tab, a name
// of two words on two lines. The narrow width is a share of what the names
// ask for: Linux's font is narrower, and 650 pixels broke nothing there.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_ui/Settings.h"

#include <string>

using mp::test::Category;

class SettingsTabNamesTest final : public mp::test::Test {
public:
  SettingsTabNamesTest() : Test("functional.ui.settings-tab-names", Category::Functional) {}

  // The names laid out as the tabs draw them (LookAndFeel_V3::drawTabButton);
  // the ones with a word broken, a line too wide or more than two lines are
  // listed, with the lines they came out as.
  static std::string cutNames(juce::LookAndFeel* look, int width) {
    juce::TabbedButtonBar bar(juce::TabbedButtonBar::TabsAtTop);
    if (look != nullptr) bar.setLookAndFeel(look);
    for (auto* name : {"General", "Audio", "MIDI", "Performance", "Reverb", "Metronome", "Combination sets",
                       "Console display", "Language", "Remote control", "Diagnostics"})
      bar.addTab(name, juce::Colours::darkgrey, -1);
    bar.setBounds(0, 0, width, 30);
    bar.resized();
    std::string cut;
    for (int i = 0; i < bar.getNumTabs(); ++i) {
      auto* tab = bar.getTabButton(i);
      const auto text = tab->getButtonText().trim();
      if (!tab->isVisible()) {
        cut += " " + text.toStdString() + "(hidden)";
        continue;
      }
      const auto area = tab->getTextArea().toFloat();
      juce::TextLayout layout;
      juce::LookAndFeel_V3::createTabTextLayout(*tab, area.getWidth(), area.getHeight(), juce::Colours::white, layout);
      bool whole = layout.getNumLines() <= 2;
      juce::String lines;
      for (int l = 0; l < layout.getNumLines(); ++l) {
        const auto& line = layout.getLine(l);
        const auto range = line.stringRange;
        lines << "[" << text.substring(range.getStart(), range.getEnd()) << "]";
        const bool startsWord = range.getStart() == 0 || text[range.getStart() - 1] == ' ';
        const bool endsWord = range.getEnd() >= text.length() || text[range.getEnd()] == ' ' ||
                              text[range.getEnd() - 1] == ' ';
        whole = whole && startsWord && endsWord && line.getLineBoundsX().getLength() <= area.getWidth() + 0.5f;
      }
      if (!whole) cut += " " + text.toStdString() + "->" + lines.toStdString();
    }
    if (look != nullptr) bar.setLookAndFeel(nullptr);
    return cut;
  }

  // The width the eleven tabs ask for with JUCE's own look, in this
  // platform's font.
  static int naturalWidth() {
    juce::TabbedButtonBar bar(juce::TabbedButtonBar::TabsAtTop);
    for (auto* name : {"General", "Audio", "MIDI", "Performance", "Reverb", "Metronome", "Combination sets",
                       "Console display", "Language", "Remote control", "Diagnostics"})
      bar.addTab(name, juce::Colours::darkgrey, -1);
    bar.setBounds(0, 0, 4000, 30);
    int total = 0;
    for (int i = 0; i < bar.getNumTabs(); ++i) total += bar.getTabButton(i)->getBestTabLength(30);
    return total;
  }

  void run() override {
    juce::MessageManager::getInstance();
    mp::ui::TabNamesLook look;
    // The tab bar of the Settings window left at 662 pixels wide on Windows,
    // as a share of what the names ask for, so the narrow case is as narrow
    // in every platform's font.
    const int natural = naturalWidth();
    const int narrow = natural * kNarrowShare / 100;
    const auto at = " at " + std::to_string(narrow) + " of " + std::to_string(natural) + " pixels:";
    const auto before = cutNames(nullptr, narrow);
    MP_CHECK(!before.empty(), "before: JUCE's own widths broke names" + at + before);
    MP_CHECK(cutNames(&look, narrow).empty(), "every name whole" + at + cutNames(&look, narrow));
    MP_CHECK(cutNames(&look, 940).empty(), "and at the window's first size:" + cutNames(&look, 940));
  }
  static constexpr int kNarrowShare = 70;  // 650 of 923 pixels on Windows
};
static SettingsTabNamesTest g_settingsTabNames;

#endif  // MP_TEST_HAS_AUDIO
