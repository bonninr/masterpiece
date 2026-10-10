#include "OpenWindow.h"

#include "Mobile.h"

#include <algorithm>
#include <map>

namespace mp::ui {

namespace {
constexpr int kRowH = kMobile ? 56 : 46;
const juce::Colour kBg(0xff15171c), kRowBg(0xff20232a), kRowHover(0xff2a2f3a),
    kText(0xffe6e8ec), kDim(0xff8c93a0);

// "GrandOrgue" or "Hauptwerk", from the file: two favourites with one name are
// usually the same organ in both formats.
juce::String formatOf(const juce::File& f) {
  const auto ext = f.getFileExtension().toLowerCase();
  if (ext == ".organ" || ext == ".orgue") return "GrandOrgue";
  if (ext.endsWith("hauptwerk_xml")) return "Hauptwerk";
  if (ext == ".rar") return "package";
  return {};
}
}  // namespace

// One favourite: its name and file, opened by a touch anywhere on it.
class OpenPanel::Row : public juce::Component {
public:
  Row(OpenPanel& owner, int slot, juce::String name, juce::String detail, bool canUp, bool canDown)
      : owner_(owner), slot_(slot), name_(std::move(name)), detail_(std::move(detail)) {
    for (auto* b : {&up_, &down_}) {
      addAndMakeVisible(*b);
      b->setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    up_.setEnabled(canUp);
    down_.setEnabled(canDown);
    up_.setTooltip("Move up the list");
    down_.setTooltip("Move down the list");
    up_.onClick = [this] { owner_.move(slot_, -1); };
    down_.onClick = [this] { owner_.move(slot_, +1); };
    addAndMakeVisible(rename_);
    rename_.onClick = [this] { owner_.rename(slot_); };
    addAndMakeVisible(remove_);
    remove_.onClick = [this] {
      owner_.proc_.favourites().organs.clear(slot_);
      owner_.proc_.saveGlobalDefaults();
      owner_.refresh();
    };
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTitle(name_);
  }

  void paint(juce::Graphics& g) override {
    auto r = getLocalBounds().toFloat().reduced(0.0f, 2.0f);
    g.setColour(isMouseOver(true) ? kRowHover : kRowBg);
    g.fillRoundedRectangle(r, 6.0f);
    auto text = getLocalBounds().reduced(12, 4).withTrimmedRight(textRight_);
    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    g.drawText(juce::String(slot_), text.removeFromLeft(26), juce::Justification::centredLeft);
    auto top = text.removeFromTop(text.getHeight() / 2 + 2);
    g.setColour(kText);
    g.setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    g.drawText(name_, top, juce::Justification::bottomLeft, true);
    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText(detail_, text, juce::Justification::topLeft, true);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(6, 8);
    const int b = kMobile ? 40 : 30;
    remove_.setBounds(r.removeFromRight(kMobile ? 86 : 74));
    r.removeFromRight(4);
    rename_.setBounds(r.removeFromRight(kMobile ? 86 : 74));
    r.removeFromRight(8);
    down_.setBounds(r.removeFromRight(b).reduced(4));
    up_.setBounds(r.removeFromRight(b).reduced(4));
    textRight_ = getWidth() - r.getRight();
  }

  int slot() const { return slot_; }
  void mouseEnter(const juce::MouseEvent&) override { repaint(); }
  void mouseExit(const juce::MouseEvent&) override { repaint(); }
  void mouseUp(const juce::MouseEvent& e) override {
    if (e.mouseWasClicked() && !e.mods.isPopupMenu()) owner_.openSlot(slot_);
  }

private:
  OpenPanel& owner_;
  int slot_;
  juce::String name_, detail_;
  int textRight_ = 0;
  juce::ArrowButton up_{"Up", 0.75f, kDim}, down_{"Down", 0.25f, kDim};
  juce::TextButton rename_{"Rename"}, remove_{"Remove"};
};

OpenPanel::OpenPanel(MasterpieceProcessor& p, std::function<void(const juce::File&)> open,
                     std::function<void()> browse)
    : proc_(p), open_(std::move(open)), browse_(std::move(browse)) {
  addAndMakeVisible(search_);
  search_.setTextToShowWhenEmpty("Search the favourites", kDim);
  search_.setColour(juce::TextEditor::backgroundColourId, kRowBg);
  search_.setColour(juce::TextEditor::textColourId, kText);
  search_.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff3a404c));
  search_.setFont(juce::Font(juce::FontOptions(15.0f)));
  search_.setIndents(10, 6);
  search_.onTextChange = [this] { refresh(); };
  // Return opens the first organ the search shows.
  search_.onReturnKey = [this] {
    if (!rowViews_.empty()) openSlot(rowViews_.front()->slot());
  };
  search_.onEscapeKey = [this] { close(); };

