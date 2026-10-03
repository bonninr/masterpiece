#include "OrganSettings.h"

#include <algorithm>
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

  addAndMakeVisible(filter_);
  filter_.setTextToShowWhenEmpty("Filter, for example: Rear, tremmed, Pedal", kTextDim);
  filter_.setTooltip("Shows only the perspectives, stops and ranks whose name holds this text");
  filter_.onTextChange = [this] { applyFilter(); };
  for (auto* b : {&loadShown_, &leaveShown_}) addAndMakeVisible(*b);
  loadShown_.onClick = [this] { setShownRanks(true); };
  leaveShown_.onClick = [this] { setShownRanks(false); };
  leaveShown_.setTooltip("Leave out every rank in the list as it is filtered now");

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
  note_.setText("A perspective left out takes every rank recorded from that position; the "
                "stops still play from the others. A stop left out costs no memory and makes "
                "no sound, and keeps its drawstop, dimmed. Both are saved for this organ and "
                "take effect when it is loaded again. Figures are estimates from the sample "
                "files, at the settings on the Loading tab.",
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
  perspectives_.clear();
  perspectivesHeading_.reset();
  ranks_.clear();
  ranksHeading_.reset();
  list_.removeAllChildren();

  // Perspectives first, when the set has them: on a set recorded from three
  // or four places they are what decides whether it fits.
  const auto groups = proc_.perspectives();
  if (!groups.empty()) {
    perspectivesHeading_ = std::make_unique<juce::Label>();
    perspectivesHeading_->setText("Perspectives", juce::dontSendNotification);
    perspectivesHeading_->setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    perspectivesHeading_->setColour(juce::Label::textColourId, juce::Colours::orange);
    list_.addAndMakeVisible(*perspectivesHeading_);
    for (const auto& [name, ranks] : groups) {
      PerspectiveRow row;
      row.name = name;
      row.ranks.assign(ranks.begin(), ranks.end());
      juce::String label(juce::CharPointer_UTF8(name.c_str()));
      label = label.substring(0, 1).toUpperCase() + label.substring(1) + "  (" +
              juce::String(static_cast<int>(ranks.size())) + " ranks)";
      row.toggle = std::make_unique<juce::ToggleButton>(label);
      row.toggle->setToggleState(proc_.excludedPerspectives().count(name) == 0,
                                 juce::dontSendNotification);
      row.toggle->setTooltip("Every rank recorded from this position. Left out, it costs "
                             "no memory; the stops still play from the other positions.");
      row.toggle->onClick = [this, clicked = row.toggle.get()] {
        // At least one position stays: without any, the organ is silent.
        if (perspectivesOut().size() == perspectives_.size())
          clicked->setToggleState(true, juce::dontSendNotification);
        proc_.setExcludedPerspectives(perspectivesOut());
        refreshFigures();
      };
      row.size = std::make_unique<juce::Label>();
      row.size->setJustificationType(juce::Justification::centredRight);
      row.size->setColour(juce::Label::textColourId, kTextDim);
      row.samples = proc_.samplesOfRanks(ranks);
      list_.addAndMakeVisible(*row.toggle);
      list_.addAndMakeVisible(*row.size);
      perspectives_.push_back(std::move(row));
    }
  }

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

  // Every rank last, by name, each saying which stops play it.
  const auto& model = proc_.organModel();
  std::unordered_map<Id, juce::StringArray> stopsOfRank;
  std::unordered_map<Id, std::vector<Id>> stopIdsOfRank;
  for (const auto& [stopId, stop] : model.stops)
    for (const auto& e : stop.ranks) {
      stopsOfRank[e.rankId].addIfNotAlreadyThere(juce::String(stop.name));
      stopIdsOfRank[e.rankId].push_back(stopId);
    }
  std::vector<std::pair<juce::String, Id>> byName;
  for (const auto& [rankId, rank] : model.ranks)
    byName.push_back({juce::String(juce::CharPointer_UTF8(rank.name.c_str())), rankId});
  std::sort(byName.begin(), byName.end(), [](const auto& a, const auto& b) {
    return a.first.compareNatural(b.first) < 0;
  });
  if (!byName.empty()) {
    ranksHeading_ = std::make_unique<juce::Label>();
    ranksHeading_->setText("Ranks", juce::dontSendNotification);
    ranksHeading_->setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    ranksHeading_->setColour(juce::Label::textColourId, juce::Colours::orange);
    list_.addAndMakeVisible(*ranksHeading_);
  }
  const auto& ranksOutNow = proc_.excludedRanks();
  for (const auto& [name, rankId] : byName) {
    RankRow row;
    row.rankId = rankId;
    row.own = ranksOutNow.count(rankId) == 0;
    if (const auto it = stopIdsOfRank.find(rankId); it != stopIdsOfRank.end()) row.stops = it->second;
    row.toggle = std::make_unique<juce::ToggleButton>(name.isEmpty() ? "Rank " + juce::String(rankId) : name);
    row.toggle->setToggleState(row.own, juce::dontSendNotification);
    const auto users = stopsOfRank.find(rankId);
    const juce::String playedBy =
        users == stopsOfRank.end()
            ? juce::String("Played by no stop: a noise, or wired to a key directly")
            : "Played by " + users->second.joinIntoString(", ");
    row.toggle->setTooltip(playedBy);
    row.toggle->getProperties().set("playedBy", playedBy);
    row.toggle->onClick = [this, rankId] {
      for (auto& r : ranks_)
        if (r.rankId == rankId) r.own = r.toggle->getToggleState();
      proc_.setExcludedRanks(ranksOut());
      refreshFigures();
    };
    row.size = std::make_unique<juce::Label>();
    row.size->setJustificationType(juce::Justification::centredRight);
    row.size->setColour(juce::Label::textColourId, kTextDim);
    row.samples = proc_.samplesOfRanks({rankId});
    list_.addAndMakeVisible(*row.toggle);
    list_.addAndMakeVisible(*row.size);
    ranks_.push_back(std::move(row));
  }
  applyFilter();
  refreshFigures();
}

