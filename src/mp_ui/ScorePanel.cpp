#include "ScorePanel.h"
#include "MpScoreFonts.h"
#include "Mobile.h"
#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_audio/DataFolder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mp::ui {

namespace {
constexpr int kBar = 34;
constexpr int kMargin = 8;
constexpr int kGap = 8;     // between two pages side by side
constexpr int kScale = 70;  // the notation at 100%, in percent of Verovio's own size
constexpr int kEngraveDelayMs = 200;
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
}  // namespace

std::unique_ptr<juce::Drawable> parseScorePage(const std::string& svg, ScorePageGeometry& geometry) {
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
    // JUCE finds a typeface made this way by name only while it is alive.
    static std::vector<juce::Typeface::Ptr> faces;
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
    return all && !faces.empty();
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
      addAndMakeVisible(row->label);
      addAndMakeVisible(row->box);
      rows_.push_back(std::move(row));
    }
    setSize(380, static_cast<int>(rows_.size()) * 30 + 12);
  }

  void resized() override {
    auto r = getLocalBounds().reduced(6);
    for (auto& row : rows_) {
      auto line = r.removeFromTop(30).reduced(0, 3);
      row->label.setBounds(line.removeFromLeft(150));
      row->box.setBounds(line);
    }
  }

private:
  struct Row {
    juce::Label label;
    juce::ComboBox box;
  };
  juce::Component::SafePointer<ScorePanel> panel_;
  std::vector<std::unique_ptr<Row>> rows_;
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
  for (auto* b : {&open_, &prev_, &next_, &zoomOut_, &zoomIn_, &zoomReset_, &twoUp_, &play_, &manuals_, &invert_})
    addAndMakeVisible(b);
  addChildComponent(detach_);
  addAndMakeVisible(pageLabel_);
  pageLabel_.setJustificationType(juce::Justification::centred);
  pageLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  for (auto* b : {&twoUp_, &invert_}) {
    b->setClickingTogglesState(true);
    b->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff4a5a78));  // as the panels' section buttons
  }

  open_.onClick = [this] { chooseFile(); };
  prev_.onClick = [this] { showPage(page_ - slotCount()); };
  next_.onClick = [this] { showPage(page_ + slotCount()); };
  zoomOut_.onClick = [this] { setZoom(std::min(zoom_, drawnZoom_) - 10); };
  zoomIn_.onClick = [this] { setZoom(zoom_ + 10); };
  zoomReset_.onClick = [this] { setZoom(100); };
  twoUp_.onClick = [this] { setTwoPages(twoUp_.getToggleState()); };
  invert_.onClick = [this] { setInverted(invert_.getToggleState()); };
  play_.onClick = [this] { playing() ? stop() : play(); };
  manuals_.onClick = [this] { chooseManuals(); };
  detach_.onClick = [this] { if (onDetachToggle) onDetachToggle(); };
  refreshButtons();
  setSize(900, 700);
}

ScorePanel::~ScorePanel() {
  stopTimer();
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
  detach_.setButtonText(detached ? TRANS("Attach") : TRANS("Detach"));
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
  return openData(bytes, file.getFileName());
}

namespace {
// A compressed MusicXML file is a zip whose META-INF/container.xml names the
// score; without one, the first .musicxml or .xml outside META-INF.
std::string scoreFromZip(const juce::MemoryBlock& bytes) {
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
  return index >= 0 ? read(index) : std::string();
}
}  // namespace