  addAndMakeVisible(viewport_);
  viewport_.setViewedComponent(&rows_, false);
  viewport_.setScrollBarsShown(true, false);
  viewport_.setScrollBarThickness(10);

  addChildComponent(empty_);
  empty_.setJustificationType(juce::Justification::centred);
  empty_.setColour(juce::Label::textColourId, kDim);

  addAndMakeVisible(status_);
  status_.setColour(juce::Label::textColourId, kDim);

  addAndMakeVisible(addCurrent_);
  addCurrent_.setTooltip("Keep the organ now on the console in this list");
  addCurrent_.onClick = [this] {
    const int slot = proc_.addCurrentOrganToFavourites();
    status_.setText(slot == 0 ? "All 64 slots are taken" : "Added on slot " + juce::String(slot),
                    juce::dontSendNotification);
    refresh();
  };
  addAndMakeVisible(another_);
  another_.setTooltip("Choose an organ file that is not in the list");
  another_.onClick = [this] {
    auto browse = browse_;
    close();
    if (browse) browse();
  };
  setSize(kMobile ? 640 : 600, 520);
  refresh();
}

void OpenPanel::refresh() {
  rowViews_.clear();
  const auto& bank = proc_.favourites().organs;
  const auto slots = bank.used();
  const auto query = search_.getText().trim().toLowerCase();
  std::map<std::string, int> named;
  for (int s : slots) ++named[bank.at(s).name];
  int y = 0;
  for (size_t i = 0; i < slots.size(); ++i) {
    const int slot = slots[i];
    const auto& fav = bank.at(slot);
    const juce::File f(juce::String::fromUTF8(fav.target.c_str()));
    const juce::String name = juce::String::fromUTF8(fav.name.c_str());
    if (query.isNotEmpty() && !name.toLowerCase().contains(query) &&
        !f.getFileName().toLowerCase().contains(query))
      continue;
    juce::String detail = f.getFileName();
    if (named[fav.name] > 1)
      if (const auto kind = formatOf(f); kind.isNotEmpty()) detail = kind + ": " + detail;
    if (!f.existsAsFile() && !MasterpieceProcessor::portableCopyFor(f).existsAsFile())
      detail = "not found: " + f.getFullPathName();
    // Moving is by place in the whole list, so it is offered only unfiltered.
    const bool canMove = query.isEmpty();
    auto row = std::make_unique<Row>(*this, slot, name, detail, canMove && i > 0,
                                     canMove && i + 1 < slots.size());
    rows_.addAndMakeVisible(*row);
    row->setBounds(0, y, 10, kRowH);
    y += kRowH;
    rowViews_.push_back(std::move(row));
  }
  empty_.setVisible(rowViews_.empty());
  empty_.setText(slots.empty() ? "No favourite organs yet. Open one, then add it here."
                               : "No favourite matches \"" + search_.getText().trim() + "\".",
                 juce::dontSendNotification);
  const auto loaded = proc_.loadedOrganFile();
  addCurrent_.setEnabled(loaded != juce::File() &&
                         bank.slotOf(loaded.getFullPathName().toStdString()) == 0);
  resized();
}

void OpenPanel::openSlot(int slot) {
  const juce::File f(juce::String::fromUTF8(proc_.favourites().organs.at(slot).target.c_str()));
  if (!f.existsAsFile() && !MasterpieceProcessor::portableCopyFor(f).existsAsFile()) {
    // A set on a drive not plugged in: said, and the favourite kept.
    status_.setText("Not found: " + f.getFullPathName(), juce::dontSendNotification);
    return;
  }
  auto open = open_;
  close();
  if (open) open(f);
}

