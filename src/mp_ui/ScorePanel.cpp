#include "ScorePanel.h"
#include "MpScoreFonts.h"
#include "Mobile.h"
#include "ScoreRegistration.h"
#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_audio/DataFolder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace mp::ui {

namespace {
constexpr int kBar = 34;
constexpr int kMargin = 8;
constexpr int kGap = 8;     // between two pages side by side
constexpr int kScale = 70;  // the notation at 100%, in percent of Verovio's own size
constexpr int kEngraveDelayMs = 200;
constexpr int kSaveDelayMs = 1000;  // a change to the marks is written this long after it
constexpr float kIcon = 18.0f;      // a mark's icon, in the panel's pixels
constexpr size_t kUndoSteps = 100;
const juce::Colour kBackground{0xff1b1e24};

bool hasClass(const juce::XmlElement& e, juce::StringRef name) {
  juce::StringArray tokens;
  tokens.addTokens(e.getStringAttribute("class"), " ", "");
  return tokens.contains(name);
}

// The transform attribute of an SVG element: the translate, scale and matrix
// steps Verovio writes, in order.
juce::AffineTransform parseTransform(const juce::String& text) {
  juce::AffineTransform t;
  auto rest = text.trim();
  while (rest.isNotEmpty()) {
    const int open = rest.indexOfChar('('), close = rest.indexOfChar(')');
    if (open <= 0 || close < open) break;
    const auto name = rest.substring(0, open).trim();
    juce::StringArray args;
    args.addTokens(rest.substring(open + 1, close).replaceCharacter(',', ' '), " ", "");
    args.removeEmptyStrings();
    auto arg = [&](int i, float fallback) { return i < args.size() ? args[i].getFloatValue() : fallback; };
    juce::AffineTransform step;
    if (name == "translate") {
      step = juce::AffineTransform::translation(arg(0, 0), arg(1, 0));
    } else if (name == "scale") {
      step = juce::AffineTransform::scale(arg(0, 1), arg(1, arg(0, 1)));
    } else if (name == "matrix" && args.size() == 6) {
      step = juce::AffineTransform(arg(0, 1), arg(2, 0), arg(4, 0), arg(1, 0), arg(3, 1), arg(5, 0));
    }
    // SVG applies the leftmost step last.
    t = step.followedBy(t);
    rest = rest.substring(close + 1).trim();
  }
  return t;
}

// A path "M x y L x y" that is horizontal: a staff line.
bool horizontalLine(const juce::String& d, float& x1, float& x2, float& y) {
  juce::StringArray parts;
  parts.addTokens(d.replaceCharacters("ML,", "   "), " ", "");
  parts.removeEmptyStrings();
  if (parts.size() != 4) return false;
  const float ya = parts[1].getFloatValue(), yb = parts[3].getFloatValue();
  if (std::abs(ya - yb) > 0.5f) return false;
  x1 = std::min(parts[0].getFloatValue(), parts[2].getFloatValue());
  x2 = std::max(parts[0].getFloatValue(), parts[2].getFloatValue());
  y = ya;
  return true;
}

struct GeometryWalk {
  ScorePageGeometry& out;
  struct Lines {
    float x1 = std::numeric_limits<float>::max(), x2 = std::numeric_limits<float>::lowest();
    float y1 = std::numeric_limits<float>::max(), y2 = std::numeric_limits<float>::lowest();
    float gap = 0;
  };
  std::vector<Lines> lines;

  void walk(const juce::XmlElement& e, const juce::AffineTransform& parent, int system,
            const juce::String& note) {
    auto t = parent;
    if (e.hasAttribute("transform")) t = parseTransform(e.getStringAttribute("transform")).followedBy(parent);
    if (e.hasTagName("g") && hasClass(e, "system")) {
      system = static_cast<int>(lines.size());
      lines.emplace_back();
    }
    auto noteId = note;
    if (e.hasTagName("g") && hasClass(e, "note")) noteId = e.getStringAttribute("id");

    if (e.hasTagName("g") && hasClass(e, "staff") && system >= 0) {
      std::vector<float> ys;
      for (auto* c : e.getChildWithTagNameIterator("path")) {
        float x1, x2, y;
        if (!horizontalLine(c->getStringAttribute("d"), x1, x2, y)) continue;
        auto a = juce::Point<float>(x1, y).transformedBy(t), b = juce::Point<float>(x2, y).transformedBy(t);
        auto& l = lines[static_cast<size_t>(system)];
        l.x1 = std::min(l.x1, a.x);
        l.x2 = std::max(l.x2, b.x);
        l.y1 = std::min(l.y1, a.y);
        l.y2 = std::max(l.y2, a.y);
        ys.push_back(a.y);
      }
      std::sort(ys.begin(), ys.end());
      for (size_t i = 1; i < ys.size(); ++i) {
        const float d = ys[i] - ys[i - 1];
        auto& gap = lines[static_cast<size_t>(system)].gap;
        if (d > 0.1f && (gap == 0 || d < gap)) gap = d;
      }
    }
    const bool notehead = noteId.isNotEmpty() && e.hasTagName("g") && hasClass(e, "notehead");
    for (auto* c : e.getChildIterator()) {
      if (c->hasTagName("use")) {
        if (!notehead) continue;
        // The notehead's glyph, placed by its own transform or by x and y.
        auto ct = c->hasAttribute("transform") ? parseTransform(c->getStringAttribute("transform")).followedBy(t) : t;
        const auto p = juce::Point<float>(static_cast<float>(c->getDoubleAttribute("x")),
                                          static_cast<float>(c->getDoubleAttribute("y")))
                           .transformedBy(ct);
        auto& n = out.notes[noteId.toStdString()];
        if (n.system < 0 || p.x < n.head.x) {
          n.head = p;
          n.system = system;
        }
        continue;
      }
      walk(*c, t, system, noteId);
    }
  }
};
// The staff lines in a colour of their own (#1012): each horizontal path of
// a staff. As an inline style: the SVG renderer lets the page's style sheet
// win over a plain stroke attribute.
void colourStaffLines(juce::XmlElement& e, const juce::String& colour) {
  if (e.hasTagName("g") && hasClass(e, "staff"))
    for (auto* c : e.getChildWithTagNameIterator("path")) {
      float x1, x2, y;
      if (horizontalLine(c->getStringAttribute("d"), x1, x2, y)) c->setAttribute("style", "stroke:" + colour);
    }
  for (auto* c : e.getChildIterator()) colourStaffLines(*c, colour);
}
}  // namespace

std::unique_ptr<juce::Drawable> parseScorePage(const std::string& svg, ScorePageGeometry& geometry,
                                               juce::Colour lines) {
  geometry = {};
  const auto doc = juce::XmlDocument::parse(juce::String::fromUTF8(svg.c_str()));
  if (doc == nullptr) return nullptr;
  geometry.width = static_cast<float>(doc->getDoubleAttribute("width"));
  geometry.height = static_cast<float>(doc->getDoubleAttribute("height"));

  // Verovio nests its drawing in a second <svg class="definition-scale">
  // element that carries the viewBox, the text font and the colour. JUCE's
  // SVG reader gives such a page no bounds and draws nothing, and does the
  // same for a font-family list on a group. The nested element is turned
  // into a plain group and its viewBox moved to the root, which JUCE draws
  // correctly, and its "Times, serif" becomes the one serif the program
  // carries, whose widths are the ones the text was laid out with.
  auto toPixels = juce::AffineTransform();
  if (auto* inner = doc->getChildByName("svg")) {
    const auto box = inner->getStringAttribute("viewBox");
    juce::StringArray v;
    v.addTokens(box, " ,", "");
    v.removeEmptyStrings();
    if (v.size() == 4 && v[2].getFloatValue() > 0 && v[3].getFloatValue() > 0 && geometry.width > 0)
      toPixels = juce::AffineTransform::translation(-v[0].getFloatValue(), -v[1].getFloatValue())
                     .scaled(geometry.width / v[2].getFloatValue(), geometry.height / v[3].getFloatValue());
    doc->setAttribute("viewBox", box);
    inner->setTagName("g");
    inner->removeAttribute("viewBox");
    inner->setAttribute("font-family", kScoreTextFont);
    GeometryWalk walk{geometry, {}};
    walk.walk(*inner, toPixels, -1, {});
    for (const auto& l : walk.lines) {
      ScorePageGeometry::System s;
      if (l.y2 >= l.y1) s.area = {l.x1, l.y1, l.x2 - l.x1, l.y2 - l.y1};
      if (l.gap > 0) s.lineGap = l.gap;
      geometry.systems.push_back(s);
    }
    if (!lines.isTransparent()) colourStaffLines(*inner, "#" + lines.toDisplayString(false));
  }
  return juce::Drawable::createFromSVGString(doc->toString());
}

bool unpackScoreFonts(const juce::File& folder) {
  folder.deleteRecursively();
  if (!folder.createDirectory().wasOk()) return false;
  juce::MemoryInputStream in(MpScoreFonts::fonts_zip, static_cast<size_t>(MpScoreFonts::fonts_zipSize), false);
  juce::ZipFile zip(in);
  if (!zip.uncompressTo(folder, true).wasOk()) return false;
  // The marker is written last: a folder without it was cut short and is
  // unpacked again.
  return folder.getChildFile(".complete").create().wasOk();
}

bool registerScoreFonts() {
  static const bool loaded = [] {
    // JUCE finds a typeface made this way by name only while it is alive, so
    // the faces are kept to the end. They are kept in a static made after the
    // first face: statics go in the reverse order they were made, and each
    // face takes itself out of JUCE's register of memory fonts, a static that
    // the first face makes. Kept in one made before it, they went after the
    // register and the program crashed on its way out.
    std::vector<juce::Typeface::Ptr> faces;
    bool all = true;
    for (int i = 0; i < MpScoreFonts::namedResourceListSize; ++i) {
      const juce::String file = MpScoreFonts::originalFilenames[i];
      if (!file.endsWithIgnoreCase(".otf") && !file.endsWithIgnoreCase(".ttf")) continue;
      int size = 0;
      const char* data = MpScoreFonts::getNamedResource(MpScoreFonts::namedResourceList[i], size);
      auto face = data != nullptr ? juce::Typeface::createSystemTypefaceFor(
                                        {reinterpret_cast<const std::byte*>(data), static_cast<size_t>(size)})
                                  : nullptr;
      if (face != nullptr) faces.push_back(face);
      else all = false;
    }
    const bool any = !faces.empty();
    static const std::vector<juce::Typeface::Ptr> kept = std::move(faces);
    return all && any;
  }();
  return loaded;
}

juce::File scoreResourceFolder() {
  const auto folder = mp::dataFolder()
                          .getChildFile("Score fonts");
  if (folder.getChildFile(".complete").existsAsFile()) return folder;
  return unpackScoreFonts(folder) ? folder : juce::File();
}

