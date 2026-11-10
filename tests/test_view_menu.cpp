// The View menu with no organ loaded: there are no stops to list or to put on
// a panel, so "Stop list" is neither ticked nor offered, and "Panel of the
// whole organ" is greyed out (reported on D#565, 2026-10-31).
//
// What the main window shows is one value: the console, the stop list, a
// panel or the score. What is drawn, the tab strip and the View menu's ticks
// agree after every change, and the stop list shown over a tab goes back to
// that tab.
#include "MpTest.h"

#ifdef MP_TEST_HAS_AUDIO

#include "../src/mp_audio/DataFolder.h"
#include "../src/mp_audio/MasterpieceProcessor.h"
#include "../src/mp_ui/Ui.h"

#include <set>
#include <string>
#include <vector>

using mp::test::Category;

class ViewMenuNoOrganTest final : public mp::test::Test {
public:
  ViewMenuNoOrganTest() : Test("functional.ui.view-menu-no-organ", Category::Functional) {}

  void run() override {
    mp::MasterpieceProcessor proc;
    MP_CHECK(proc.loadedOrganFile() == juce::File(), "no organ is loaded");
    mp::ui::MasterpieceEditor editor(proc);
    juce::PopupMenu menu;
    editor.addPanelItems(menu);

    bool sawStopList = false, sawPanel = false;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
      const auto& item = it.getItem();
      if (item.itemID == 1002) {
        sawStopList = true;
        MP_CHECK(!item.isTicked, "Stop list is not ticked with no organ");
        MP_CHECK(!item.isEnabled, "and is greyed out");
      }
      if (item.itemID == 1001) {
        sawPanel = true;
        MP_CHECK(!item.isEnabled, "Panel of the whole organ is greyed out with no organ");
      }
    }
    MP_CHECK(sawStopList && sawPanel, "both items are in the menu");
  }
};
static ViewMenuNoOrganTest g_viewMenuNoOrgan;

namespace {

using View = mp::ui::MasterpieceEditor::MainView;
using Kind = View::Kind;

// The View menu's view items that are ticked: a console page (1400 on), the
// stop list (15, or 1002 on an organ that draws no console), a panel (1300
// on) and the score (16).
std::set<int> viewTicks(mp::ui::MasterpieceEditor& editor) {
  juce::PopupMenu menu;
  editor.addPanelItems(menu);
  std::set<int> ticked;
  for (juce::PopupMenu::MenuItemIterator it(menu); it.next();) {
    const auto& item = it.getItem();
    const int id = item.itemID;
    const bool viewItem = id == 15 || id == 16 || id == 1002 || (id >= 1300 && id < 1400) || id >= 1400;
    if (viewItem && item.isTicked) ticked.insert(id);
  }
  return ticked;
}

std::string tickList(const std::set<int>& ticks) {
  std::string s;
  for (int t : ticks) s += std::to_string(t) + " ";
  return s.empty() ? "none" : s;
}

// What is drawn, the tab strip and the ticks, against the view. `pages` is
// the console's page count.
void checkAgree(mp::ui::MasterpieceEditor& editor, int pages, bool artwork, const std::string& when) {
  const View v = editor.viewForTest();
  const auto drawn = editor.drawnForTest().toStdString();
  const auto ticks = viewTicks(editor);
  const int tab = editor.currentTabForTest();
  const std::string state = " (" + when + "; drawn " + drawn + ", tab " + std::to_string(tab) + ", ticked " +
                            tickList(ticks) + ")";
  MP_CHECK(ticks.size() == 1, "one view item is ticked" + state);
  switch (v.kind) {
    case Kind::Console:
      MP_CHECK(drawn.rfind("console ", 0) == 0, "the console is drawn" + state);
      MP_CHECK(drawn == "console " + std::to_string(tab), "the strip is on the page drawn" + state);
      MP_CHECK(ticks.count(1400 + tab) == 1, "and the menu ticks that page" + state);
      break;
    case Kind::StopList:
      MP_CHECK(drawn == "stop list", "the stop list is drawn" + state);
      MP_CHECK(!editor.tabStripShownForTest(), "with no tab strip" + state);
      MP_CHECK(ticks.count(artwork ? 15 : 1002) == 1, "and the menu ticks Stop list" + state);
      break;
    case Kind::Panel:
      MP_CHECK(drawn == "panel " + std::to_string(v.panel), "the panel is drawn" + state);
      MP_CHECK(tab == pages + v.panel, "the strip is on its tab" + state);
      MP_CHECK(ticks.count(1300 + v.panel) == 1, "and the menu ticks it" + state);
      break;
    case Kind::Score:
      MP_CHECK(drawn == "score", "the score is drawn" + state);
      MP_CHECK(tab == editor.tabCountForTest() - 1, "the strip is on its tab" + state);
      MP_CHECK(ticks.count(16) == 1, "and the menu ticks it" + state);
      break;
  }
}

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Console: return "console";
    case Kind::StopList: return "stop list";
    case Kind::Panel: return "panel";
    case Kind::Score: return "score";
  }
  return "?";
}

