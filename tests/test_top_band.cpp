// The band along the top of the window stays one row.
//
// From 1024 pixels up every control of the band -- the menus, the volume
// bar, the sequencer, the tuning button, save and panic -- sits in one row,
// in every language the program ships, with the menus as wide as their
// translated names up to a cap. The layout box joins the row where it fits
// with its longest name and steps out where it does not (View > Console
// layout offers the same choice). Nothing overlaps, and the volume bar keeps
// the width a hand can set a level on. A narrow window gives the menus and
// the bar a row of their own, with the layout box back on the second.
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

struct Measured {
  std::array<int, 5> menus{};
  int layout = 0;  // the widest name the layout box can show
};

// The names measured as the window measures them, in the language in force.
Measured measure() {
  Measured m;
  juce::TextButton file{TRANS("File")}, organ{TRANS("Organ")}, view{TRANS("View")},
      settings{TRANS("Settings")}, help{TRANS("Help")};
  juce::TextButton* const buttons[] = {&file, &organ, &view, &settings, &help};
  for (size_t i = 0; i < m.menus.size(); ++i) m.menus[i] = mp::ui::textButtonTextWidth(*buttons[i]);
  juce::ComboBox box;
  box.addItem(TRANS("Console: main"), 1);
  box.addItem(TRANS("%1 (portrait)").replace("%1", TRANS("Console: alt %1").replace("%1", "9")), 2);
  for (int id : {1, 2}) {
    box.setSelectedId(id, juce::dontSendNotification);
    m.layout = std::max(m.layout, mp::ui::comboTextWidth(box));
  }
  return m;
}

std::vector<juce::Rectangle<int>> cells(const mp::ui::TopBandLayout& b, bool withLayout) {
  std::vector<juce::Rectangle<int>> out(b.menus.begin(), b.menus.end());
  for (const auto& r : {b.bar, b.setter, b.stepPrev, b.stepFrame, b.stepNext, b.tuning, b.save, b.panic})
    out.push_back(r);
  if (withLayout) out.push_back(b.layout);
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
      const auto m = measure();
      const std::string lang = code.toStdString();

      // One row from 1024 pixels up, for an organ with several layouts and
      // one without; below 1024 the band may take two rows. The window opens
      // 1280 wide, and 1240 fits a 1280 screen with its frame.
      struct Case { int width; bool layoutBox; };
      for (const auto c : {Case{1280, true}, Case{1240, true}, Case{kOneRowFrom, true}, Case{kOneRowFrom, false}}) {
        juce::Rectangle<int> area(0, 0, c.width, 600);
        const auto band = mp::ui::layoutTopBand(area, m.menus, c.layoutBox ? m.layout : -1, false);
        const std::string where = lang + " at " + std::to_string(c.width) +
                                  (c.layoutBox ? " with the layout box" : "");
        if (c.width == 1280) {
          // The row this language needs, without the layout box: the menus,
          // the fixed buttons and the fader at its narrowest.
          int menus = 0;
          for (const auto& r : band.menus) menus += r.getWidth();
          const int fixed = band.save.getWidth() + band.panic.getWidth() + band.tuning.getWidth() +
                            band.stepPrev.getWidth() + band.stepFrame.getWidth() + band.stepNext.getWidth() +
                            band.setter.getWidth();
          std::printf("        %s: menus %d, row %d, layout box %d\n", lang.c_str(), menus,
                      menus + fixed + mp::ui::TopBar::kMinWidth, band.layout.getWidth());
        }
        MP_CHECK(!band.stacked, where + ": the band is one row");
        MP_CHECK(area.getY() == mp::ui::kTopBandHeight, where + ": the band takes one row's height");
        const auto all = cells(band, band.layoutShown);
        MP_CHECK(c.layoutBox || !band.layoutShown, where + ": no layout box for an organ with one layout");
        // In English the box has room at the usual widths: it gives way only
        // where the row needs it.
        if (code == "en" && c.layoutBox && c.width >= 1240)
          MP_CHECK(band.layoutShown, where + ": the layout box is in the row");
        for (size_t i = 0; i < all.size(); ++i) {
          MP_CHECK(all[i].getY() == 0 && all[i].getHeight() == mp::ui::kTopBandHeight,
                   where + ": control " + std::to_string(i) + " sits in the row");
          MP_CHECK(all[i].getX() >= 0 && all[i].getRight() <= c.width,
                   where + ": control " + std::to_string(i) + " is inside the window");
          for (size_t j = i + 1; j < all.size(); ++j)
            MP_CHECK(!all[i].intersects(all[j]),
                     where + ": controls " + std::to_string(i) + " and " + std::to_string(j) + " overlap");
        }
        MP_CHECK(band.bar.getWidth() >= mp::ui::TopBar::kMinWidth,
                 where + ": the volume bar keeps " + std::to_string(mp::ui::TopBar::kMinWidth) + " px, got " +
                     std::to_string(band.bar.getWidth()));
        for (size_t i = 0; i < m.menus.size(); ++i)
          MP_CHECK(band.menus[i].getWidth() >= std::min(m.menus[i] + mp::ui::kMenuPadding, mp::ui::kMenuMaxWidth),
                   where + ": menu " + std::to_string(i) + " fits its name");
      }

      // Sanity: a narrow window does take the second row, and the bar keeps
      // its width there.
      juce::Rectangle<int> narrow(0, 0, 700, 600);
      const auto band = mp::ui::layoutTopBand(narrow, m.menus, m.layout, false);
      MP_CHECK(band.stacked, lang + " at 700: the menus and the bar take a row of their own");
      MP_CHECK(narrow.getY() == 2 * mp::ui::kTopBandHeight, lang + " at 700: two rows' height");
      MP_CHECK(band.bar.getY() == 0 && band.tuning.getY() == mp::ui::kTopBandHeight,
               lang + " at 700: the bar above, the buttons below");
      MP_CHECK(band.layoutShown && band.layout.getY() == mp::ui::kTopBandHeight &&
                   !band.layout.intersects(band.tuning) && band.layout.getX() >= 0,
               lang + " at 700: the layout box is back, on the second row");
    }
    mp::applyLanguage("en");
  }
};

}  // namespace

static TopBandOneRowTest g_topBandOneRow;

#endif  // MP_TEST_HAS_AUDIO