namespace {
// The manual each staff plays, one row per staff.
class ManualsDialog : public juce::Component {
public:
  explicit ManualsDialog(juce::Component::SafePointer<ScorePanel> panel,
                         const std::vector<ScorePanel::Staff>& staves,
                         const std::vector<std::pair<Id, juce::String>>& keyboards)
      : panel_(panel) {
    for (const auto& s : staves) {
      auto row = std::make_unique<Row>();
      row->label.setText(s.name, juce::dontSendNotification);
      row->box.addItem(TRANS("None"), 1);
      for (size_t i = 0; i < keyboards.size(); ++i)
        row->box.addItem(keyboards[i].second, static_cast<int>(i) + 2);
      const Id current = panel->keyboardForStaff(s.number);
      int selected = 1;
      for (size_t i = 0; i < keyboards.size(); ++i)
        if (keyboards[i].first == current) selected = static_cast<int>(i) + 2;
      row->box.setSelectedId(selected, juce::dontSendNotification);
      const int staff = s.number;
      row->box.onChange = [this, staff, keyboards, box = &row->box] {
        const int id = box->getSelectedId();
        const Id kb = id >= 2 ? keyboards[static_cast<size_t>(id - 2)].first : 0;
        if (panel_ != nullptr) panel_->setKeyboardForStaff(staff, kb);
      };
      // Silent, for the player to play it; the notes still shown (#1012).
      row->mute.setButtonText(TRANS("Mute"));
      row->mute.setTooltip(TRANS("Leave this staff silent, to play it yourself; it is saved with the score"));
      row->mute.setToggleState(panel->muted(staff), juce::dontSendNotification);
      row->mute.onClick = [this, staff, mute = &row->mute] {
        if (panel_ != nullptr) panel_->setMuted(staff, mute->getToggleState());
      };
      addAndMakeVisible(row->label);
      addAndMakeVisible(row->box);
      addAndMakeVisible(row->mute);
      rows_.push_back(std::move(row));
    }
    setSize(460, static_cast<int>(rows_.size()) * 30 + 12);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(6);
    for (auto& row : rows_) {
      auto line = r.removeFromTop(30).reduced(0, 3);
      row->label.setBounds(line.removeFromLeft(150));
      row->mute.setBounds(line.removeFromRight(76));
      line.removeFromRight(6);
      row->box.setBounds(line);
    }
  }

private:
  struct Row {
    juce::Label label;
    juce::ComboBox box;
    juce::ToggleButton mute;
  };
  juce::Component::SafePointer<ScorePanel> panel_;
  std::vector<std::unique_ptr<Row>> rows_;
};

void closeCallOut(juce::Component& c) {
  if (auto* box = c.findParentComponentOfClass<juce::CallOutBox>()) box->dismiss();
}

// A note's text, on as many lines as it needs. What is typed is kept however
// the box closes (Save, Escape, a click elsewhere); a note left empty is
// taken away: there is nothing in it to keep.
class MarkNoteEditor : public juce::Component {
public:
  MarkNoteEditor(juce::Component::SafePointer<ScorePanel> panel, int index, const juce::String& text, bool fresh)
      : panel_(panel), index_(index), fresh_(fresh) {
    editor_.setMultiLine(true, true);
    editor_.setReturnKeyStartsNewLine(true);
    editor_.setFont(juce::FontOptions(15.0f));
    editor_.setText(text, false);
    editor_.moveCaretToEnd();
    editor_.onEscapeKey = [this] { closeCallOut(*this); };
    save_.onClick = [this] { closeCallOut(*this); };
    delete_.onClick = [this] {
      if (panel_ != nullptr) panel_->deleteMark(index_);
      index_ = -1;
      closeCallOut(*this);
    };
    for (auto* c : std::initializer_list<juce::Component*>{&editor_, &save_, &delete_}) addAndMakeVisible(c);
    setSize(380, 230);
  }
  ~MarkNoteEditor() override {
    if (panel_ == nullptr || index_ < 0 || index_ >= static_cast<int>(panel_->marks().size())) return;
    if (editor_.getText().toStdString() != panel_->marks()[static_cast<size_t>(index_)].text)
      panel_->setMarkText(index_, editor_.getText());
    panel_->dropEmptyNote(index_, fresh_);
  }
  // Ready to go on writing, after what is there.
  void visibilityChanged() override {
    if (isShowing()) editor_.grabKeyboardFocus();
  }
  void resized() override {
    auto r = getLocalBounds().reduced(6);
    auto buttons = r.removeFromBottom(28);
    r.removeFromBottom(6);
    editor_.setBounds(r);
    save_.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(6);
    delete_.setBounds(buttons.removeFromRight(80));
  }

private:
  juce::Component::SafePointer<ScorePanel> panel_;
  int index_;
  bool fresh_;
  juce::TextEditor editor_;
  juce::TextButton save_{TRANS("Save")}, delete_{TRANS("Delete")};
};

// A registration: its name, what it holds for the organ loaded now, and what
// can be done with it.
class MarkRegistrationEditor : public juce::Component {
public:
  MarkRegistrationEditor(juce::Component::SafePointer<ScorePanel> panel, int index,
                         const MasterpieceProcessor& proc)
      : panel_(panel), index_(index) {
    const auto& mark = panel->marks()[static_cast<size_t>(index)];
    name_.setText(juce::String::fromUTF8(mark.text.c_str()), false);
    name_.setFont(juce::FontOptions(15.0f));
    name_.setSelectAllWhenFocused(true);
    name_.onReturnKey = [this] {
      rename();
      closeCallOut(*this);
    };
    name_.onEscapeKey = [this] { closeCallOut(*this); };
    name_.onFocusLost = [this] { rename(); };
    const auto states = parseScoreStates(mark.states);
    juce::StringArray lines;
    if (const auto* mine = stateForOrgan(states, proc.organKey())) {
      lines = describeScoreState(proc, *mine);
    } else {
      lines.add(TRANS("Not set for this organ: Store sets it."));
      forget_.setEnabled(false);
      recall_.setEnabled(false);
    }
    juce::StringArray others;
    for (const auto& s : states)
      if (s.organ != proc.organKey()) others.add(s.organName.isNotEmpty() ? s.organName : juce::String(s.organ));
    if (!others.isEmpty()) lines.add(TRANS("Also set for") + ": " + others.joinIntoString(", "));
    content_.setMultiLine(true, true);
    content_.setReadOnly(true);
    content_.onEscapeKey = [this] { closeCallOut(*this); };
    content_.setText(lines.joinIntoString("\n"), false);
    recall_.onClick = [this] {
      if (panel_ != nullptr) panel_->recallRegistration(index_);
      closeCallOut(*this);
    };
    store_.onClick = [this] {
      rename();
      if (panel_ != nullptr) panel_->storeRegistration(index_);
      closeCallOut(*this);
    };
    forget_.onClick = [this] {
      if (panel_ != nullptr) panel_->forgetRegistration(index_);
      closeCallOut(*this);
    };
    delete_.onClick = [this] {
      if (panel_ != nullptr) panel_->deleteMark(index_);
      index_ = -1;
      closeCallOut(*this);
    };
    store_.setTooltip(TRANS("Keep the organ's state now in this registration, for this organ"));
    forget_.setTooltip(TRANS("Take this organ's state out of the registration; other organs' stay"));
    for (auto* c : std::initializer_list<juce::Component*>{&name_, &content_, &recall_, &store_, &forget_, &delete_})
      addAndMakeVisible(c);
    setSize(440, 250);
  }
  ~MarkRegistrationEditor() override { rename(); }
  // The name ready to be typed over.
  void visibilityChanged() override {
    if (isShowing()) name_.grabKeyboardFocus();
  }
  void resized() override {
    auto r = getLocalBounds().reduced(6);
    name_.setBounds(r.removeFromTop(26));
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(28);
    r.removeFromBottom(6);
    content_.setBounds(r);
    const int w = (buttons.getWidth() - 18) / 4;
    for (auto* b : {&recall_, &store_, &forget_, &delete_}) {
      b->setBounds(buttons.removeFromLeft(w));
      buttons.removeFromLeft(6);
    }
  }

private:
  void rename() {
    if (panel_ == nullptr || index_ < 0 || index_ >= static_cast<int>(panel_->marks().size())) return;
    const auto text = name_.getText().trim();
    if (text.isNotEmpty() && text.toStdString() != panel_->marks()[static_cast<size_t>(index_)].text)
      panel_->setMarkText(index_, text);
  }

  juce::Component::SafePointer<ScorePanel> panel_;
  int index_;
  juce::TextEditor name_, content_;
  juce::TextButton recall_{TRANS("Set now")}, store_{TRANS("Store")}, forget_{TRANS("Forget organ")},
      delete_{TRANS("Delete")};
};

// A jump mark's name; or a combination's name and piston: a general or a
// step of the sequencer, by number (#1012).
class MarkPlaceEditor : public juce::Component {
public:
  MarkPlaceEditor(juce::Component::SafePointer<ScorePanel> panel, int index) : panel_(panel), index_(index) {
    const auto& mark = panel->marks()[static_cast<size_t>(index)];
    combination_ = mark.kind == ScoreMark::Kind::Combination;
    name_.setText(juce::String::fromUTF8(mark.text.c_str()), false);
    name_.setFont(juce::FontOptions(15.0f));
    name_.setSelectAllWhenFocused(true);
    name_.onReturnKey = [this] { closeCallOut(*this); };
    name_.onEscapeKey = [this] { closeCallOut(*this); };
    addAndMakeVisible(name_);
    if (combination_) {
      piston_.addItem(TRANS("General"), 1);
      piston_.addItem(TRANS("Sequencer step"), 2);
      const auto states = juce::String::fromUTF8(mark.states.c_str());
      piston_.setSelectedId(states.startsWith("step") ? 2 : 1, juce::dontSendNotification);
      number_.setInputRestrictions(4, "0123456789");
      number_.setText(juce::String(juce::jmax(1, states.fromFirstOccurrenceOf(" ", false, false).getIntValue())),
                      false);
      addAndMakeVisible(piston_);
      addAndMakeVisible(number_);
    }
    act_.setButtonText(combination_ ? TRANS("Set now") : TRANS("Go there"));
    act_.onClick = [this] {
      keep();
      if (panel_ != nullptr) {
        if (combination_) panel_->recallRegistration(index_);
        else panel_->goToMark(index_);
      }
      closeCallOut(*this);
    };
    delete_.onClick = [this] {
      if (panel_ != nullptr) panel_->deleteMark(index_);
      index_ = -1;
      closeCallOut(*this);
    };
    addAndMakeVisible(act_);
    addAndMakeVisible(delete_);
    setSize(340, combination_ ? 110 : 76);
  }
  ~MarkPlaceEditor() override { keep(); }
  void visibilityChanged() override {
    if (isShowing()) name_.grabKeyboardFocus();
  }
  void resized() override {
    auto r = getLocalBounds().reduced(6);
    name_.setBounds(r.removeFromTop(26));
    r.removeFromTop(6);
    if (combination_) {
      auto row = r.removeFromTop(26);
      piston_.setBounds(row.removeFromLeft(180));
      row.removeFromLeft(6);
      number_.setBounds(row.removeFromLeft(70));
      r.removeFromTop(6);
    }
    auto buttons = r.removeFromTop(28);
    act_.setBounds(buttons.removeFromLeft(110));
    delete_.setBounds(buttons.removeFromRight(90));
  }

private:
  void keep() {
    if (panel_ == nullptr || index_ < 0 || index_ >= static_cast<int>(panel_->marks().size())) return;
    const auto& mark = panel_->marks()[static_cast<size_t>(index_)];
    const auto text = name_.getText().trim();
    if (text.isNotEmpty() && text.toStdString() != mark.text) panel_->setMarkText(index_, text);
    if (combination_) {
      const std::string states = std::string(piston_.getSelectedId() == 2 ? "step " : "general ") +
                                 std::to_string(juce::jmax(1, number_.getText().getIntValue()));
      if (states != panel_->marks()[static_cast<size_t>(index_)].states) panel_->setMarkStates(index_, states);
    }
  }

  juce::Component::SafePointer<ScorePanel> panel_;
  int index_;
  bool combination_ = false;
  juce::TextEditor name_, number_;
  juce::ComboBox piston_;
  juce::TextButton act_, delete_{TRANS("Delete")};
};
}  // namespace

