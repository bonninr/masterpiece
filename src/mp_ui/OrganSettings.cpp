#include "OrganSettings.h"
#include "MidiEventDialog.h"

#include <algorithm>
#include <cmath>
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
  drawn_.setTooltip(TRANS("Load only the stops drawn on the console at this moment"));
  drawn_.onClick = [this] {
    std::set<Id> out;
    for (const auto& r : rows_)
      if (r.effect ? !r.toggle->getToggleState() : !proc_.shownStopEngaged(r.stopId))
        out.insert(r.stopId);
    choose(std::move(out));
  };

  addAndMakeVisible(filter_);
  filter_.setTextToShowWhenEmpty(TRANS("Filter, for example: Rear, tremmed, Pedal"), kTextDim);
  filter_.setTooltip(TRANS("Shows only the perspectives, stops and ranks whose name holds this text"));
  filter_.onTextChange = [this] { applyFilter(); };
  for (auto* b : {&loadShown_, &leaveShown_}) addAndMakeVisible(*b);
  loadShown_.onClick = [this] { setShownRanks(true); };
  leaveShown_.onClick = [this] { setShownRanks(false); };
  leaveShown_.setTooltip(TRANS("Leave out every rank in the list as it is filtered now"));

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
  note_.setText(TRANS("A perspective left out takes every rank recorded from that position; the "
                      "stops still play from the others. A stop left out costs no memory and makes "
                      "no sound, and keeps its drawstop, dimmed. Both are saved for this organ and "
                      "take effect when it is loaded again. Figures are estimates from the sample "
                      "files, at the settings on the Loading tab."),
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
    // A GrandOrgue set's windchest groups stand in for perspectives (#136).
    const bool chests = groupedByWindchest(proc_.organModel());
    perspectivesHeading_->setText(chests ? TRANS("Windchest groups") : TRANS("Perspectives"), juce::dontSendNotification);
    perspectivesHeading_->setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
    perspectivesHeading_->setColour(juce::Label::textColourId, juce::Colours::orange);
    list_.addAndMakeVisible(*perspectivesHeading_);
    for (const auto& [name, ranks] : groups) {
      PerspectiveRow row;
      row.name = name;
      row.ranks.assign(ranks.begin(), ranks.end());
      juce::String label(juce::CharPointer_UTF8(name.c_str()));
      label = label.substring(0, 1).toUpperCase() + label.substring(1) + "  " +
              TRANS("(%1 ranks)").replace("%1", juce::String(static_cast<int>(ranks.size())));
      row.toggle = std::make_unique<juce::ToggleButton>(label);
      row.toggle->setToggleState(proc_.excludedPerspectives().count(name) == 0,
                                 juce::dontSendNotification);
      row.toggle->setTooltip(TRANS("Every rank recorded from this position. Left out, it costs "
                                   "no memory; the stops still play from the other positions."));
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
    h.label->setTooltip(TRANS("Click to select or clear every stop in this division"));
    h.label->setMouseCursor(juce::MouseCursor::PointingHandCursor);
    h.label->addMouseListener(this, false);
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
      heading(TRANS("Effects and noises"));
      inEffects = true;
    } else if (!effect && s.divisionId != lastDivision) {
      const auto it = divisions.find(s.divisionId);
      heading(it != divisions.end() && !it->second.name.empty()
                  ? juce::String(it->second.name)
                  : TRANS("Division %1").replace("%1", juce::String(s.divisionId)));
      lastDivision = s.divisionId;
    }
    Row row;
    row.stopId = s.stopId;
    row.effect = effect;
    row.toggle = std::make_unique<juce::ToggleButton>(juce::String(s.name));
    row.toggle->setToggleState(excluded.count(s.stopId) == 0, juce::dontSendNotification);
    row.toggle->setEnabled(s.playable);
    if (!s.playable) row.toggle->setTooltip(TRANS("This stop's ranks ship no pipes in this sample set"));
    row.toggle->addMouseListener(this, false);
    row.toggle->addKeyListener(this);
    row.toggle->onClick = [this, clicked = row.toggle.get()] {
      // Released over the stop the drag began on: the drag decided.
      if (dragging_) clicked->setToggleState(dragState_, juce::dontSendNotification);
      applyStopTicks();
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
    ranksHeading_->setText(TRANS("Ranks"), juce::dontSendNotification);
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
    row.toggle = std::make_unique<juce::ToggleButton>(name.isEmpty() ? TRANS("Rank %1").replace("%1", juce::String(rankId)) : name);
    row.toggle->setToggleState(row.own, juce::dontSendNotification);
    const auto users = stopsOfRank.find(rankId);
    const juce::String playedBy =
        users == stopsOfRank.end()
            ? juce::String(TRANS("Played by no stop: a noise, or wired to a key directly"))
            : TRANS("Played by %1").replace("%1", users->second.joinIntoString(", "));
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

void StopsLoadPanel::applyStopTicks() {
  std::set<Id> out;
  for (const auto& r : rows_)
    if (!r.toggle->getToggleState()) out.insert(r.stopId);
  proc_.setExcludedStops(std::move(out));
  refreshFigures();
}

void StopsLoadPanel::mouseDown(const juce::MouseEvent& e) {
  dragging_ = false;
  for (const auto& r : rows_)
    if (e.eventComponent == r.toggle.get()) dragState_ = !r.toggle->getToggleState();
}

void StopsLoadPanel::mouseDrag(const juce::MouseEvent& e) {
  juce::ToggleButton* source = nullptr;
  for (auto& r : rows_)
    if (e.eventComponent == r.toggle.get()) source = r.toggle.get();
  if (source == nullptr) return;
  // A press that has moved a few pixels is a drag; the stop it began on takes
  // the new state with the rest.
  if (!dragging_) {
    if (e.getDistanceFromDragStart() < 4) return;
    dragging_ = true;
    source->setToggleState(dragState_, juce::dontSendNotification);
  }
  const auto at = e.getEventRelativeTo(&list_).getPosition();
  for (auto& r : rows_)
    if (r.toggle->isEnabled() && r.toggle->isVisible() && r.toggle->getBounds().contains(at))
      r.toggle->setToggleState(dragState_, juce::dontSendNotification);
  applyStopTicks();
}

bool StopsLoadPanel::keyPressed(const juce::KeyPress& key, juce::Component* origin) {
  int at = -1;
  for (size_t i = 0; i < rows_.size(); ++i)
    if (rows_[i].toggle.get() == origin) at = static_cast<int>(i);
  if (at < 0) return false;
  auto& here = *rows_[static_cast<size_t>(at)].toggle;
  if (key.isKeyCode(juce::KeyPress::spaceKey)) {
    if (here.isEnabled()) here.setToggleState(!here.getToggleState(), juce::dontSendNotification);
    applyStopTicks();
    return true;
  }
  const bool up = key.isKeyCode(juce::KeyPress::upKey);
  if (!up && !key.isKeyCode(juce::KeyPress::downKey)) return false;
  // The next stop shown that can be chosen, that way.
  int next = at;
  do {
    next += up ? -1 : 1;
  } while (next >= 0 && next < static_cast<int>(rows_.size()) &&
           !(rows_[static_cast<size_t>(next)].toggle->isVisible() &&
             rows_[static_cast<size_t>(next)].toggle->isEnabled()));
  if (next < 0 || next >= static_cast<int>(rows_.size())) return true;
  auto& there = *rows_[static_cast<size_t>(next)].toggle;
  if (key.getModifiers().isShiftDown()) {
    there.setToggleState(here.getToggleState(), juce::dontSendNotification);
    applyStopTicks();
  }
  there.grabKeyboardFocus();
  // Kept in view, a row's height of room above and below.
  const auto r = there.getBounds();
  const int top = viewport_.getViewPositionY(), height = viewport_.getViewHeight();
  if (r.getY() - r.getHeight() < top)
    viewport_.setViewPosition(0, std::max(0, r.getY() - r.getHeight()));
  else if (r.getBottom() + r.getHeight() > top + height)
    viewport_.setViewPosition(0, r.getBottom() + r.getHeight() - height);
  return true;
}

void StopsLoadPanel::mouseUp(const juce::MouseEvent& e) {
  for (size_t h = 0; h < headings_.size(); ++h) {
    if (e.eventComponent != headings_[h].label.get()) continue;
    const size_t from = static_cast<size_t>(headings_[h].beforeRow);
    const size_t to = h + 1 < headings_.size() ? static_cast<size_t>(headings_[h + 1].beforeRow) : rows_.size();
    // Any stop of the division in: all out. None in: all in.
    bool anyIn = false;
    for (size_t i = from; i < to && i < rows_.size(); ++i)
      if (rows_[i].toggle->isEnabled() && rows_[i].toggle->getToggleState()) anyIn = true;
    for (size_t i = from; i < to && i < rows_.size(); ++i)
      if (rows_[i].toggle->isEnabled()) rows_[i].toggle->setToggleState(!anyIn, juce::dontSendNotification);
    applyStopTicks();
    return;
  }
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
      why = (groupedByWindchest(proc_.organModel()) ? TRANS("Left out with the %1 windchest group")
                                                    : TRANS("Left out with the %1 perspective"))
                .replace("%1", juce::String(juce::CharPointer_UTF8(it->second.c_str())));
    else if (!r.stops.empty() &&
             std::all_of(r.stops.begin(), r.stops.end(),
                         [&](Id s) { return stopsOut.count(s) != 0; }))
      why = r.stops.size() == 1 ? TRANS("Left out with its stop") : TRANS("Left out with every stop that plays it");
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
    total_.setText(TRANS("Estimating the memory each stop takes..."), juce::dontSendNotification);
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
  juce::String text = TRANS("About %1 for this choice").replace("%1", megabytes(total));
  if (limit > 0) text << " " << TRANS("(the limit is %1)").replace("%1", megabytes(limit));
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

// ------------------------------------------------------------ VoicingEqPanel

juce::String voicingEqPresetName(const VoicingEqPreset& p) {
  if (!p.factory()) return juce::String::fromUTF8(p.name.c_str());
  // Spelt out, so the translation tool finds each.
  switch (p.id) {
    case 1: return TRANS("Flat");
    case 2: return TRANS("Brighter");
    case 3: return TRANS("Chiff");
    case 4: return TRANS("Warm");
    case 5: return TRANS("Less boom");
    case 6: return TRANS("Small speakers");
    case 7: return TRANS("Softer treble");
    case 8: return TRANS("Clarity");
    case 9: return TRANS("Quiet listening");
    case 10: return TRANS("Headphones");
    default: return juce::String::fromUTF8(p.name.c_str());
  }
}

namespace {

// A mapping's message, for the MIDI menu's header.
juce::String describeBinding(const MidiBinding& b) {
  juce::String what = b.source.kind == MidiSourceKind::Note            ? TRANS("Note %1")
                      : b.source.kind == MidiSourceKind::ProgramChange ? TRANS("Program %1")
                      : b.source.kind == MidiSourceKind::SysEx         ? TRANS("SysEx #%1")
                                                                       : TRANS("CC %1");
  what = what.replace("%1", juce::String(b.source.number));
  if (b.source.channel > 0) what << " " << TRANS("ch %1").replace("%1", juce::String(b.source.channel));
  return what;
}

}  // namespace

VoicingEqPanel::VoicingEqPanel(MasterpieceProcessor& p) : proc_(p) {
  addAndMakeVisible(on_);
  on_.onClick = [this] { apply(); };
  addAndMakeVisible(presetLabel_);
  presetLabel_.setText(TRANS("Preset"), juce::dontSendNotification);
  presetLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffb9c2d0));
  addAndMakeVisible(presets_);
  presets_.setTooltip(TRANS("The program's ten presets, then your own. Choosing one sets the EQ at once "
                            "and turns it on; Save for this organ or Save as default keeps it."));
  presets_.onChange = [this] {
    const int id = presets_.getSelectedId();
    if (id <= 0 || id == proc_.currentVoicingEqPreset()) return;
    proc_.recallVoicingEqPreset(id);
    showValues();
    save_.setButtonText(TRANS("Save for this organ"));
    saveDefault_.setButtonText(TRANS("Save as default"));
    repaint(curveArea_);
  };
  addAndMakeVisible(savePreset_);
  savePreset_.setTooltip(TRANS("Keep the EQ as it is now as a preset of your own, for every organ"));
  savePreset_.onClick = [this] {
    askName(TRANS("Save as preset"), {}, [this](const juce::String& name) {
      proc_.saveVoicingEqPreset(name);
      fillPresets();
    });
  };
  addAndMakeVisible(updatePreset_);
  updatePreset_.setTooltip(TRANS("Store the EQ as it is now in the preset of your own last chosen"));
  updatePreset_.onClick = [this] {
    proc_.updateVoicingEqPreset(proc_.lastVoicingEqPreset());
    fillPresets();
  };
  addAndMakeVisible(renamePreset_);
  renamePreset_.onClick = [this] {
    const auto* p = proc_.voicingEqPresets().find(proc_.lastVoicingEqPreset());
    if (p == nullptr || p->factory()) return;
    const int id = p->id;
    askName(TRANS("Rename preset"), voicingEqPresetName(*p), [this, id](const juce::String& name) {
      proc_.renameVoicingEqPreset(id, name);
      fillPresets();
    });
  };
  addAndMakeVisible(deletePreset_);
  deletePreset_.onClick = [this] {
    const auto* p = proc_.voicingEqPresets().find(proc_.lastVoicingEqPreset());
    if (p == nullptr || p->factory()) return;
    const int id = p->id;
    juce::Component::SafePointer<VoicingEqPanel> self(this);
    juce::AlertWindow::showOkCancelBox(
        juce::MessageBoxIconType::QuestionIcon, TRANS("Delete preset"),
        TRANS("Delete the preset %1? Its MIDI mapping goes with it.").replace("%1", voicingEqPresetName(*p)),
        TRANS("Delete"), TRANS("Cancel"), this,
        juce::ModalCallbackFunction::create([self, id](int result) {
          if (result != 1 || self == nullptr) return;
          self->proc_.deleteVoicingEqPreset(id);
          self->fillPresets();
        }));
  };
  addAndMakeVisible(midiPreset_);
  midiPreset_.setTooltip(TRANS("Map a MIDI button to this preset, or to the next and the previous one"));
  midiPreset_.onClick = [this] { presetMidiMenu(); };
  addAndMakeVisible(save_);
  save_.setTooltip(TRANS("Keep this EQ with this organ for the next time it is loaded"));
  save_.onClick = [this] {
    save_.setButtonText(proc_.saveVoicingEqForOrgan() ? TRANS("Saved") : TRANS("Could not save"));
  };
  addAndMakeVisible(saveDefault_);
  saveDefault_.setTooltip(TRANS("Start every organ with this EQ, unless one is saved for it: "
                                "a correction for your speakers and room"));
  saveDefault_.onClick = [this] {
    saveDefault_.setButtonText(proc_.saveVoicingEqAsDefault() ? TRANS("Saved") : TRANS("Could not save"));
  };
  addAndMakeVisible(reset_);
  reset_.setTooltip(TRANS("Back to the default EQ, and forget the one saved for this organ"));
  reset_.onClick = [this] {
    proc_.resetVoicingEqToDefault();
    showValues();
    save_.setButtonText(TRANS("Save for this organ"));
    repaint(curveArea_);
  };
  addAndMakeVisible(note_);
  note_.setText(TRANS("A correction for the room and the speakers, on the whole organ. "
                      "Off unless turned on here; the organ as recorded is the reference."),
                juce::dontSendNotification);
  note_.setColour(juce::Label::textColourId, juce::Colour(0xff9aa3b2));
  note_.setFont(juce::Font(juce::FontOptions(13.0f)));
  auto style = [this](juce::Slider& sl, juce::Label& name, const juce::String& text) {
    sl.setSliderStyle(juce::Slider::LinearVertical);
    sl.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 46, 18);
    sl.setRange(-VoicingEqSettings::kMaxDb, VoicingEqSettings::kMaxDb, 0.5);
    sl.setDoubleClickReturnValue(true, 0.0);
    sl.setTextValueSuffix(" dB");
    sl.onValueChange = [this] { apply(); };
    addAndMakeVisible(sl);
    name.setText(text, juce::dontSendNotification);
    name.setJustificationType(juce::Justification::centred);
    name.setColour(juce::Label::textColourId, juce::Colour(0xffb9c2d0));
    addAndMakeVisible(name);
  };
  for (int i = 0; i < VoicingEqSettings::kBands; ++i) {
    const double f = VoicingEqSettings::kFrequencies[static_cast<size_t>(i)];
    style(bands_[static_cast<size_t>(i)], bandNames_[static_cast<size_t>(i)],
          f >= 1000.0 ? juce::String(f / 1000.0, 0) + "k" : juce::String(f, f < 40 ? 1 : 0).upToFirstOccurrenceOf(".0", false, false));
  }
  style(gain_, gainName_, TRANS("Gain"));
  showValues();
  // A preset recalled from MIDI while the panel is open.
  startTimerHz(4);
}