void OpenPanel::move(int slot, int by) {
  auto& bank = proc_.favourites().organs;
  const auto slots = bank.used();
  const auto at = std::find(slots.begin(), slots.end(), slot);
  if (at == slots.end()) return;
  const auto i = static_cast<long>(at - slots.begin()) + by;
  if (i < 0 || i >= static_cast<long>(slots.size())) return;
  // The two swap slots, so the gaps a player left stay where they are.
  const int other = slots[static_cast<size_t>(i)];
  const Favourite a = bank.at(slot), b = bank.at(other);
  bank.set(slot, b);
  bank.set(other, a);
  proc_.saveGlobalDefaults();
  status_.setText(juce::String::fromUTF8(a.name.c_str()) + " is on slot " + juce::String(other),
                  juce::dontSendNotification);
  refresh();
}

void OpenPanel::rename(int slot) {
  const auto& fav = proc_.favourites().organs.at(slot);
  auto* w = new juce::AlertWindow("Rename", "The name this favourite shows.",
                                  juce::MessageBoxIconType::NoIcon);
  w->addTextEditor("name", juce::String::fromUTF8(fav.name.c_str()));
  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<OpenPanel> self(this);
  w->enterModalState(true, juce::ModalCallbackFunction::create([self, w, slot](int r) {
                       if (self == nullptr || r != 1) return;
                       const auto name = w->getTextEditorContents("name").trim();
                       if (name.isEmpty()) return;
                       auto& bank = self->proc_.favourites().organs;
                       auto f = bank.at(slot);
                       f.name = name.toStdString();
                       bank.set(slot, f);
                       self->proc_.saveGlobalDefaults();
                       self->refresh();
                     }),
                     true);
}

void OpenPanel::close() {
  // Posted: the button that asked is inside what is being closed.
  juce::Component::SafePointer<juce::DialogWindow> dw(findParentComponentOfClass<juce::DialogWindow>());
  juce::MessageManager::callAsync([dw] {
    if (dw != nullptr) dw->closeButtonPressed();
  });
}

void OpenPanel::visibilityChanged() {
  // Typing finds an organ at once on a computer; on a phone or tablet the
  // keyboard would cover the list before it was asked for.
  if (isShowing() && !kMobile) search_.grabKeyboardFocus();
}

void OpenPanel::paint(juce::Graphics& g) { g.fillAll(kBg); }

void OpenPanel::resized() {
  auto r = getLocalBounds().reduced(14);
  search_.setBounds(r.removeFromTop(kMobile ? 44 : 34));
  r.removeFromTop(10);
  auto bottom = r.removeFromBottom(kMobile ? 44 : 34);
  another_.setBounds(bottom.removeFromRight(200));
  bottom.removeFromRight(8);
  addCurrent_.setBounds(bottom.removeFromRight(210));
  r.removeFromBottom(6);
  status_.setBounds(r.removeFromBottom(22));
  r.removeFromBottom(4);
  viewport_.setBounds(r);
  empty_.setBounds(r);
  const int w = juce::jmax(0, r.getWidth() - 12);
  rows_.setSize(w, static_cast<int>(rowViews_.size()) * kRowH);
  for (auto& row : rowViews_) row->setSize(w, kRowH);
}

void OpenPanel::show(MasterpieceProcessor& p, std::function<void(const juce::File&)> open,
                     std::function<void()> browse) {
  if (p.favourites().organs.used().empty()) {
    if (browse) browse();
    return;
  }
  juce::DialogWindow::LaunchOptions o;
  auto panel = std::make_unique<OpenPanel>(p, std::move(open), std::move(browse));
  o.content.setOwned(panel.release());
  o.dialogTitle = "Open";
  o.dialogBackgroundColour = kBg;
  o.escapeKeyTriggersCloseButton = true;
  o.useNativeTitleBar = true;
  o.resizable = true;
  launchDialog(o);
}

}  // namespace mp::ui
