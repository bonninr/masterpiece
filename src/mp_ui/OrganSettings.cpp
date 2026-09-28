#include "OrganSettings.h"

#include <unordered_set>

namespace mp::ui {

namespace {

const juce::Colour kBackground{0xff1b1e24};
const juce::Colour kText{0xffe6e9ef};
const juce::Colour kTextDim{0xff7d8594};
constexpr int kRowH = 24;

juce::String megabytes(int64_t bytes) {
  const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  if (mb >= 1024.0) return juce::String(mb / 1024.0, 1) + " GB";
  return juce::String(juce::roundToInt(mb)) + " MB";
}

}  // namespace

// ------------------------------------------------------------ StopsLoadPanel

StopsLoadPanel::StopsLoadPanel(MasterpieceProcessor& p, std::function<void()> reload)
    : proc_(p), reload_(std::move(reload)) {
  for (auto* b : {&all_, &none_, &drawn_}) addAndMakeVisible(*b);
  all_.onClick = [this] { choose({}); };
  // None and "drawn now" are about the stops a player registers with; the
  // noises and effects keep whatever they are set to.
  none_.onClick = [this] {
    std::set<Id> out;
    for (const auto& r : rows_)
      if (!r.effect || !r.toggle->getToggleState()) out.insert(r.stopId);
    choose(std::move(out));
  };
  drawn_.setTooltip("Load only the stops drawn on the console at this moment");
  drawn_.onClick = [this] {
    std::set<Id> out;
    for (const auto& r : rows_)
      if (r.effect ? !r.toggle->getToggleState() : !proc_.stopEngaged(r.stopId))
        out.insert(r.stopId);
    choose(std::move(out));
  };

  addAndMakeVisible(viewport_);
  viewport_.setViewedComponent(&list_, false);
  viewport_.setScrollBarsShown(true, false);

  addAndMakeVisible(total_);
  total_.setColour(juce::Label::textColourId, kText);
  addAndMakeVisible(reloadNow_);
  reloadNow_.onClick = [this] {
    if (reload_) reload_();
  };
  addAndMakeVisible(note_);
  note_.setColour(juce::Label::textColourId, kTextDim);
  note_.setJustificationType(juce::Justification::topLeft);
  note_.setText("A stop left out costs no memory and makes no sound. It keeps its "
                "drawstop, dimmed, and its place in pistons. The choice is saved for "
                "this organ and takes effect when it is loaded again. Figures are "
                "estimates from the sample files, at the settings on the Loading tab.",
                juce::dontSendNotification);

  build();
  startEstimate();
  startTimerHz(2);
}

StopsLoadPanel::~StopsLoadPanel() {
  stopTimer();
  alive_->store(false);
}

void StopsLoadPanel::build() {
  rows_.clear();
  headings_.clear();
  list_.removeAllChildren();
  const auto& excluded = proc_.excludedStops();
  const auto& divisions = proc_.organModel().divisions;
  auto heading = [&](const juce::String& text) {
    Heading h;
    h.beforeRow = static_cast<int>(rows_.size());
    h.label = std::make_unique<juce::Label>();
    h.label->setText(text, juce::dontSendNotification);
    h.label->setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    h.label->setColour(juce::Label::textColourId, juce::Colours::orange);
    list_.addAndMakeVisible(*h.label);
    headings_.push_back(std::move(h));
  };
  // The drawstops first, by division; then what the set keeps in stops
  // nobody draws -- key-action noises, coupler and tremulant effects.
  std::vector<MasterpieceProcessor::StopEntry> drawnStops, effects;
  for (const auto& s : proc_.stopList())
    (proc_.stopDrawn(s.stopId) ? drawnStops : effects).push_back(s);
  std::vector<std::pair<const MasterpieceProcessor::StopEntry*, bool>> order;
  for (const auto& s : drawnStops) order.push_back({&s, false});
  for (const auto& s : effects) order.push_back({&s, true});

  Id lastDivision = -1;
  bool inEffects = false;
  for (const auto& [entry, effect] : order) {
    const auto& s = *entry;
    if (effect && !inEffects) {
      heading("Effects and noises");
      inEffects = true;
    } else if (!effect && s.divisionId != lastDivision) {
      const auto it = divisions.find(s.divisionId);
      heading(it != divisions.end() && !it->second.name.empty()
                  ? juce::String(it->second.name)
                  : "Division " + juce::String(s.divisionId));
      lastDivision = s.divisionId;
    }
    Row row;
    row.stopId = s.stopId;
    row.effect = effect;
    row.toggle = std::make_unique<juce::ToggleButton>(juce::String(s.name));
    row.toggle->setToggleState(excluded.count(s.stopId) == 0, juce::dontSendNotification);
    row.toggle->setEnabled(s.playable);
    if (!s.playable) row.toggle->setTooltip("This stop's ranks ship no pipes in this sample set");
    row.toggle->onClick = [this] {
      std::set<Id> out;
      for (const auto& r : rows_)
        if (!r.toggle->getToggleState()) out.insert(r.stopId);
      proc_.setExcludedStops(std::move(out));
      refreshFigures();
    };
    row.size = std::make_unique<juce::Label>();
    row.size->setJustificationType(juce::Justification::centredRight);
    row.size->setColour(juce::Label::textColourId, kTextDim);
    row.samples = proc_.samplesOfStop(s.stopId);
    list_.addAndMakeVisible(*row.toggle);
    list_.addAndMakeVisible(*row.size);
    rows_.push_back(std::move(row));
  }
  resized();
  refreshFigures();
}

void StopsLoadPanel::choose(std::set<Id> excluded) {
  for (auto& r : rows_)
    r.toggle->setToggleState(excluded.count(r.stopId) == 0, juce::dontSendNotification);
  proc_.setExcludedStops(std::move(excluded));
  refreshFigures();
}

void StopsLoadPanel::startEstimate() {
  // The jobs are taken from the model here, on the message thread; the worker
  // only reads files, so a load starting meanwhile cannot pull the model out
  // from under it.
  auto jobs = std::make_shared<std::vector<SampleLibrary::ShapeJob>>(proc_.sampleShapeJobs());
  auto alive = alive_;
  juce::Component::SafePointer<StopsLoadPanel> safe(this);
  juce::Thread::launch([jobs, alive, safe] {
    auto shapes = std::make_shared<std::unordered_map<Id, SampleLibrary::SampleShape>>();
    shapes->reserve(jobs->size());
    for (const auto& job : *jobs) {
      if (!alive->load()) return;
      (*shapes)[job.sampleId] = SampleLibrary::readShape(job);
    }
    juce::MessageManager::callAsync([shapes, safe] {
      if (safe == nullptr) return;
      safe->shapes_ = std::move(*shapes);
      safe->ready_ = true;
      safe->refreshFigures();
    });
  });
}

void StopsLoadPanel::refreshFigures() {
  std::set<Id> loaded;  // what the last load left out
  for (const auto& r : rows_)
    if (!proc_.stopLoaded(r.stopId)) loaded.insert(r.stopId);
  reloadNow_.setEnabled(loaded != proc_.excludedStops() && !proc_.loadedOrganFile().getFullPathName().isEmpty());

  if (!ready_) {
    total_.setText("Estimating the memory each stop takes...", juce::dontSendNotification);
    return;
  }
  auto bytesOf = [this](Id sampleId) -> int64_t {
    const auto it = shapes_.find(sampleId);
    return it == shapes_.end() ? 0 : proc_.residentBytesFor(it->second);
  };
  int64_t all = 0;
  for (const auto& [id, shape] : shapes_) {
    (void)shape;
    all += bytesOf(id);
  }
  // Everything, less what only left-out stops use: a sample shared with a
  // stop that stays is loaded anyway, and so is anything no stop owns.
  std::unordered_set<Id> kept;
  for (const auto& r : rows_)
    if (r.toggle->getToggleState()) kept.insert(r.samples.begin(), r.samples.end());
  std::unordered_set<Id> dropped;
  for (const auto& r : rows_) {
    int64_t mine = 0;
    for (Id id : r.samples) mine += bytesOf(id);
    r.size->setText(megabytes(mine), juce::dontSendNotification);
    if (!r.toggle->getToggleState())
      for (Id id : r.samples)
        if (kept.count(id) == 0) dropped.insert(id);
  }
  int64_t total = all;
  for (Id id : dropped) total -= bytesOf(id);

  const int64_t limit = proc_.memoryLimitBytes();
  juce::String text = "About " + megabytes(total) + " for this choice";
  if (limit > 0) text << " (the limit is " << megabytes(limit) << ")";
  total_.setText(text, juce::dontSendNotification);
  total_.setColour(juce::Label::textColourId,
                   limit > 0 && total > limit ? juce::Colour(0xffe06c5c) : kText);
}

void StopsLoadPanel::paint(juce::Graphics& g) { g.fillAll(kBackground); }

void StopsLoadPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  auto top = r.removeFromTop(28);
  all_.setBounds(top.removeFromLeft(60).reduced(0, 2));
  top.removeFromLeft(6);
  none_.setBounds(top.removeFromLeft(60).reduced(0, 2));
  top.removeFromLeft(6);
  drawn_.setBounds(top.removeFromLeft(180).reduced(0, 2));
  r.removeFromTop(8);