bool StopsLoadPanel::shown(const juce::String& name) const {
  const auto text = filter_.getText().trim();
  return text.isEmpty() || name.containsIgnoreCase(text);
}

void StopsLoadPanel::applyFilter() {
  const bool filtering = filter_.getText().trim().isNotEmpty();
  for (auto& p : perspectives_) {
    const bool on = shown(p.toggle->getButtonText());
    p.toggle->setVisible(on);
    p.size->setVisible(on);
  }
  for (auto& r : rows_) {
    const bool on = shown(r.toggle->getButtonText());
    r.toggle->setVisible(on);
    r.size->setVisible(on);
  }
  bool anyRank = false;
  for (auto& r : ranks_) {
    const bool on = shown(r.toggle->getButtonText());
    r.toggle->setVisible(on);
    r.size->setVisible(on);
    anyRank = anyRank || on;
  }
  // Division headings mean nothing in a filtered list; the Ranks one still does.
  for (auto& h : headings_) h.label->setVisible(!filtering);
  if (perspectivesHeading_ != nullptr) perspectivesHeading_->setVisible(!filtering);
  if (ranksHeading_ != nullptr) ranksHeading_->setVisible(anyRank);
  loadShown_.setEnabled(anyRank);
  leaveShown_.setEnabled(anyRank);
  resized();
}

void StopsLoadPanel::setShownRanks(bool load) {
  std::set<Id> out = ranksOut();
  for (auto& r : ranks_) {
    if (!r.toggle->isVisible()) continue;
    r.own = load;
    if (load) out.erase(r.rankId);
    else out.insert(r.rankId);
  }
  proc_.setExcludedRanks(std::move(out));
  refreshFigures();
}

std::set<Id> StopsLoadPanel::ranksOut() const {
  std::set<Id> out;
  for (const auto& r : ranks_)
    if (!r.own) out.insert(r.rankId);
  return out;
}