ScorePanel::ScorePanel(MasterpieceProcessor& proc, const juce::File& fonts) : proc_(proc) {
  registerScoreFonts();
  fonts_ =fonts != juce::File() ? fonts : scoreResourceFolder();
  if (fonts_ != juce::File())
    engraver_ = std::make_unique<mp::ScoreEngraver>(fonts_.getFullPathName().toStdString());
  if (engraver_ == nullptr || !engraver_->ready()) {
    message_ = TRANS("The score fonts could not be prepared.");
    engraver_.reset();
  } else {
    message_ = TRANS("Open a MusicXML score (.musicxml, .xml or .mxl).");
  }
  for (auto* b : {&open_, &prev_, &next_, &zoomOut_, &zoomIn_, &zoomReset_, &play_, &manuals_,
                  &mark_, &regPrev_, &regNext_, &view_, &console_})
    addAndMakeVisible(b);
  view_.setTooltip(TRANS("Pages or one line, the paper and the staff lines"));
  console_.setTooltip(TRANS("Learn a console's buttons for Play, a measure back or on, the jump marks and the pages. "
                            "On the computer: Space plays and stops, the arrows go a measure back or on, "
                            "up and down go to the jump marks, Page Up and Page Down turn the pages."));
  view_.onClick = [this] { chooseView(); };
  console_.onClick = [this] { chooseConsoleButtons(); };
  addChildComponent(detach_);
  addAndMakeVisible(tempo_);
  addAndMakeVisible(pageLabel_);
  pageLabel_.setJustificationType(juce::Justification::centred);
  pageLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  addAndMakeVisible(regLabel_);
  regLabel_.setJustificationType(juce::Justification::centred);
  regLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  regPrev_.setTooltip(TRANS("The registration before"));
  regNext_.setTooltip(TRANS("The next registration"));
  mark_.setTooltip(TRANS("Add a note or a registration on the note the cursor marks (or right-click a note)"));
  setWantsKeyboardFocus(true);
  for (auto* b : {&twoUp_, &invert_}) {
    b->setClickingTogglesState(true);
    b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff4a5a78));  // as the panels' section buttons
  }

  open_.onClick = [this] { chooseFile(); };
  prev_.onClick = [this] { line_ ? scrollLine(-1.0f) : showPage(page_ - slotCount()); };
  next_.onClick = [this] { line_ ? scrollLine(1.0f) : showPage(page_ + slotCount()); };
  zoomOut_.onClick = [this] { setZoom(std::min(zoom_, drawnZoom_) - 10); };
  zoomIn_.onClick = [this] { setZoom(zoom_ + 10); };
  zoomReset_.onClick = [this] { setZoom(100); };
  twoUp_.onClick = [this] { setTwoPages(twoUp_.getToggleState()); };
  invert_.onClick = [this] { setInverted(invert_.getToggleState()); };
  play_.onClick = [this] { playing() ? stop() : play(); };
  tempo_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 70, kBar - 8);
  tempo_.setIncDecButtonsMode(juce::Slider::incDecButtonsNotDraggable);
  tempo_.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v)) + " BPM"; };
  tempo_.valueFromTextFunction = [](const juce::String& t) { return t.retainCharacters("0123456789.").getDoubleValue(); };
  tempo_.setTooltip(TRANS("The tempo, in beats a minute: the score's own to start with, and its tempo changes "
                          "followed. Type or step another; a double-click returns to the score's."));
  tempo_.onValueChange = [this] {
    // The score's own tempo, shown whole, is exactly the score's.
    const double v = tempo_.getValue();
    proc_.scorePlayer().setSpeed(v == std::round(scoreBpm_) ? 1.0 : v / scoreBpm_);
    settingsChanged();
  };
  showTempo();
  manuals_.onClick = [this] { chooseManuals(); };
  detach_.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
  regPrev_.onClick = [this] { stepRegistration(-1); };
  regNext_.onClick = [this] { stepRegistration(1); };
  mark_.onClick = [this] {
    // On the note the cursor marks: the one clicked last, or the first on
    // the page.
    std::string note = cursorNotes_.empty() ? std::string() : cursorNotes_.front();
    if (note.empty()) {
      double best = std::numeric_limits<double>::max();
      for (const auto& [id, n] : shown_[0].geometry.notes) {
        const auto it = noteStartMs_.find(id);
        if (it != noteStartMs_.end() && it->second < best) {
          best = it->second;
          note = id;
        }
      }
    }
    if (!note.empty()) addMarkMenu(note, mark_.getScreenBounds());
  };
  refreshButtons();
  setSize(900, 700);
}

ScorePanel::~ScorePanel() {
  stopTimer();
  if (marksDirty_) saveMarks();
  if (playing()) proc_.scorePlayer().stop();
}

int ScorePanel::pageCount() const { return loaded_ && engraver_ != nullptr ? engraver_->pageCount() : 0; }

bool ScorePanel::playing() const { return loaded_ && proc_.scorePlayer().isPlaying(); }

void ScorePanel::setDetachable(bool can) {
  detachable_ = can && !kMobile;
  detach_.setVisible(detachable_);
  resized();
}

void ScorePanel::setDetached(bool detached) {
  detached_ = detached;
  detach_.setButtonText(detached ? TRANS("Attach") : TRANS("Detach"));
}

namespace {
// The score window's own preference, the program's and not a score's: a
// line in a small file of the app-data folder.
juce::File scoreWindowFile() { return mp::dataFolder().getChildFile("score-window.txt"); }
}  // namespace

bool ScorePanel::keptOnTop() const {
  return scoreWindowFile().loadFileAsString().contains("ontop 1");
}

void ScorePanel::setKeptOnTop(bool top) {
  scoreWindowFile().replaceWithText(top ? "ontop 1\n" : "ontop 0\n");
  if (onKeepOnTop) onKeepOnTop(top);
}

// --- opening -----------------------------------------------------------

void ScorePanel::chooseFile() {
  chooser_ = std::make_unique<juce::FileChooser>(TRANS("Open score..."), juce::File(), "*.musicxml;*.xml;*.mxl");
  chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                        [safe = juce::Component::SafePointer<ScorePanel>(this)](const juce::FileChooser& fc) {
                          if (safe == nullptr) return;
                          const auto file = fc.getResult();
                          if (file != juce::File()) safe->openFile(file);
                        });
}

bool ScorePanel::openFile(const juce::File& file) {
  juce::MemoryBlock bytes;
  if (!file.loadFileAsData(bytes)) {
    stop();
    loaded_ = false;
    message_ = TRANS("The file could not be read.");
    refreshButtons();
    repaint();
    return false;
  }
  const bool ok = openData(bytes, file.getFileName());
  if (ok) file_ = file;
  return ok;
}

namespace {
// A compressed MusicXML file is a zip whose META-INF/container.xml names the
// score; without one, the first .musicxml or .xml outside META-INF.
std::string scoreFromZip(const juce::MemoryBlock& bytes, juce::String* entry = nullptr) {
  juce::MemoryInputStream in(bytes, false);
  juce::ZipFile zip(in);
  auto read = [&zip](int index) {
    std::unique_ptr<juce::InputStream> s(zip.createStreamForEntry(index));
    return s != nullptr ? s->readEntireStreamAsString().toStdString() : std::string();
  };
  juce::String path;
  const int container = zip.getIndexOfFileName("META-INF/container.xml");
  if (container >= 0)
    if (const auto xml = juce::XmlDocument::parse(juce::String::fromUTF8(read(container).c_str())))
      if (auto* files = xml->getChildByName("rootfiles"))
        if (auto* root = files->getChildByName("rootfile")) path = root->getStringAttribute("full-path");
  int index = path.isNotEmpty() ? zip.getIndexOfFileName(path) : -1;
  for (int i = 0; index < 0 && i < zip.getNumEntries(); ++i) {
    const auto name = zip.getEntry(i)->filename;
    if (!name.startsWithIgnoreCase("META-INF") && (name.endsWithIgnoreCase(".musicxml") || name.endsWithIgnoreCase(".xml")))
      index = i;
  }
  if (entry != nullptr && index >= 0) *entry = zip.getEntry(index)->filename;
  return index >= 0 ? read(index) : std::string();
}
}  // namespace

bool ScorePanel::openData(const juce::MemoryBlock& bytes, const juce::String& name) {
  stop();
  // The marks of the score open before go into its file first.
  if (marksDirty_) saveMarks();
  file_ = juce::File();
  undo_.clear();
  currentReg_ = -1;
  lastPlayMs_ = -1;
  loaded_ = false;
  for (auto& s : shown_) s = {};
  notes_.clear();
  staves_.clear();
  noteStartMs_.clear();
  noteTimes_.clear();
  measureMs_.clear();
  muted_.clear();
  keyboards_.clear();
  savedCodes_.clear();
  cursorNotes_.clear();
  startMs_ = 0;
  scrollX_ = 0;
  page_ = 1;
  if (engraver_ == nullptr) {
    refreshButtons();
    repaint();
    return false;
  }
  const bool zipped = bytes.getSize() > 4 && static_cast<const char*>(bytes.getData())[0] == 'P' &&
                      static_cast<const char*>(bytes.getData())[1] == 'K';
  zipped_ = zipped;
  const std::string text = zipped ? scoreFromZip(bytes)
                                  : std::string(static_cast<const char*>(bytes.getData()), bytes.getSize());
  marks_.load(text);
  if (text.empty()) {
    message_ = TRANS("No score was found in this file.");
  } else {
    // Without the marks, and with every note named, so a mark finds its note
    // on the page. Played with its repeats unrolled and its ornaments.
    playback_.load(marks_.engraving());
    loaded_ = engraver_->load(marks_.engraving(), playback_.unrolled());
    message_ = loaded_ ? name : juce::String::fromUTF8(engraver_->error().c_str());
  }
  if (loaded_) {
    readTimeline();
    // The score's own settings: its tempo, manuals, mutes and view.
    applySettings(marks_.settings);
    engrave();
    startTimer(33);
  }
  refreshButtons();
  repaint();
  return loaded_;
}

// The notes by staff from the MIDI, and when each element starts from the
// time map. Both come from the same rendering of the score's tempo.
void ScorePanel::readTimeline() {
  juce::MemoryOutputStream decoded;
  if (juce::Base64::convertFromBase64(decoded, juce::String(engraver_->midiBase64()))) {
    juce::MemoryInputStream in(decoded.getData(), decoded.getDataSize(), false);
    juce::MidiFile file;
    if (file.readFrom(in)) {
      // The score's tempo: its first tempo mark, or the engraver's 120.
      scoreBpm_ = 120.0;
      bool found = false;
      for (int t = 0; t < file.getNumTracks() && !found; ++t)
        for (const auto* ev : *file.getTrack(t))
          if (ev->message.isTempoMetaEvent() && ev->message.getTempoSecondsPerQuarterNote() > 0.0) {
            scoreBpm_ = 60.0 / ev->message.getTempoSecondsPerQuarterNote();
            found = true;
            break;
          }
      // A new score starts at its own tempo.
      proc_.scorePlayer().setSpeed(1.0);
      showTempo();
      file.convertTimestampTicksToSeconds();
      for (int t = 1; t < file.getNumTracks(); ++t) {
        juce::MidiMessageSequence seq(*file.getTrack(t));
        seq.updateMatchedPairs();
        juce::String trackName;
        bool any = false;
        for (int i = 0; i < seq.getNumEvents(); ++i) {
          const auto* ev = seq.getEventPointer(i);
          if (ev->message.isTrackNameEvent() && trackName.isEmpty())
            trackName = ev->message.getTextFromTextMetaEvent();
          if (!ev->message.isNoteOn()) continue;
          Note n;
          n.on = ev->message.getTimeStamp();
          n.off = ev->noteOffObject != nullptr ? ev->noteOffObject->message.getTimeStamp() : n.on;
          n.staff = t;
          n.pitch = ev->message.getNoteNumber();
          notes_.push_back(n);
          any = true;
        }
        if (any) staves_.push_back({t, TRANS("Staff") + " " + juce::String(t) +
                                           (trackName.isNotEmpty() ? " (" + trackName + ")" : juce::String())});
      }
    }
  }
  const auto map = juce::JSON::parse(juce::String::fromUTF8(engraver_->timemapJson().c_str()));
  if (const auto* entries = map.getArray())
    for (const auto& entry : *entries) {
      const double ms = static_cast<double>(entry["tstamp"]);
      if (entry.hasProperty("measureOn") && (measureMs_.empty() || ms > measureMs_.back() + 0.5))
        measureMs_.push_back(ms);
      if (const auto* on = entry["on"].getArray())
        for (const auto& id : *on) {
          const auto key = id.toString().toStdString();
          noteStartMs_.emplace(key, ms);
          noteTimes_[key].push_back(ms);
        }
    }
  // The ornamented notes among the MIDI's, by their time and pitch, each time
  // a repeat plays them.
  ornamented_.clear();
  for (const auto& [id, o] : playback_.ornaments()) {
    const auto times = noteTimes_.find(id);
    if (times == noteTimes_.end()) continue;
    for (const double ms : times->second)
      for (size_t k = 0; k < notes_.size(); ++k)
        if (notes_[k].pitch == o.pitch && std::abs(notes_[k].on * 1000.0 - ms) < 3.0) {
          ornamented_.emplace(k, o);
          break;
        }
  }
}

