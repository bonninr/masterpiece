#include "JOrganImport.h"

#include "JOrganDisposition.h"
#include "SoundFont2.h"

#include "mp_core/PathCase.h"
#include "mp_core/Utf8Path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace mp {
namespace {

// Bumped whenever the conversion changes what it writes, so a work folder
// from an earlier version is made again.
constexpr int kImporterVersion = 8;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

uint64_t fnv(const std::string& bytes, uint64_t h = 1469598103934665603ull) {
  for (unsigned char c : bytes) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex(uint64_t v, int digits) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf + 16 - digits);
}

std::string n3(int n) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%03d", n);
  return buf;
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(pathOf(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// A name fit for a file: letters, digits, dot, dash.
std::string fileSafe(const std::string& s) {
  std::string out;
  for (unsigned char c : s) out += std::isalnum(c) || c == '-' || c == '.' ? static_cast<char>(c) : '_';
  return out.empty() ? std::string("x") : out;
}

// A sound's SoundFont on this machine: the path the disposition gives,
// relative to the disposition's folder, in any case; and when that is not
// there -- dispositions shared between players name "C:\Sounds\..." -- the
// file of that name beside the disposition.
std::filesystem::path soundFontPath(const std::string& dispositionDir, const std::string& font) {
  std::string rel = font;
  std::replace(rel.begin(), rel.end(), '\\', '/');
  const bool windowsAbsolute = rel.size() > 2 && rel[1] == ':' && rel[2] == '/';
  std::filesystem::path p = pathOf(rel);
  if (!p.is_absolute() && !windowsAbsolute) p = pathOf(dispositionDir) / p;
  p = resolvePathIgnoringCase(p);
  std::error_code ec;
  if (std::filesystem::exists(p, ec)) return p;
  return resolvePathIgnoringCase(pathOf(dispositionDir) / pathOf(rel.substr(rel.find_last_of('/') + 1)));
}

// What a control's knob shows: the "stop=" line of its description, which
// jOrgan's skins print on the knob, or else its name without the keyboard
// prefix ("M3.Flute.Tibia.8'" shows "Flute.Tibia.8'").
std::string labelOf(const std::string& name, const std::string& description) {
  std::istringstream lines(description);
  for (std::string line; std::getline(lines, line);) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
    if (lower(key) != "stop") continue;
    std::string out;
    for (char ch : value) {
      const bool space = std::isspace(static_cast<unsigned char>(ch)) != 0;
      if (space && (out.empty() || out.back() == ' ')) continue;
      out += space ? ' ' : ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty()) return out;
  }
  if (name.size() > 3 && (name[0] == 'M' || name[0] == 'm') && std::isdigit(static_cast<unsigned char>(name[1]))) {
    const auto dot = name.find('.');
    if (dot != std::string::npos && dot + 1 < name.size()) return name.substr(dot + 1);
  }
  return name;
}

// A text for one line of a GrandOrgue definition.
std::string oneLine(std::string s) {
  for (char& c : s)
    if (c == '\r' || c == '\n') c = ' ';
  return s;
}

// The cents a SoundFont's initialAttenuation stands for, as FluidSynth --
// jOrgan's player -- applies it: 0.4 of the value, as the hardware the format
// came from did.
constexpr double kAttenuationFactor = 0.4;

// A rank's sound, as its engaged messages choose it.
struct RankSound {
  int sound = 0;          // the sound element's id
  int bank = 0, bankLsb = -1, program = 0;
  int velocity = 127;     // what its notes are played with
  bool fixedVelocity = false;
  std::vector<int> enclosures, tremulants;  // filter element ids
};

// What a continuous filter does to the level of its ranks: the amplitude
// it sends at value 0 and 1, whether anything moves it, and, when nothing
// does, the level it holds.
struct FilterLevel {
  bool level = false, moved = false, bend = false;
  double closed = 1.0, open = 1.0, fixedDb = 0.0;
  double bendCents = 0.0;  // a tuning knob: the pitch bend it sends at its stored value
};

// What a tremulant filter does to a rank.
struct TremulantKind {
  bool wave = false;      // switches the rank to another program
  int programDelta = 0;   // ... this far from its own
  bool synth = false;     // sends the modulation controller
};

// The level a controller value stands for in FluidSynth's default
// modulators for volume and expression: concave, so amplitude goes with the
// square of the value.
double controllerAmplitude(int value) {
  const double x = std::clamp(value, 0, 127) / 127.0;
  return x * x;
}

struct Converter {
  const JOrganDisposition& d;
  std::string dispositionDir, work;
  JOrganImportReport& rep;

  std::map<int, std::unique_ptr<SoundFont2>> fonts;  // by sound element id
  std::map<int, double> soundGainDb;
  std::map<std::string, std::string> wavByKey;
  int wavCount = 0;

  void note(const std::string& s) {
    if (std::find(rep.notes.begin(), rep.notes.end(), s) == rep.notes.end()) rep.notes.push_back(s);
  }

  bool isSound(const JOrganElement& e) const {
    return e.type.size() >= 5 && lower(e.type).find("sound") != std::string::npos;
  }
  bool isStop(const JOrganElement& e) const { return e.type == "stop"; }
  bool isCoupler(const JOrganElement& e) const { return e.type == "coupler"; }

  // ---- sounds --------------------------------------------------------------
  void openSounds() {
    double loudest = 0.0;
    for (const JOrganElement& e : d.elements)
      if (isSound(e)) loudest = std::max(loudest, e.real("gain", 0.5));
    for (const JOrganElement& e : d.elements) {
      if (!isSound(e)) continue;
      const std::string font = e.prop("soundfont");
      if (font.empty()) {
        note("sound \"" + e.name + "\" (" + e.type + ") names no SoundFont; its ranks are silent");
        continue;
      }
      const std::filesystem::path p = soundFontPath(dispositionDir, font);
      auto sf = std::make_unique<SoundFont2>();
      std::string err;
      if (!sf->open(utf8Of(p), err)) {
        note("SoundFont " + font + ": " + err + "; its ranks are silent");
        continue;
      }
      fonts[e.id] = std::move(sf);
      const double gain = e.real("gain", 0.5);
      soundGainDb[e.id] = loudest > 0 && gain > 0 ? 20.0 * std::log10(gain / loudest) : 0.0;
    }
  }

  // ---- ranks ---------------------------------------------------------------
  RankSound soundOf(const JOrganElement& rank) {
    RankSound rs;
    for (const JOrganReference& r : rank.references) {
      const JOrganElement* t = d.find(r.id);
      if (!t) continue;
      if (isSound(*t) && !rs.sound) rs.sound = t->id;
      else if (t->type == "continuousFilter") rs.enclosures.push_back(t->id);
      else if (t->type == "switchFilter") rs.tremulants.push_back(t->id);
    }
    for (const std::string& m : rank.messagesOf("rank-engaged")) {
      const std::vector<int> b = mplBytes(m);
      if (b.empty() || b[0] < 0) continue;
      const int status = b[0] & 0xf0;
      if (status == 0xb0 && b.size() >= 3 && b[1] == 0 && b[2] >= 0) rs.bank = b[2];
      if (status == 0xb0 && b.size() >= 3 && b[1] == 32 && b[2] >= 0) rs.bankLsb = b[2];
      if (status == 0xc0 && b.size() >= 2 && b[1] >= 0) rs.program = b[1];
    }
    for (const std::string& m : rank.messagesOf("rank-notePlayed")) {
      const std::vector<int> b = mplBytes(m, {{"pitch", 60}});
      if (b.size() >= 3 && b[2] >= 0) {
        rs.velocity = b[2];
        rs.fixedVelocity = true;
      }
    }
    if (!rs.fixedVelocity) note("ranks played with the key velocity sound as at full velocity");
    return rs;
  }

  // The preset a rank's bank and program name, as FluidSynth finds it: the
  // bank select MSB, then the LSB, then bank 0.
  int presetOf(const SoundFont2& sf, int bank, int lsb, int program) {
    int p = sf.findPreset(bank, program);
    if (p < 0 && lsb >= 0) p = sf.findPreset(lsb, program);
    if (p < 0 && lsb >= 0) p = sf.findPreset(bank * 128 + lsb, program);
    if (p < 0) p = sf.findPreset(0, program);
    return p;
  }

  TremulantKind tremulantKind(const JOrganElement& f, int program) {
    TremulantKind k;
    for (const std::string& m : f.messagesOf("switchFilter-engaged")) {
      const std::vector<int> b = mplBytes(m, {{"program", static_cast<double>(program)}});
      if (b.empty() || b[0] < 0) continue;
      if ((b[0] & 0xf0) == 0xc0 && b.size() >= 2 && b[1] >= 0 && b[1] != program) {
        k.wave = true;
        k.programDelta = b[1] - program;
      }
      if ((b[0] & 0xf0) == 0xb0 && b.size() >= 3 && b[1] == 1) k.synth = true;
    }
    return k;
  }

  // ---- samples -------------------------------------------------------------
  // The WAV file for a region, made once: the sample's frames, its loop, and
  // after the loop's end the tail the release envelope plays.
  std::string wavFor(const SoundFont2& sf, int soundId, const Sf2Region& r, const Sf2Region* right) {
    const auto& samples = sf.samples();
    const Sf2Sample& s = samples[r.sample];
    // Release and attack times to the nearest octave step: a SoundFont that
    // gives every key a slightly different release would otherwise write
    // the same sample once per key.
    auto octaveStep = [](double sec, double floor) {
      return sec <= floor ? floor : floor * std::pow(2.0, std::round(std::log2(sec / floor)));
    };
    const double relSec = octaveStep(r.releaseSec, 0.05);
    const double atkSec = r.attackSec <= 0.005 ? 0.0 : octaveStep(r.attackSec, 0.01);
    const int releaseMs = static_cast<int>(std::lround(relSec * 1000.0));
    const int attackMs = static_cast<int>(std::lround(atkSec * 1000.0));
    std::ostringstream key;
    key << soundId << ':' << r.sample << ':' << (right ? right->sample : -1) << ':' << r.startOffset << ':'
        << r.endOffset << ':' << r.loopStartOffset << ':' << r.loopEndOffset << ':' << r.loopMode << ':'
        << releaseMs << ':' << attackMs;
    const auto found = wavByKey.find(key.str());
    if (found != wavByKey.end()) return found->second;

    auto frames = [&](const Sf2Sample& smp, const Sf2Region& reg, std::vector<float>& out, int64_t& ls,
                      int64_t& le) {
      const int64_t from = std::max<int64_t>(0, static_cast<int64_t>(smp.start) + reg.startOffset);
      const int64_t to = std::max<int64_t>(from, static_cast<int64_t>(smp.end) + reg.endOffset);
      ls = static_cast<int64_t>(smp.loopStart) + reg.loopStartOffset - from;
      le = static_cast<int64_t>(smp.loopEnd) + reg.loopEndOffset - from;
      return sf.readFrames(static_cast<uint32_t>(from), static_cast<uint32_t>(to), out);
    };
    std::vector<float> left, rightFrames;
    int64_t ls = -1, le = -1, rls = -1, rle = -1;
    if (!frames(s, r, left, ls, le) || left.size() < 2) return {};
    const int channels = right ? 2 : 1;
    if (right && (!frames(samples[right->sample], *right, rightFrames, rls, rle) || rightFrames.empty()))
      return {};
    const int64_t len = static_cast<int64_t>(left.size());
    const bool looped = (r.loopMode == 1 || r.loopMode == 3) && ls >= 0 && le > ls + 2 && le <= len;

    // The attack envelope, as a ramp, where it ends before the loop.
    const int64_t attackFrames = static_cast<int64_t>(atkSec * s.rate);
    const bool ramp = attackFrames > static_cast<int64_t>(s.rate / 200) && (!looped || attackFrames <= ls);
    if (attackFrames > static_cast<int64_t>(s.rate / 200) && !ramp)
      note("a volume attack that runs into the sample's loop is left out");

    auto at = [&](const std::vector<float>& src, int64_t i) {
      return i >= 0 && i < static_cast<int64_t>(src.size()) ? src[static_cast<size_t>(i)] : 0.0f;
    };
    std::vector<float> out;
    int64_t bodyEnd = looped ? le : len;
    int64_t tail = 0;
    if (looped) {
      // FluidSynth's release falls 100 dB over the release time; the tail
      // ends 70 dB down, and is at least 20 ms long.
      const double sec = std::clamp(relSec * 0.7, 0.02, 6.0);
      tail = static_cast<int64_t>(sec * s.rate);
    }
    out.reserve(static_cast<size_t>((bodyEnd + tail) * channels));
    for (int64_t i = 0; i < bodyEnd + tail; ++i) {
      double g = 1.0;
      int64_t src = i;
      if (i >= bodyEnd) {
        const int64_t t = i - bodyEnd;
        src = r.loopMode == 1 ? ls + (t % (le - ls)) : le + t;  // loop on, or play past the loop
        const double secs = static_cast<double>(t) / s.rate;
        g = std::pow(10.0, -100.0 * secs / relSec / 20.0);
        const int64_t fadeFrom = tail - static_cast<int64_t>(s.rate / 200);
        if (t > fadeFrom) g *= static_cast<double>(tail - t) / std::max<int64_t>(1, tail - fadeFrom);
      }
      if (ramp && i < attackFrames) g *= static_cast<double>(i) / attackFrames;
      out.push_back(static_cast<float>(at(left, src) * g));
      if (channels == 2) out.push_back(static_cast<float>(at(rightFrames, src) * g));
    }

    const std::string rel = "samples/" + fileSafe(std::to_string(soundId)) + "/" + n3(++wavCount) + "-" +
                            fileSafe(s.name) + ".wav";
    std::error_code ec;
    std::filesystem::create_directories((pathOf(work) / pathOf(rel)).parent_path(), ec);
    std::string err;
    if (!writeWav(utf8Of(pathOf(work) / pathOf(rel)), out, channels, s.rate, sf.bitsPerSample(), r.rootKey,
                  -s.pitchCorrection, looped ? ls : -1, looped ? le : -1, looped ? le : -1, err)) {
      note(err);
      return {};
    }
    wavByKey[key.str()] = rel;
    return rel;
  }

  // The region that plays `key` at `velocity`, and its right channel when the
  // region is the left of a stereo pair.
  bool regionFor(const SoundFont2& sf, const std::vector<Sf2Region>& regions, int key, int velocity,
                 const Sf2Region*& main, const Sf2Region*& right) {
    std::vector<const Sf2Region*> hits;
    for (const Sf2Region& r : regions)
      if (key >= r.keyLo && key <= r.keyHi && velocity >= r.velLo && velocity <= r.velHi) hits.push_back(&r);
    main = right = nullptr;
    if (hits.empty()) return false;
    const auto& samples = sf.samples();
    // A stereo pair: the left sample and the right one it links to.
    for (const Sf2Region* a : hits) {
      if (!(samples[a->sample].type & 4)) continue;
      for (const Sf2Region* b : hits)
        if (b != a && (samples[b->sample].type & 2) && samples[a->sample].link == b->sample) {
          main = a;
          right = b;
          return true;
        }
    }
    // Several layers: the one made for the fewest keys. A drum kit lays a
    // filler over the whole keyboard and one instrument on each key.
    main = *std::min_element(hits.begin(), hits.end(), [](const Sf2Region* a, const Sf2Region* b) {
      return a->keyHi - a->keyLo < b->keyHi - b->keyLo;
    });
    if (hits.size() > 1) note("a key with several SoundFont layers plays the layer made for the fewest keys");
    return true;
  }
};

}  // namespace