bool ScorePanel::openData(const juce::MemoryBlock& bytes, const juce::String& name) {
  stop();
  loaded_ = false;
  for (auto& s : shown_) s = {};
  notes_.clear();
  staves_.clear();
  noteStartMs_.clear();
  cursorNotes_.clear();
  startMs_ = 0;
  page_ = 1;
  if (engraver_ == nullptr) {
    refreshButtons();
    repaint();
    return false;
  }
  const bool zipped = bytes.getSize() > 4 && static_cast<const char*>(bytes.getData())[0] == 'P' &&
                      static_cast<const char*>(bytes.getData())[1] == 'K';
  const std::string text = zipped ? scoreFromZip(bytes)
                                  : std::string(static_cast<const char*>(bytes.getData()), bytes.getSize());
  if (text.empty()) {
    message_ = TRANS("No score was found in this file.");
  } else {
    loaded_ = engraver_->load(text);
    message_ = loaded_ ? name : juce::String::fromUTF8(engraver_->error().c_str());
  }
  if (loaded_) {
    readTimeline();
    engrave();
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
    for (const auto& entry : *entries)
      if (const auto* on = entry["on"].getArray())
        for (const auto& id : *on) noteStartMs_.emplace(id.toString().toStdString(), static_cast<double>(entry["tstamp"]));
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
  twoUp_.setBounds(bar.removeFromLeft(86));
  bar.removeFromLeft(8);
  play_.setBounds(bar.removeFromLeft(64));
  manuals_.setBounds(bar.removeFromLeft(90));
  if (detach_.isVisible()) detach_.setBounds(bar.removeFromRight(76));
  invert_.setBounds(bar.removeFromRight(70));
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
  engraver_->setPage(area.getWidth(), area.getHeight(), scale);
  // And one whole system from top to bottom: a taller one would run off the
  // page, so the scale steps down until the first page's systems fit.
  for (int tries = 0; tries < 4 && scale > 10; ++tries) {
    ScorePageGeometry first;
    parseScorePage(engraver_->renderPage(1), first);
    float bottom = 0;
    for (const auto& sys : first.systems) bottom = std::max(bottom, sys.area.getBottom() + sys.lineGap * 2.0f);
    if (first.height <= 0 || bottom <= first.height) break;
    scale = std::max(10, static_cast<int>(scale * first.height / bottom * 0.97f));
    engraver_->setPage(area.getWidth(), area.getHeight(), scale);
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
  const int p = at.empty() ? 1 : engraver_->pageWithElement(at);
  page_ = std::clamp(p > 0 ? p : page_, 1, std::max(1, pageCount()));
  if (twoPages_ && page_ > 1 && page_ == pageCount() && pageCount() % 2 == 0) --page_;
  renderShown();
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
    s.drawable = parseScorePage(engraver_->renderPage(p), s.geometry);
    s.image = {};
  }
  refreshButtons();
  repaint();
}

void ScorePanel::showPage(int page) {
  if (!loaded_) return;
  page = std::clamp(page, 1, std::max(1, pageCount()));
  if (page == page_ && shown_[0].page == page) return;
  page_ = page;
  renderShown();
  // Turned by hand while stopped: playing starts from the top of the page.
  if (!playing()) {
    const double ms = firstNoteMsOnPage(0);
    if (ms >= 0) setStart(ms / 1000.0);
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
}

void ScorePanel::setZoom(int percent) {
  const int z = std::clamp(percent, 50, 250);
  if (z == zoom_) return;
  zoom_ = z;
  if (loaded_) engrave();
  refreshButtons();
}

void ScorePanel::setInverted(bool inverted) {
  inverted_ = inverted;
  invert_.setToggleState(inverted, juce::dontSendNotification);
  for (auto& s : shown_) s.image = {};
  repaint();
}

void ScorePanel::refreshButtons() {
  const int count = pageCount();
  prev_.setEnabled(loaded_ && page_ > 1);
  next_.setEnabled(loaded_ && page_ + slotCount() <= count);
  pageLabel_.setText(loaded_ ? juce::String(page_) + " / " + juce::String(count) : juce::String(),
                     juce::dontSendNotification);
  for (auto* b : {&zoomOut_, &zoomIn_, &zoomReset_, &twoUp_, &play_, &manuals_}) b->setEnabled(loaded_);
  zoomReset_.setButtonText(juce::String(loaded_ ? drawnZoom_ : zoom_) + "%");
  // Stopped by the page's size: a larger zoom would draw the same page.
  if (loaded_ && drawnZoom_ < zoom_) zoomIn_.setEnabled(false);
  play_.setButtonText(playing() ? TRANS("Stop") : TRANS("Play"));
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
  if (playing()) {
    startMs_ = proc_.scorePlayer().positionSeconds() * 1000.0;
    play();
  }
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
  for (const auto& n : notes_) {
    const int ch = channels[n.staff];
    if (ch < 1 || ch > 16) continue;
    const auto c = static_cast<uint8_t>(ch), p = static_cast<uint8_t>(n.pitch & 127);
    events.push_back({n.on, c, p, true});
    events.push_back({std::max(n.off, n.on), c, p, false});
  }
  ScorePlayer::orderEvents(events);
  return events;
}

void ScorePanel::play() {
  if (!loaded_) return;
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
  repaint();
}

void ScorePanel::followPlaying() {
  auto& player = proc_.scorePlayer();
  if (player.isPlaying()) {
    const auto at = juce::JSON::parse(
        juce::String::fromUTF8(engraver_->elementsAtTime(static_cast<int>(player.positionSeconds() * 1000.0)).c_str()));
    std::vector<std::string> ids;
    if (const auto* notes = at["notes"].getArray())
      for (const auto& id : *notes) ids.push_back(id.toString().toStdString());
    const int page = static_cast<int>(at["page"]);
    // The page turns as the playing reaches the next one.
    if (page >= 1 && (page < page_ || page >= page_ + slotCount())) {
      page_ = std::clamp(page, 1, std::max(1, pageCount()));
      renderShown();
    }
    if (!ids.empty() && ids != cursorNotes_) {
      cursorNotes_ = std::move(ids);
      repaint();
    }
    return;
  }
  if (wasPlaying_) {
    wasPlaying_ = false;
    // Played to the end: the next Play starts from the top.
    if (player.positionSeconds() >= player.lengthSeconds() - 0.001) setStart(0);
    else setStart(player.positionSeconds());
    refreshButtons();
  }
}

void ScorePanel::timerCallback() {
  if (needsEngrave_ && juce::Time::getMillisecondCounter() >= engraveDue_) engrave();
  if (loaded_) followPlaying();
  if (!needsEngrave_ && !wasPlaying_) stopTimer();
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
  return juce::RectanglePlacement(juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize)
      .getTransformToFit({0, 0, geo.width, geo.height}, slotArea(slot).toFloat());
}

void ScorePanel::drawSlot(juce::Graphics& g, int slot) {
  auto& s = shown_[slot];
  if (s.drawable == nullptr) return;
  const float density = std::max(1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
  const auto bounds = juce::Rectangle<float>(0, 0, s.geometry.width, s.geometry.height).transformedBy(pageToPanel(slot));
  const int w = juce::roundToInt(bounds.getWidth() * density), h = juce::roundToInt(bounds.getHeight() * density);
  if (w <= 0 || h <= 0) return;
  if (!s.image.isValid() || s.image.getWidth() != w || s.image.getHeight() != h || s.density != density) {
    s.image = juce::Image(juce::Image::RGB, w, h, true);
    s.density = density;
    {
      juce::Graphics ig(s.image);
      ig.fillAll(juce::Colours::white);
      // The whole page onto the image, by the page's own size: drawWithin
      // would fit the ink, and the notes would leave the geometry's places.
      s.drawable->draw(ig, 1.0f,
                       juce::AffineTransform::scale(static_cast<float>(w) / s.geometry.width,
                                                    static_cast<float>(h) / s.geometry.height));
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
}

void ScorePanel::mouseDown(const juce::MouseEvent& e) {
  if (!loaded_) return;
  // A click on a note: playing starts there.
  const auto at = e.position;
  std::string best;
  float bestDistance = 14.0f;
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
  if (best.empty()) return;
  const double ms = noteStartMs_[best];
  if (playing()) {
    startMs_ = ms;
    play();
  } else {
    setStart(ms / 1000.0);
  }
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