// --- layout and engraving ------------------------------------------------

juce::Rectangle<int> ScorePanel::pageArea() const {
  return getLocalBounds().withTrimmedTop(kBar + kMargin).reduced(kMargin);
}

juce::Rectangle<int> ScorePanel::slotArea(int slot) const {
  auto area = pageArea();
  if (slotCount() == 1) return area;
  const int w = (area.getWidth() - kGap) / 2;
  return slot == 0 ? area.withWidth(w) : area.withTrimmedLeft(area.getWidth() - w);
}

void ScorePanel::resized() {
  auto bar = getLocalBounds().removeFromTop(kBar).reduced(4, 3);
  open_.setBounds(bar.removeFromLeft(110));
  bar.removeFromLeft(8);
  prev_.setBounds(bar.removeFromLeft(30));
  pageLabel_.setBounds(bar.removeFromLeft(70));
  next_.setBounds(bar.removeFromLeft(30));
  bar.removeFromLeft(8);
  zoomOut_.setBounds(bar.removeFromLeft(28));
  zoomReset_.setBounds(bar.removeFromLeft(52));
  zoomIn_.setBounds(bar.removeFromLeft(28));
  bar.removeFromLeft(8);
  play_.setBounds(bar.removeFromLeft(64));
  tempo_.setBounds(bar.removeFromLeft(108));
  bar.removeFromLeft(4);
  manuals_.setBounds(bar.removeFromLeft(90));
  bar.removeFromLeft(8);
  mark_.setBounds(bar.removeFromLeft(70));
  bar.removeFromLeft(8);
  regPrev_.setBounds(bar.removeFromLeft(28));
  regLabel_.setBounds(bar.removeFromLeft(80));
  regNext_.setBounds(bar.removeFromLeft(28));
  if (detach_.isVisible()) detach_.setBounds(bar.removeFromRight(76));
  view_.setBounds(bar.removeFromRight(70));
  bar.removeFromRight(4);
  console_.setBounds(bar.removeFromRight(84));
  if (loaded_) requestEngrave();
}

void ScorePanel::requestEngrave() {
  needsEngrave_ = true;
  engraveDue_ = juce::Time::getMillisecondCounter() + kEngraveDelayMs;
  startTimer(33);
}

// The lines break again to the page's new width; the page shown is then the
// one holding the notes the cursor marks, so it stays in sight.
void ScorePanel::engrave() {
  needsEngrave_ = false;
  if (!loaded_ || engraver_ == nullptr) return;
  const auto area = slotArea(0);
  if (area.getWidth() < 50 || area.getHeight() < 50) return;
  // A page holds at least the staff names, the clefs and one measure, about
  // 700 Verovio units across: on a narrow page the zoom stops there.
  int scale = std::min(kScale * zoom_ / 100, std::max(10, area.getWidth() * 100 / 700));
  engraver_->setPage(area.getWidth(), area.getHeight(), scale, line_);
  // And one whole system from top to bottom: a taller one would run off the
  // page, so the scale steps down until the first page's systems fit. On one
  // line, the page is as high as its system, which must fit the panel.
  for (int tries = 0; tries < 4 && scale > 10; ++tries) {
    ScorePageGeometry first;
    parseScorePage(engraver_->renderPage(1), first);
    float bottom = 0;
    for (const auto& sys : first.systems) bottom = std::max(bottom, sys.area.getBottom() + sys.lineGap * 2.0f);
    const float room = line_ ? static_cast<float>(area.getHeight()) : first.height;
    const float need = line_ ? first.height : bottom;
    if (first.height <= 0 || need <= room) break;
    scale = std::max(10, static_cast<int>(scale * room / need * 0.97f));
    engraver_->setPage(area.getWidth(), area.getHeight(), scale, line_);
  }
  drawnZoom_ = (scale * 100 + kScale / 2) / kScale;
  // The cursor's notes sound at the start, or at the playing position while
  // it plays; the one that began last is nearest that point. Inside a page's
  // last held notes, the next note to sound is on the next page, and showing
  // it would leave the cursor out of sight. With the cursor on a rest, the
  // next note to sound.
  std::string at;
  double latest = -1;
  for (const auto& id : cursorNotes_) {
    const auto it = noteStartMs_.find(id);
    if (it != noteStartMs_.end() && it->second > latest) {
      latest = it->second;
      at = id;
    }
  }
  if (at.empty()) {
    double best = std::numeric_limits<double>::max();
    for (const auto& [id, ms] : noteStartMs_)
      if (ms >= startMs_ - 1 && ms < best) {
        best = ms;
        at = id;
      }
  }
  const int p = at.empty() || line_ ? 1 : engraver_->pageWithElement(at);
  page_ = std::clamp(p > 0 ? p : page_, 1, std::max(1, pageCount()));
  if (slotCount() == 2 && page_ > 1 && page_ == pageCount() && pageCount() % 2 == 0) --page_;
  renderShown();
  if (line_) {
    scrollX_ = 0;
    keepInView();
  }
}

void ScorePanel::renderShown() {
  for (int slot = 0; slot < 2; ++slot) {
    auto& s = shown_[slot];
    const int p = page_ + slot;
    if (slot >= slotCount() || p > pageCount()) {
      s = {};
      continue;
    }
    s.page = p;
    s.drawable = parseScorePage(engraver_->renderPage(p), s.geometry,
                                blueLines_ ? juce::Colour(0xff2f5fa8) : juce::Colour());
    s.image = {};
  }
  refreshButtons();
  repaint();
}

void ScorePanel::showPage(int page) {
  if (!loaded_) return;
  page = std::clamp(page, 1, std::max(1, pageCount()));
  if (page == page_ && shown_[0].page == page) return;
  const bool forward = page > page_;
  page_ = page;
  renderShown();
  // Turned by hand while stopped: playing starts from the top of the page.
  if (!playing()) {
    const double ms = firstNoteMsOnPage(0);
    if (ms >= 0) setStart(ms / 1000.0);
  }
  if (forward) pageTurnedTo(page);
}

// A page turned forward by hand sets its first registration, when that comes
// after the one set last: looking back a page leaves the organ as it is.
void ScorePanel::pageTurnedTo(int page) {
  juce::ignoreUnused(page);
  const double currentMs = currentReg_ >= 0 ? markMs(currentReg_) : -1.0;
  for (const int i : registrationOrder()) {
    const auto& note = marks_.marks[static_cast<size_t>(i)].note;
    bool shown = false;
    for (int slot = 0; slot < slotCount() && !shown; ++slot) shown = shown_[slot].geometry.notes.count(note) != 0;
    if (!shown) continue;
    if (markMs(i) > currentMs) recallRegistration(i);
    return;
  }
}

double ScorePanel::firstNoteMsOnPage(int slot) const {
  double best = -1;
  for (const auto& [id, n] : shown_[slot].geometry.notes) {
    const auto it = noteStartMs_.find(id);
    if (it != noteStartMs_.end() && (best < 0 || it->second < best)) best = it->second;
  }
  return best;
}

const ScorePageGeometry* ScorePanel::geometryOf(int page) const {
  for (const auto& s : shown_)
    if (s.page == page && s.drawable != nullptr) return &s.geometry;
  return nullptr;
}

void ScorePanel::setTwoPages(bool two) {
  twoPages_ = two;
  twoUp_.setToggleState(two, juce::dontSendNotification);
  if (loaded_) engrave();
  refreshButtons();
  settingsChanged();
}

void ScorePanel::setZoom(int percent) {
  const int z = std::clamp(percent, 50, 250);
  if (z == zoom_) return;
  zoom_ = z;
  if (loaded_) engrave();
  refreshButtons();
  settingsChanged();
}

void ScorePanel::setInverted(bool inverted) {
  inverted_ = inverted;
  invert_.setToggleState(inverted, juce::dontSendNotification);
  for (auto& s : shown_) s.image = {};
  repaint();
  settingsChanged();
}

// --- one line, the paper ---------------------------------------------------

void ScorePanel::setLineView(bool line) {
  if (line_ == line) return;
  line_ = line;
  scrollX_ = 0;
  if (loaded_) engrave();
  refreshButtons();
  settingsChanged();
}

void ScorePanel::setPaper(Paper paper) {
  if (paper_ == paper) return;
  paper_ = paper;
  for (auto& s : shown_) s.image = {};
  repaint();
  settingsChanged();
}

void ScorePanel::setBlueLines(bool blue) {
  if (blueLines_ == blue) return;
  blueLines_ = blue;
  if (loaded_) renderShown();
  settingsChanged();
}

// On one line, the page's pixels to the panel's: the line's height fitted
// into the panel's, never larger than engraved.
float ScorePanel::lineScale() const {
  const auto& geo = shown_[0].geometry;
  const auto area = slotArea(0);
  if (geo.height <= 0) return 1.0f;
  return std::min(1.0f, static_cast<float>(area.getHeight()) / geo.height);
}

// A screen's width on, or back (most of one, so a bar stays in sight).
void ScorePanel::scrollLine(float screens) {
  const auto& geo = shown_[0].geometry;
  const float visible = static_cast<float>(slotArea(0).getWidth()) / lineScale();
  const float most = std::max(0.0f, geo.width - visible);
  scrollX_ = std::clamp(scrollX_ + screens * visible * 0.85f, 0.0f, most);
  if (!playing()) {
    // Stopped: the start moves to the first note in sight, as a page turned does.
    double best = -1;
    for (const auto& [id, n] : geo.notes) {
      if (n.head.x < scrollX_ || n.head.x > scrollX_ + visible) continue;
      const auto it = noteStartMs_.find(id);
      if (it != noteStartMs_.end() && (best < 0 || it->second < best)) best = it->second;
    }
    if (best >= 0) {
      const float keep = scrollX_;
      setStart(best / 1000.0);
      scrollX_ = keep;
    }
  }
  refreshButtons();
  repaint();
}

// On one line, the cursor kept in sight: when it nears the right edge, or is
// left of the view, the line moves so it stands a little in from the left.
void ScorePanel::keepInView() {
  if (!line_ || shown_[0].drawable == nullptr) return;
  const auto& geo = shown_[0].geometry;
  float x = -1;
  for (const auto& id : cursorNotes_)
    if (const auto it = geo.notes.find(id); it != geo.notes.end()) x = std::max(x, it->second.head.x);
  if (x < 0) return;
  const float visible = static_cast<float>(slotArea(0).getWidth()) / lineScale();
  if (x >= scrollX_ + visible * 0.05f && x <= scrollX_ + visible * 0.8f) return;
  const float most = std::max(0.0f, geo.width - visible);
  scrollX_ = std::clamp(x - visible * 0.15f, 0.0f, most);
  refreshButtons();
  repaint();
}

void ScorePanel::chooseView() {
  juce::PopupMenu menu;
  menu.addItem(1, TRANS("Pages"), true, !line_);
  menu.addItem(2, TRANS("One line, scrolling with the playing"), true, line_);
  menu.addItem(7, TRANS("Two pages"), !line_, twoPages_);
  menu.addSeparator();
  menu.addItem(3, TRANS("White paper"), true, paper_ == Paper::White);
  menu.addItem(4, TRANS("Cream paper"), true, paper_ == Paper::Cream);
  menu.addItem(5, TRANS("Blue staff lines"), true, blueLines_);
  menu.addItem(8, TRANS("Invert"), true, inverted_);
  if (detached_) {
    menu.addSeparator();
    menu.addItem(6, TRANS("Keep this window on top"), true, keptOnTop());
  }
  showMenu(menu, juce::PopupMenu::Options().withTargetComponent(&view_),
           [safe = juce::Component::SafePointer<ScorePanel>(this)](int chosen) {
             if (safe == nullptr) return;
             switch (chosen) {
               case 1: safe->setLineView(false); break;
               case 2: safe->setLineView(true); break;
               case 3: safe->setPaper(Paper::White); break;
               case 4: safe->setPaper(Paper::Cream); break;
               case 5: safe->setBlueLines(!safe->blueLines()); break;
               case 6: safe->setKeptOnTop(!safe->keptOnTop()); break;
               case 7: safe->setTwoPages(!safe->twoPages_); break;
               case 8: safe->setInverted(!safe->inverted()); break;
               default: break;
             }
           });
}