bool isJOrganDisposition(const std::string& path) {
  const auto dot = path.find_last_of('.');
  return dot != std::string::npos && lower(path.substr(dot)) == ".disposition";
}

std::string jorganWorkFolder(const std::string& dispositionPath, const std::string& baseFolder) {
  std::filesystem::path base = pathOf(baseFolder);
  if (baseFolder.empty()) {
    // No folder given: the user's data folder, not a cache, as favourites
    // and settings name the converted organ.
    auto env = [](const char* name) {
      const char* v = std::getenv(name);
      return v && *v ? std::string(v) : std::string();
    };
#if defined(_WIN32)
    const std::string appData = env("APPDATA");
    base = (appData.empty() ? std::filesystem::temp_directory_path() : pathOf(appData) / "Masterpiece") / "jOrgan";
#elif defined(__APPLE__)
    base = pathOf(env("HOME")) / "Library" / "Application Support" / "Masterpiece" / "jOrgan";
#else
    const std::string xdg = env("XDG_DATA_HOME");
    base = (xdg.empty() ? pathOf(env("HOME")) / ".local" / "share" / "Masterpiece" : pathOf(xdg) / "Masterpiece") /
           "jOrgan";
#endif
  }
  std::error_code ec;
  const std::filesystem::path abs = std::filesystem::absolute(pathOf(dispositionPath), ec);
  const std::string stem = utf8Of(pathOf(dispositionPath).stem());
  return utf8Of(base / (fileSafe(stem) + "-" + hex(fnv(utf8Of(ec ? pathOf(dispositionPath) : abs)), 8)));
}

