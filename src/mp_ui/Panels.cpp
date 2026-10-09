#include "Panels.h"

#include "MidiEventDialog.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>

namespace mp::ui {

// ---- the organ as sections -------------------------------------------------

std::string PanelElement::key() const {
  const char* k = kind == Kind::Stop ? "s" : kind == Kind::Switch ? "w"
                : kind == Kind::Piston ? "p" : "c";
  return k + std::to_string(static_cast<long long>(id));
}

void splitFootage(const std::string& label, std::string& name, std::string& footage) {
  // A footage at the end: 8', 2 2/3', 1 1/3, 16; a mixture's ranks: IV, III-IV,
  // 4f, 3-5f.
  static const std::regex tail(
      R"(^(.*?)[\s,]+((?:\d+\s+)?\d+(?:/\d+)?\s*(?:'|\x{2019}|\x{2032}|ft\.?)?|[IVX]+(?:\s*-\s*[IVX]+)?|\d+(?:\s*-\s*\d+)?\s*(?:f|fach|rangs?|rks?)\.?)\s*$)",
      std::regex::icase);
  std::smatch m;
  if (std::regex_match(label, m, tail) && !m[1].str().empty()) {
    name = m[1].str();
    footage = m[2].str();
  } else {
    name = label;
    footage.clear();
  }
}

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The division a coupler feeds: the key action it enables, followed through
// up to a few switch linkages, because the drawn knob is often upstream of the
// switch the key action names.
Id couplerDivision(const OrganModel& m, Id switchId) {
  std::vector<Id> frontier{switchId};
  std::set<Id> seen{switchId};
  for (int hop = 0; hop < 4 && !frontier.empty(); ++hop) {
    for (Id sw : frontier)
      for (const auto& a : m.keyActions) {
        if (a.conditionSwitchId != sw) continue;
        if (!a.destIsKeyboard && a.destDivision != 0) return static_cast<Id>(a.destDivision);
        const auto kb = m.keyboards.find(static_cast<Id>(a.destKeyboard));
        if (kb != m.keyboards.end() && kb->second.primaryDivisionHint != 0)
          return kb->second.primaryDivisionHint;
      }
    std::vector<Id> next;
    for (Id sw : frontier)
      for (const auto& l : m.switchLinkages)
        if (l.sourceSwitchId == sw && seen.insert(l.destSwitchId).second)
          next.push_back(l.destSwitchId);
    frontier.swap(next);
  }
  return 0;
}

int romanValue(const std::string& s) {
  static const std::map<std::string, int> r{{"i", 1}, {"ii", 2}, {"iii", 3}, {"iv", 4}, {"v", 5}, {"vi", 6}};
  const auto it = r.find(lower(s));
  return it == r.end() ? 0 : it->second;
}

// A tremulant's division, from its name: the division's own name in it, or a
// manual number in roman numerals ("Tremolo II").
Id tremulantDivision(const OrganModel& m, const std::string& name) {
  const std::string n = lower(name);
  for (const auto& [id, d] : m.divisions)
    if (!d.name.empty() && n.find(lower(d.name)) != std::string::npos) return id;
  std::istringstream words(name);
  std::string w, last;
  while (words >> w) last = w;
  if (const int v = romanValue(last); v > 0)
    for (const auto& [id, d] : m.divisions)
      if (d.manualNumber == v) return id;
  return 0;
}

}  // namespace

