#include "ConsolePages.h"

#include "../mp_audio/MasterpieceProcessor.h"
#include "../mp_ui/Console.h"

#include <algorithm>
#include <vector>

namespace mp::api {

namespace {

// Stands where the console's picture of itself would be, and writes down
// every rectangle the console marks to be drawn again. A console drawn for a
// browser is on no screen, so that note is all a repaint does.
class Recorder final : public juce::CachedComponentImage {
public:
  explicit Recorder(std::function<void(juce::Rectangle<int>, bool)> note) : note_(std::move(note)) {}
  void paint(juce::Graphics&) override {}
  bool invalidateAll() override {
    note_({}, true);
    return false;
  }
  bool invalidate(const juce::Rectangle<int>& area) override {
    note_(area, false);
    return false;
  }
  void releaseResources() override {}

private:
  std::function<void(juce::Rectangle<int>, bool)> note_;
};

juce::var area(juce::Rectangle<int> r) {
  auto* o = new juce::DynamicObject();
  juce::var v(o);
  o->setProperty("x", r.getX());
  o->setProperty("y", r.getY());
  o->setProperty("w", r.getWidth());
  o->setProperty("h", r.getHeight());
  return v;
}

}  // namespace

struct ConsolePages::View {
  // The page's artwork, in the console's coordinates; the browser's (0, 0)
  // is its top-left corner.
  juce::Rectangle<int> art;
  juce::uint32 used = 0;
  // Changes as drawn: each with its serial. Anything at or before `full`
  // needs the whole page.
  struct Entry {
    int64_t serial;
    juce::Rectangle<int> area;
  };
  std::deque<Entry> log;
  int64_t serial = 0;
  int64_t full = 0;
  bool held = false;
  juce::uint32 heldSince = 0;
  juce::Point<int> heldAt;
  // Last: destroyed first, while the log it writes to is still here.
  std::unique_ptr<ui::ConsoleView> console;

