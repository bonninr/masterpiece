// The band along the top of the window stays one row.
//
// From a 1024-pixel window up every control of the band -- the menus, the
// volume bar, the sequencer, the tuning button, save and panic -- sits in one
// row, in every language the program ships, with the menus as wide as their
// translated names up to a cap. Nothing overlaps, and the volume bar keeps
// the width a hand can set a level on. A narrow window gives the menus and
// the bar a row of their own.
#ifdef MP_TEST_HAS_AUDIO

#include "MpTest.h"

#include "../src/mp_audio/Translations.h"
#include "../src/mp_ui/Ui.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using mp::test::Category;

namespace {

// The narrowest window the band holds to one row: 1024 pixels with its
// frame, which takes 8 a side on Windows. The band lays out inside it.
constexpr int kOneRowFrom = 1024 - 16;

// The menu names measured as the window measures them, in the language in
// force.
std::array<int, 5> measureMenus() {
  std::array<int, 5> widths{};
  juce::TextButton file{TRANS("File")}, organ{TRANS("Organ")}, view{TRANS("View")},
      settings{TRANS("Settings")}, help{TRANS("Help")};
  juce::TextButton* const buttons[] = {&file, &organ, &view, &settings, &help};
  for (size_t i = 0; i < widths.size(); ++i) widths[i] = mp::ui::textButtonTextWidth(*buttons[i]);
  return widths;
}

std::vector<juce::Rectangle<int>> cells(const mp::ui::TopBandLayout& b) {
  std::vector<juce::Rectangle<int>> out(b.menus.begin(), b.menus.end());
  for (const auto& r : {b.bar, b.setter, b.stepPrev, b.stepFrame, b.stepNext, b.tuning, b.save, b.panic})
    out.push_back(r);
  return out;
}

class TopBandOneRowTest final : public mp::test::Test {
public:
  TopBandOneRowTest() : Test("functional.ui.top-band-one-row", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    std::vector<juce::String> codes{"en"};
    for (const auto& l : mp::availableLanguages()) codes.push_back(l.code);

    for (const auto& code : codes) {
      mp::applyLanguage(code);
      const auto menus = measureMenus();
      const std::string lang = code.toStdString();

      // The window opens 1280 wide, and 1240 fits a 1280 screen with its
      // frame.
      for (const int width : {1280, 1240, kOneRowFrom}) {
        juce::Rectangle<int> area(0, 0, width, 600);
        const auto band = mp::ui::layoutTopBand(area, menus, false);
        const std::string where = lang + " at " + std::to_string(width);
        if (width == kOneRowFrom) {
          // The row this language needs: the menus, the fixed buttons and the
          // fader at its narrowest.
          int needed = mp::ui::TopBar::kMinWidth;
          for (const auto& r : cells(band))
            if (r != band.bar) needed += r.getWidth();
          std::printf("        %s: row %d of %d\n", lang.c_str(), needed, width);
        }
        MP_CHECK(!band.stacked, where + ": the band is one row");
        MP_CHECK(area.getY() == mp::ui::kTopBandHeight, where + ": the band takes one row's height");
        const auto all = cells(band);
        for (size_t i = 0; i < all.size(); ++i) {
          MP_CHECK(all[i].getY() == 0 && all[i].getHeight() == mp::ui::kTopBandHeight,
                   where + ": control " + std::to_string(i) + " sits in the row");
          MP_CHECK(all[i].getX() >= 0 && all[i].getRight() <= width,
                   where + ": control " + std::to_string(i) + " is inside the window");
          for (size_t j = i + 1; j < all.size(); ++j)
            MP_CHECK(!all[i].intersects(all[j]),
                     where + ": controls " + std::to_string(i) + " and " + std::to_string(j) + " overlap");
        }
        MP_CHECK(band.bar.getWidth() >= mp::ui::TopBar::kMinWidth,
                 where + ": the volume bar keeps " + std::to_string(mp::ui::TopBar::kMinWidth) + " px, got " +
                     std::to_string(band.bar.getWidth()));
        for (size_t i = 0; i < menus.size(); ++i)
          MP_CHECK(band.menus[i].getWidth() >= std::min(menus[i] + mp::ui::kMenuPadding, mp::ui::kMenuMaxWidth),
                   where + ": menu " + std::to_string(i) + " fits its name");
      }

      // Sanity: a narrow window does take the second row, and the bar keeps
      // its width there.
      juce::Rectangle<int> narrow(0, 0, 700, 600);
      const auto band = mp::ui::layoutTopBand(narrow, menus, false);
      MP_CHECK(band.stacked, lang + " at 700: the menus and the bar take a row of their own");
      MP_CHECK(narrow.getY() == 2 * mp::ui::kTopBandHeight, lang + " at 700: two rows' height");
      MP_CHECK(band.bar.getY() == 0 && band.tuning.getY() == mp::ui::kTopBandHeight,
               lang + " at 700: the bar above, the buttons below");
      MP_CHECK(band.bar.getWidth() >= mp::ui::TopBar::kMinWidth, lang + " at 700: the bar keeps its width");
    }
    mp::applyLanguage("en");
  }
};

}  // namespace

static TopBandOneRowTest g_topBandOneRow;

#endif  // MP_TEST_HAS_AUDIO