std::vector<PanelSection> panelSections(const MasterpieceProcessor& p) {
  const auto& m = p.organModel();
  std::vector<PanelSection> divisions;
  std::map<Id, size_t> byDivision;
  auto sectionFor = [&](Id div) -> PanelSection& {
    if (const auto it = byDivision.find(div); it != byDivision.end()) return divisions[it->second];
    PanelSection s;
    s.kind = PanelSection::Kind::Division;
    s.divisionId = div;
    s.key = "d" + std::to_string(static_cast<long long>(div));
    const auto d = m.divisions.find(div);
    s.title = d != m.divisions.end() && !d->second.name.empty()
                  ? d->second.name
                  : "Division " + std::to_string(static_cast<long long>(div));
    byDivision[div] = divisions.size();
    divisions.push_back(std::move(s));
    return divisions.back();
  };

  // Stops, in the order of the stop list: by division, then as the organ
  // numbers them. A stop whose ranks ship no pipes would be a rectangle that
  // does nothing, and is left out.
  std::set<Id> stopSwitches;
  for (const auto& st : p.stopList()) {
    if (const auto s = m.stops.find(st.stopId); s != m.stops.end())
      stopSwitches.insert(s->second.controllingSwitchId);
    if (!st.playable) continue;
    PanelElement e;
    e.kind = PanelElement::Kind::Stop;
    e.id = st.stopId;
    splitFootage(st.name, e.name, e.footage);
    sectionFor(st.divisionId).elements.push_back(std::move(e));
  }

  // Couplers, then tremulants, each in the division it belongs to.
  std::vector<Id> switchIds;
  for (const auto& [id, sw] : m.switches) switchIds.push_back(id);
  std::sort(switchIds.begin(), switchIds.end());
  PanelSection other;
  other.kind = PanelSection::Kind::Other;
  other.key = "other";
  other.title = "Other";
  for (const bool tremulants : {false, true})
    for (Id id : switchIds) {
      const auto& sw = m.switches.at(id);
      if ((tremulants ? !sw.isTremulant : !sw.isCoupler) || !sw.clickable ||
          stopSwitches.count(id) != 0)
        continue;
      PanelElement e;
      e.kind = PanelElement::Kind::Switch;
      e.id = id;
      splitFootage(sw.name, e.name, e.footage);
      const Id div = tremulants ? tremulantDivision(m, sw.name) : couplerDivision(m, id);
      if (div != 0 && m.divisions.count(div) != 0)
        sectionFor(div).elements.push_back(std::move(e));
      else
        other.elements.push_back(std::move(e));
    }

  // The organ's own pistons: the switch each combination is fired by.
  PanelSection presets;
  presets.kind = PanelSection::Kind::Presets;
  presets.key = "presets";
  presets.title = "Pistons";
  std::vector<const Combination*> combos;
  for (const auto& [id, c] : m.combinations) combos.push_back(&c);
  std::sort(combos.begin(), combos.end(), [](const Combination* a, const Combination* b) {
    return a->type != b->type ? a->type < b->type : a->combinationId < b->combinationId;
  });
  std::set<Id> pistons;
  for (const Combination* c : combos) {
    Id sw = c->activatingSwitchId;
    if (sw == 0 && c->type >= 100)
      for (const auto& [id, s] : m.switches)
        if (s.asgnCode == c->type) { sw = id; break; }
    if (sw == 0 || !pistons.insert(sw).second) continue;
    PanelElement e;
    e.kind = PanelElement::Kind::Piston;
    e.id = sw;
    const auto s = m.switches.find(sw);
    e.name = !c->name.empty() ? c->name
             : s != m.switches.end() && !s->second.name.empty() ? s->second.name
                                                               : "Piston";
    presets.elements.push_back(std::move(e));
  }

  // Expression: the shoe each enclosure is worked by, and the crescendo.
  PanelSection controls;
  controls.kind = PanelSection::Kind::Controls;
  controls.key = "controls";
  controls.title = "Expression";
  std::set<Id> shoes;
  auto addControl = [&](Id controlId, const std::string& fallback) {
    if (controlId == 0 || !shoes.insert(controlId).second) return;
    PanelElement e;
    e.kind = PanelElement::Kind::Control;
    e.id = controlId;
    const auto c = m.continuousControls.find(controlId);
    e.name = c != m.continuousControls.end() && !c->second.name.empty() ? c->second.name : fallback;
    controls.elements.push_back(std::move(e));
  };
  for (const auto& [id, enc] : m.enclosures) {
    if (enc.continuousControlId == 0) continue;
    if (const auto c = m.continuousControls.find(enc.continuousControlId);
        c != m.continuousControls.end() && c->second.rememberState && c->second.imageSetInstanceId != 0)
      continue;  // a level set on its own page, not a shoe (see ExpressionBar)
    addControl(p.playerControlFor(enc.continuousControlId), enc.name.empty() ? "Swell" : enc.name);
  }
  Id crescendo = 0;
  size_t steps = 0;
  for (Id id : p.stageSwitches().stagedControls())
    if (const size_t n = p.stageSwitches().stepCount(id); n > steps) {
      steps = n;
      crescendo = id;
    }
  if (steps >= 3) addControl(crescendo, "Crescendo");

  std::vector<PanelSection> out = std::move(divisions);
  if (!other.elements.empty()) out.push_back(std::move(other));
  if (!presets.elements.empty()) out.push_back(std::move(presets));
  if (!controls.elements.empty()) out.push_back(std::move(controls));
  return out;
}

