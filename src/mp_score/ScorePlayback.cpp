#include "ScorePlayback.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>

namespace mp {

namespace {

// What a measure holds of the repeats and the jumps.
struct Measure {
  bool forward = false, backward = false;
  int times = 2;
  bool endingStart = false, endingEnd = false;
  std::set<int> endings;  // the passes an ending starting here is played on
  bool segno = false, coda = false;
  std::string segnoName, codaName;
  bool dacapo = false, fine = false;
  bool dalsegno = false, tocoda = false;
  std::string dalsegnoName, tocodaName;
};

bool yes(const pugi::xml_attribute& a) {
  const std::string v = a.value();
  return !v.empty() && v != "no";
}

Measure readMeasure(const pugi::xml_node& measure) {
  Measure m;
  for (const auto barline : measure.children("barline")) {
    if (const auto repeat = barline.child("repeat")) {
      const std::string dir = repeat.attribute("direction").value();
      if (dir == "forward") m.forward = true;
      if (dir == "backward") {
        m.backward = true;
        if (const auto times = repeat.attribute("times")) m.times = std::max(2, times.as_int(2));
      }
    }
    if (const auto ending = barline.child("ending")) {
      const std::string type = ending.attribute("type").value();
      if (type == "start") {
        m.endingStart = true;
        std::string numbers = ending.attribute("number").value();
        for (char& c : numbers)
          if (c < '0' || c > '9') c = ' ';
        std::istringstream in(numbers);
        for (int n = 0; in >> n;) m.endings.insert(n);
      } else if (type == "stop" || type == "discontinue") {
        m.endingEnd = true;
      }
    }
  }
  // The jumps: the playback attributes of <sound>, in a direction or on
  // their own; the signs alone mark where a jump lands.
  for (const auto& x : measure.select_nodes(".//sound")) {
    const auto s = x.node();
    if (const auto a = s.attribute("segno")) m.segno = true, m.segnoName = a.value();
    if (const auto a = s.attribute("coda")) m.coda = true, m.codaName = a.value();
    if (yes(s.attribute("dacapo"))) m.dacapo = true;
    if (yes(s.attribute("fine"))) m.fine = true;
    if (const auto a = s.attribute("dalsegno")) m.dalsegno = true, m.dalsegnoName = a.value();
    if (const auto a = s.attribute("tocoda")) m.tocoda = true, m.tocodaName = a.value();
  }
  if (measure.select_node(".//direction-type/segno")) m.segno = true;
  if (measure.select_node(".//direction-type/coda")) m.coda = true;
  return m;
}

// The order the measures are played in. Repeats are taken as often as they
// say, each pass playing the ending numbered for it; after a da capo or a dal
// segno the repeats are not taken again, the last ending is played, and the
// playing stops at Fine or goes to the coda at To Coda.
std::vector<int> playOrder(const std::vector<Measure>& m) {
  const int n = static_cast<int>(m.size());
  std::vector<int> endingLast(static_cast<size_t>(n), -1);
  for (int i = 0; i < n; ++i) {
    if (!m[static_cast<size_t>(i)].endingStart) continue;
    int j = i;
    while (j < n - 1 && !m[static_cast<size_t>(j)].endingEnd && !m[static_cast<size_t>(j + 1)].endingStart) ++j;
    endingLast[static_cast<size_t>(i)] = j;
  }
  const auto findMark = [&](bool coda, const std::string& name, int from) {
    for (int pass = 0; pass < 2; ++pass)
      for (int k = pass == 0 ? from : 0; k < n; ++k) {
        const auto& x = m[static_cast<size_t>(k)];
        if (coda ? x.coda && (name.empty() || x.codaName.empty() || x.codaName == name)
                 : x.segno && (name.empty() || x.segnoName.empty() || x.segnoName == name))
          return k;
      }
    return -1;
  };
  std::vector<int> out;
  std::vector<int> taken(static_cast<size_t>(n), 0);  // passes back from each backward repeat
  int i = 0, repeatStart = 0, pass = 1;
  bool jumped = false, cameBack = false;
  const size_t guard = static_cast<size_t>(n) * 16 + 64;
  while (i >= 0 && i < n && out.size() < guard) {
    const auto& x = m[static_cast<size_t>(i)];
    if (x.forward && !cameBack) {
      repeatStart = i;
      pass = 1;
    }
    cameBack = false;
    if (x.endingStart) {
      const int last = endingLast[static_cast<size_t>(i)];
      const bool another = last + 1 < n && m[static_cast<size_t>(last + 1)].endingStart;
      const bool skip = jumped ? another : !x.endings.empty() && x.endings.count(pass) == 0;
      if (skip) {
        i = last + 1;
        continue;
      }
    }
    out.push_back(i);
    if (jumped && x.fine) break;
    if (jumped && x.tocoda) {
      const int k = findMark(true, x.tocodaName, i + 1);
      if (k > i) {
        i = k;
        continue;
      }
    }
    if (x.backward && !jumped) {
      int& count = taken[static_cast<size_t>(i)];
      if (count + 1 < x.times) {
        ++count;
        pass = count + 1;
        i = repeatStart;
        cameBack = true;
        continue;
      }
      count = 0;
      repeatStart = i + 1;
    }
    if (!jumped && x.dacapo) {
      jumped = true;
      repeatStart = 0;
      i = 0;
      continue;
    }
    if (!jumped && x.dalsegno) {
      const int k = findMark(false, x.dalsegnoName, 0);
      if (k >= 0) {
        jumped = true;
        repeatStart = k;
        i = k;
        continue;
      }
    }
    ++i;
  }
  return out;
}

// --- pitches and ornaments --------------------------------------------------

int stepIndex(char step) {
  const char* steps = "CDEFGAB";
  const char* at = std::strchr(steps, step);
  return at != nullptr && step != 0 ? static_cast<int>(at - steps) : -1;
}

int semitoneOf(int index) {
  static const int base[] = {0, 2, 4, 5, 7, 9, 11};
  return base[index];
}

// The alteration the key signature gives a step.
int keyAlter(int index, int fifths) {
  const char step = "CDEFGAB"[index];
  if (fifths > 0) {
    const char* sharps = "FCGDAEB";
    const auto pos = static_cast<int>(std::strchr(sharps, step) - sharps);
    return pos < fifths ? 1 : 0;
  }
  if (fifths < 0) {
    const char* flats = "BEADGCF";
    const auto pos = static_cast<int>(std::strchr(flats, step) - flats);
    return pos < -fifths ? -1 : 0;
  }
  return 0;
}

int accidentalAlter(const std::string& name, int otherwise) {
  if (name == "sharp") return 1;
  if (name == "flat") return -1;
  if (name == "natural") return 0;
  if (name == "double-sharp" || name == "sharp-sharp") return 2;
  if (name == "flat-flat") return -2;
  return otherwise;
}

}  // namespace

struct ScorePlayback::Impl {
  pugi::xml_document doc;
};

ScorePlayback::ScorePlayback() : impl_(new Impl) {}
ScorePlayback::~ScorePlayback() = default;

std::string ScorePlayback::originalId(const std::string& id) {
  const auto at = id.rfind("-mpr");
  if (at == std::string::npos || at + 4 >= id.size()) return id;
  for (size_t k = at + 4; k < id.size(); ++k)
    if (id[k] < '0' || id[k] > '9') return id;
  return id.substr(0, at);
}

bool ScorePlayback::load(const std::string& musicXml) {
  order_.clear();
  numbers_.clear();
  repeats_ = false;
  ornaments_.clear();
  impl_->doc.reset();
  if (!impl_->doc.load_buffer(musicXml.data(), musicXml.size())) return false;
  const auto root = impl_->doc.document_element();
  if (std::string(root.name()) != "score-partwise") return true;  // a timewise score plays as written

  // The repeats, from the first part: every part carries the same signs.
  const auto first = root.child("part");
  std::vector<Measure> measures;
  for (const auto measure : first.children("measure")) {
    measures.push_back(readMeasure(measure));
    numbers_.emplace_back(measure.attribute("number").value());
  }
  order_ = playOrder(measures);
  repeats_ = order_.size() != measures.size();
  for (size_t k = 0; k < order_.size() && !repeats_; ++k) repeats_ = order_[k] != static_cast<int>(k);

  // The ornaments, each note's neighbours taken from the key in force.
  for (const auto part : root.children("part")) {
    int fifths = 0;
    for (const auto measure : part.children("measure")) {
      for (const auto child : measure.children()) {
        const std::string name = child.name();
        if (name == "attributes") {
          if (const auto key = child.child("key").child("fifths")) fifths = std::atoi(key.child_value());
          continue;
        }
        if (name != "note") continue;
        const auto ornaments = child.child("notations").child("ornaments");
        const auto pitch = child.child("pitch");
        const auto id = child.attribute("id");
        if (!ornaments || !pitch || !id) continue;
        ScoreOrnament o;
        if (ornaments.child("trill-mark") || ornaments.child("shake")) o.kind = ScoreOrnament::Kind::Trill;
        else if (ornaments.child("inverted-mordent")) o.kind = ScoreOrnament::Kind::InvertedMordent;
        else if (ornaments.child("mordent")) o.kind = ScoreOrnament::Kind::Mordent;
        else if (ornaments.child("inverted-turn")) o.kind = ScoreOrnament::Kind::InvertedTurn;
        else if (ornaments.child("turn") || ornaments.child("delayed-turn")) o.kind = ScoreOrnament::Kind::Turn;
        else continue;
        const int index = stepIndex(pitch.child_value("step")[0]);
        if (index < 0) continue;
        const int octave = std::atoi(pitch.child_value("octave"));
        const int alter = static_cast<int>(std::lround(std::atof(pitch.child_value("alter"))));
        o.pitch = 12 * (octave + 1) + semitoneOf(index) + alter;
        // An accidental over or under the sign names the neighbour's.
        int upperAlter = 99, lowerAlter = 99;
        for (const auto mark : ornaments.children("accidental-mark")) {
          const std::string place = mark.attribute("placement").value();
          const int a = accidentalAlter(mark.child_value(), 99);
          if (place == "below" || (place.empty() && o.kind == ScoreOrnament::Kind::Mordent)) lowerAlter = a;
          else upperAlter = a;
        }
        const int up = (index + 1) % 7, upOctave = octave + (index == 6 ? 1 : 0);
        const int down = (index + 6) % 7, downOctave = octave - (index == 0 ? 1 : 0);
        if (upperAlter == 99) upperAlter = keyAlter(up, fifths);
        if (lowerAlter == 99) lowerAlter = keyAlter(down, fifths);
        o.upper = 12 * (upOctave + 1) + semitoneOf(up) + upperAlter - o.pitch;
        o.lower = 12 * (downOctave + 1) + semitoneOf(down) + lowerAlter - o.pitch;
        if (o.upper < 1 || o.upper > 3) o.upper = 2;
        if (o.lower > -1 || o.lower < -3) o.lower = -2;
        ornaments_[id.value()] = o;
      }
    }
  }
  return true;
}

std::string ScorePlayback::unrolled() const {
  if (!repeats_) return {};
  pugi::xml_document copy;
  copy.reset(impl_->doc);
  const auto root = copy.document_element();
  for (auto part : root.children("part")) {
    std::vector<pugi::xml_node> measures;
    for (const auto measure : part.children("measure")) measures.push_back(measure);
    std::vector<int> seen(measures.size(), 0);
    pugi::xml_document played;
    auto holder = played.append_child("part");
    for (const int index : order_) {
      if (index < 0 || static_cast<size_t>(index) >= measures.size()) continue;
      auto m = holder.append_copy(measures[static_cast<size_t>(index)]);
      const int pass = ++seen[static_cast<size_t>(index)];
      // Played again: its own ids, which name the notes of the page.
      if (pass > 1)
        for (const auto& x : m.select_nodes(".//*[@id]")) {
          auto a = x.node().attribute("id");
          a.set_value((std::string(a.value()) + "-mpr" + std::to_string(pass)).c_str());
        }
      // The repeats and the jumps are in the order now.
      for (auto barline = m.child("barline"); barline;) {
        const auto next = barline.next_sibling("barline");
        barline.remove_child("repeat");
        barline.remove_child("ending");
        barline = next;
      }
      for (const auto& x : m.select_nodes(".//sound")) {
        auto s = x.node();
        for (const char* a : {"segno", "coda", "dacapo", "dalsegno", "tocoda", "fine"}) s.remove_attribute(a);
      }
    }
    for (const auto measure : measures) part.remove_child(measure);
    for (const auto measure : holder.children("measure")) part.append_copy(measure);
  }
  struct Writer : pugi::xml_writer {
    std::string out;
    void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
  } w;
  copy.save(w, "", pugi::format_raw);
  return w.out;
}

}  // namespace mp