// The minimal organ, which draws a console; and the same organ with its
// console pages and artwork left out.
juce::File writeOrgans(const juce::File& dir, juce::File& plain) {
  const juce::File fixture(juce::String(MP_TEST_FIXTURES_DIR) + "/minimal.Organ_Hauptwerk_xml");
  const juce::String text = fixture.loadFileAsString();
  const juce::File drawn = dir.getChildFile("drawn.Organ_Hauptwerk_xml");
  drawn.replaceWithText(text);
  const int from = text.indexOf("<ObjectList ObjectType=\"DisplayPage\">");
  const int to = text.indexOf("<ObjectList ObjectType=\"Combination\">");
  plain = dir.getChildFile("plain.Organ_Hauptwerk_xml");
  if (from > 0 && to > from)
    plain.replaceWithText((text.substring(0, from) + text.substring(to))
                              .replace("<Identification_UniqueOrganID>90001", "<Identification_UniqueOrganID>90002"));
  return drawn;
}

}  // namespace

class ViewOneStateTest final : public mp::test::Test {
public:
  ViewOneStateTest() : Test("functional.ui.view-one-state", Category::Functional) {}

  void run() override {
    juce::MessageManager::getInstance();
    const auto data = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mp-view-one-state");
    data.deleteRecursively();
    data.createDirectory();
    // The player's own panels are not this test's to change.
    const auto previous = mp::dataFolder();
    mp::setDataFolder(data);
    juce::File plain;
    const juce::File drawn = writeOrgans(data, plain);
    drawnOrgan(drawn);
    plainOrgan(plain);
    mp::setDataFolder(previous);
    data.deleteRecursively();
  }

private:
  void drawnOrgan(const juce::File& odf) {
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ loads");
    mp::ui::MasterpieceEditor editor(proc);
    editor.setSize(1200, 700);
    editor.organLoadedForTest();
    MP_CHECK(editor.drawnForTest() == "console 0", "the organ opens on its console, drew " +
                                                       editor.drawnForTest().toStdString());
    if (editor.drawnForTest() != "console 0") return;
    const int pages = editor.tabCountForTest();
    checkAgree(editor, pages, true, "loaded");

    // The first finding: on a panel's tab, View > Stop list twice. The list
    // goes back to the panel, and its tab is the strip's current one.
    editor.chooseViewItemForTest(1001);
    MP_CHECK((editor.viewForTest() == View{Kind::Panel, 0}), "View > Panel of the whole organ shows it");
    checkAgree(editor, pages, true, "the new panel");
    editor.chooseViewItemForTest(15);
    MP_CHECK(editor.viewForTest().kind == Kind::StopList, "View > Stop list over the panel");
    checkAgree(editor, pages, true, "the list over the panel");
    editor.chooseViewItemForTest(15);
    MP_CHECK((editor.viewForTest() == View{Kind::Panel, 0}), "View > Stop list again: back to the panel");
    checkAgree(editor, pages, true, "the list hidden again");
    // From the console page, the panel's tab works.
    editor.chooseViewItemForTest(1400);
    checkAgree(editor, pages, true, "View > the page");
    editor.pressTabForTest(pages);
    MP_CHECK((editor.viewForTest() == View{Kind::Panel, 0}), "the panel's tab shows the panel");
    checkAgree(editor, pages, true, "the panel's tab pressed");

    // The list over the console page, and hidden again: the page.
    editor.chooseViewItemForTest(1400);
    editor.chooseViewItemForTest(15);
    editor.chooseViewItemForTest(15);
    checkAgree(editor, pages, true, "the list over the console and hidden");
    MP_CHECK(editor.viewForTest().kind == Kind::Console, "back to the console");

    std::vector<std::pair<int, Kind>> items = {
        {1400, Kind::Console}, {15, Kind::StopList}, {1300, Kind::Panel}};
#if MP_HAS_SCORE
    // The second finding: on the score's tab, View > Stop list. Only the
    // stop list is ticked, and hiding it goes back to the score.
    editor.chooseViewItemForTest(16);
    MP_CHECK(editor.viewForTest().kind == Kind::Score, "View > Score shows it");
    checkAgree(editor, pages, true, "the score");
    editor.chooseViewItemForTest(15);
    checkAgree(editor, pages, true, "the list over the score");
    const auto ticks = viewTicks(editor);
    MP_CHECK(ticks.count(16) == 0 && ticks.count(15) == 1,
             "over the score, Stop list is ticked and Score is not: " + tickList(ticks));
    editor.chooseViewItemForTest(15);
    MP_CHECK(editor.viewForTest().kind == Kind::Score, "View > Stop list again: back to the score");
    checkAgree(editor, pages, true, "the list hidden over the score");
    items.push_back({16, Kind::Score});
#endif

    // Every view from every other one, by the View menu. The stop list's item
    // hides it again, so "from the list" is reached by showing it first.
    for (const auto& [fromItem, fromKind] : items)
      for (const auto& [toItem, toKind] : items) {
        if (fromKind == toKind) continue;
        editor.chooseViewItemForTest(1400);
        editor.chooseViewItemForTest(fromItem);
        const std::string when = std::string(kindName(fromKind)) + " to " + kindName(toKind);
        MP_CHECK(editor.viewForTest().kind == fromKind, "on the " + std::string(kindName(fromKind)) + ", " + when);
        checkAgree(editor, pages, true, "on the first, " + when);
        editor.chooseViewItemForTest(toItem);
        MP_CHECK(editor.viewForTest().kind == toKind, "the menu goes " + when);
        checkAgree(editor, pages, true, when);
      }

    // And every tab from every other one, by the tab strip.
    std::vector<int> tabs;
    for (int t = 0; t < editor.tabCountForTest(); ++t) tabs.push_back(t);
    for (int from : tabs)
      for (int to : tabs) {
        if (from == to) continue;
        editor.pressTabForTest(from);
        editor.pressTabForTest(to);
        checkAgree(editor, pages, true, "tab " + std::to_string(from) + " to tab " + std::to_string(to));
        MP_CHECK(editor.currentTabForTest() == to, "tab " + std::to_string(to) + " is current");
      }

    // A panel deleted while it shows: back to the console.
    editor.chooseViewItemForTest(1300);
    editor.chooseViewItemForTest(1100);
    MP_CHECK(editor.viewForTest().kind == Kind::Console, "a panel deleted: back to the console");
    checkAgree(editor, pages, true, "the panel deleted");
  }