// ---- saving ----------------------------------------------------------------

std::string PanelLayout::toText() const {
  std::ostringstream o;
  for (const auto& s : sectionsOff) o << "off " << s << "\n";
  for (const auto& h : hidden) o << "hide " << h << "\n";
  o << "detached " << (detached ? 1 : 0) << "\n";
  o << "window " << x << " " << y << " " << w << " " << h << "\n";
  o << "scheme " << scheme << "\n";
  return o.str();
}

bool PanelLayout::fromText(const std::string& text) {
  *this = PanelLayout{};
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string key;
    if (!(l >> key)) continue;
    if (key == "off") { std::string v; if (l >> v) sectionsOff.insert(v); }
    else if (key == "hide") { std::string v; if (l >> v) hidden.insert(v); }
    else if (key == "detached") { int v = 0; l >> v; detached = v != 0; }
    else if (key == "window") { l >> x >> y >> w >> h; }
    else if (key == "scheme") { l >> scheme; scheme = std::clamp(scheme, 0, 3); }
  }
  return true;
}

bool PanelLayout::operator==(const PanelLayout& o) const {
  return sectionsOff == o.sectionsOff && hidden == o.hidden && detached == o.detached &&
         x == o.x && y == o.y && w == o.w && h == o.h && scheme == o.scheme;
}

namespace {
juce::File panelsFile(const MasterpieceProcessor& p) {
  return p.organFileForSaving("organs", ".mppanels");
}
}  // namespace

std::vector<PanelLayout> loadPanelLayouts(const MasterpieceProcessor& p) {
  std::vector<PanelLayout> out;
  const auto f = panelsFile(p);
  if (f == juce::File() || !f.existsAsFile()) return out;
  // One "panel" line opens each panel's block.
  std::string block;
  bool open = false;
  auto flush = [&] {
    if (!open || static_cast<int>(out.size()) >= kMaxPanels) return;
    PanelLayout l;
    l.fromText(block);
    out.push_back(std::move(l));
  };
  for (const auto& line : juce::StringArray::fromLines(f.loadFileAsString())) {
    const auto t = line.trim();
    if (t.startsWith("#") || t.isEmpty()) continue;
    if (t == "panel") {
      flush();
      block.clear();
      open = true;
      continue;
    }
    block += t.toStdString() + "\n";
  }
  flush();
  return out;
}

bool savePanelLayouts(const MasterpieceProcessor& p, const std::vector<PanelLayout>& panels) {
  const auto f = panelsFile(p);
  if (f == juce::File()) return false;
  std::string text = "# Masterpiece panels\n";
  for (const auto& l : panels) text += "panel\n" + l.toText();
  f.getParentDirectory().createDirectory();
  return f.replaceWithText(juce::String(text));
}

// ---- colours -----------------------------------------------------------------

juce::StringArray panelSchemeNames() {
  return {"Dark", "Light", "High contrast", "By division"};
}