JOrganImportReport convertJOrganText(const std::string& text, const std::string& dispositionDir,
                                     const std::string& workFolder) {
  JOrganImportReport rep;
  rep.workFolder = workFolder;
  JOrganDisposition d;
  if (!readJOrganDisposition(text, d, rep.error)) return rep;
  std::error_code ec;
  std::filesystem::create_directories(pathOf(workFolder), ec);
  if (ec) {
    rep.error = "cannot make the work folder " + workFolder + ": " + ec.message();
    return rep;
  }

  Converter c{d, dispositionDir, workFolder, rep};
  c.openSounds();

  // ---- keyboards: the manuals ---------------------------------------------
  struct Manual {
    int element = 0;
    std::string name;
    int channel = 99;
    bool pedal = false, secondTouch = false;
    int transpose = 0;  // what the keyboard adds to the pitch of its keys
    int firstMidi = 36, keys = 61;
    std::vector<int> stops, couplers;  // jOrgan ids
  };
  std::vector<Manual> manuals;
  for (const JOrganElement& e : d.elements) {
    if (e.type != "keyboard") continue;
    Manual m;
    m.element = e.id;
    m.name = e.name.empty() ? "Keyboard " + std::to_string(e.id) : e.name;
    for (const std::string& msg : e.messagesOf("keyboard-pressKey")) {
      const int status = mplMatchedStatus(msg);
      if (status >= 0x80) m.channel = std::min(m.channel, status & 0x0f);
      // A keyboard that takes only the harder notes is a second touch.
      const auto g = msg.find("greater ");
      if (g != std::string::npos && std::atoi(msg.c_str() + g + 8) > 0) {
        m.secondTouch = true;
        c.note("keyboard \"" + m.name + "\" takes only the harder notes (second touch); it is a manual of its own");
      }
    }
    m.pedal = lower(m.name).find("ped") != std::string::npos;
    if (m.pedal) m.keys = 32;
    // The keys it takes, where its key message limits the pitch:
    // "greaterEqual 24 | lessEqual 67 | get pitch".
    for (const std::string& msg : e.messagesOf("keyboard-pressKey")) {
      const auto comma = msg.find(',');
      const std::string pitch = comma == std::string::npos ? std::string() : msg.substr(comma + 1);
      const auto ge = pitch.find("greaterEqual "), le = pitch.find("lessEqual ");
      if (ge == std::string::npos || le == std::string::npos) continue;
      const int lo = std::atoi(pitch.c_str() + ge + 13), hi = std::atoi(pitch.c_str() + le + 10);
      if (lo >= 0 && hi <= 127 && hi - lo >= 11) {
        m.firstMidi = lo;
        m.keys = hi - lo + 1;
      }
    }
    // A keyboard that shifts its pitch ("... | sub 12 | get pitch") plays
    // every stop and coupler on it shifted.
    for (const std::string& msg : e.messagesOf("keyboard-pressKey")) {
      const auto comma = msg.find(',');
      std::string pitch = comma == std::string::npos ? std::string() : msg.substr(comma + 1);
      pitch = pitch.substr(0, pitch.find(','));
      pitch = pitch.substr(0, pitch.find("get pitch"));
      std::istringstream steps(pitch);
      for (std::string word; steps >> word;) {
        int n = 0;
        if ((word == "add" || word == "sub") && steps >> n) m.transpose += word == "add" ? n : -n;
      }
      break;
    }
    for (const JOrganReference& r : e.references) {
      const JOrganElement* t = d.find(r.id);
      if (!t) continue;
      if (c.isStop(*t)) m.stops.push_back(t->id);
      else if (c.isCoupler(*t)) m.couplers.push_back(t->id);
    }
    if (m.stops.empty() && m.couplers.empty()) continue;
    manuals.push_back(m);
  }
  if (manuals.empty()) {
    rep.error = "the disposition has no keyboard that plays a stop";
    return rep;
  }
  // The pedal first, then the manuals by their MIDI channel.
  std::stable_sort(manuals.begin(), manuals.end(), [](const Manual& a, const Manual& b) {
    if (a.pedal != b.pedal) return a.pedal;
    if (a.channel != b.channel) return a.channel < b.channel;
    return !a.secondTouch && b.secondTouch;
  });
  if (std::count_if(manuals.begin(), manuals.end(), [](const Manual& m) { return m.pedal; }) > 1) {
    for (size_t i = 1; i < manuals.size(); ++i) manuals[i].pedal = false;
    c.note("only the first pedal keyboard is the pedal; the others are manuals");
  }
  const bool hasPedals = manuals.front().pedal;
  // GrandOrgue numbers: the pedal 0, the manuals from 1.
  auto goManual = [&](size_t index) { return static_cast<int>(index) + (hasPedals ? 0 : 1); };
  // Which manual each stop belongs to. A keyboard that names the stop
  // itself holds it. A theatre disposition reaches most stops through
  // couplers instead -- a keyboard's own "unison" coupler, played while it is
  // off (jOrgan's inverse action), and couplers from the other keyboards --
  // so a stop no keyboard names belongs to the keyboard whose unison coupler
  // reaches it, else whose coupler reaches it at unison, else any coupler's.
  std::map<int, size_t> ownerOfStop, ownerOfCoupler;
  std::map<size_t, std::vector<int>> stopsOwned;  // in the order they are reached
  auto own = [&](int stop, size_t manual) {
    if (ownerOfStop.emplace(stop, manual).second) stopsOwned[manual].push_back(stop);
  };
  // A stop several keyboards name -- a pedal that borrows the manual's
  // stops -- is a stop on each, all worked by one drawstop: a GrandOrgue
  // switch of its own.
  std::map<int, std::vector<size_t>> alsoOn;  // stop -> the other manuals that name it
  for (size_t i = 0; i < manuals.size(); ++i) {
    for (int s : manuals[i].stops) {
      if (ownerOfStop.count(s) && ownerOfStop[s] != i) alsoOn[s].push_back(i);
      own(s, i);
    }
    for (int cp : manuals[i].couplers) ownerOfCoupler.emplace(cp, i);
  }
  auto isUnisonOff = [&](const JOrganElement& cp) {
    return cp.num("action", 0) == 6 && cp.num("transpose", 0) == 0;
  };
  for (int pass = 0; pass < 3; ++pass)
    for (size_t i = 0; i < manuals.size(); ++i)
      for (int cid : manuals[i].couplers) {
        if (ownerOfCoupler[cid] != i) continue;
        const JOrganElement& cp = *d.find(cid);
        if (pass == 0 && !isUnisonOff(cp)) continue;
        if (pass == 1 && cp.num("transpose", 0) != 0) continue;
        for (const JOrganReference& r : cp.references) {
          const JOrganElement* t = d.find(r.id);
          if (t && c.isStop(*t)) own(t->id, i);
        }
      }

  // ---- ranks, filters --------------------------------------------------------
  std::map<int, RankSound> rankSound;
  for (const JOrganElement& e : d.elements)
    if (e.type == "rank") rankSound[e.id] = c.soundOf(e);

  // Enclosures and tremulants in the order the filters appear.
  std::map<int, int> enclosureNo, tremulantNo;
  std::vector<const JOrganElement*> enclosureEls, tremulantEls;
  std::map<int, TremulantKind> tremKindOf;  // by filter id, from the first rank that uses it
  // A continuous filter that sends volume or expression is a swell shoe when
  // something moves it -- a message from the console's shoe -- and a fixed
  // level on its ranks when nothing does: a voicing knob, as the Morton's
  // per-rank amplitudes are. One that sends anything else (a tuning, a tone
  // control) has no counterpart here.
  std::map<int, FilterLevel> filterLevelOf;
  for (const JOrganElement& e : d.elements) {
    if (e.type != "continuousFilter") continue;
    FilterLevel f;
    const double now = std::clamp(e.real("value", 1.0), 0.0, 1.0);
    double atNow = 1.0;
    for (const std::string& m : e.messagesOf("continuousFilter-engaging")) {
      const std::vector<int> b0 = mplBytes(m, {{"value", 0.0}});
      const std::vector<int> b1 = mplBytes(m, {{"value", 1.0}});
      const std::vector<int> bn = mplBytes(m, {{"value", now}});
      if (b0.size() < 3 || b1.size() < 3 || bn.size() < 3 || (b0[0] & 0xf0) != 0xb0 || (b0[1] != 7 && b0[1] != 11))
        continue;
      f.closed *= controllerAmplitude(b0[2]);
      f.open *= controllerAmplitude(b1[2]);
      atNow *= controllerAmplitude(bn[2]);
      f.level = true;
    }
    // A pitch bend is a tuning knob: its ranks keep the tuning it sends at
    // its stored value, with FluidSynth's bend range of two semitones.
    for (const std::string& m : e.messagesOf("continuousFilter-engaging")) {
      const std::vector<int> b = mplBytes(m, {{"value", now}});
      if (b.size() < 3 || (b[0] & 0xf0) != 0xe0 || b[1] < 0 || b[2] < 0) continue;
      f.bend = true;
      f.bendCents = (b[2] * 128 + b[1] - 8192) / 8192.0 * 200.0;
    }
    f.moved = !e.messagesOf("continuous-change").empty();
    if (f.bend) {
      c.note("tuning filters are the tuning of their ranks at their stored value");
    } else if (f.level && f.moved) {
      enclosureNo[e.id] = static_cast<int>(enclosureEls.size()) + 1;
      enclosureEls.push_back(&e);
    } else if (f.level) {
      f.fixedDb = f.open > 0 && atNow > 0 ? 20.0 * std::log10(atNow / f.open) : 0.0;
      c.note("level filters no input moves are the level of their ranks");
    } else {
      c.note("continuous filter \"" + e.name + "\" sends no volume or expression; it is left out");
    }
    filterLevelOf[e.id] = f;
  }
  for (auto& [rid, rs] : rankSound)
    for (int f : rs.tremulants) {
      const JOrganElement* fe = d.find(f);
      if (!fe || tremKindOf.count(f)) continue;
      tremKindOf[f] = c.tremulantKind(*fe, rs.program);
    }
  for (const JOrganElement& e : d.elements) {
    if (e.type != "switchFilter" || !tremKindOf.count(e.id)) continue;
    const TremulantKind& k = tremKindOf[e.id];
    if (!k.wave && !k.synth) {
      c.note("switch filter \"" + e.name + "\" sends messages that have no counterpart here; it is left out");
      continue;
    }
    tremulantNo[e.id] = static_cast<int>(tremulantEls.size()) + 1;
    tremulantEls.push_back(&e);
  }

  // Windchest groups: one per set of filters.
  std::map<std::pair<std::vector<int>, std::vector<int>>, int> chestOf;
  std::vector<std::pair<std::vector<int>, std::vector<int>>> chests;
  auto chestFor = [&](const RankSound& rs) {
    std::vector<int> enc, trem;
    for (int f : rs.enclosures)
      if (enclosureNo.count(f)) enc.push_back(enclosureNo[f]);
    for (int f : rs.tremulants)
      if (tremulantNo.count(f)) trem.push_back(tremulantNo[f]);
    std::sort(enc.begin(), enc.end());
    std::sort(trem.begin(), trem.end());
    const auto key = std::make_pair(enc, trem);
    const auto it = chestOf.find(key);
    if (it != chestOf.end()) return it->second;
    chests.push_back(key);
    return chestOf[key] = static_cast<int>(chests.size());
  };

  // ---- the definition --------------------------------------------------------
  std::ostringstream ranksText, stopsText, couplersText;
  std::map<std::pair<int, int>, int> goRankOf;  // (rank id, constant pitch or -1) -> GO rank
  int rankCount = 0;
  auto buildRank = [&](int rankId, int constantPitch) -> int {
    const auto key = std::make_pair(rankId, constantPitch);
    const auto found = goRankOf.find(key);
    if (found != goRankOf.end()) return found->second;
    const JOrganElement* re = d.find(rankId);
    const RankSound& rs = rankSound[rankId];
    if (!re) return goRankOf[key] = 0;
    // A rank whose SoundFont or preset is missing stays, silent: its stops
    // are on the console all the same.
    const auto font = c.fonts.find(rs.sound);
    const SoundFont2* sf = font == c.fonts.end() ? nullptr : font->second.get();
    std::vector<Sf2Region> regions, tremRegions;
    if (sf) {
      const int preset = c.presetOf(*sf, rs.bank, rs.bankLsb, rs.program);
      if (preset >= 0) regions = sf->regions(preset);
      else
        c.note("rank \"" + re->name + "\": no preset at bank " + std::to_string(rs.bank) + " program " +
               std::to_string(rs.program) + "; it is silent");
      // The tremulant take: the preset a wave tremulant switches to.
      for (int f : rs.tremulants) {
        if (!tremulantNo.count(f) || !tremKindOf[f].wave) continue;
        const int tp = c.presetOf(*sf, rs.bank, rs.bankLsb, rs.program + tremKindOf[f].programDelta);
        if (tp >= 0) tremRegions = sf->regions(tp);
        else c.note("rank \"" + re->name + "\": its tremulant's preset is not in the SoundFont");
        break;
      }
    }
    const int number = ++rankCount;
    goRankOf[key] = number;
    const double velocityDb = 40.0 * std::log10(std::max(1, rs.velocity) / 127.0);
    std::ostringstream& o = ranksText;
    o << "\n[Rank" << n3(number) << "]\n";
    o << "Name=" << oneLine(re->name.empty() ? "Rank " + std::to_string(rankId) : re->name)
      << (constantPitch >= 0 ? " (" + std::to_string(constantPitch) + ")" : "") << "\n";
    o << "FirstMidiNoteNumber=0\nNumberOfLogicalPipes=128\n";
    o << "WindchestGroup=" << chestFor(rs) << "\n";
    o << "AmplitudeLevel=100\n";
    double fixedDb = 0.0, tuningCents = 0.0;
    for (int f : rs.enclosures) {
      if (!filterLevelOf.count(f)) continue;
      if (!enclosureNo.count(f)) fixedDb += filterLevelOf[f].fixedDb;
      tuningCents += filterLevelOf[f].bendCents;
    }
    o << "PitchTuning=" << tuningCents << "\n";
    o << "Gain=" << (velocityDb + c.soundGainDb[rs.sound] + fixedDb) << "\n";
    int sounding = 0;
    for (int k = 0; k < 128; ++k) {
      const std::string pk = "Pipe" + n3(k + 1);
      const int playKey = constantPitch >= 0 ? constantPitch : k;
      const Sf2Region *r = nullptr, *right = nullptr;
      std::string file;
      if (sf && c.regionFor(*sf, regions, playKey, rs.velocity, r, right)) file = c.wavFor(*sf, rs.sound, *r, right);
      if (file.empty()) {
        o << pk << "=DUMMY\n";
        continue;
      }
      ++sounding;
      const double cents = (playKey - r->rootKey) * static_cast<double>(r->scaleTuning) + r->tuneCents;
      const double db = -r->attenuationCb * kAttenuationFactor / 10.0 -
                        (r->decaySec <= 0.2 ? r->sustainCb / 10.0 : 0.0);
      o << pk << "=" << file << "\n";
      if (cents != 0.0) o << pk << "PitchTuning=" << cents << "\n";
      if (db != 0.0) o << pk << "Gain=" << db << "\n";
      const bool looped = r->loopMode == 1 || r->loopMode == 3;
      if (!looped) o << pk << "Percussive=Y\n";
      if (r->filterFcCents < 13500.0) c.note("a SoundFont region's low-pass filter is left out");
      // The tremulant take, with the same retuning from its own root.
      const Sf2Region *tr = nullptr, *tright = nullptr;
      if (!tremRegions.empty() && c.regionFor(*sf, tremRegions, playKey, rs.velocity, tr, tright)) {
        const std::string tf = c.wavFor(*sf, rs.sound, *tr, tright);
        if (!tf.empty()) {
          o << pk << "IsTremulant=0\n";
          o << pk << "AttackCount=1\n";
          o << pk << "Attack001=" << tf << "\n";
          o << pk << "Attack001IsTremulant=1\n";
          const double tc = (playKey - tr->rootKey) * static_cast<double>(tr->scaleTuning) + tr->tuneCents;
          if (tc != cents) c.note("a tremulant take tuned apart from its pipe plays at the pipe's tuning");
        }
      }
    }
    if (sounding == 0 && !regions.empty()) c.note("rank \"" + re->name + "\": its preset sounds no key");
    if (re->num("delay", 0) > 0) c.note("rank delays are left out");
    return number;
  };

  // A drawstop's place, written once the grid's size is known: every
  // control counted, they fill it row by row across both jambs.
  auto place = [](int index) {
    return "DispDrawstopCol=\x01" "C" + std::to_string(index) + "\x01\nDispDrawstopRow=\x01" "R" +
           std::to_string(index) + "\x01\n";
  };

  // Stops, by GrandOrgue number, each on its manual.
  int stopCount = 0, couplerCount = 0;
  std::map<size_t, std::vector<int>> stopsOfManual, couplersOfManual;  // GO numbers
  int drawstop = 0;
  std::ostringstream switchesText;
  std::map<int, int> switchOf;  // shared stop -> its GrandOrgue switch
  for (size_t mi = 0; mi < manuals.size(); ++mi) {
    Manual& m = manuals[mi];
    std::vector<int> here = stopsOwned[mi];
    for (const auto& [sid, others] : alsoOn)
      if (std::find(others.begin(), others.end(), mi) != others.end()) here.push_back(sid);
    for (int sid : here) {
      const JOrganElement& st = *d.find(sid);
      const int action = st.num("action", 0);
      // The pitch the stop hears is the key shifted by its keyboard; it adds
      // its own transposition to that.
      int transpose = st.num("transpose", 0) + m.transpose;
      int constantPitch = -1;
      if (action == 1) {
        constantPitch = std::clamp(60 + st.num("transpose", 0), 0, 127);
        transpose = 0;
      } else if (action != 0) {
        c.note("stop actions other than straight and constant pitch play straight");
      }
      std::vector<int> ranks;
      for (const JOrganReference& r : st.references) {
        const JOrganElement* t = d.find(r.id);
        if (t && t->type == "rank") {
          const int gr = buildRank(t->id, constantPitch);
          if (gr) ranks.push_back(gr);
        }
      }
      if (ranks.empty()) continue;
      const int number = ++stopCount;
      stopsOfManual[mi].push_back(number);
      std::ostringstream& o = stopsText;
      o << "\n[Stop" << n3(number) << "]\n";
      o << "Name=" << oneLine(st.name.empty() ? "Stop " + std::to_string(sid) : st.name) << "\n";
      o << "DispLabelText=" << oneLine(labelOf(st.name.empty() ? "Stop " + std::to_string(sid) : st.name, st.description)) << "\n";
      if (alsoOn.count(sid)) {
        // On several keyboards: worked by the switch they share.
        int& sw = switchOf[sid];
        if (sw == 0) {
          sw = static_cast<int>(switchOf.size());
          switchesText << "\n[Switch" << n3(sw) << "]\n";
          switchesText << "Name=" << oneLine(st.name.empty() ? "Stop " + std::to_string(sid) : st.name) << "\n";
          switchesText << "DispLabelText=" << oneLine(labelOf(st.name.empty() ? "Stop " + std::to_string(sid) : st.name, st.description)) << "\n";
          switchesText << "Displayed=Y\nDefaultToEngaged=" << (st.prop("active") == "true" ? "Y" : "N") << "\n";
          switchesText << place(drawstop);
          ++drawstop;
        }
        o << "Displayed=N\nFunction=And\nSwitchCount=1\nSwitch001=" << sw << "\n";
      } else {
        o << "Displayed=Y\nDefaultToEngaged=" << (st.prop("active") == "true" ? "Y" : "N") << "\n";
        o << place(drawstop);
        ++drawstop;
      }
      o << "FirstAccessiblePipeLogicalKeyNumber=1\nFirstAccessiblePipeLogicalPipeNumber=1\n";
      o << "NumberOfAccessiblePipes=" << m.keys << "\n";
      o << "NumberOfRanks=" << ranks.size() << "\n";
      // The stop's key range and its transposition.
      // Its key range applies to the shifted pitch.
      const int lo = std::max(m.firstMidi, st.num("from", 0) - m.transpose);
      const int hi = std::min(m.firstMidi + m.keys - 1, st.num("to", 127) - m.transpose);
      for (size_t r = 0; r < ranks.size(); ++r) {
        const std::string rk = "Rank" + n3(static_cast<int>(r) + 1);
        int keyLo = lo, keyHi = hi;
        // Rank pipes are MIDI notes 0..127, pipe numbers from 1.
        keyLo = std::max(keyLo, -transpose);
        keyHi = std::min(keyHi, 127 - transpose);
        if (keyHi < keyLo) keyHi = keyLo - 1;
        o << rk << "=" << ranks[r] << "\n";
        o << rk << "FirstAccessibleKeyNumber=" << (keyLo - m.firstMidi + 1) << "\n";
        o << rk << "FirstPipeNumber=" << (keyLo + transpose + 1) << "\n";
        o << rk << "PipeCount=" << std::max(0, keyHi - keyLo + 1) << "\n";
      }
    }
  }

  // Couplers: to the manual that holds the stops they reach.
  for (size_t mi = 0; mi < manuals.size(); ++mi) {
    for (int cid : manuals[mi].couplers) {
      if (ownerOfCoupler[cid] != mi) continue;
      const JOrganElement& cp = *d.find(cid);
      std::map<size_t, int> reached;  // manual -> stops reached there
      bool chained = false;
      for (const JOrganReference& r : cp.references) {
        const JOrganElement* t = d.find(r.id);
        if (!t) continue;
        if (c.isStop(*t) && ownerOfStop.count(t->id)) ++reached[ownerOfStop[t->id]];
        else if (c.isCoupler(*t)) chained = true;
      }
      if (chained) c.note("a coupler that reaches other couplers couples their keyboard's stops only");
      if (reached.empty()) continue;
      if (reached.size() > 1) c.note("a coupler reaching stops on several keyboards is a coupler to each");
      for (const auto& [dest, count] : reached) {
        if (count < static_cast<int>(stopsOwned[dest].size()))
          c.note("a coupler that reaches some of a keyboard's stops couples all of them");
        const int transpose = cp.num("transpose", 0);
        // A keyboard's own coupler at unison, played while off: drawn, it
        // silences the keyboard's stops, as a unison off does. Played while
        // on, it adds nothing.
        const bool unisonOff = dest == mi && isUnisonOff(cp);
        if (dest == mi && transpose == 0 && !unisonOff) continue;
        if (cp.num("action", 0) == 6 && !unisonOff)
          c.note("a coupler played while off (inverse action) to another keyboard plays while on");
        const int number = ++couplerCount;
        couplersOfManual[mi].push_back(number);
        std::ostringstream& o = couplersText;
        o << "\n[Coupler" << n3(number) << "]\n";
        std::string name = cp.name.empty() ? "Coupler " + std::to_string(cid) : cp.name;
        if (reached.size() > 1) name += " (" + manuals[dest].name + ")";
        o << "Name=" << oneLine(name) << "\n";
        o << "DispLabelText=" << oneLine(labelOf(cp.name.empty() ? "Coupler " + std::to_string(cid) : cp.name, cp.description)) << "\n";
        o << "Displayed=Y\nDefaultToEngaged=" << (cp.prop("active") == "true" ? "Y" : "N") << "\n";
        o << place(drawstop);
        ++drawstop;
        o << "UnisonOff=" << (unisonOff ? "Y" : "N") << "\nCouplerType=Normal\n";
        // jOrgan hands the other keyboard's stops this keyboard's shifted
        // pitch plus the coupler's transposition, past that keyboard's own
        // shift; here the coupler presses that keyboard's keys, which shift.
        const int keyshift = transpose + manuals[mi].transpose - manuals[dest].transpose;
        o << "DestinationManual=" << goManual(dest) << "\nDestinationKeyshift=" << keyshift << "\n";
        o << "CoupleToSubsequentUnisonIntermanualCouplers=N\n";
        o << "CoupleToSubsequentUpwardIntermanualCouplers=N\nCoupleToSubsequentDownwardIntermanualCouplers=N\n";
        o << "CoupleToSubsequentUpwardIntramanualCouplers=N\nCoupleToSubsequentDownwardIntramanualCouplers=N\n";
      }
    }
  }

  std::ostringstream o;
  std::string organName;
  for (const JOrganElement& e : d.elements)
    if (e.type == "console" && !e.name.empty()) {
      organName = e.name;
      break;
    }
  if (organName.empty()) organName = utf8Of(pathOf(workFolder).filename());
  // The drawstop grid: two jambs, up to six columns each, as GrandOrgue
  // draws them, about as many rows as columns; the screen as wide as both
  // jambs and the manuals between them, and as tall as the jambs.
  const int controls = drawstop + static_cast<int>(tremulantEls.size());
  const int drawstopCols =
      std::clamp(2 * static_cast<int>(std::ceil(std::sqrt(std::max(1, controls) / 2.0))), 2, 12);
  const int drawstopRows = std::max(1, (controls + drawstopCols - 1) / drawstopCols);
  const int screenW = std::max(800, drawstopCols * 78 + 61 * 12 + 160);
  const int screenH = std::max({500, (drawstopRows + 1) * 69 + 100,
                                static_cast<int>(manuals.size()) * 70 + 260});
  o << "; Made by Masterpiece from a jOrgan disposition (version " << d.version << ")\n";
  o << "[Organ]\nChurchName=" << oneLine(organName) << "\nChurchAddress=\nOrganBuilder=\nOrganBuildDate=\n";
  o << "OrganComments=Converted from a jOrgan disposition\n";
  o << "HasPedals=" << (hasPedals ? "Y" : "N") << "\n";
  o << "NumberOfManuals=" << (manuals.size() - (hasPedals ? 1 : 0)) << "\n";
  o << "NumberOfEnclosures=" << enclosureEls.size() << "\n";
  o << "NumberOfTremulants=" << tremulantEls.size() << "\n";
  o << "NumberOfWindchestGroups=" << std::max<size_t>(1, chests.size()) << "\n";
  o << "NumberOfReversiblePistons=0\nNumberOfGenerals=0\nNumberOfDivisionalCouplers=0\n";
  o << "NumberOfSwitches=" << switchOf.size() << "\n";
  o << "NumberOfRanks=" << rankCount << "\nNumberOfPanels=0\n";
  o << "AmplitudeLevel=100\nPitchTuning=0\n";
  o << "DispScreenSizeHoriz=" << screenW << "\n";
  o << "DispScreenSizeVert=" << screenH << "\n";
  o << "DispDrawstopCols=" << drawstopCols << "\nDispDrawstopRows=" << drawstopRows << "\n";
  o << "DispDrawstopColsOffset=N\nDispPairDrawstopCols=N\nDispExtraDrawstopRows=0\nDispExtraDrawstopCols=0\n";
  o << "DispButtonCols=10\nDispExtraButtonRows=0\nDispButtonsAboveManuals=N\n";
  o << "DispTrimAboveManuals=Y\nDispTrimBelowManuals=Y\nDispTrimAboveExtraRows=N\n";
  o << "DispExtraPedalButtonRow=N\nDispExtraPedalButtonRowOffset=N\nDispExtraDrawstopRowsAboveExtraButtonRows=N\n";
  o << "DispDrawstopBackgroundImageNum=1\nDispConsoleBackgroundImageNum=1\nDispKeyHorizBackgroundImageNum=1\n";
  o << "DispKeyVertBackgroundImageNum=1\nDispDrawstopInsetBackgroundImageNum=1\n";

  for (size_t mi = 0; mi < manuals.size(); ++mi) {
    const Manual& m = manuals[mi];
    o << "\n[Manual" << n3(goManual(mi)) << "]\n";
    o << "Name=" << oneLine(m.name) << "\n";
    o << "NumberOfLogicalKeys=" << m.keys << "\nFirstAccessibleKeyLogicalKeyNumber=1\n";
    o << "FirstAccessibleKeyMIDINoteNumber=" << m.firstMidi << "\nNumberOfAccessibleKeys=" << m.keys << "\n";
    o << "MIDIInputNumber=" << (goManual(mi) + 1) << "\nDisplayed=Y\n";
    const auto& ss = stopsOfManual[mi];
    o << "NumberOfStops=" << ss.size() << "\n";
    for (size_t i = 0; i < ss.size(); ++i) o << "Stop" << n3(static_cast<int>(i) + 1) << "=" << ss[i] << "\n";
    const auto& cs = couplersOfManual[mi];
    o << "NumberOfCouplers=" << cs.size() << "\n";
    for (size_t i = 0; i < cs.size(); ++i) o << "Coupler" << n3(static_cast<int>(i) + 1) << "=" << cs[i] << "\n";
    o << "NumberOfDivisionals=0\nNumberOfTremulants=0\n";
  }
  for (size_t i = 0; i < enclosureEls.size(); ++i) {
    const JOrganElement& e = *enclosureEls[i];
    const FilterLevel& f = filterLevelOf[e.id];
    for (const std::string& m : e.messagesOf("continuousFilter-engaging")) {
      const std::vector<int> b = mplBytes(m, {{"value", 0.0}});
      if (b.size() >= 3 && (b[0] & 0xf0) == 0xb0 && (b[1] == 99 || b[1] == 6))
        c.note("a swell filter's tone filter (sent as NRPN) is left out; the level follows the shoe");
    }
    const double pct = f.open > 0 ? std::clamp(100.0 * f.closed / f.open, 0.1, 100.0) : 1.0;
    o << "\n[Enclosure" << n3(static_cast<int>(i) + 1) << "]\n";
    o << "Name=" << oneLine(e.name.empty() ? "Swell" : e.name) << "\n";
    o << "AmpMinimumLevel=" << pct << "\nMIDIInputNumber=" << (i + 1) << "\nDisplayed=Y\n";
  }
  for (size_t i = 0; i < tremulantEls.size(); ++i) {
    const JOrganElement& e = *tremulantEls[i];
    const TremulantKind& k = tremKindOf[e.id];
    o << "\n[Tremulant" << n3(static_cast<int>(i) + 1) << "]\n";
    o << "Name=" << oneLine(e.name.empty() ? "Tremulant" : e.name) << "\n";
    o << "DispLabelText=" << oneLine(labelOf(e.name.empty() ? "Tremulant" : e.name, e.description)) << "\n";
    o << "Displayed=Y\nDefaultToEngaged=" << (e.prop("active") == "true" ? "Y" : "N") << "\n";
    o << place(drawstop);
    ++drawstop;
    if (k.wave) {
      o << "TremulantType=Wave\n";
    } else {
      // FluidSynth's modulation wheel: its vibrato LFO, 8.2 Hz by default.
      o << "TremulantType=Synth\nPeriod=122\nStartRate=10\nStopRate=10\nAmpModDepth=6\n";
      c.note("a tremulant sent as the modulation controller is a level tremulant at 8.2 Hz");
    }
  }
  if (chests.empty()) chests.push_back({});
  for (size_t i = 0; i < chests.size(); ++i) {
    o << "\n[WindchestGroup" << n3(static_cast<int>(i) + 1) << "]\n";
    o << "Name=Windchest " << (i + 1) << "\n";
    o << "NumberOfEnclosures=" << chests[i].first.size() << "\n";
    for (size_t e = 0; e < chests[i].first.size(); ++e)
      o << "Enclosure" << n3(static_cast<int>(e) + 1) << "=" << chests[i].first[e] << "\n";
    o << "NumberOfTremulants=" << chests[i].second.size() << "\n";
    for (size_t t = 0; t < chests[i].second.size(); ++t)
      o << "Tremulant" << n3(static_cast<int>(t) + 1) << "=" << chests[i].second[t] << "\n";
  }
  o << switchesText.str() << stopsText.str() << couplersText.str() << ranksText.str();

  // What the disposition has that this leaves out.
  std::map<std::string, int> others;
  for (const JOrganElement& e : d.elements)
    if (e.type == "combination" || e.type == "activator" || e.type == "captor" || e.type == "incrementer" ||
        e.type == "regulator" || e.type == "keyer" || e.type == "synchronizer" || e.type.find('.') != std::string::npos)
      if (!c.isSound(e) && e.type.rfind("fluidsynth.", 0) != 0) ++others[e.type];
  for (const auto& [type, n] : others)
    c.note(std::to_string(n) + " " + type + " element(s) are left out");

  rep.organText = o.str();
  // The drawstop places, now that the grid's size is known: "=\x01C12\x01"
  // is column, "=\x01R12\x01" row, of control 12.
  const std::string kPlace = "=\x01";
  for (size_t at = rep.organText.find(kPlace); at != std::string::npos; at = rep.organText.find(kPlace, at)) {
    const size_t end = rep.organText.find('\x01', at + 3);
    if (end == std::string::npos) break;
    const bool col = rep.organText[at + 2] == 'C';
    const int index = std::atoi(rep.organText.c_str() + at + 3);
    const int value = col ? index % drawstopCols + 1 : index / drawstopCols + 1;
    rep.organText.replace(at + 1, end - at, std::to_string(value));
  }
  rep.organFile = utf8Of(pathOf(workFolder) / "organ.organ");
  {
    std::ofstream f(pathOf(rep.organFile), std::ios::binary | std::ios::trunc);
    if (!f || !f.write(rep.organText.data(), static_cast<std::streamsize>(rep.organText.size()))) {
      rep.error = "cannot write " + rep.organFile;
      return rep;
    }
  }
  rep.ok = true;
  return rep;
}

