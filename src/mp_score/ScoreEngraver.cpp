#include "ScoreEngraver.h"

#include <vrv/toolkit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>
#include <vector>

namespace mp {

namespace {

std::string fromBase64(const std::string& text) {
  std::string out;
  uint32_t bits = 0;
  int count = 0;
  for (const char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else continue;
    bits = (bits << 6) | static_cast<uint32_t>(v);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(static_cast<char>((bits >> count) & 0xff));
    }
  }
  return out;
}

// The tempo changes of a standard MIDI file, in the order they take effect,
// and its ticks per quarter note (0 if the file cannot be read that way).
struct TempoMap {
  int ppq = 0;
  std::vector<std::pair<uint64_t, uint32_t>> changes;  // tick, microseconds a quarter

  // Seconds from the start to a point given in quarter notes.
  double secondsAt(double quarters) const {
    const double ticks = quarters * ppq;
    double seconds = 0.0, at = 0.0;
    uint32_t tempo = 500000;  // the format's default, 120 a minute
    for (const auto& [tick, us] : changes) {
      if (static_cast<double>(tick) > ticks) break;
      seconds += (static_cast<double>(tick) - at) / ppq * tempo / 1e6;
      at = static_cast<double>(tick);
      tempo = us;
    }
    return seconds + (ticks - at) / ppq * tempo / 1e6;
  }
};

TempoMap readTempos(const std::string& smf) {
  TempoMap map;
  const auto* p = reinterpret_cast<const unsigned char*>(smf.data());
  const size_t size = smf.size();
  auto u32 = [&](size_t at) {
    return (uint32_t(p[at]) << 24) | (uint32_t(p[at + 1]) << 16) | (uint32_t(p[at + 2]) << 8) | p[at + 3];
  };
  if (size < 14 || smf.compare(0, 4, "MThd") != 0) return map;
  const int division = (p[12] << 8) | p[13];
  if (division <= 0 || (division & 0x8000) != 0) return map;  // timecode, not quarters
  size_t at = 8 + u32(4);
  while (at + 8 <= size) {
    const size_t length = u32(at + 4);
    const bool track = smf.compare(at, 4, "MTrk") == 0;
    size_t i = at + 8;
    const size_t end = std::min(size, i + length);
    at = i + length;
    if (!track) continue;
    uint64_t tick = 0;
    unsigned status = 0;
    auto vlq = [&]() {
      uint64_t v = 0;
      while (i < end) {
        const unsigned char b = p[i++];
        v = (v << 7) | (b & 0x7f);
        if ((b & 0x80) == 0) break;
      }
      return v;
    };
    while (i < end) {
      tick += vlq();
      if (i >= end) break;
      if (p[i] & 0x80) status = p[i++];
      if (status == 0xff) {
        if (i >= end) break;
        const unsigned type = p[i++];
        const uint64_t n = vlq();
        if (type == 0x51 && n == 3 && i + 3 <= end)
          map.changes.emplace_back(tick, (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2]);
        i += static_cast<size_t>(n);
      } else if (status == 0xf0 || status == 0xf7) {
        i += static_cast<size_t>(vlq());
      } else if (status >= 0x80) {
        const unsigned kind = status & 0xf0;
        i += (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
      } else {
        break;  // data with no status before it: not a file to trust
      }
    }
  }
  // Several changes at one tick: the last one written is the one that holds.
  std::stable_sort(map.changes.begin(), map.changes.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  map.ppq = division;
  return map;
}

// The number after "key": in text, from `from`; npos when absent.
size_t numberAfter(const std::string& text, const char* key, size_t from, size_t to, double& value) {
  const size_t k = text.find(key, from);
  if (k == std::string::npos || k >= to) return std::string::npos;
  const char* start = text.c_str() + k + std::char_traits<char>::length(key);
  char* stop = nullptr;
  value = std::strtod(start, &stop);
  return stop == start ? std::string::npos : k;
}

}  // namespace

struct ScoreEngraver::Impl {
  vrv::Toolkit tk{false};
  int widthPx = 0, heightPx = 0, scale = 0;
  // The time map in the MIDI's time: (played ms, Verovio's ms), in order.
  // Empty when the two agree, as they do unless the score has two tempos at
  // one point, where the MIDI keeps the last and the time map the first (#964).
  std::vector<std::pair<double, double>> clock;

  void buildClock() {
    clock.clear();
    const TempoMap tempos = readTempos(fromBase64(tk.RenderToMIDI()));
    if (tempos.ppq <= 0) return;
    const std::string map = tk.RenderToTimemap();
    bool differs = false;
    for (size_t open = map.find('{'); open != std::string::npos; open = map.find('{', open + 1)) {
      const size_t close = map.find('}', open);
      if (close == std::string::npos) break;
      double q = 0.0, t = 0.0;
      if (numberAfter(map, "\"qstamp\":", open, close, q) != std::string::npos &&
          numberAfter(map, "\"tstamp\":", open, close, t) != std::string::npos) {
        const double played = tempos.secondsAt(q) * 1000.0;
        if (std::abs(played - t) > 1.0) differs = true;
        clock.emplace_back(played, t);
      } else if (map.find("\"tstamp\":", open) < close) {
        clock.clear();  // a time with no place in quarters: Verovio's times as they are
        return;
      }
    }
    if (!differs) clock.clear();
  }

  // Verovio's time for a moment of the playing.
  double verovioMs(double playedMs) const {
    if (clock.empty()) return playedMs;
    auto hi = std::lower_bound(clock.begin(), clock.end(), playedMs,
                               [](const auto& e, double v) { return e.first < v; });
    if (hi == clock.begin()) return clock.front().second + (playedMs - clock.front().first);
    if (hi == clock.end()) return clock.back().second + (playedMs - clock.back().first);
    const auto lo = std::prev(hi);
    const double span = hi->first - lo->first;
    return span <= 0.0 ? lo->second
                       : lo->second + (playedMs - lo->first) / span * (hi->second - lo->second);
  }
};

ScoreEngraver::ScoreEngraver(const std::string& resourceDir) : impl_(new Impl) {
  ready_ = impl_->tk.SetResourcePath(resourceDir);
  if (!ready_) error_ = "The score fonts could not be loaded from " + resourceDir;
}

ScoreEngraver::~ScoreEngraver() = default;

void ScoreEngraver::setPage(int widthPx, int heightPx, int scalePercent) {
  widthPx = std::max(widthPx, 100);
  heightPx = std::max(heightPx, 100);
  scalePercent = std::clamp(scalePercent, 10, 400);
  Impl& i = *impl_;
  if (i.widthPx == widthPx && i.heightPx == heightPx && i.scale == scalePercent) return;
  i.widthPx = widthPx;
  i.heightPx = heightPx;
  i.scale = scalePercent;
  // The page is given in Verovio units, which the scale shrinks or grows to
  // pixels: pixels = units * scale / 100. The first page opens with the
  // score's title, composer and other credits. Music symbols inside text are
  // drawn from the Bravura the program registers, so no page carries its own
  // copy of the font: a page is half the size without it.
  char opts[384];
  std::snprintf(opts, sizeof opts,
                "{\"pageWidth\":%d,\"pageHeight\":%d,\"scale\":%d,\"breaks\":\"auto\","
                "\"adjustPageHeight\":false,\"header\":\"auto\",\"footer\":\"none\","
                "\"font\":\"Bravura\",\"smuflTextFont\":\"none\",\"svgViewBox\":false,\"svgRemoveXlink\":false}",
                widthPx * 100 / scalePercent, heightPx * 100 / scalePercent, scalePercent);
  i.tk.SetOptions(opts);
  if (loaded_) i.tk.RedoLayout();
}

bool ScoreEngraver::load(const std::string& musicXml) {
  loaded_ = false;
  error_.clear();
  if (!ready_) {
    error_ = "The score fonts are not available";
    return false;
  }
  if (musicXml.empty()) {
    error_ = "The file is empty";
    return false;
  }
  // Verovio would take any text and engrave an empty page.
  if (musicXml.find("<score-partwise") == std::string::npos &&
      musicXml.find("<score-timewise") == std::string::npos) {
    error_ = "This is not a MusicXML score";
    return false;
  }
  vrv::Toolkit& tk = impl_->tk;
  tk.SetOptions("{\"inputFrom\":\"musicxml\"}");
  if (!tk.LoadData(musicXml) || tk.GetPageCount() < 1) {
    error_ = "This MusicXML score could not be read";
    return false;
  }
  loaded_ = true;
  impl_->buildClock();
  return true;
}

int ScoreEngraver::pageCount() const { return loaded_ ? impl_->tk.GetPageCount() : 0; }

std::string ScoreEngraver::renderPage(int page) {
  if (!loaded_ || page < 1 || page > pageCount()) return {};
  return impl_->tk.RenderToSVG(page, true);
}

std::string ScoreEngraver::midiBase64() { return loaded_ ? impl_->tk.RenderToMIDI() : std::string(); }

std::string ScoreEngraver::timemapJson() {
  if (!loaded_) return {};
  std::string map = impl_->tk.RenderToTimemap();
  if (impl_->clock.empty()) return map;
  // Each entry's time, as the MIDI plays it. The entries come in the same
  // order the clock was built in.
  std::string out;
  out.reserve(map.size() + map.size() / 8);
  size_t at = 0, entry = 0;
  static constexpr char kKey[] = "\"tstamp\":";
  for (size_t k = map.find(kKey); k != std::string::npos; k = map.find(kKey, at)) {
    const size_t value = k + sizeof kKey - 1;
    char* stop = nullptr;
    std::strtod(map.c_str() + value, &stop);
    out.append(map, at, value - at);
    if (entry < impl_->clock.size()) {
      char number[32];
      std::snprintf(number, sizeof number, "%.3f", impl_->clock[entry++].first);
      out += number;
    } else {
      out.append(map.c_str() + value, static_cast<size_t>(stop - (map.c_str() + value)));
    }
    at = static_cast<size_t>(stop - map.c_str());
  }
  out.append(map, at, std::string::npos);
  return out;
}

int ScoreEngraver::pageWithElement(const std::string& id) {
  return loaded_ ? impl_->tk.GetPageWithElement(id) : 0;
}

std::string ScoreEngraver::elementsAtTime(int milliseconds) {
  if (!loaded_) return {};
  return impl_->tk.GetElementsAtTime(static_cast<int>(std::lround(impl_->verovioMs(milliseconds))));
}

} // namespace mp