namespace {

struct Colours {
  juce::Colour background, cellOff, cellOn, textOff, textOn, border;
};

Colours schemeColours(int scheme, size_t sectionIndex) {
  switch (static_cast<PanelScheme>(scheme)) {
    case PanelScheme::Light:
      return {juce::Colour(0xffe9e5dc), juce::Colour(0xffd3ccbf), juce::Colour(0xff2f5d8c),
              juce::Colour(0xff26231f), juce::Colour(0xfff4f1ea), juce::Colour(0xffb3ab9c)};
    case PanelScheme::HighContrast:
      return {juce::Colours::black, juce::Colours::black, juce::Colour(0xffffd400),
              juce::Colours::white, juce::Colours::black, juce::Colours::white};
    case PanelScheme::ByDivision: {
      static const float hues[] = {0.08f, 0.58f, 0.33f, 0.78f, 0.0f, 0.15f, 0.48f};
      const float hue = hues[sectionIndex % (sizeof(hues) / sizeof(hues[0]))];
      return {juce::Colour(0xff15171c), juce::Colour::fromHSV(hue, 0.45f, 0.30f, 1.0f),
              juce::Colour::fromHSV(hue, 0.30f, 0.92f, 1.0f), juce::Colour(0xffdfe3ea),
              juce::Colour(0xff15171c), juce::Colour::fromHSV(hue, 0.40f, 0.45f, 1.0f)};
    }
    case PanelScheme::Dark:
    default:
      return {juce::Colour(0xff15171c), juce::Colour(0xff2a2f3a), juce::Colour(0xffeee3c6),
              juce::Colour(0xffb9c2d0), juce::Colour(0xff1b1e24), juce::Colour(0xff3a4150)};
  }
}

}  // namespace

// ---- one rectangle -----------------------------------------------------------

class PanelView::Cell : public juce::Component {
public:
  Cell(PanelView& owner, PanelElement e, size_t sectionIndex)
      : owner_(owner), e_(std::move(e)), section_(sectionIndex) {}

  bool engaged() const {
    auto& p = owner_.proc_;
    switch (e_.kind) {
      case PanelElement::Kind::Stop: return p.stopEngaged(e_.id);
      case PanelElement::Kind::Switch: return p.switchEngaged(e_.id);
      case PanelElement::Kind::Piston: return pressed_;
      case PanelElement::Kind::Control: return false;
    }
    return false;
  }
  // Repaint only when what it shows has changed.
  void follow() {
    const bool now = engaged();
    if (now != shown_) {
      shown_ = now;
      repaint();
    }
  }

  void paint(juce::Graphics& g) override {
    const auto c = schemeColours(owner_.layout_.scheme, section_);
    const bool hidden = owner_.layout_.hidden.count(e_.key()) != 0;
    const bool on = engaged();
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(on ? c.cellOn : c.cellOff);
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(c.border);
    g.drawRoundedRectangle(r, 5.0f, 1.2f);
    g.setColour(on ? c.textOn : c.textOff);
    // The name, shrinking to fit; the footage on a line of its own.
    auto text = r.reduced(6.0f, 4.0f);
    const float h = std::min(20.0f, text.getHeight() * (e_.footage.empty() ? 0.42f : 0.34f));
    g.setFont(juce::Font(juce::FontOptions(h, juce::Font::bold)));
    if (!e_.footage.empty()) {
      auto foot = text.removeFromBottom(text.getHeight() * 0.36f);
      g.drawFittedText(juce::String::fromUTF8(e_.footage.c_str()), foot.toNearestInt(),
                       juce::Justification::centredTop, 1, 0.7f);
    }
    g.drawFittedText(juce::String::fromUTF8(e_.name.c_str()), text.toNearestInt(),
                     juce::Justification::centred, 2, 0.6f);
    if (owner_.editing_) {
      // The tick: shown, or hidden from the panel.
      auto box = getLocalBounds().toFloat().removeFromTop(22.0f).removeFromRight(22.0f).reduced(4.0f);
      g.setColour(juce::Colours::white);
      g.drawRect(box, 1.5f);
      if (!hidden) {
        juce::Path tick;
        tick.startNewSubPath(box.getX() + 2.0f, box.getCentreY());
        tick.lineTo(box.getX() + box.getWidth() * 0.42f, box.getBottom() - 2.0f);
        tick.lineTo(box.getRight() - 1.0f, box.getY() + 1.0f);
        g.strokePath(tick, juce::PathStrokeType(2.0f));
      }
      if (hidden) {
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(r, 5.0f);
      }
    }
  }