// A console's buttons for the score, learned as any other: the menu names
// what each is mapped to; choosing one learns it, or clears it.
void ScorePanel::chooseConsoleButtons() {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    proc_.cancelLearn();
    return;
  }
  struct Item {
    ScoreCommandId id;
    juce::String name;
  };
  const std::vector<Item> items = {
      {ScoreCommandId::PlayStop, TRANS("Play / Stop")},
      {ScoreCommandId::Back, TRANS("A measure back")},
      {ScoreCommandId::Forward, TRANS("A measure on")},
      {ScoreCommandId::JumpPrevious, TRANS("The jump mark before")},
      {ScoreCommandId::JumpNext, TRANS("The next jump mark")},
      {ScoreCommandId::PageBack, TRANS("Page back")},
      {ScoreCommandId::PageForward, TRANS("Page forward")},
  };
  juce::PopupMenu menu;
  menu.addSectionHeader(TRANS("Learn a console button for"));
  for (const auto& item : items) {
    const auto id = static_cast<int>(item.id);
    // The page turns are the program's page buttons, which also turn a score
    // app's page.
    const auto kind = item.id == ScoreCommandId::PageBack      ? MidiTargetKind::ScorePageBack
                      : item.id == ScoreCommandId::PageForward ? MidiTargetKind::ScorePageForward
                                                               : MidiTargetKind::ScoreCommand;
    const Id target = kind == MidiTargetKind::ScoreCommand ? static_cast<Id>(id) : 0;
    const bool mapped = !map.bindingsFor(kind, target).empty();
    juce::PopupMenu sub;
    sub.addItem(id * 10 + 1, TRANS("Learn"));
    sub.addItem(id * 10 + 2, TRANS("Clear"), mapped);
    menu.addSubMenu(item.name, sub, true, nullptr, mapped);
  }
  showMenu(menu, juce::PopupMenu::Options().withTargetComponent(&console_),
           [safe = juce::Component::SafePointer<ScorePanel>(this)](int chosen) {
             if (safe == nullptr || chosen < 10) return;
             const int id = chosen / 10;
             const auto kind = id == static_cast<int>(ScoreCommandId::PageBack)      ? MidiTargetKind::ScorePageBack
                               : id == static_cast<int>(ScoreCommandId::PageForward) ? MidiTargetKind::ScorePageForward
                                                                                    : MidiTargetKind::ScoreCommand;
             const Id target = kind == MidiTargetKind::ScoreCommand ? static_cast<Id>(id) : 0;
             if (chosen % 10 == 1) safe->proc_.beginLearn(kind, target, false);
             else safe->proc_.setObjectBindings(kind, target, {});
           });
}

// --- the score's settings ----------------------------------------------------

std::string ScorePanel::settingsText() const {
  std::ostringstream out;
  const double bpm = tempoBpm();
  if (std::abs(bpm - std::round(scoreBpm_)) >= 0.5) out << "tempo " << juce::roundToInt(bpm) << "\n";
  for (const auto& s : staves_) {
    const auto it = keyboards_.find(s.number);
    const bool mute = muted(s.number);
    if (it == keyboards_.end() && !mute) continue;
    // The manual by its place on a console: the same on another organ.
    const int code = it == keyboards_.end() ? -1 : it->second == 0 ? 0 : proc_.assignmentCodeOf(it->second);
    out << "staff " << s.number << " " << code << " " << (mute ? 1 : 0) << "\n";
  }
  if (line_) out << "view line\n";
  if (twoPages_) out << "pages two\n";
  if (zoom_ != 100) out << "zoom " << zoom_ << "\n";
  if (inverted_) out << "inverted\n";
  if (paper_ == Paper::Cream) out << "paper cream\n";
  if (blueLines_) out << "lines blue\n";
  return out.str();
}

void ScorePanel::applySettings(const std::string& text) {
  const juce::ScopedValueSetter<bool> applying(applyingSettings_, true);
  std::istringstream in(text);
  std::string line;
  bool line2 = false, two = false, inverted = false, blue = false;
  int zoom = 100;
  Paper paper = Paper::White;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key;
    words >> key;
    if (key == "tempo") {
      double bpm = 0;
      if (words >> bpm && bpm > 0) setTempoBpm(bpm);
    } else if (key == "staff") {
      int staff = 0, code = -1, mute = 0;
      if (!(words >> staff >> code)) continue;
      words >> mute;
      if (code == 0) keyboards_[staff] = 0;
      else if (code > 0) savedCodes_[staff] = code;
      if (mute != 0) muted_.insert(staff);
    } else if (key == "view") {
      std::string v;
      words >> v;
      line2 = v == "line";
    } else if (key == "pages") {
      std::string v;
      words >> v;
      two = v == "two";
    } else if (key == "zoom") {
      words >> zoom;
    } else if (key == "inverted") {
      inverted = true;
    } else if (key == "paper") {
      std::string v;
      words >> v;
      if (v == "cream") paper = Paper::Cream;
    } else if (key == "lines") {
      std::string v;
      words >> v;
      blue = v == "blue";
    }
  }
  // Each staff's manual, by its place, on this organ's keyboard there.
  for (const auto& [staff, code] : savedCodes_)
    for (const Id kb : proc_.playableKeyboards())
      if (proc_.assignmentCodeOf(kb) == code) {
        keyboards_[staff] = kb;
        break;
      }
  line_ = line2;
  twoPages_ = two;
  twoUp_.setToggleState(two, juce::dontSendNotification);
  zoom_ = std::clamp(zoom, 50, 250);
  inverted_ = inverted;
  invert_.setToggleState(inverted, juce::dontSendNotification);
  paper_ = paper;
  blueLines_ = blue;
  for (auto& s : shown_) s.image = {};
  refreshButtons();
}

// A setting changed: written into the score's file with the marks, a
// moment later.
void ScorePanel::settingsChanged() {
  if (!loaded_ || applyingSettings_) return;
  marksChanged();
}

void ScorePanel::setMuted(int staff, bool mute) {
  if (mute) muted_.insert(staff);
  else muted_.erase(staff);
  if (playing()) restart();
  settingsChanged();
}

double ScorePanel::tempoBpm() const { return tempo_.getValue(); }

void ScorePanel::setTempoBpm(double bpm) {
  tempo_.setValue(std::round(bpm), juce::sendNotificationSync);
}

// --- moving about -------------------------------------------------------------

// Playing again from where it is, with what changed (a manual, a mute).
void ScorePanel::restart() {
  startMs_ = proc_.scorePlayer().positionSeconds() * 1000.0;
  play();
}

// The start, and the playing if it plays, to a moment of the score.
void ScorePanel::moveTo(double ms) {
  ms = std::max(0.0, ms);
  if (playing()) {
    startMs_ = ms;
    play();
    return;
  }
  setStart(ms / 1000.0);
  // Its page in sight.
  if (!line_ && !cursorNotes_.empty()) {
    const int p = engraver_->pageWithElement(cursorNotes_.front());
    if (p >= 1 && (p < page_ || p >= page_ + slotCount())) {
      page_ = std::clamp(p, 1, std::max(1, pageCount()));
      renderShown();
    }
  }
}

bool ScorePanel::stepMeasure(int step) {
  if (!loaded_ || measureMs_.empty() || step == 0) return false;
  const double now = playing() ? proc_.scorePlayer().positionSeconds() * 1000.0 : startMs_;
  // The measure the point is in; a step back from a little way into it goes
  // to its own start, as a player's "back" does.
  auto it = std::upper_bound(measureMs_.begin(), measureMs_.end(), now + 1.0);
  int at = static_cast<int>(it - measureMs_.begin()) - 1;
  if (at < 0) at = 0;
  int to = at + step;
  if (step < 0 && now - measureMs_[static_cast<size_t>(at)] > 250.0) to = at + step + 1;
  if (to < 0 || to >= static_cast<int>(measureMs_.size())) return false;
  moveTo(measureMs_[static_cast<size_t>(to)]);
  return true;
}

std::vector<int> ScorePanel::jumpOrder() const {
  std::vector<int> out;
  for (size_t i = 0; i < marks_.marks.size(); ++i)
    if (marks_.marks[i].kind == ScoreMark::Kind::Jump) out.push_back(static_cast<int>(i));
  std::stable_sort(out.begin(), out.end(), [this](int a, int b) { return markMs(a) < markMs(b); });
  return out;
}

bool ScorePanel::stepJump(int step) {
  if (!loaded_ || step == 0) return false;
  const double now = playing() ? proc_.scorePlayer().positionSeconds() * 1000.0 : startMs_;
  const auto order = jumpOrder();
  int target = -1;
  if (step > 0) {
    for (const int i : order)
      if (markMs(i) > now + 1.0) {
        target = i;
        break;
      }
  } else {
    // While playing, a step back from just past a mark goes to the one before.
    const double from = playing() ? now - 1500.0 : now - 1.0;
    for (const int i : order)
      if (markMs(i) < from) target = i;
  }
  if (target < 0) return false;
  goToMark(target);
  return true;
}

void ScorePanel::goToMark(int index) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  const double ms = markMs(index);
  if (ms < std::numeric_limits<double>::max()) moveTo(ms);
}

void ScorePanel::setMarkStates(int index, const std::string& states) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  remember();
  marks_.marks[static_cast<size_t>(index)].states = states;
  marksChanged();
}

// The console's buttons, raised on the audio thread, done here.
void ScorePanel::takeConsoleCommands() {
  for (int n = proc_.takeScoreCommand(ScoreCommandId::PlayStop); n > 0; --n) playing() ? stop() : play();
  for (int n = proc_.takeScoreCommand(ScoreCommandId::Back); n > 0; --n) stepMeasure(-1);
  for (int n = proc_.takeScoreCommand(ScoreCommandId::Forward); n > 0; --n) stepMeasure(1);
  for (int n = proc_.takeScoreCommand(ScoreCommandId::JumpPrevious); n > 0; --n) stepJump(-1);
  for (int n = proc_.takeScoreCommand(ScoreCommandId::JumpNext); n > 0; --n) stepJump(1);
  for (int n = proc_.takeScoreCommand(ScoreCommandId::PageBack); n > 0; --n)
    line_ ? scrollLine(-1.0f) : showPage(page_ - slotCount());
  for (int n = proc_.takeScoreCommand(ScoreCommandId::PageForward); n > 0; --n)
    line_ ? scrollLine(1.0f) : showPage(page_ + slotCount());
}

// The tempo box over a quarter of the score's tempo to four times it, at the
// tempo the player plays at now.
void ScorePanel::showTempo() {
  const double bpm = std::max(1.0, scoreBpm_);
  tempo_.setRange(std::max(1.0, std::round(bpm * ScorePlayer::kMinSpeed)), std::round(bpm * ScorePlayer::kMaxSpeed), 1.0);
  tempo_.setDoubleClickReturnValue(true, std::round(bpm));
  tempo_.setValue(std::round(bpm * proc_.scorePlayer().speed()), juce::dontSendNotification);
  tempo_.updateText();
}

