#include "ScorePanel.h"
#include "MpScoreFonts.h"
#include "Mobile.h"

#include <algorithm>

namespace mp::ui {

namespace {
constexpr int kBar = 34;
constexpr int kMargin = 8;
constexpr int kScale = 70;  // percent of Verovio's natural size
const juce::Colour kBackground{0xff1b1e24};

// Verovio nests its drawing in a second <svg class="definition-scale"> element
// that carries the viewBox, the text font and the colour. JUCE's SVG reader
// gives such a page no bounds and draws nothing, and does the same for a
// font-family list on a group. The nested element is turned into a plain
// group and its viewBox moved to the root, which JUCE draws correctly. The
// line colour comes from a rule in the page's own <style> element, which
// JUCE applies.
std::unique_ptr<juce::Drawable> drawableFromSvg(const std::string& svg) {
  const auto doc = juce::XmlDocument::parse(juce::String::fromUTF8(svg.c_str()));
  if (doc == nullptr) return nullptr;
  if (auto* inner = doc->getChildByName("svg")) {
    doc->setAttribute("viewBox", inner->getStringAttribute("viewBox"));
    inner->setTagName("g");
    inner->removeAttribute("viewBox");
    inner->removeAttribute("font-family");
  }
  return juce::Drawable::createFromSVGString(doc->toString());
}
}  // namespace

bool unpackScoreFonts(const juce::File& folder) {
  folder.deleteRecursively();
  if (!folder.createDirectory().wasOk()) return false;
  juce::MemoryInputStream in(MpScoreFonts::fonts_zip, static_cast<size_t>(MpScoreFonts::fonts_zipSize),
                             false);
  juce::ZipFile zip(in);
  if (!zip.uncompressTo(folder, true).wasOk()) return false;
  // The marker is written last: a folder without it was cut short and is
  // unpacked again.
  return folder.getChildFile(".complete").create().wasOk();
}

juce::File scoreResourceFolder() {
  const auto folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("Masterpiece")
                          .getChildFile("Score fonts");
  if (folder.getChildFile(".complete").existsAsFile()) return folder;
  return unpackScoreFonts(folder) ? folder : juce::File();
}

ScorePanel::ScorePanel(const juce::File& fonts) {
  const auto folder = fonts != juce::File() ? fonts : scoreResourceFolder();
  if (folder != juce::File())
    engraver_ = std::make_unique<mp::ScoreEngraver>(folder.getFullPathName().toStdString());
  if (engraver_ == nullptr || !engraver_->ready()) {
    message_ = "The score fonts could not be prepared.";
    engraver_.reset();
  } else {
    message_ = "Open a MusicXML score (.musicxml, .xml or .mxl).";
  }
  addAndMakeVisible(open_);
  addAndMakeVisible(prev_);
  addAndMakeVisible(next_);
  addAndMakeVisible(status_);
  status_.setJustificationType(juce::Justification::centred);
  status_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
  open_.onClick = [this] { chooseFile(); };
  prev_.onClick = [this] { showPage(page_ - 1); };
  next_.onClick = [this] { showPage(page_ + 1); };
  prev_.setEnabled(false);
  next_.setEnabled(false);
  setSize(900, 700);
}

ScorePanel::~ScorePanel() { stopTimer(); }

int ScorePanel::pageCount() const { return engraver_ != nullptr ? engraver_->pageCount() : 0; }

void ScorePanel::chooseFile() {
  chooser_ = std::make_unique<juce::FileChooser>("Open a score", juce::File(),
                                                 "*.musicxml;*.xml;*.mxl");
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
    message_ = "The file could not be read.";
    loaded_ = false;
    drawable_.reset();
    repaint();
    return false;
  }
  return openData(bytes, file.getFileName());
}

bool ScorePanel::openData(const juce::MemoryBlock& bytes, const juce::String& name) {
  drawable_.reset();
  loaded_ = false;
  page_ = 1;
  if (engraver_ == nullptr) {
    repaint();
    return false;
  }
  const std::string text(static_cast<const char*>(bytes.getData()), bytes.getSize());
  loaded_ = engraver_->load(text);
  message_ = loaded_ ? name : juce::String(engraver_->error());
  if (loaded_) engrave();
  prev_.setEnabled(false);
  next_.setEnabled(loaded_ && pageCount() > 1);
  repaint();
  return loaded_;
}

juce::Rectangle<int> ScorePanel::pageArea() const {
  return getLocalBounds().withTrimmedTop(kBar + kMargin).reduced(kMargin);
}

void ScorePanel::engrave() {
  stopTimer();
  drawable_.reset();
  if (!loaded_ || engraver_ == nullptr) return;
  const auto area = pageArea();
  engraver_->setPage(area.getWidth(), area.getHeight(), kScale);
  page_ = std::clamp(page_, 1, std::max(1, engraver_->pageCount()));
  const auto svg = engraver_->renderPage(page_);
  if (!svg.empty()) drawable_ = drawableFromSvg(svg);
  status_.setText(juce::String(page_) + " / " + juce::String(engraver_->pageCount()),
                  juce::dontSendNotification);
  prev_.setEnabled(page_ > 1);
  next_.setEnabled(page_ < engraver_->pageCount());
  repaint();
}

void ScorePanel::showPage(int page) {
  if (!loaded_) return;
  page_ = std::clamp(page, 1, std::max(1, pageCount()));
  engrave();
}

void ScorePanel::resized() {
  auto bar = getLocalBounds().removeFromTop(kBar).reduced(kMargin, 4);
  open_.setBounds(bar.removeFromLeft(130));
  next_.setBounds(bar.removeFromRight(40));
  bar.removeFromRight(4);
  prev_.setBounds(bar.removeFromRight(40));
  status_.setBounds(bar);
  // Dragging an edge resizes many times a second; engrave once it settles.
  if (loaded_) startTimer(200);
}

void ScorePanel::timerCallback() { engrave(); }

juce::Image ScorePanel::render() {
  if (isTimerRunning()) engrave();
  juce::Image image(juce::Image::ARGB, std::max(1, getWidth()), std::max(1, getHeight()), true);
  juce::Graphics g(image);
  paint(g);
  return image;
}

void ScorePanel::paint(juce::Graphics& g) {
  g.fillAll(kBackground);
  const auto area = pageArea();
  g.setColour(juce::Colours::white);
  g.fillRect(area);
  if (drawable_ != nullptr) {
    drawable_->drawWithin(g, area.toFloat(),
                          juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yTop |
                              juce::RectanglePlacement::onlyReduceInSize,
                          1.0f);
  } else {
    g.setColour(juce::Colours::dimgrey);
    g.setFont(juce::FontOptions(15.0f));
    g.drawFittedText(message_, area.reduced(20), juce::Justification::centred, 4);
  }
}

namespace {
class ScoreWindow : public juce::DocumentWindow {
public:
  ScoreWindow() : juce::DocumentWindow("Score", kBackground, juce::DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    setContentOwned(new ScorePanel, true);
    setResizable(true, true);
    centreWithSize(900, 700);
    fitToScreen(*this);
  }
  void closeButtonPressed() override { setVisible(false); }
};
}  // namespace

void toggleScoreWindow(std::unique_ptr<juce::DocumentWindow>& slot, juce::Rectangle<int> besideThis) {
  if (slot == nullptr) {
    slot = std::make_unique<ScoreWindow>();
    if (!kMobile) slot->setTopLeftPosition(besideThis.getX() + 40, besideThis.getY() + 60);
  }
  slot->setVisible(!slot->isVisible());
  if (slot->isVisible()) slot->toFront(true);
}

}  // namespace mp::ui