  void mouseDown(const juce::MouseEvent& ev) override {
    auto& p = owner_.proc_;
    if (owner_.editing_) {
      auto& hidden = owner_.layout_.hidden;
      if (!hidden.erase(e_.key())) hidden.insert(e_.key());
      repaint();
      owner_.changed();
      return;
    }
    if (ev.mods.isPopupMenu()) {
      // The element's own mapping: a stop is mapped through its drawstop.
      Id target = e_.id;
      if (e_.kind == PanelElement::Kind::Stop) {
        const auto s = p.organModel().stops.find(e_.id);
        target = s != p.organModel().stops.end() ? s->second.controllingSwitchId : 0;
      }
      if (target != 0) MidiEventDialog::show(p, MidiTargetKind::Switch, target);
      return;
    }
    switch (e_.kind) {
      case PanelElement::Kind::Stop: p.setStopEngaged(e_.id, !p.stopEngaged(e_.id)); break;
      case PanelElement::Kind::Switch: p.setSwitchEngaged(e_.id, !p.switchEngaged(e_.id)); break;
      case PanelElement::Kind::Piston:
        // Fired on press, as on a console; let go with the finger.
        pressed_ = true;
        p.setSwitchEngaged(e_.id, true);
        break;
      case PanelElement::Kind::Control: break;
    }
    repaint();
  }
  void mouseUp(const juce::MouseEvent&) override {
    if (!pressed_) return;
    pressed_ = false;
    owner_.proc_.setSwitchEngaged(e_.id, false);
    repaint();
  }

private:
  PanelView& owner_;
  PanelElement e_;
  size_t section_ = 0;
  bool shown_ = false;
  bool pressed_ = false;
};

// ---- one fader -----------------------------------------------------------------

class PanelView::Fader : public juce::Component {
public:
  Fader(PanelView& owner, PanelElement e, size_t sectionIndex)
      : owner_(owner), e_(std::move(e)), section_(sectionIndex) {}

  void follow() {
    const int v = owner_.proc_.continuousControlValue(e_.id);
    if (v != shown_) {
      shown_ = v;
      repaint();
    }
  }
  void paint(juce::Graphics& g) override {
    const auto c = schemeColours(owner_.layout_.scheme, section_);
    auto r = getLocalBounds().toFloat().reduced(2.0f);
    g.setColour(c.cellOff);
    g.fillRoundedRectangle(r, 5.0f);
    auto title = r.removeFromTop(22.0f);
    auto track = r.reduced(10.0f, 6.0f);
    const float frac = juce::jlimit(0.0f, 1.0f, owner_.proc_.continuousControlValue(e_.id) / 127.0f);
    g.setColour(c.border);
    g.drawRoundedRectangle(track, 4.0f, 1.2f);
    g.setColour(c.cellOn);
    g.fillRoundedRectangle(track.withTop(track.getBottom() - track.getHeight() * frac), 4.0f);
    g.setColour(c.textOff);
    g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    g.drawFittedText(juce::String::fromUTF8(e_.name.c_str()), title.toNearestInt(),
                     juce::Justification::centred, 1, 0.6f);
    if (owner_.editing_ && owner_.layout_.hidden.count(e_.key()) != 0) {
      g.setColour(juce::Colours::black.withAlpha(0.55f));
      g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 5.0f);
    }
  }
  void mouseDown(const juce::MouseEvent& ev) override {
    if (owner_.editing_) {
      auto& hidden = owner_.layout_.hidden;
      if (!hidden.erase(e_.key())) hidden.insert(e_.key());
      repaint();
      owner_.changed();
      return;
    }
    if (ev.mods.isPopupMenu()) {
      MidiEventDialog::show(owner_.proc_, MidiTargetKind::ContinuousControl, e_.id);
      return;
    }
    mouseDrag(ev);
  }
  void mouseDrag(const juce::MouseEvent& ev) override {
    if (owner_.editing_ || ev.mods.isPopupMenu()) return;
    const auto track = getLocalBounds().toFloat().reduced(2.0f).withTrimmedTop(22.0f).reduced(10.0f, 6.0f);
    const float frac = juce::jlimit(0.0f, 1.0f, (track.getBottom() - ev.position.y) / track.getHeight());
    owner_.proc_.setContinuousControl(e_.id, juce::roundToInt(frac * 127.0f));
    follow();
  }