void VoicingEqPanel::showValues() {
  const auto& eq = proc_.voicingEq();
  on_.setToggleState(eq.on, juce::dontSendNotification);
  for (size_t i = 0; i < bands_.size(); ++i)
    bands_[i].setValue(eq.bandsDb[i], juce::dontSendNotification);
  gain_.setValue(eq.gainDb, juce::dontSendNotification);
  fillPresets();
  seenChanges_ = proc_.voicingEqChanges();
}

void VoicingEqPanel::apply() {
  VoicingEqSettings eq;
  eq.on = on_.getToggleState();
  for (size_t i = 0; i < bands_.size(); ++i) eq.bandsDb[i] = bands_[i].getValue();
  eq.gainDb = gain_.getValue();
  proc_.setVoicingEq(eq);
  seenChanges_ = proc_.voicingEqChanges();
  fillPresets();
  save_.setButtonText(TRANS("Save for this organ"));
  saveDefault_.setButtonText(TRANS("Save as default"));
  repaint(curveArea_);
}

// The list, with the preset the EQ is now chosen; "Edited" when the sliders
// have moved away from every preset. Update, Rename and Delete act on the
// player's preset last chosen.
void VoicingEqPanel::fillPresets() {
  const auto& bank = proc_.voicingEqPresets();
  presets_.clear(juce::dontSendNotification);
  presets_.addSectionHeading(TRANS("Presets"));
  for (const auto& p : VoicingEqPresets::factory()) presets_.addItem(voicingEqPresetName(p), p.id);
  if (!bank.own().empty()) {
    presets_.addSeparator();
    presets_.addSectionHeading(TRANS("Your presets"));
    for (const auto& p : bank.own()) presets_.addItem(voicingEqPresetName(p), p.id);
  }
  const int now = proc_.currentVoicingEqPreset();
  if (now != 0)
    presets_.setSelectedId(now, juce::dontSendNotification);
  else
    presets_.setText(proc_.voicingEq().on ? TRANS("Edited") : TRANS("Off"), juce::dontSendNotification);

  const auto* last = bank.find(proc_.lastVoicingEqPreset());
  const bool own = last != nullptr && !last->factory();
  updatePreset_.setEnabled(own && now != last->id);
  renamePreset_.setEnabled(own);
  deletePreset_.setEnabled(own);
  updatePreset_.setTooltip(own ? TRANS("Store the EQ as it is now in %1").replace("%1", voicingEqPresetName(*last))
                               : TRANS("Store the EQ as it is now in the preset of your own last chosen"));
}