void ScorePanel::refreshButtons() {
  const int count = pageCount();
  if (line_) {
    // One line: the arrows scroll it a screen at a time.
    const float visible = static_cast<float>(slotArea(0).getWidth()) / lineScale();
    prev_.setEnabled(loaded_ && scrollX_ > 0.5f);
    next_.setEnabled(loaded_ && scrollX_ + visible < shown_[0].geometry.width - 0.5f);
    pageLabel_.setText(loaded_ ? TRANS("Line") : juce::String(), juce::dontSendNotification);
  } else {
    prev_.setEnabled(loaded_ && page_ > 1);
    next_.setEnabled(loaded_ && page_ + slotCount() <= count);
    pageLabel_.setText(loaded_ ? juce::String(page_) + " / " + juce::String(count) : juce::String(),
                       juce::dontSendNotification);
  }
  for (auto* b : {&zoomOut_, &zoomIn_, &zoomReset_, &play_, &manuals_}) b->setEnabled(loaded_);
  twoUp_.setEnabled(loaded_ && !line_);
  tempo_.setEnabled(loaded_);
  zoomReset_.setButtonText(juce::String(loaded_ ? drawnZoom_ : zoom_) + "%");
  // Stopped by the page's size: a larger zoom would draw the same page.
  if (loaded_ && drawnZoom_ < zoom_) zoomIn_.setEnabled(false);
  play_.setButtonText(playing() ? TRANS("Stop") : TRANS("Play"));
  mark_.setEnabled(loaded_);
  const auto order = registrationOrder();
  const auto at = std::find(order.begin(), order.end(), currentReg_);
  regPrev_.setEnabled(loaded_ && at != order.end() && at != order.begin());
  regNext_.setEnabled(loaded_ && !order.empty() && (at == order.end() || at + 1 != order.end()));
  juce::String reg;
  if (at != order.end()) reg = juce::String::fromUTF8(marks_.marks[static_cast<size_t>(currentReg_)].text.c_str());
  else if (!order.empty()) reg = TRANS("Registrations") + " (" + juce::String(order.size()) + ")";
  regLabel_.setText(reg, juce::dontSendNotification);
}

// --- staves and manuals --------------------------------------------------

Id ScorePanel::keyboardForStaff(int staff) const {
  const auto& playable = proc_.playableKeyboards();
  const auto it = keyboards_.find(staff);
  if (it != keyboards_.end() && (it->second == 0 || std::find(playable.begin(), playable.end(), it->second) != playable.end()))
    return it->second;
  int last = 0;
  for (const auto& s : staves_) last = std::max(last, s.number);
  const auto isPedal = [this](Id kb) { return proc_.assignmentCodeOf(kb) == 1; };
  if (staves_.size() >= 3 && staff == last)
    for (const Id kb : playable)
      if (isPedal(kb)) return kb;
  // A hand's staff on a manual: the organ's preferred keyboard, unless that
  // is the pedal (it can be, on a set whose pedal is as wide as its manuals).
  const Id preferred = proc_.preferredKeyboard();
  if (preferred != 0 && !isPedal(preferred)) return preferred;
  for (const Id kb : playable)
    if (!isPedal(kb)) return kb;
  return preferred;
}

void ScorePanel::setKeyboardForStaff(int staff, Id keyboard) {
  keyboards_[staff] = keyboard;
  savedCodes_.erase(staff);
  if (playing()) restart();
  settingsChanged();
}

void ScorePanel::chooseManuals() {
  if (staves_.empty()) return;
  std::vector<std::pair<Id, juce::String>> keyboards;
  for (const Id kb : proc_.playableKeyboards())
    keyboards.emplace_back(kb, juce::String::fromUTF8(proc_.keyboardName(kb).c_str()));
  auto dialog = std::make_unique<ManualsDialog>(juce::Component::SafePointer<ScorePanel>(this), staves_, keyboards);
  juce::CallOutBox::launchAsynchronously(std::move(dialog), manuals_.getScreenBounds(), nullptr);
}

// --- playing -------------------------------------------------------------

std::vector<ScorePlayer::Event> ScorePanel::eventsForPlaying() const {
  std::map<int, int> channels;
  for (const auto& s : staves_) {
    const Id kb = keyboardForStaff(s.number);
    channels[s.number] = kb != 0 ? proc_.clickChannelForKeyboard(kb) : 0;
  }
  std::vector<ScorePlayer::Event> events;
  events.reserve(notes_.size() * 2);
  // An ornament's notes: thirty-seconds of the score's beat, between 50 and
  // 125 ms, fewer and faster on a short note.
  const double step = std::clamp(60.0 / std::max(1.0, scoreBpm_) / 8.0, 0.05, 0.125);
  for (size_t k = 0; k < notes_.size(); ++k) {
    const auto& n = notes_[k];
    if (muted(n.staff)) continue;
    const int ch = channels[n.staff];
    if (ch < 1 || ch > 16) continue;
    const auto c = static_cast<uint8_t>(ch);
    const double off = std::max(n.off, n.on);
    const auto note = [&](double from, double to, int pitch) {
      const auto p = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
      events.push_back({from, c, p, true});
      events.push_back({std::max(from, to), c, p, false});
    };
    const auto found = ornamented_.find(k);
    if (found == ornamented_.end()) {
      note(n.on, off, n.pitch);
      continue;
    }
    const auto& o = found->second;
    const double length = off - n.on;
    std::vector<int> figure;  // the ornament's notes before the main note held to the end
    switch (o.kind) {
      case ScoreOrnament::Kind::Trill: {
        // From the note above, as a baroque trill starts, to the end: the
        // main note last.
        const int count = std::max(2, static_cast<int>(length / step) & ~1);
        for (int i = 0; i < count - 1; ++i) figure.push_back(i % 2 == 0 ? n.pitch + o.upper : n.pitch);
        break;
      }
      case ScoreOrnament::Kind::Mordent: figure = {n.pitch, n.pitch + o.lower}; break;
      case ScoreOrnament::Kind::InvertedMordent: figure = {n.pitch, n.pitch + o.upper}; break;
      case ScoreOrnament::Kind::Turn: figure = {n.pitch + o.upper, n.pitch, n.pitch + o.lower}; break;
      case ScoreOrnament::Kind::InvertedTurn: figure = {n.pitch + o.lower, n.pitch, n.pitch + o.upper}; break;
    }
    const double each = std::min(step, length / static_cast<double>(figure.size() + 1));
    double t = n.on;
    for (const int pitch : figure) {
      note(t, t + each, pitch);
      t += each;
    }
    note(t, off, n.pitch);
  }
  ScorePlayer::orderEvents(events);
  return events;
}

void ScorePanel::play() {
  if (!loaded_) return;
  // The organ as the score has it where the playing starts: the last
  // registration at or before that point.
  int at = -1;
  for (const int i : registrationOrder())
    if (markMs(i) <= startMs_ + 1.0) at = i;
  if (at >= 0 && at != currentReg_) recallRegistration(at);
  lastPlayMs_ = startMs_;
  // A note the time map places at the start may sit a rounding below it in
  // the MIDI: start a moment earlier so it is played.
  proc_.scorePlayer().play(eventsForPlaying(), std::max(0.0, (startMs_ - 2.0) / 1000.0));
  wasPlaying_ = true;
  startTimer(33);
  refreshButtons();
}

void ScorePanel::stop() {
  if (proc_.scorePlayer().isPlaying()) {
    startMs_ = proc_.scorePlayer().positionSeconds() * 1000.0;
    proc_.scorePlayer().stop();
  }
  refreshButtons();
}

void ScorePanel::setStart(double seconds) {
  startMs_ = std::max(0.0, seconds * 1000.0);
  cursorNotes_.clear();
  if (loaded_) {
    const auto at = juce::JSON::parse(juce::String::fromUTF8(engraver_->elementsAtTime(static_cast<int>(startMs_ + 1)).c_str()));
    if (const auto* ids = at["notes"].getArray())
      for (const auto& id : *ids) cursorNotes_.push_back(id.toString().toStdString());
  }
  keepInView();
  repaint();
}

void ScorePanel::followPlaying() {
  auto& player = proc_.scorePlayer();
  if (player.isPlaying()) {
    const double nowMs = player.positionSeconds() * 1000.0;
    if (lastPlayMs_ >= 0 && nowMs > lastPlayMs_) registrationsReached(lastPlayMs_, nowMs);
    lastPlayMs_ = nowMs;
    const auto at = juce::JSON::parse(
        juce::String::fromUTF8(engraver_->elementsAtTime(static_cast<int>(nowMs)).c_str()));
    std::vector<std::string> ids;
    if (const auto* notes = at["notes"].getArray())
      for (const auto& id : *notes) ids.push_back(id.toString().toStdString());
    const int page = static_cast<int>(at["page"]);
    // The page turns as the playing reaches the next one.
    if (!line_ && page >= 1 && (page < page_ || page >= page_ + slotCount())) {
      page_ = std::clamp(page, 1, std::max(1, pageCount()));
      renderShown();
    }
    if (!ids.empty() && ids != cursorNotes_) {
      cursorNotes_ = std::move(ids);
      keepInView();
      repaint();
    }
    return;
  }
  if (wasPlaying_) {
    wasPlaying_ = false;
    lastPlayMs_ = -1;
    // Played to the end: the next Play starts from the top.
    if (player.positionSeconds() >= player.lengthSeconds() - 0.001) setStart(0);
    else setStart(player.positionSeconds());
    refreshButtons();
  }
}

void ScorePanel::timerCallback() {
  if (needsEngrave_ && juce::Time::getMillisecondCounter() >= engraveDue_) engrave();
  if (loaded_) {
    takeConsoleCommands();
    followPlaying();
  }
  if (marksDirty_ && juce::Time::getMillisecondCounter() >= saveDue_) saveMarks();
  // With a score open the console's buttons are listened for.
  if (!loaded_ && !needsEngrave_ && !wasPlaying_ && !marksDirty_) stopTimer();
}

// --- drawing -------------------------------------------------------------

juce::Point<float> ScorePanel::notePosition(const std::string& id) const {
  for (int slot = 0; slot < slotCount(); ++slot) {
    const auto& s = shown_[slot];
    if (s.drawable == nullptr) continue;
    const auto it = s.geometry.notes.find(id);
    if (it == s.geometry.notes.end() || it->second.system < 0) continue;
    // The head's point is the glyph's left end on the middle of its line or
    // space: its centre is most of a staff space to the right.
    float gap = 0;
    if (static_cast<size_t>(it->second.system) < s.geometry.systems.size())
      gap = s.geometry.systems[static_cast<size_t>(it->second.system)].lineGap;
    return it->second.head.translated(gap * 0.6f, 0).transformedBy(pageToPanel(slot));
  }
  return {-1, -1};
}

juce::AffineTransform ScorePanel::pageToPanel(int slot) const {
  const auto& geo = shown_[slot].geometry;
  if (geo.width <= 0 || geo.height <= 0) return {};
  if (line_) {
    // One line: as high as the panel allows, scrolled along, centred up and down.
    const auto area = slotArea(slot).toFloat();
    const float s = lineScale();
    return juce::AffineTransform::translation(-scrollX_, 0)
        .scaled(s)
        .translated(area.getX(), area.getY() + (area.getHeight() - geo.height * s) * 0.5f);
  }
  return juce::RectanglePlacement(juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize)
      .getTransformToFit({0, 0, geo.width, geo.height}, slotArea(slot).toFloat());
}