  void note(juce::Rectangle<int> r, bool all) {
    ++serial;
    if (!all) r = r.getIntersection(art);
    if (all || log.size() >= kMaxLog) {
      log.clear();
      full = serial;
      return;
    }
    if (!r.isEmpty()) log.push_back({serial, r - art.getPosition()});
  }
};

ConsolePages::ConsolePages(MasterpieceProcessor& p) : proc_(p) {}

ConsolePages::~ConsolePages() = default;

bool ConsolePages::current(Error& error) {
  if (proc_.organLoading()) {
    // The organ they were drawn from is being replaced.
    views_.clear();
    list_ = juce::var();
    builtFor_ = -1;
    error = Error::Busy;
    return false;
  }
  if (builtFor_ != proc_.organLoads()) {
    views_.clear();
    list_ = juce::var();
    builtFor_ = proc_.organLoads();
  }
  if (builtFor_ == 0) {
    error = Error::NoOrgan;
    return false;
  }
  error = Error::None;
  return true;
}

juce::var ConsolePages::list(Error& error) {
  if (!current(error)) return {};
  if (list_.isVoid()) {
    auto* o = new juce::DynamicObject();
    list_ = juce::var(o);
    o->setProperty("session", builtFor_);
    juce::Array<juce::var> pages;
    ui::ConsoleView probe(proc_);
    if (probe.hasArtwork()) {
      juce::Array<juce::var> layouts;
      for (int l = 0; l < probe.layoutCount(); ++l) {
        auto* lo = new juce::DynamicObject();
        lo->setProperty("layout", l);
        lo->setProperty("portrait", probe.isPortrait(l));
        layouts.add(juce::var(lo));
      }
      for (int i = 0; i < probe.pageCount(); ++i) {
        probe.setPage(i);
        const auto art = probe.artworkBounds();
        auto* po = new juce::DynamicObject();
        po->setProperty("index", i);
        po->setProperty("name", probe.pageName(i));
        po->setProperty("width", art.getWidth());
        po->setProperty("height", art.getHeight());
        po->setProperty("layouts", layouts);
        pages.add(juce::var(po));
      }
    }
    o->setProperty("pages", pages);
  }
  return list_;
}

ConsolePages::View* ConsolePages::view(int page, int layout, Error& error) {
  if (!current(error)) return nullptr;
  const juce::var pages = list(error);
  const auto* all = pages["pages"].getArray();
  if (all == nullptr || page < 0 || page >= all->size()) {
    error = Error::OutOfRange;
    return nullptr;
  }
  const auto* layouts = (*all)[page]["layouts"].getArray();
  if (layouts == nullptr || layout < 0 || layout >= layouts->size()) {
    error = Error::OutOfRange;
    return nullptr;
  }
  const int key = page * 16 + layout;
  const juce::uint32 now = juce::Time::getMillisecondCounter();
  if (auto it = views_.find(key); it != views_.end()) {
    it->second->used = now;
    releaseStale(*it->second);
    return it->second.get();
  }
  if (static_cast<int>(views_.size()) >= kMaxViews) {
    auto oldest = views_.begin();
    for (auto it = views_.begin(); it != views_.end(); ++it)
      if (it->second->used < oldest->second->used) oldest = it;
    views_.erase(oldest);
  }
  auto v = std::make_unique<View>();
  View* raw = v.get();
  // As a page window builds its console: the layout, then the page.
  v->console = std::make_unique<ui::ConsoleView>(proc_);
  auto& c = *v->console;
  c.setLayout(layout);
  c.rebuild();
  c.setPage(page);
  v->art = c.artworkBounds();
  c.setBounds(0, 0, v->art.getRight(), v->art.getBottom());
  c.setVisible(true);
  c.setCachedComponentImage(new Recorder([raw](juce::Rectangle<int> r, bool all) { raw->note(r, all); }));
  // The first look finds every drawn thing as it stands; the picture a
  // client fetches next is drawn from the same state.
  c.lookForChanges();
  raw->log.clear();
  raw->full = raw->serial;
  raw->used = now;
  views_[key] = std::move(v);
  return raw;
}

void ConsolePages::releaseStale(View& v) {
  if (v.held && juce::Time::getMillisecondCounter() - v.heldSince > kHeldMs) {
    v.console->releaseAt(v.heldAt);
    v.held = false;
  }
}

juce::Image ConsolePages::picture(int page, int layout, juce::Rectangle<int> part, int64_t& serial, Error& error) {
  View* v = view(page, layout, error);
  if (v == nullptr) return {};
  v->console->lookForChanges();
  const auto whole = juce::Rectangle<int>(0, 0, v->art.getWidth(), v->art.getHeight());
  const auto r = part.isEmpty() ? whole : part.getIntersection(whole);
  if (r.isEmpty()) {
    error = Error::OutOfRange;
    return {};
  }
  serial = v->serial;
  return v->console->createComponentSnapshot(r + v->art.getPosition(), false);
}

juce::var ConsolePages::changes(int page, int layout, int64_t since, Error& error) {
  View* v = view(page, layout, error);
  if (v == nullptr) return {};
  v->console->lookForChanges();
  auto* o = new juce::DynamicObject();
  juce::var out(o);
  o->setProperty("serial", static_cast<juce::int64>(v->serial));
  if (since < v->full || since > v->serial) {
    o->setProperty("full", true);
    return out;
  }
  // One rectangle per changed thing: a knob is marked by the click and again,
  // a pixel larger, by the look that follows, and a rectangle inside another
  // is sent with it.
  std::vector<juce::Rectangle<int>> seen;
  for (const auto& e : v->log) {
    if (e.serial <= since) continue;
    if (std::any_of(seen.begin(), seen.end(), [&](const auto& s) { return s.contains(e.area); })) continue;
    seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const auto& s) { return e.area.contains(s); }),
               seen.end());
    seen.push_back(e.area);
  }
  juce::Array<juce::var> areas;
  for (const auto& s : seen) areas.add(area(s));
  o->setProperty("areas", areas);
  return out;
}

Error ConsolePages::pointer(int page, int layout, Phase phase, juce::Point<int> at) {
  Error error;
  View* v = view(page, layout, error);
  if (v == nullptr) return error;
  auto& c = *v->console;
  const auto p = at + v->art.getPosition();
  switch (phase) {
    case Phase::Down:
      // A press left open by a browser that never sent its release ends here.
      if (v->held) c.releaseAt(v->heldAt);
      c.pressAt(p, false);
      v->held = true;
      v->heldSince = juce::Time::getMillisecondCounter();
      v->heldAt = p;
      break;
    case Phase::Move:
      if (!v->held) break;
      c.dragTo(p);
      v->heldAt = p;
      break;
    case Phase::Up:
      if (!v->held) break;
      c.releaseAt(p);
      v->held = false;
      break;
  }
  c.lookForChanges();
  return Error::None;
}

}  // namespace mp::api