void VoicingEqPanel::timerCallback() {
  if (proc_.voicingEqChanges() == seenChanges_) return;
  showValues();
  repaint(curveArea_);
}

void VoicingEqPanel::askName(const juce::String& title, const juce::String& initial,
                             std::function<void(const juce::String&)> then) {
  auto* w = new juce::AlertWindow(title, TRANS("A name for the preset"), juce::MessageBoxIconType::NoIcon, this);
  w->addTextEditor("name", initial);
  w->addButton(TRANS("OK"), 1, juce::KeyPress(juce::KeyPress::returnKey));
  w->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
  juce::Component::SafePointer<VoicingEqPanel> self(this);
  w->enterModalState(true, juce::ModalCallbackFunction::create([self, w, then](int result) {
                       const juce::String name = w->getTextEditorContents("name").trim();
                       if (result == 1 && self != nullptr) then(name);
                     }),
                     true);
}

// Learn a MIDI button for the preset chosen, or for the next and the previous
// one; the mapping is the console's, the same on every organ.
void VoicingEqPanel::presetMidiMenu() {
  auto& map = proc_.midiMap();
  if (map.learning()) {
    map.cancelLearn();
    return;
  }
  const int id = proc_.currentVoicingEqPreset() != 0 ? proc_.currentVoicingEqPreset() : proc_.lastVoicingEqPreset();
  const auto* preset = proc_.voicingEqPresets().find(id);
  struct Target {
    MidiTargetKind kind;
    Id id;
    juce::String name;
  };
  std::vector<Target> targets;
  if (preset != nullptr)
    targets.push_back({MidiTargetKind::VoicingEqPreset, static_cast<Id>(preset->id),
                       TRANS("Voicing EQ: %1").replace("%1", voicingEqPresetName(*preset))});
  targets.push_back({MidiTargetKind::VoicingEqPresetNext, 0, TRANS("Voicing EQ: next preset")});
  targets.push_back({MidiTargetKind::VoicingEqPresetPrev, 0, TRANS("Voicing EQ: previous preset")});

  juce::PopupMenu menu;
  int item = 1;
  for (const auto& t : targets) {
    juce::PopupMenu sub;
    const auto existing = map.bindingsFor(t.kind, t.id);
    if (existing.empty()) sub.addSectionHeader(TRANS("Not mapped"));
    for (const MidiBinding* b : existing) sub.addSectionHeader(describeBinding(*b));
    sub.addItem(item, TRANS("Learn"));
    sub.addItem(item + 1, TRANS("MIDI window..."));
    sub.addItem(item + 2, TRANS("Clear mapping"), !existing.empty());
    menu.addSubMenu(t.name, sub);
    item += 10;
  }
  juce::Component::SafePointer<VoicingEqPanel> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&midiPreset_),
                     [self, targets](int choice) {
                       if (self == nullptr || choice <= 0) return;
                       const auto& t = targets[static_cast<size_t>((choice - 1) / 10)];
                       auto& m = self->proc_.midiMap();
                       switch ((choice - 1) % 10) {
                         case 0:
                           m.beginLearnAs(t.kind, t.id, MidiTrigger::Momentary);
                           break;
                         case 1:
                           MidiEventDialog::show(self->proc_, t.kind, t.id, t.name);
                           break;
                         case 2:
                           m.unbindTarget(t.kind, t.id);
                           self->proc_.saveMidiMap();
                           break;
                         default:
                           break;
                       }
                     });
}