JOrganImportReport convertJOrgan(const std::string& path, const std::string& workFolder) {
  JOrganImportReport rep;
  std::string text;
  if (!readFile(path, text)) {
    rep.error = "cannot open " + path;
    return rep;
  }
  const std::string work = workFolder.empty() ? jorganWorkFolder(path) : workFolder;
  const std::filesystem::path dir = pathOf(path).parent_path();

  // The stamp: this importer, the disposition, and every SoundFont's size
  // and time. A work folder with the same stamp holds this conversion.
  std::string stamp = "jOrgan import " + std::to_string(kImporterVersion) + "\n" + hex(fnv(text), 16) + "\n";
  {
    JOrganDisposition d;
    std::string err;
    if (readJOrganDisposition(text, d, err))
      for (const JOrganElement& e : d.elements) {
        const std::string font = e.prop("soundfont");
        if (font.empty()) continue;
        const std::filesystem::path p = soundFontPath(utf8Of(dir), font);
        std::error_code ec;
        const auto size = std::filesystem::file_size(p, ec);
        const auto time = std::filesystem::last_write_time(p, ec);
        stamp += font + " " + std::to_string(ec ? 0 : size) + " " +
                 std::to_string(ec ? 0 : time.time_since_epoch().count()) + "\n";
      }
  }
  const std::filesystem::path stampFile = pathOf(work) / "import.stamp";
  std::string old, organText;
  if (readFile(utf8Of(stampFile), old) && old == stamp &&
      readFile(utf8Of(pathOf(work) / "organ.organ"), organText)) {
    rep.organText = organText;
    rep.organFile = utf8Of(pathOf(work) / "organ.organ");
    std::string notes;
    if (readFile(utf8Of(pathOf(work) / "import.notes"), notes)) {
      std::istringstream lines(notes);
      for (std::string line; std::getline(lines, line);)
        if (!line.empty()) rep.notes.push_back(line);
    }
    rep.workFolder = work;
    rep.ok = rep.reused = true;
    return rep;
  }
  std::error_code ec;
  std::filesystem::remove_all(pathOf(work) / "samples", ec);
  rep = convertJOrganText(text, utf8Of(dir), work);
  if (rep.ok) {
    std::ofstream n(pathOf(work) / "import.notes", std::ios::binary | std::ios::trunc);
    for (const auto& line : rep.notes) n << line << '\n';
    std::ofstream f(stampFile, std::ios::binary | std::ios::trunc);
    f << stamp;
  }
  return rep;
}

}  // namespace mp
