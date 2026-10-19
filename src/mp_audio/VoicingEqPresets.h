// Presets for the voicing EQ: ten that come with the program and cannot be
// changed, and as many of the player's own as they save. Program-wide, as the
// default EQ is: speakers, the room and the player's ear are the same whatever
// organ is played.
//
// Each has an id that never changes, for a MIDI mapping and for anything that
// controls the program from outside: 1 to 10 for the program's, 101 upward
// for the player's, never given twice. A preset holds the bands and the gain;
// recalled, it turns the EQ on.
#pragma once

#include "VoicingEq.h"

#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace mp {

struct VoicingEqPreset {
  int id = 0;
  std::string name; // the program's are English, translated where shown
  VoicingEqSettings eq;
  bool factory() const { return id >= 1 && id <= 100; }
};

class VoicingEqPresets {
public:
  static constexpr int kFirstOwn = 101;

  // The ten that come with the program, by the bands from 31 Hz to 16 kHz.
  static const std::vector<VoicingEqPreset>& factory() {
    static const std::vector<VoicingEqPreset> presets = [] {
      std::vector<VoicingEqPreset> v;
      auto add = [&v](const char* name, std::array<double, VoicingEqSettings::kBands> bands, double gain) {
        VoicingEqPreset p;
        p.id = static_cast<int>(v.size()) + 1;
        p.name = name;
        p.eq.on = true;
        p.eq.bandsDb = bands;
        p.eq.gainDb = gain;
        v.push_back(p);
      };
      //                    31.5  63   125  250  500   1k   2k   4k   8k  16k
      add("Flat",           {0,   0,   0,   0,   0,   0,   0,   0,   0,   0}, 0.0);
      // The upper harmonics and the speaking transient, for a set that sounds
      // dull through the speakers.
      add("Brighter",       {0,   0,   0,   0,   0,   0,   1,   2,   3,   3}, -1.0);
      // The band the chiff of a flue pipe's attack lives in.
      add("Chiff",          {0,   0,   0,   0,   0,   1,   2,   3,   2,   1}, -1.5);
      add("Warm",           {1,   2,   2,   1,   0,   0,   0,  -1,  -2,  -3}, -1.0);
      // A room whose bass booms, or speakers close to a wall.
      add("Less boom",      {-3, -3,  -2,  -1,   0,   0,   0,   0,   0,   0}, 0.0);
      // Speakers that cannot reach the pedal's fundamentals.
      add("Small speakers", {5,   4,   2,   0,   0,   0,   0,   0,   0,   0}, -3.0);
      // Mixtures and reeds that sound harsh in a small room.
      add("Softer treble",  {0,   0,   0,   0,   0,   0,  -1,  -2,  -3,  -3}, 0.0);
      // Less of the low middle that makes a full registration muddy.
      add("Clarity",        {0,   0,  -1,  -2,  -2,   0,   1,   1,   0,   0}, 0.0);
      // The ear hears less bass and treble at a low level: both lifted.
      add("Quiet listening",{5,   4,   2,   0,   0,   0,   0,   1,   2,   3}, -3.0);
      add("Headphones",     {1,   1,   0,   0,   0,   0,  -1, -1.5,  -1,   0}, 0.0);
      return v;
    }();
    return presets;
  }

  const std::vector<VoicingEqPreset>& own() const { return own_; }

  // The program's, then the player's, in the order they were saved.
  std::vector<VoicingEqPreset> all() const {
    std::vector<VoicingEqPreset> v = factory();
    v.insert(v.end(), own_.begin(), own_.end());
    return v;
  }

  const VoicingEqPreset* find(int id) const {
    for (const auto& p : factory())
      if (p.id == id) return &p;
    for (const auto& p : own_)
      if (p.id == id) return &p;
    return nullptr;
  }
  // By name, the player's first, then the program's; case as given.
  const VoicingEqPreset* findNamed(const std::string& name) const {
    for (const auto& p : own_)
      if (p.name == name) return &p;
    for (const auto& p : factory())
      if (p.name == name) return &p;
    return nullptr;
  }