void ScorePanel::drawSlot(juce::Graphics& g, int slot) {
  auto& s = shown_[slot];
  if (s.drawable == nullptr) return;
  const float density = std::max(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
  auto bounds = juce::Rectangle<float>(0, 0, s.geometry.width, s.geometry.height).transformedBy(pageToPanel(slot));
  // On one line, only the part in sight is drawn.
  if (line_) bounds = bounds.getIntersection(slotArea(slot).toFloat());
  // On whole screen pixels: the image is then copied, not resampled, and the
  // notation keeps the edges it was drawn with.
  bounds.setPosition(std::round(bounds.getX() * density) / density, std::round(bounds.getY() * density) / density);
  const int w = juce::roundToInt(bounds.getWidth() * density), h = juce::roundToInt(bounds.getHeight() * density);
  if (w <= 0 || h <= 0) return;
  bounds.setSize(static_cast<float>(w) / density, static_cast<float>(h) / density);
  if (!s.image.isValid() || s.image.getWidth() != w || s.image.getHeight() != h || s.density != density ||
      (line_ && s.scroll != scrollX_)) {
    s.image = juce::Image(juce::Image::RGB, w, h, true);
    s.density = density;
    s.scroll = scrollX_;
    {
      juce::Graphics ig(s.image);
      // The paper: white, or cream, a little sepia (#1012).
      ig.fillAll(paper_ == Paper::Cream ? juce::Colour(0xfff6efdc) : juce::Colours::white);
      // The whole page onto the image, by the page's own size: drawWithin
      // would fit the ink, and the notes would leave the geometry's places.
      // On one line, from where it is scrolled to, at the line's scale.
      const auto toImage =
          line_ ? juce::AffineTransform::translation(-scrollX_, 0).scaled(lineScale() * density)
                : juce::AffineTransform::scale(static_cast<float>(w) / s.geometry.width,
                                               static_cast<float>(h) / s.geometry.height);
      s.drawable->draw(ig, 1.0f, toImage);
    }
    if (inverted_) {
      juce::Image::BitmapData data(s.image, juce::Image::BitmapData::readWrite);
      for (int y = 0; y < data.height; ++y) {
        auto* p = data.getLinePointer(y);
        for (int x = 0; x < data.width; ++x, p += data.pixelStride)
          for (int c = 0; c < 3; ++c) p[c] = static_cast<juce::uint8>(255 - p[c]);
      }
    }
  }
  // Opaque: drawImage takes the alpha of the colour last set, and the other
  // page's cursor band leaves it faint.
  g.setOpacity(1.0f);
  g.drawImage(s.image, bounds);

  // The cursor: a band across each system holding a note that sounds, at
  // the rightmost of those notes, the one struck last, and a ring round each
  // notehead. A note held from earlier, such as a pedal whole note, keeps
  // its ring and leaves the band with the beat.
  const auto toPanel = pageToPanel(slot);
  const auto colour = inverted_ ? juce::Colour(0xffffb000) : juce::Colour(0xff1e6fd9);
  std::map<int, float> bandX;
  for (const auto& id : cursorNotes_) {
    const auto it = s.geometry.notes.find(id);
    if (it == s.geometry.notes.end() || it->second.system < 0) continue;
    const auto& n = it->second;
    auto [b, inserted] = bandX.emplace(n.system, n.head.x);
    if (!inserted) b->second = std::max(b->second, n.head.x);
    if (static_cast<size_t>(n.system) < s.geometry.systems.size()) {
      const float gap = s.geometry.systems[static_cast<size_t>(n.system)].lineGap;
      const auto head = juce::Rectangle<float>(n.head.x - gap * 0.4f, n.head.y - gap * 0.9f, gap * 2.0f, gap * 1.8f)
                            .transformedBy(toPanel);
      g.setColour(colour);
      g.drawEllipse(head, 1.6f);
    }
  }
  for (const auto& [system, x] : bandX) {
    if (static_cast<size_t>(system) >= s.geometry.systems.size()) continue;
    const auto& sys = s.geometry.systems[static_cast<size_t>(system)];
    const float gap = sys.lineGap;
    const auto band = juce::Rectangle<float>(x - gap * 0.6f, sys.area.getY() - gap * 2.0f, gap * 2.4f,
                                             sys.area.getHeight() + gap * 4.0f)
                          .transformedBy(toPanel);
    g.setColour(colour.withAlpha(0.22f));
    g.fillRoundedRectangle(band, 3.0f);
  }
}

void ScorePanel::paint(juce::Graphics& g) {
  g.fillAll(kBackground);
  if (!loaded_ || shown_[0].drawable == nullptr) {
    g.setColour(juce::Colours::lightgrey);
    g.setFont(16.0f);
    g.drawFittedText(message_, pageArea(), juce::Justification::centred, 3);
    return;
  }
  for (int slot = 0; slot < slotCount(); ++slot) drawSlot(g, slot);
  drawMarks(g);
}

std::string ScorePanel::noteAt(juce::Point<float> at, float within) const {
  std::string best;
  float bestDistance = within;
  for (int slot = 0; slot < slotCount(); ++slot) {
    const auto toPanel = pageToPanel(slot);
    for (const auto& [id, n] : shown_[slot].geometry.notes) {
      const float d = n.head.transformedBy(toPanel).getDistanceFrom(at);
      if (d < bestDistance && noteStartMs_.count(id) != 0) {
        bestDistance = d;
        best = id;
      }
    }
  }
  return best;
}

void ScorePanel::mouseDown(const juce::MouseEvent& e) {
  if (!loaded_) return;
  grabKeyboardFocus();
  const auto at = e.position;
  // An icon: opened by a click, or dragged to another note.
  if (const int mark = markAt(at); mark >= 0) {
    if (e.mods.isPopupMenu()) {
      openMark(mark);
      return;
    }
    dragging_ = mark;
    dragMoved_ = false;
    dragAt_ = at;
    return;
  }
  // A right click on a note: a mark is added there.
  if (e.mods.isPopupMenu()) {
    const auto note = noteAt(at, 14.0f);
    if (!note.empty()) addMarkMenu(note, juce::Rectangle<int>(e.getScreenX(), e.getScreenY(), 1, 1));
    return;
  }
  // A click on a note: playing starts there.
  const std::string best = noteAt(at, 14.0f);
  if (best.empty()) return;
  const double ms = noteStartMs_[best];
  if (playing()) {
    startMs_ = ms;
    play();
  } else {
    setStart(ms / 1000.0);
  }
}

void ScorePanel::mouseDrag(const juce::MouseEvent& e) {
  if (dragging_ < 0) return;
  if (!dragMoved_ && e.position.getDistanceFrom(dragAt_) < 5.0f) return;
  dragMoved_ = true;
  dragAt_ = e.position;
  repaint();
}

void ScorePanel::mouseUp(const juce::MouseEvent& e) {
  const int mark = dragging_;
  dragging_ = -1;
  if (mark < 0) return;
  if (!dragMoved_) {
    openMark(mark);
    return;
  }
  // Dropped near a note: the mark belongs to it now. The icon stands above
  // the staff, so the note is looked for further off.
  const auto note = noteAt(e.position, 40.0f);
  if (!note.empty() && note != marks_.marks[static_cast<size_t>(mark)].note) moveMark(mark, note);
  repaint();
}

// The keys of a player at the computer (#1012): Space plays and stops, the
// arrows go a measure back or on and to the jump marks, the page keys turn
// the pages.
bool ScorePanel::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) return undo();
  if (!loaded_ || key.getModifiers().isAnyModifierKeyDown()) return false;
  const int code = key.getKeyCode();
  if (code == juce::KeyPress::spaceKey) {
    playing() ? stop() : play();
    return true;
  }
  if (code == juce::KeyPress::leftKey) return stepMeasure(-1) || true;
  if (code == juce::KeyPress::rightKey) return stepMeasure(1) || true;
  if (code == juce::KeyPress::upKey) return stepJump(-1) || true;
  if (code == juce::KeyPress::downKey) return stepJump(1) || true;
  if (code == juce::KeyPress::pageUpKey) {
    line_ ? scrollLine(-1.0f) : showPage(page_ - slotCount());
    return true;
  }
  if (code == juce::KeyPress::pageDownKey) {
    line_ ? scrollLine(1.0f) : showPage(page_ + slotCount());
    return true;
  }
  return false;
}

// --- marks ---------------------------------------------------------------

double ScorePanel::markMs(int index) const {
  const auto it = noteStartMs_.find(marks_.marks[static_cast<size_t>(index)].note);
  return it != noteStartMs_.end() ? it->second : std::numeric_limits<double>::max();
}

std::vector<int> ScorePanel::registrationOrder() const {
  std::vector<int> out;
  for (size_t i = 0; i < marks_.marks.size(); ++i)
    if (marks_.marks[i].kind == ScoreMark::Kind::Registration || marks_.marks[i].kind == ScoreMark::Kind::Combination)
      out.push_back(static_cast<int>(i));
  std::stable_sort(out.begin(), out.end(), [this](int a, int b) { return markMs(a) < markMs(b); });
  return out;
}

void ScorePanel::remember() {
  undo_.push_back(marks_.marks);
  if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
}

void ScorePanel::marksChanged() {
  marksDirty_ = true;
  saveDue_ = juce::Time::getMillisecondCounter() + kSaveDelayMs;
  startTimer(33);
  refreshButtons();
  repaint();
}

int ScorePanel::addMark(ScoreMark::Kind kind, const std::string& note, const juce::String& text) {
  if (!loaded_ || !marks_.hasNote(note)) return -1;
  remember();
  ScoreMark m;
  m.kind = kind;
  m.note = note;
  m.text = text.toStdString();
  if (kind == ScoreMark::Kind::Registration) {
    if (m.text.empty()) m.text = (TRANS("Registration") + " " + juce::String(registrationOrder().size() + 1)).toStdString();
    m.states = writeScoreStates({captureScoreState(proc_)});
  } else if (kind == ScoreMark::Kind::Jump) {
    if (m.text.empty()) m.text = (TRANS("Jump") + " " + juce::String(jumpOrder().size() + 1)).toStdString();
  } else if (kind == ScoreMark::Kind::Combination) {
    // The general lit now, or the sequencer's step; general 1 when neither.
    const auto& player = proc_.playerCombinations();
    const int general = player.litGeneral();
    const bool step = general <= 0 && player.frame() > 0;
    const int n = general > 0 ? general : step ? player.frame() : 1;
    m.states = std::string(step ? "step " : "general ") + std::to_string(n);
    if (m.text.empty())
      m.text = ((step ? TRANS("Step") : TRANS("General")) + " " + juce::String(n)).toStdString();
  }
  marks_.marks.push_back(std::move(m));
  const int index = static_cast<int>(marks_.marks.size()) - 1;
  // A registration stored now is the one the organ has.
  if (kind == ScoreMark::Kind::Registration || kind == ScoreMark::Kind::Combination) currentReg_ = index;
  marksChanged();
  return index;
}

void ScorePanel::setMarkText(int index, const juce::String& text) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  remember();
  marks_.marks[static_cast<size_t>(index)].text = text.toStdString();
  marksChanged();
}

void ScorePanel::moveMark(int index, const std::string& note) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size()) || !marks_.hasNote(note)) return;
  remember();
  marks_.marks[static_cast<size_t>(index)].note = note;
  marksChanged();
}

void ScorePanel::deleteMark(int index) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  remember();
  marks_.marks.erase(marks_.marks.begin() + index);
  if (currentReg_ == index) currentReg_ = -1;
  else if (currentReg_ > index) --currentReg_;
  marksChanged();
}

// A note closed with nothing in it goes. One added and closed unwritten
// leaves no step to take back; one emptied later can be brought back.
void ScorePanel::dropEmptyNote(int index, bool fresh) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  const auto& m = marks_.marks[static_cast<size_t>(index)];
  if (m.kind != ScoreMark::Kind::Note || !juce::String::fromUTF8(m.text.c_str()).trim().isEmpty()) return;
  if (!fresh) {
    deleteMark(index);
    return;
  }
  marks_.marks.erase(marks_.marks.begin() + index);
  if (currentReg_ > index) --currentReg_;
  if (!undo_.empty()) undo_.pop_back();
  marksChanged();
}

void ScorePanel::storeRegistration(int index) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  auto& m = marks_.marks[static_cast<size_t>(index)];
  if (m.kind != ScoreMark::Kind::Registration) return;
  remember();
  auto states = parseScoreStates(m.states);
  auto now = captureScoreState(proc_);
  bool replaced = false;
  for (auto& s : states)
    if (s.organ == now.organ) {
      s = now;
      replaced = true;
    }
  if (!replaced) states.push_back(std::move(now));
  m.states = writeScoreStates(states);
  currentReg_ = index;
  marksChanged();
}

void ScorePanel::forgetRegistration(int index) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  auto& m = marks_.marks[static_cast<size_t>(index)];
  if (m.kind != ScoreMark::Kind::Registration) return;
  remember();
  auto states = parseScoreStates(m.states);
  const auto organ = proc_.organKey();
  states.erase(std::remove_if(states.begin(), states.end(), [&](const auto& s) { return s.organ == organ; }),
               states.end());
  m.states = writeScoreStates(states);
  marksChanged();
}

bool ScorePanel::undo() {
  if (undo_.empty()) return false;
  marks_.marks = std::move(undo_.back());
  undo_.pop_back();
  if (currentReg_ >= static_cast<int>(marks_.marks.size()) ||
      (currentReg_ >= 0 && marks_.marks[static_cast<size_t>(currentReg_)].kind != ScoreMark::Kind::Registration &&
       marks_.marks[static_cast<size_t>(currentReg_)].kind != ScoreMark::Kind::Combination))
    currentReg_ = -1;
  marksChanged();
  return true;
}