  note_.setBounds(r.removeFromBottom(52));
  r.removeFromBottom(6);
  auto footer = r.removeFromBottom(28);
  reloadNow_.setBounds(footer.removeFromRight(180).reduced(0, 2));
  total_.setBounds(footer);
  r.removeFromBottom(6);
  viewport_.setBounds(r);

  const int width = juce::jmax(200, r.getWidth() - viewport_.getScrollBarThickness());
  int y = 0;
  size_t h = 0;
  for (size_t i = 0; i < rows_.size(); ++i) {
    while (h < headings_.size() && headings_[h].beforeRow == static_cast<int>(i)) {
      headings_[h++].label->setBounds(0, y, width, kRowH);
      y += kRowH;
    }
    rows_[i].toggle->setBounds(8, y, width - 100, kRowH);
    rows_[i].size->setBounds(width - 92, y, 88, kRowH);
    y += kRowH;
  }
  list_.setSize(width, y);
}

// -------------------------------------------------------- OrganSettingsWindow

OrganSettingsWindow::OrganSettingsWindow(MasterpieceProcessor& p, std::function<void()> reload)
    : reload_(std::move(reload)), engine_(p), stops_(p, [this] { reloadOnClose(); }) {
  addAndMakeVisible(tabs_);
  tabs_.addTab("Loading", kBackground, &engineScroll_, false);
  tabs_.addTab("Stops", kBackground, &stops_, false);
  setSize(660, 560);
}

OrganSettingsWindow::~OrganSettingsWindow() {
  // Posted, not called: the window is still being torn down here.
  if (onClosed) juce::MessageManager::callAsync(std::move(onClosed));
  else if (reloadOnClose_ && reload_) juce::MessageManager::callAsync(std::move(reload_));
}

void OrganSettingsWindow::reloadOnClose() {
  reloadOnClose_ = true;
  if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) dialog->closeButtonPressed();
}

void OrganSettingsWindow::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff15171c)); }
void OrganSettingsWindow::resized() { tabs_.setBounds(getLocalBounds()); }

}  // namespace mp::ui