  // An organ that draws no console has the stop list where the console would
  // be: the base view, from a panel, the score, and a panel deleted.
  void plainOrgan(const juce::File& odf) {
    MP_CHECK(odf.existsAsFile(), "the organ without artwork is written");
    if (!odf.existsAsFile()) return;
    mp::MasterpieceProcessor proc;
    proc.prepareToPlay(48000.0, 256);
    MP_CHECK(proc.loadOrgan(odf, 0, false).ok, "the organ without artwork loads");
    mp::ui::MasterpieceEditor editor(proc);
    editor.setSize(1200, 700);
    editor.organLoadedForTest();
    MP_CHECK(editor.viewForTest().kind == Kind::StopList, "it opens on the stop list");
    checkAgree(editor, 0, false, "loaded without artwork");
    editor.chooseViewItemForTest(1001);
    MP_CHECK((editor.viewForTest() == View{Kind::Panel, 0}), "a panel shows");
    checkAgree(editor, 0, false, "its panel");
    editor.chooseViewItemForTest(1002);
    MP_CHECK(editor.viewForTest().kind == Kind::StopList, "View > Stop list: back to the list");
    checkAgree(editor, 0, false, "back to the list");
    editor.chooseViewItemForTest(1300);
    editor.chooseViewItemForTest(15);
    MP_CHECK(editor.viewForTest().kind == Kind::StopList, "the stop list key from the panel shows the list");
    checkAgree(editor, 0, false, "the stop list key");
#if MP_HAS_SCORE
    editor.chooseViewItemForTest(16);
    checkAgree(editor, 0, false, "the score without artwork");
    editor.chooseViewItemForTest(1002);
    MP_CHECK(editor.viewForTest().kind == Kind::StopList, "from the score, back to the list");
    checkAgree(editor, 0, false, "from the score");
#endif
    editor.chooseViewItemForTest(1300);
    editor.chooseViewItemForTest(1100);
    MP_CHECK(editor.viewForTest().kind == Kind::StopList, "a panel deleted: back to the list");
    MP_CHECK(editor.drawnForTest() == "stop list", "and the list is drawn, drew " +
                                                       editor.drawnForTest().toStdString());
  }
};
static ViewOneStateTest g_viewOneState;

#endif  // MP_TEST_HAS_AUDIO