// Sets it on the organ, when it holds a state for this organ; it is the
// current one either way, so stepping goes on from it.
void ScorePanel::recallRegistration(int index) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  const auto& m = marks_.marks[static_cast<size_t>(index)];
  if (m.kind == ScoreMark::Kind::Combination) {
    // By piston, the same on every organ: a general pressed, or a step of
    // the sequencer gone to.
    currentReg_ = index;
    const auto piston = juce::String::fromUTF8(m.states.c_str());
    const int n = piston.fromFirstOccurrenceOf(" ", false, false).getIntValue();
    if (n >= 1) {
      if (piston.startsWith("step")) proc_.stepperGoto(n);
      else proc_.pressGeneral(n);
    }
    refreshButtons();
    repaint();
    return;
  }
  if (m.kind != ScoreMark::Kind::Registration) return;
  currentReg_ = index;
  const auto states = parseScoreStates(m.states);
  if (const auto* mine = stateForOrgan(states, proc_.organKey())) applyScoreState(proc_, *mine);
  refreshButtons();
  repaint();
}

bool ScorePanel::stepRegistration(int step) {
  if (!loaded_) return false;
  const auto order = registrationOrder();
  if (order.empty()) return false;
  const auto at = std::find(order.begin(), order.end(), currentReg_);
  const int from = at != order.end() ? static_cast<int>(at - order.begin()) : -1;
  const int to = from + step;
  if (to < 0 || to >= static_cast<int>(order.size())) return false;
  const int index = order[static_cast<size_t>(to)];
  recallRegistration(index);
  // Its page in sight, and playing goes on from its note.
  if (!playing()) {
    const int p = engraver_->pageWithElement(marks_.marks[static_cast<size_t>(index)].note);
    if (p >= 1 && (p < page_ || p >= page_ + slotCount())) {
      page_ = std::clamp(p, 1, std::max(1, pageCount()));
      renderShown();
    }
    const double ms = markMs(index);
    if (ms < std::numeric_limits<double>::max()) setStart(ms / 1000.0);
  }
  return true;
}

// The playing has gone from one moment to the next: the last registration
// passed on the way is set.
void ScorePanel::registrationsReached(double fromMs, double toMs) {
  // The last one passed; on a repeat, the marks of the measures played again
  // are reached again.
  int last = -1;
  double lastMs = -1;
  for (const int i : registrationOrder()) {
    const auto it = noteTimes_.find(marks_.marks[static_cast<size_t>(i)].note);
    if (it == noteTimes_.end()) continue;
    for (const double ms : it->second)
      if (ms > fromMs && ms <= toMs && ms >= lastMs) {
        last = i;
        lastMs = ms;
      }
  }
  if (last >= 0 && last != currentReg_) recallRegistration(last);
}

juce::Rectangle<float> ScorePanel::markBounds(int index) const {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return {};
  const auto& note = marks_.marks[static_cast<size_t>(index)].note;
  // Beside the marks before it on the same note.
  int before = 0;
  for (int i = 0; i < index; ++i)
    if (marks_.marks[static_cast<size_t>(i)].note == note) ++before;
  for (int slot = 0; slot < slotCount(); ++slot) {
    const auto& s = shown_[slot];
    if (s.drawable == nullptr) continue;
    const auto it = s.geometry.notes.find(note);
    if (it == s.geometry.notes.end() || it->second.system < 0 ||
        static_cast<size_t>(it->second.system) >= s.geometry.systems.size())
      continue;
    const auto& sys = s.geometry.systems[static_cast<size_t>(it->second.system)];
    // Above the system's top line, over the note.
    const auto toPanel = pageToPanel(slot);
    const auto head = it->second.head.transformedBy(toPanel);
    const float top = juce::Point<float>(0.0f, sys.area.getY() - sys.lineGap * 2.5f).transformedBy(toPanel).y;
    return {head.x - 2.0f + static_cast<float>(before) * (kIcon + 3.0f), top - kIcon, kIcon, kIcon};
  }
  return {};
}

int ScorePanel::markAt(juce::Point<float> at) const {
  for (int i = static_cast<int>(marks_.marks.size()) - 1; i >= 0; --i)
    if (markBounds(i).expanded(2.0f).contains(at)) return i;
  return -1;
}

void ScorePanel::drawMarks(juce::Graphics& g) {
  const auto organ = proc_.organKey();
  for (int i = 0; i < static_cast<int>(marks_.marks.size()); ++i) {
    auto r = markBounds(i);
    if (r.isEmpty()) continue;
    if (i == dragging_ && dragMoved_) r.setCentre(dragAt_);
    const auto& m = marks_.marks[static_cast<size_t>(i)];
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRoundedRectangle(r.translated(1.0f, 1.0f), 3.0f);
    if (m.kind == ScoreMark::Kind::Note) {
      // A sticky note: yellow, its corner folded, lines of writing.
      juce::Path p;
      const float fold = r.getWidth() * 0.3f;
      p.startNewSubPath(r.getX(), r.getY());
      p.lineTo(r.getRight(), r.getY());
      p.lineTo(r.getRight(), r.getBottom() - fold);
      p.lineTo(r.getRight() - fold, r.getBottom());
      p.lineTo(r.getX(), r.getBottom());
      p.closeSubPath();
      g.setColour(juce::Colour(0xffffd54a));
      g.fillPath(p);
      g.setColour(juce::Colour(0xff7a5a00));
      g.strokePath(p, juce::PathStrokeType(1.0f));
      for (int line = 1; line <= 3; ++line) {
        const float y = r.getY() + r.getHeight() * static_cast<float>(line) / 4.6f;
        g.drawLine(r.getX() + 3.0f, y, r.getRight() - 4.0f, y, 1.0f);
      }
    } else if (m.kind == ScoreMark::Kind::Jump) {
      // A jump mark: a green flag on its pole.
      g.setColour(juce::Colours::white);
      g.fillRoundedRectangle(r, 3.0f);
      const float pole = r.getX() + r.getWidth() * 0.28f;
      g.setColour(juce::Colour(0xff2f3a2f));
      g.drawLine(pole, r.getY() + 2.0f, pole, r.getBottom() - 2.0f, 1.6f);
      juce::Path flag;
      flag.addTriangle(pole, r.getY() + 2.5f, r.getRight() - 2.5f, r.getY() + r.getHeight() * 0.36f, pole,
                       r.getY() + r.getHeight() * 0.68f);
      g.setColour(juce::Colour(0xff2e9e4f));
      g.fillPath(flag);
    } else if (m.kind == ScoreMark::Kind::Combination) {
      // A combination: a piston, square, with G for a general or S for a
      // sequencer step. Orange when set now.
      g.setColour(i == currentReg_ ? juce::Colour(0xffff8c1a) : juce::Colour(0xff6a4fb3));
      g.fillRoundedRectangle(r, 4.0f);
      g.setColour(juce::Colours::white);
      g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.2f);
      g.setFont(juce::FontOptions(r.getHeight() * 0.62f, juce::Font::bold));
      g.drawText(juce::String(m.states.rfind("step", 0) == 0 ? "S" : "G"), r, juce::Justification::centred, false);
    } else {
      // A registration: a stop knob. Filled orange for the one set now, blue
      // for the others, grey for one not set on this organ.
      const bool mine = stateForOrgan(parseScoreStates(m.states), organ) != nullptr;
      const auto fill = i == currentReg_ ? juce::Colour(0xffff8c1a)
                        : mine           ? juce::Colour(0xff3d78c8)
                                         : juce::Colour(0xff8a8f99);
      g.setColour(fill);
      g.fillEllipse(r);
      g.setColour(juce::Colours::white);
      g.drawEllipse(r.reduced(0.5f), 1.2f);
      g.setFont(juce::FontOptions(r.getHeight() * 0.62f, juce::Font::bold));
      g.drawText("R", r, juce::Justification::centred, false);
    }
  }
}

void ScorePanel::openMark(int index, bool fresh) {
  if (index < 0 || index >= static_cast<int>(marks_.marks.size())) return;
  const auto& m = marks_.marks[static_cast<size_t>(index)];
  const juce::Component::SafePointer<ScorePanel> safe(this);
  std::unique_ptr<juce::Component> editor;
  if (m.kind == ScoreMark::Kind::Note)
    editor = std::make_unique<MarkNoteEditor>(safe, index, juce::String::fromUTF8(m.text.c_str()), fresh);
  else if (m.kind == ScoreMark::Kind::Registration)
    editor = std::make_unique<MarkRegistrationEditor>(safe, index, proc_);
  else
    editor = std::make_unique<MarkPlaceEditor>(safe, index);
  auto area = markBounds(index).toNearestInt();
  if (area.isEmpty()) area = getLocalBounds().withSizeKeepingCentre(1, 1);
  juce::CallOutBox::launchAsynchronously(std::move(editor), localAreaToGlobal(area), nullptr);
}

void ScorePanel::addMarkMenu(const std::string& note, juce::Rectangle<int> screenArea) {
  juce::PopupMenu menu;
  menu.addItem(1, TRANS("Add a note here"));
  menu.addItem(2, TRANS("Add a registration here (the organ as it is now)"));
  menu.addItem(4, TRANS("Add a combination here (a general or a sequencer step, for every organ)"));
  menu.addItem(5, TRANS("Add a jump mark here"));
  // Also Ctrl+Z; here for a touch screen, which has no keys.
  menu.addSeparator();
  menu.addItem(3, TRANS("Undo the last change to the marks"), !undo_.empty());
  showMenu(menu, juce::PopupMenu::Options().withTargetScreenArea(screenArea),
                     [safe = juce::Component::SafePointer<ScorePanel>(this), note](int chosen) {
                       if (safe == nullptr || chosen == 0) return;
                       if (chosen == 3) {
                         safe->undo();
                         return;
                       }
                       const auto kind = chosen == 1   ? ScoreMark::Kind::Note
                                         : chosen == 2 ? ScoreMark::Kind::Registration
                                         : chosen == 4 ? ScoreMark::Kind::Combination
                                                       : ScoreMark::Kind::Jump;
                       const int index = safe->addMark(kind, note);
                       if (index >= 0) safe->openMark(index, true);
                     });
}

bool ScorePanel::saveMarks() {
  marksDirty_ = false;
  if (file_ == juce::File()) return false;
  const std::string text = marks_.save();
  // The file as it was before the first mark was written, kept once.
  const auto backup = file_.getSiblingFile(file_.getFileName() + ".bak");
  if (!backup.existsAsFile()) file_.copyFileTo(backup);
  if (!zipped_) return file_.replaceWithData(text.data(), text.size());
  // A compressed score: the same zip with the score's entry written anew.
  juce::MemoryBlock bytes;
  if (!file_.loadFileAsData(bytes)) return false;
  juce::String entry;
  if (scoreFromZip(bytes, &entry).empty() || entry.isEmpty()) return false;
  juce::MemoryInputStream in(bytes, false);
  juce::ZipFile zip(in);
  juce::ZipFile::Builder builder;
  for (int i = 0; i < zip.getNumEntries(); ++i) {
    const auto* e = zip.getEntry(i);
    if (e->filename == entry) {
      builder.addEntry(new juce::MemoryInputStream(text.data(), text.size(), true), 9, entry, juce::Time::getCurrentTime());
    } else if (auto* s = zip.createStreamForEntry(i)) {
      juce::MemoryBlock data;
      s->readIntoMemoryBlock(data);
      delete s;
      builder.addEntry(new juce::MemoryInputStream(data, true), 9, e->filename, e->fileTime);
    }
  }
  juce::TemporaryFile temp(file_);
  {
    auto out = temp.getFile().createOutputStream();
    if (out == nullptr || !builder.writeToStream(*out, nullptr)) return false;
  }
  return temp.overwriteTargetFileWithTemporary();
}

juce::Image ScorePanel::render() {
  if (needsEngrave_) engrave();
  if (loaded_) followPlaying();
  juce::Image image(juce::Image::RGB, std::max(1, getWidth()), std::max(1, getHeight()), true);
  juce::Graphics g(image);
  paintEntireComponent(g, true);
  return image;
}

}  // namespace mp::ui