  // The preset this EQ is, or 0 when it is none of them: the bands and the gain
  // the same to a twentieth of a decibel. The On box does not count.
  int matching(const VoicingEqSettings& eq) const {
    auto same = [&eq](const VoicingEqSettings& p) {
      if (std::fabs(p.gainDb - eq.gainDb) > 0.05) return false;
      for (size_t i = 0; i < p.bandsDb.size(); ++i)
        if (std::fabs(p.bandsDb[i] - eq.bandsDb[i]) > 0.05) return false;
      return true;
    };
    for (const auto& p : own_)
      if (same(p.eq)) return p.id;
    for (const auto& p : factory())
      if (same(p.eq)) return p.id;
    return 0;
  }

  // A new preset of the player's from this EQ. Returns its id.
  int add(const std::string& name, const VoicingEqSettings& eq) {
    VoicingEqPreset p;
    p.id = nextId_++;
    p.name = name.empty() ? "Preset " + std::to_string(p.id - kFirstOwn + 1) : name;
    p.eq = eq;
    p.eq.on = true;
    own_.push_back(p);
    return p.id;
  }
  // The player's own only; the program's stay as they come.
  bool update(int id, const VoicingEqSettings& eq) {
    VoicingEqPreset* p = ownPreset(id);
    if (p == nullptr) return false;
    p->eq = eq;
    p->eq.on = true;
    return true;
  }
  bool rename(int id, const std::string& name) {
    VoicingEqPreset* p = ownPreset(id);
    if (p == nullptr || name.empty()) return false;
    p->name = name;
    return true;
  }
  bool remove(int id) {
    for (auto it = own_.begin(); it != own_.end(); ++it)
      if (it->id == id) {
        own_.erase(it);
        return true;
      }
    return false;
  }

  // The preset after (step 1) or before (step -1) this one, round the whole
  // list; from none, the first or the last.
  int step(int fromId, int step) const {
    const auto list = all();
    if (list.empty()) return 0;
    const int n = static_cast<int>(list.size());
    int at = -1;
    for (int i = 0; i < n; ++i)
      if (list[static_cast<size_t>(i)].id == fromId) at = i;
    if (at < 0) return list[step > 0 ? 0 : static_cast<size_t>(n - 1)].id;
    return list[static_cast<size_t>(((at + step) % n + n) % n)].id;
  }

  // The settings file's lines, one per preset of the player's:
  //   eqpreset <id> <gain> <band1> ... <band10> <name>
  // and `eqpresetnext <id>`, so an id is not given again after a delete.
  std::vector<std::string> toLines() const {
    std::vector<std::string> lines;
    for (const auto& p : own_) {
      std::ostringstream o;
      o << "eqpreset " << p.id << " " << p.eq.gainDb;
      for (double b : p.eq.bandsDb) o << " " << b;
      o << " " << p.name;
      lines.push_back(o.str());
    }
    if (!own_.empty() || nextId_ != kFirstOwn) lines.push_back("eqpresetnext " + std::to_string(nextId_));
    return lines;
  }
  // The value of an `eqpreset` line (after the key). False and nothing kept
  // when it does not read.
  bool readLine(const std::string& value) {
    std::istringstream in(value);
    VoicingEqPreset p;
    if (!(in >> p.id >> p.eq.gainDb) || p.id < kFirstOwn) return false;
    for (double& b : p.eq.bandsDb)
      if (!(in >> b)) return false;
    std::getline(in >> std::ws, p.name);
    if (p.name.empty() || find(p.id) != nullptr) return false;
    auto clamp = [](double v) {
      return v < -VoicingEqSettings::kMaxDb ? -VoicingEqSettings::kMaxDb
             : v > VoicingEqSettings::kMaxDb ? VoicingEqSettings::kMaxDb
                                             : v;
    };
    p.eq.gainDb = clamp(p.eq.gainDb);
    for (double& b : p.eq.bandsDb) b = clamp(b);
    p.eq.on = true;
    own_.push_back(p);
    if (p.id >= nextId_) nextId_ = p.id + 1;
    return true;
  }
  void readNext(int next) {
    if (next > nextId_) nextId_ = next;
  }
  void clearOwn() {
    own_.clear();
    nextId_ = kFirstOwn;
  }

private:
  VoicingEqPreset* ownPreset(int id) {
    for (auto& p : own_)
      if (p.id == id) return &p;
    return nullptr;
  }
  std::vector<VoicingEqPreset> own_;
  int nextId_ = kFirstOwn;
};

} // namespace mp