std::set<std::string> StopsLoadPanel::perspectivesOut() const {
  std::set<std::string> out;
  for (const auto& p : perspectives_)
    if (!p.toggle->getToggleState()) out.insert(p.name);
  return out;
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

// What each rank's tick shows: its own choice, unless something above it
// already leaves it out -- its perspective, or every stop that plays it. Then
// it is shown unticked and dimmed, saying why, and its own choice waits.
void StopsLoadPanel::showRankStates() {
  std::unordered_map<Id, std::string> byPerspective;
  for (const auto& p : perspectives_)
    if (!p.toggle->getToggleState())
      for (Id rankId : p.ranks) byPerspective[rankId] = p.name;
  std::unordered_set<Id> stopsOut;
  for (const auto& r : rows_)
    if (!r.toggle->getToggleState()) stopsOut.insert(r.stopId);
  for (auto& r : ranks_) {
    juce::String why;
    if (const auto it = byPerspective.find(r.rankId); it != byPerspective.end())
      why = "Left out with the " + juce::String(juce::CharPointer_UTF8(it->second.c_str())) +
            " perspective";
    else if (!r.stops.empty() &&
             std::all_of(r.stops.begin(), r.stops.end(),
                         [&](Id s) { return stopsOut.count(s) != 0; }))
      why = r.stops.size() == 1 ? "Left out with its stop" : "Left out with every stop that plays it";
    const bool covered = why.isNotEmpty();
    r.toggle->setEnabled(!covered);
    r.toggle->setToggleState(covered ? false : r.own, juce::dontSendNotification);
    r.toggle->setTooltip(covered ? why : r.toggle->getProperties()["playedBy"].toString());
  }
}

void StopsLoadPanel::refreshFigures() {
  showRankStates();
  std::set<Id> loaded;  // what the last load left out
  for (const auto& r : rows_)
    if (!proc_.stopLoaded(r.stopId)) loaded.insert(r.stopId);
  // Always available while an organ is loaded (#134), and marked when the
  // choice here differs from what is loaded.
  const bool pending = loaded != proc_.excludedStops() ||
                       perspectivesOut() != proc_.perspectivesLeftOut() ||
                       ranksOut() != proc_.ranksLeftOut();
  reloadNow_.setEnabled(!proc_.loadedOrganFile().getFullPathName().isEmpty());
  reloadNow_.setColour(juce::TextButton::buttonColourId,
                       pending ? juce::Colour(0xff8a5a12) : juce::Colour(0xff2a2f38));

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
  // Everything, less the perspectives left out, less what only left-out stops
  // use: a sample shared with a stop that stays is loaded anyway, and so is
  // anything no stop owns.
  std::unordered_set<Id> dropped;
  for (const auto& p : perspectives_) {
    int64_t mine = 0;
    for (Id id : p.samples) mine += bytesOf(id);
    p.size->setText(megabytes(mine), juce::dontSendNotification);
    if (!p.toggle->getToggleState()) dropped.insert(p.samples.begin(), p.samples.end());
  }
  std::unordered_set<Id> kept;
  for (const auto& r : rows_)
    if (r.toggle->getToggleState()) kept.insert(r.samples.begin(), r.samples.end());
  for (const auto& r : rows_) {
    int64_t mine = 0;
    for (Id id : r.samples) mine += bytesOf(id);
    r.size->setText(megabytes(mine), juce::dontSendNotification);
    if (!r.toggle->getToggleState())
      for (Id id : r.samples)
        if (kept.count(id) == 0) dropped.insert(id);
  }
  for (const auto& r : ranks_) {
    int64_t mine = 0;
    for (Id id : r.samples) mine += bytesOf(id);
    r.size->setText(megabytes(mine), juce::dontSendNotification);
    if (!r.toggle->getToggleState()) dropped.insert(r.samples.begin(), r.samples.end());
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
  all_.setBounds(top.removeFromLeft(90).reduced(0, 2));
  top.removeFromLeft(6);
  none_.setBounds(top.removeFromLeft(90).reduced(0, 2));
  top.removeFromLeft(6);
  drawn_.setBounds(top.removeFromLeft(210).reduced(0, 2));
  r.removeFromTop(6);
  auto find = r.removeFromTop(28);
  leaveShown_.setBounds(find.removeFromRight(170).reduced(0, 2));
  find.removeFromRight(6);
  loadShown_.setBounds(find.removeFromRight(140).reduced(0, 2));
  find.removeFromRight(6);
  filter_.setBounds(find.reduced(0, 2));
  r.removeFromTop(8);

  note_.setBounds(r.removeFromBottom(68));
  r.removeFromBottom(6);
  auto footer = r.removeFromBottom(28);
  reloadNow_.setBounds(footer.removeFromRight(180).reduced(0, 2));
  total_.setBounds(footer);
  r.removeFromBottom(6);
  viewport_.setBounds(r);

  const int width = juce::jmax(200, r.getWidth() - viewport_.getScrollBarThickness());
  int y = 0;
  // Hidden rows take no room, so a filtered list closes up.
  auto place = [&](juce::Component& toggle, juce::Component& size) {
    if (!toggle.isVisible()) return;
    toggle.setBounds(8, y, width - 100, kRowH);
    size.setBounds(width - 92, y, 88, kRowH);
    y += kRowH;
  };
  auto head = [&](juce::Label* label) {
    if (label == nullptr || !label->isVisible()) return;
    label->setBounds(0, y, width, kRowH);
    y += kRowH;
  };
  if (perspectivesHeading_ != nullptr) {
    head(perspectivesHeading_.get());
    for (auto& p : perspectives_) place(*p.toggle, *p.size);
    y += 6;
  }
  size_t h = 0;
  for (size_t i = 0; i < rows_.size(); ++i) {
    while (h < headings_.size() && headings_[h].beforeRow == static_cast<int>(i))
      head(headings_[h++].label.get());
    place(*rows_[i].toggle, *rows_[i].size);
  }
  if (!ranks_.empty()) {
    y += 6;
    head(ranksHeading_.get());
    for (auto& r : ranks_) place(*r.toggle, *r.size);
  }
  list_.setSize(width, y);
}

// -------------------------------------------------------- OrganSettingsWindow

OrganSettingsWindow::OrganSettingsWindow(MasterpieceProcessor& p, std::function<void()> reload,
                                         juce::AudioDeviceManager* devices)
    : reload_(std::move(reload)), engine_(p), stops_(p, [this] { reloadOnClose(); }) {
  addAndMakeVisible(tabs_);
  engine_.onReload = [this] { reloadOnClose(); };
  tabs_.addTab("Loading", kBackground, &engineScroll_, false);
  tabs_.addTab("Stops and perspectives", kBackground, &stops_, false);
  if (devices != nullptr) {
    midi_ = std::make_unique<MidiPanel>(p, *devices);
    tabs_.addTab("MIDI", kBackground, midi_.get(), false);
  }
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