void VoicingEqPanel::resized() {
  auto r = getLocalBounds().reduced(12);
  auto top = r.removeFromTop(28);
  on_.setBounds(top.removeFromLeft(70));
  save_.setBounds(top.removeFromRight(150).reduced(2, 0));
  saveDefault_.setBounds(top.removeFromRight(120).reduced(2, 0));
  reset_.setBounds(top.removeFromRight(70).reduced(2, 0));
  r.removeFromTop(4);
  auto row = r.removeFromTop(28);
  presetLabel_.setBounds(row.removeFromLeft(60));
  midiPreset_.setBounds(row.removeFromRight(70).reduced(2, 0));
  deletePreset_.setBounds(row.removeFromRight(70).reduced(2, 0));
  renamePreset_.setBounds(row.removeFromRight(90).reduced(2, 0));
  updatePreset_.setBounds(row.removeFromRight(80).reduced(2, 0));
  savePreset_.setBounds(row.removeFromRight(130).reduced(2, 0));
  presets_.setBounds(row.reduced(2, 0));
  note_.setBounds(r.removeFromTop(36));
  curveArea_ = r.removeFromTop(130).reduced(0, 4);
  r.removeFromTop(6);
  const int columns = VoicingEqSettings::kBands + 1;
  const int w = r.getWidth() / columns;
  auto names = r.removeFromTop(20);
  for (int i = 0; i < columns; ++i) {
    auto col = r.withX(r.getX() + i * w).withWidth(w);
    auto name = names.withX(names.getX() + i * w).withWidth(w);
    if (i < VoicingEqSettings::kBands) {
      bandNames_[static_cast<size_t>(i)].setBounds(name);
      bands_[static_cast<size_t>(i)].setBounds(col.reduced(2, 0));
    } else {
      gainName_.setBounds(name);
      gain_.setBounds(col.reduced(2, 0));
    }
  }
}