private:
  PanelView& owner_;
  PanelElement e_;
  size_t section_ = 0;
  int shown_ = -1;
};

// ---- the panel -------------------------------------------------------------------

PanelView::PanelView(MasterpieceProcessor& p, int index, PanelLayout layout)
    : proc_(p), index_(index), layout_(std::move(layout)), saved_(layout_) {
  addAndMakeVisible(edit_);
  edit_.setClickingTogglesState(true);
  edit_.setTooltip("Tick the elements this panel shows");
  edit_.onClick = [this] { setEditing(edit_.getToggleState()); };
  addAndMakeVisible(save_);
  save_.setTooltip("Keep the sections, hidden elements, window and colours");
  save_.onClick = [this] { if (onSave) onSave(); };
  addAndMakeVisible(detach_);
  detach_.setTooltip("Show this panel in a window of its own, or back among the tabs");
  detach_.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
  detach_.setVisible(!kMobile);
  addAndMakeVisible(scheme_);
  scheme_.addItemList(panelSchemeNames(), 1);
  scheme_.setSelectedId(layout_.scheme + 1, juce::dontSendNotification);
  scheme_.onChange = [this] {
    layout_.scheme = scheme_.getSelectedId() - 1;
    repaint();
    for (auto* c : cells_) c->repaint();
    changed();
  };
  viewport_.setViewedComponent(&body_, false);
  viewport_.setScrollBarsShown(true, true);
  addAndMakeVisible(viewport_);
  rebuild();
  startTimerHz(15);
}

PanelView::~PanelView() { stopTimer(); }

void PanelView::setDetached(bool detached) {
  layout_.detached = detached;
  detach_.setButtonText(detached ? "Attach" : "Detach");
}

void PanelView::rebuild() {
  sections_ = panelSections(proc_);
  sectionButtons_.clear();
  for (const auto& s : sections_) {
    auto* b = sectionButtons_.add(new juce::TextButton(juce::String::fromUTF8(s.title.c_str())));
    b->setClickingTogglesState(true);
    b->setToggleState(layout_.sectionsOff.count(s.key) == 0, juce::dontSendNotification);
    b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff4a5a78));
    const std::string key = s.key;
    b->onClick = [this, b, key] {
      if (b->getToggleState()) layout_.sectionsOff.erase(key);
      else layout_.sectionsOff.insert(key);
      changed();
      layoutSections();
    };
    addAndMakeVisible(b);
  }
  setDetached(layout_.detached);
  resized();
}

void PanelView::setEditing(bool editing) {
  editing_ = editing;
  layoutSections();
}

void PanelView::changed() {
  save_.setButtonText(hasUnsavedChanges() ? "Save *" : "Save");
}

