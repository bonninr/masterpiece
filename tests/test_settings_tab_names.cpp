// The Settings tabs keep their names whole in a narrow window ("Performanc",
// "Metronom" and "Languag" before): every word of a name fits its tab, a name
// of two words on two lines.
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

  void run() override {
    juce::MessageManager::getInstance();
    mp::ui::TabNamesLook look;
    // 650: the tab bar of the Settings window left at 662 pixels wide.
    const auto before = cutNames(nullptr, 650);
    MP_CHECK(!before.empty(), "before: JUCE's own widths broke names at 650 pixels:" + before);
    MP_CHECK(cutNames(&look, 650).empty(), "every name whole at 650 pixels:" + cutNames(&look, 650));
    MP_CHECK(cutNames(&look, 940).empty(), "and at the window's first size:" + cutNames(&look, 940));
  }
};
static SettingsTabNamesTest g_settingsTabNames;

#endif  // MP_TEST_HAS_AUDIO