void VoicingEqPanel::paint(juce::Graphics& g) {
  g.fillAll(juce::Colour(0xff15171c));
  // The resulting curve, 20 Hz to 20 kHz on a log scale, +-12 dB.
  const auto a = curveArea_.toFloat();
  g.setColour(juce::Colour(0xff1f232b));
  g.fillRoundedRectangle(a, 4.0f);
  g.setColour(juce::Colour(0xff3a4150));
  g.drawHorizontalLine(juce::roundToInt(a.getCentreY()), a.getX(), a.getRight());
  VoicingEqSettings eq;
  eq.on = true;
  for (size_t i = 0; i < bands_.size(); ++i) eq.bandsDb[i] = bands_[i].getValue();
  eq.gainDb = gain_.getValue();
  juce::Path curve;
  const int n = 160;
  for (int i = 0; i <= n; ++i) {
    const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / n);
    const double db = juce::jlimit(-18.0, 18.0, VoicingEq::responseDb(eq, hz));
    const float x = a.getX() + a.getWidth() * static_cast<float>(i) / n;
    const float y = a.getCentreY() - static_cast<float>(db / 18.0) * a.getHeight() * 0.5f;
    if (i == 0) curve.startNewSubPath(x, y);
    else curve.lineTo(x, y);
  }
  g.setColour(on_.getToggleState() ? juce::Colour(0xffe0a050) : juce::Colour(0xff6a7180));
  g.strokePath(curve, juce::PathStrokeType(2.0f));
}