void PanelView::resized() {
  auto r = getLocalBounds();
  auto bar = r.removeFromTop(kMobile ? 44 : 34).reduced(2);
  save_.setBounds(bar.removeFromRight(70).reduced(2));
  edit_.setBounds(bar.removeFromRight(60).reduced(2));
  if (detach_.isVisible()) detach_.setBounds(bar.removeFromRight(76).reduced(2));
  scheme_.setBounds(bar.removeFromRight(130).reduced(2));
  // The section toggles share what is left.
  if (!sectionButtons_.isEmpty()) {
    const int w = std::max(60, bar.getWidth() / sectionButtons_.size());
    for (auto* b : sectionButtons_) b->setBounds(bar.removeFromLeft(w).reduced(2));
  }
  viewport_.setBounds(r);
  layoutSections();
}

void PanelView::layoutSections() {
  titles_.clear();
  cells_.clear();
  std::vector<size_t> shown;
  for (size_t i = 0; i < sections_.size(); ++i)
    if (layout_.sectionsOff.count(sections_[i].key) == 0) shown.push_back(i);

  const int viewW = std::max(1, viewport_.getMaximumVisibleWidth());
  const int minColumn = kMobile ? 150 : 170;
  const int n = std::max<int>(1, static_cast<int>(shown.size()));
  const int columnW = std::max(minColumn, viewW / n);
  const int cellH = kMobile ? 64 : 58;
  const int faderH = 220;
  int tallest = 0;
  int x = 0;
  for (size_t si : shown) {
    const auto& s = sections_[si];
    auto* t = titles_.add(new juce::Label({}, juce::String::fromUTF8(s.title.c_str())));
    t->setFont(juce::Font(juce::FontOptions(15.0f, juce::Font::bold)));
    t->setColour(juce::Label::textColourId, juce::Colour(0xffe0a050));
    t->setJustificationType(juce::Justification::centredLeft);
    t->setBounds(x + 6, 4, columnW - 12, 22);
    body_.addAndMakeVisible(t);

    const bool faders = s.kind == PanelSection::Kind::Controls;
    const int cols = std::max(1, (columnW - 8) / (faders ? 90 : 104));
    const int cellW = (columnW - 8) / cols;
    int k = 0;
    for (const auto& e : s.elements) {
      if (!editing_ && layout_.hidden.count(e.key()) != 0) continue;
      const int col = k % cols, row = k / cols;
      const int h = faders ? faderH : cellH;
      juce::Component* c = faders ? static_cast<juce::Component*>(new Fader(*this, e, si))
                                  : static_cast<juce::Component*>(new Cell(*this, e, si));
      c->setBounds(x + 4 + col * cellW, 30 + row * h, cellW, h);
      body_.addAndMakeVisible(c);
      cells_.add(c);
      tallest = std::max(tallest, 30 + (row + 1) * h + 6);
      ++k;
    }
    x += columnW;
  }
  body_.setSize(std::max(x, viewW), std::max(tallest, viewport_.getMaximumVisibleHeight()));
  repaint();
}

void PanelView::paint(juce::Graphics& g) {
  g.fillAll(schemeColours(layout_.scheme, 0).background);
}

void PanelView::timerCallback() {
  for (auto* c : cells_) {
    if (auto* cell = dynamic_cast<Cell*>(c)) cell->follow();
    else if (auto* fader = dynamic_cast<Fader*>(c)) fader->follow();
  }
}

// ---- a panel in its own window -------------------------------------------------

PanelWindow::PanelWindow(const juce::String& title, PanelView& view)
    : juce::DocumentWindow(title, juce::Colour(0xff15171c), juce::DocumentWindow::allButtons) {
  setUsingNativeTitleBar(true);
  setResizable(true, true);
  setContentNonOwned(&view, false);
  const auto& l = view.layout();
  if (l.w > 100 && l.h > 100) setBounds(l.x, l.y, l.w, l.h);
  else centreWithSize(1024, 600);
}

void PanelWindow::closeButtonPressed() {
  if (onClose) onClose();
}

}  // namespace mp::ui