// -------------------------------------------------------- OrganSettingsWindow

OrganSettingsWindow::OrganSettingsWindow(MasterpieceProcessor& p, std::function<void()> reload,
                                         juce::AudioDeviceManager* devices)
    : reload_(std::move(reload)), engine_(p), stops_(p, [this] { reloadOnClose(); }), eq_(p) {
  addAndMakeVisible(tabs_);
  engine_.onReload = [this] { reloadOnClose(); };
  tabs_.addTab(TRANS("Loading"), kBackground, &engineScroll_, false);
  tabs_.addTab(TRANS("Stops and perspectives"), kBackground, &stops_, false);
  tabs_.addTab(TRANS("Voicing EQ"), kBackground, &eq_, false);
  if (devices != nullptr) {
    midi_ = std::make_unique<MidiPanel>(p, *devices);
    tabs_.addTab(TRANS("MIDI"), kBackground, midi_.get(), false);
  }
  setSize(660, 560);
}

OrganSettingsWindow::~OrganSettingsWindow() {
  // Posted, not called: the window is still being torn down here.
  if (onClosed) {
    juce::MessageManager::callAsync(std::move(onClosed));
  } else if (reloadOnClose_ && reload_) {
    juce::MessageManager::callAsync(std::move(reload_));
  } else if (engine_.cacheRebuildPending() && reload_) {
    // A change that rebuilds the cache, and the window closed without loading
    // the organ again: asked now, once, for everything changed. Not accepted,
    // the window opens again, where the change can be undone.
    juce::MessageManager::callAsync([reload = std::move(reload_), reopen = std::move(onReopen)] {
      juce::AlertWindow::showAsync(
          juce::MessageBoxOptions()
              .withIconType(juce::MessageBoxIconType::QuestionIcon)
              .withTitle(TRANS("Rebuild the sample cache?"))
              .withMessage(TRANS("This changes the samples the cache holds. The organ is loaded again "
                                 "now, and its cache is built anew."))
              .withButton(TRANS("Rebuild and load again"))
              .withButton(TRANS("Back to settings")),
          [reload, reopen](int result) {
            if (result == 1) {
              if (reload) reload();
            } else if (reopen) {
              reopen();
            }
          });
    });
  }
}

void OrganSettingsWindow::reloadOnClose() {
  reloadOnClose_ = true;
  if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) dialog->closeButtonPressed();
}

void OrganSettingsWindow::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff15171c)); }
void OrganSettingsWindow::resized() { tabs_.setBounds(getLocalBounds()); }

}  // namespace mp::ui
