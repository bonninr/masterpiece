#include "ConsoleProfile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace mp {

// ------------------------------------------------------------ names

namespace {

// U+00C0 to U+00FF, lower case and without accents.
const char* const kLatin1[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o",  " ", "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o",  " ", "o", "u", "u", "u", "u", "y", "th", "y"};

const char* foldCodePoint(uint32_t cp) {
  if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
  switch (cp) {
    case 0x00BD: return " 1/2 ";
    case 0x00BC: return " 1/4 ";
    case 0x2153: return " 1/3 ";
    case 0x2154: return " 2/3 ";
    case 0x2155: return " 1/5 ";
    case 0x2156: return " 2/5 ";
    case 0x2157: return " 3/5 ";
    case 0x0152: case 0x0153: return "oe";
    case 0x0160: case 0x0161: case 0x015A: case 0x015B: return "s";
    case 0x017D: case 0x017E: case 0x0179: case 0x017A: case 0x017B: case 0x017C: return "z";
    case 0x010C: case 0x010D: case 0x0106: case 0x0107: return "c";
    case 0x0158: case 0x0159: return "r";
    case 0x011A: case 0x011B: case 0x0118: case 0x0119: return "e";
    case 0x016E: case 0x016F: return "u";
    case 0x0141: case 0x0142: return "l";
    case 0x0104: case 0x0105: return "a";
    case 0x0143: case 0x0144: case 0x0147: case 0x0148: return "n";
    case 0x0131: return "i";
    default: return " ";
  }
}

std::vector<std::string> tokens(const std::string& folded) {
  std::vector<std::string> out;
  std::istringstream in(folded);
  for (std::string t; in >> t;) out.push_back(t);
  return out;
}

std::string compact(const std::string& folded) {
  std::string out;
  for (char c : folded)
    if (c != ' ') out += c;
  return out;
}

bool isDigits(const std::string& t) {
  return !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool fraction(const std::string& t, double& v) {
  const auto slash = t.find('/');
  if (slash == std::string::npos || slash == 0 || slash + 1 >= t.size()) return false;
  const std::string a = t.substr(0, slash), b = t.substr(slash + 1);
  if (!isDigits(a) || !isDigits(b) || a.size() > 2 || b.size() > 2) return false;
  const double den = std::stod(b);
  if (den <= 0.0) return false;
  v = std::stod(a) / den;
  return true;
}

int romanValue(const std::string& t) {
  static const std::map<std::string, int> roman{{"i", 1},    {"ii", 2},  {"iii", 3}, {"iv", 4},
                                                {"v", 5},    {"vi", 6},  {"vii", 7}, {"viii", 8},
                                                {"ix", 9},   {"x", 10},  {"xi", 11}, {"xii", 12}};
  const auto it = roman.find(t);
  return it == roman.end() ? 0 : it->second;
}

// The footages an organ stop is built at: the unison series and the
// mutations from 32' down.
bool knownFootage(double v) {
  static const double kFeet[] = {64.0,       32.0,      21.0 + 1.0 / 3, 16.0,      10.0 + 2.0 / 3,
                                 8.0,        6.4,       5.0 + 1.0 / 3,  4.0,       3.2,
                                 2.0 + 2.0 / 7, 2.0 + 2.0 / 3, 2.0,       1.6,       1.0 + 1.0 / 3,
                                 1.0 + 1.0 / 7, 1.0,       0.8,           2.0 / 3,   0.5,
                                 0.4,        1.0 / 3,   0.25};
  for (double f : kFeet)
    if (std::abs(v - f) < 0.02) return true;
  return false;
}

bool unisonFootage(double v) {
  for (double f : {64.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25})
    if (std::abs(v - f) < 0.02) return true;
  return false;
}

// The families, by the words that name them, in the order they are looked
// for: a more particular word ahead of the word it contains ("Rohrflöte" is a
// stopped flute before it is a flute, "Quintadena" a stopped flute before a
// quint, "Stopped Diapason" a stopped flute before a diapason).
struct FamilyWord {
  const char* word;
  ToneFamily family;
};
const FamilyWord kFamilyWords[] = {
    {"dulciana", ToneFamily::String},
    // Celestes and undulants.
    {"celeste", ToneFamily::Celeste}, {"coelestis", ToneFamily::Celeste},
    {"undamaris", ToneFamily::Celeste}, {"schwebung", ToneFamily::Celeste},
    {"voceumana", ToneFamily::Celeste}, {"voxangelica", ToneFamily::Celeste},
    // Percussion.
    {"glocken", ToneFamily::Percussion}, {"chime", ToneFamily::Percussion},
    {"cloche", ToneFamily::Percussion}, {"carillon", ToneFamily::Percussion},
    {"celesta", ToneFamily::Percussion}, {"zimbelstern", ToneFamily::Percussion},
    {"cymbelstern", ToneFamily::Percussion}, {"cymbalstern", ToneFamily::Percussion},
    {"harp", ToneFamily::Percussion}, {"harfe", ToneFamily::Percussion},
    {"xylophon", ToneFamily::Percussion}, {"campanel", ToneFamily::Percussion},
    // Regals: short resonators.
    {"voxhumana", ToneFamily::Regal}, {"voixhumaine", ToneFamily::Regal},
    {"regal", ToneFamily::Regal}, {"menschenstimme", ToneFamily::Regal},
    {"barpfeife", ToneFamily::Regal}, {"baerpfeife", ToneFamily::Regal},
    // Solo and half-length reeds.
    {"schalmei", ToneFamily::SoloReed}, {"chalumeau", ToneFamily::SoloReed},
    {"oboe", ToneFamily::SoloReed}, {"hautbois", ToneFamily::SoloReed},
    {"hoboe", ToneFamily::SoloReed}, {"hobo", ToneFamily::SoloReed},
    {"clarinet", ToneFamily::SoloReed}, {"klarinet", ToneFamily::SoloReed},
    {"cromorne", ToneFamily::SoloReed}, {"krummhorn", ToneFamily::SoloReed},
    {"cromorno", ToneFamily::SoloReed}, {"cormorne", ToneFamily::SoloReed},
    {"kromhoorn", ToneFamily::SoloReed}, {"fagot", ToneFamily::SoloReed},
    {"bassoon", ToneFamily::SoloReed}, {"basson", ToneFamily::SoloReed},
    {"cornodibassetto", ToneFamily::SoloReed}, {"coranglais", ToneFamily::SoloReed},
    {"englishhorn", ToneFamily::SoloReed}, {"englischhorn", ToneFamily::SoloReed},
    {"cornoinglese", ToneFamily::SoloReed}, {"musette", ToneFamily::SoloReed},
    {"dulzian", ToneFamily::SoloReed}, {"dulcian", ToneFamily::SoloReed},
    {"dulciaan", ToneFamily::SoloReed}, {"rankett", ToneFamily::SoloReed},
    {"sordun", ToneFamily::SoloReed}, {"frenchhorn", ToneFamily::SoloReed},
    // Chorus reeds.
    {"trompet", ToneFamily::ChorusReed}, {"trumpet", ToneFamily::ChorusReed},
    {"tromba", ToneFamily::ChorusReed}, {"trombon", ToneFamily::ChorusReed},
    {"posaune", ToneFamily::ChorusReed}, {"bombard", ToneFamily::ChorusReed},
    {"tuba", ToneFamily::ChorusReed}, {"clairon", ToneFamily::ChorusReed},
    {"clarion", ToneFamily::ChorusReed}, {"clarin", ToneFamily::ChorusReed},
    {"cornopean", ToneFamily::ChorusReed}, {"ophicleide", ToneFamily::ChorusReed},
    {"buzain", ToneFamily::ChorusReed}, {"bazuin", ToneFamily::ChorusReed},
    {"basuin", ToneFamily::ChorusReed}, {"fanfare", ToneFamily::ChorusReed},
    // Principals named for something else: the Spanish flautado, the
    // Italian ripieno ranks, the stringy Geigenprinzipal.
    {"flautado", ToneFamily::Principal}, {"geigenprin", ToneFamily::Principal},
    {"quintadecima", ToneFamily::Principal}, {"decimaquinta", ToneFamily::Principal},
    {"decimanona", ToneFamily::Principal}, {"vigesima", ToneFamily::Principal},
    {"trigesima", ToneFamily::Principal}, {"ottava", ToneFamily::Principal},
    // Soft flutes named dolce.
    {"flautodolce", ToneFamily::OpenFlute}, {"flutedouce", ToneFamily::OpenFlute},
    {"dolceflute", ToneFamily::OpenFlute},
    // Compound stops.
    {"cornet", ToneFamily::Cornet}, {"kornett", ToneFamily::Cornet},
    {"sesquialt", ToneFamily::Cornet}, {"sexquialt", ToneFamily::Cornet},
    {"tertian", ToneFamily::Cornet},
    {"mixt", ToneFamily::Mixture}, {"fourniture", ToneFamily::Mixture},
    {"pleinjeu", ToneFamily::Mixture}, {"plenum", ToneFamily::Mixture},
    {"cymbal", ToneFamily::Mixture}, {"cymbel", ToneFamily::Mixture},
    {"cimbel", ToneFamily::Mixture}, {"zimbel", ToneFamily::Mixture},
    {"cimbale", ToneFamily::Mixture}, {"scharf", ToneFamily::Mixture},
    {"scherp", ToneFamily::Mixture}, {"ripieno", ToneFamily::Mixture},
    {"lleno", ToneFamily::Mixture}, {"acuta", ToneFamily::Mixture},
    {"akuta", ToneFamily::Mixture}, {"furniture", ToneFamily::Mixture},
    {"sharp", ToneFamily::Mixture}, {"rauschpfeife", ToneFamily::Mixture},
    {"rauschquint", ToneFamily::Mixture}, {"hintersatz", ToneFamily::Mixture},
    // Stopped and half-stopped flutes.
    {"quintaton", ToneFamily::StoppedFlute}, {"quintadena", ToneFamily::StoppedFlute},
    {"quintade", ToneFamily::StoppedFlute}, {"bourdon", ToneFamily::StoppedFlute},
    {"bordun", ToneFamily::StoppedFlute}, {"bordone", ToneFamily::StoppedFlute},
    {"gedackt", ToneFamily::StoppedFlute}, {"gedeckt", ToneFamily::StoppedFlute},
    {"gedakt", ToneFamily::StoppedFlute}, {"gedekt", ToneFamily::StoppedFlute},
    {"gedact", ToneFamily::StoppedFlute}, {"stopped", ToneFamily::StoppedFlute},
    {"stopt", ToneFamily::StoppedFlute}, {"lieblich", ToneFamily::StoppedFlute},
    {"rohrflot", ToneFamily::StoppedFlute}, {"rohrflut", ToneFamily::StoppedFlute},
    {"roerfluit", ToneFamily::StoppedFlute}, {"rohrfluit", ToneFamily::StoppedFlute},
    {"chimneyflute", ToneFamily::StoppedFlute}, {"cheminee", ToneFamily::StoppedFlute},
    {"subbas", ToneFamily::StoppedFlute}, {"soubasse", ToneFamily::StoppedFlute},
    {"untersatz", ToneFamily::StoppedFlute}, {"copula", ToneFamily::StoppedFlute},
    {"koppelflot", ToneFamily::StoppedFlute}, {"tapad", ToneFamily::StoppedFlute},
    {"tappat", ToneFamily::StoppedFlute}, {"pommer", ToneFamily::StoppedFlute},
    {"cordenuit", ToneFamily::StoppedFlute}, {"nason", ToneFamily::StoppedFlute},
    // Mutations.
    {"nasard", ToneFamily::Mutation}, {"nazard", ToneFamily::Mutation},
    {"nasat", ToneFamily::Mutation}, {"nassat", ToneFamily::Mutation},
    {"tierce", ToneFamily::Mutation}, {"terz", ToneFamily::Mutation},
    {"larigot", ToneFamily::Mutation}, {"septim", ToneFamily::Mutation},
    {"quint", ToneFamily::Mutation}, {"twelfth", ToneFamily::Mutation},
    {"seventeenth", ToneFamily::Mutation}, {"nineteenth", ToneFamily::Mutation},
    // Strings.
    {"gamb", ToneFamily::String}, {"viol", ToneFamily::String},
    {"cello", ToneFamily::String}, {"salicion", ToneFamily::String},
    {"salicet", ToneFamily::String}, {"salizional", ToneFamily::String},
    {"aeolin", ToneFamily::String}, {"eolin", ToneFamily::String},
    {"fugara", ToneFamily::String}, {"keraulophon", ToneFamily::String},
    {"contrabas", ToneFamily::String}, {"kontrabas", ToneFamily::String},
    {"contrebasse", ToneFamily::String}, {"geigen", ToneFamily::String},
    {"string", ToneFamily::String}, {"dolce", ToneFamily::String},
    // Open flutes.
    {"flot", ToneFamily::OpenFlute}, {"flut", ToneFamily::OpenFlute},
    {"flaut", ToneFamily::OpenFlute}, {"fluit", ToneFamily::OpenFlute},
    {"fluyt", ToneFamily::OpenFlute}, {"hohl", ToneFamily::OpenFlute},
    {"spitz", ToneFamily::OpenFlute}, {"gemsh", ToneFamily::OpenFlute},
    {"waldfl", ToneFamily::OpenFlute}, {"blockfl", ToneFamily::OpenFlute},
    {"nachthorn", ToneFamily::OpenFlute}, {"sifflet", ToneFamily::OpenFlute},
    {"flageol", ToneFamily::OpenFlute}, {"piccolo", ToneFamily::OpenFlute},
    {"octavin", ToneFamily::OpenFlute}, {"travers", ToneFamily::OpenFlute},
    {"clarabel", ToneFamily::OpenFlute}, {"melodia", ToneFamily::OpenFlute},
    {"tibia", ToneFamily::OpenFlute}, {"openwood", ToneFamily::OpenFlute},
    // Principals.
    {"princip", ToneFamily::Principal}, {"prinzip", ToneFamily::Principal},
    {"prestant", ToneFamily::Principal}, {"praestant", ToneFamily::Principal},
    {"prastant", ToneFamily::Principal}, {"diapason", ToneFamily::Principal},
    {"montre", ToneFamily::Principal}, {"octav", ToneFamily::Principal},
    {"oktav", ToneFamily::Principal}, {"octaaf", ToneFamily::Principal},
    {"doublette", ToneFamily::Principal}, {"fifteenth", ToneFamily::Principal},
    {"choralbas", ToneFamily::Principal},
};

// The words of a name without its numbers: what two names are compared by.
std::vector<std::string> nameWords(const std::string& folded) {
  std::vector<std::string> out;
  for (const auto& t : tokens(folded)) {
    if (t.size() < 2 || romanValue(t) != 0) continue;
    if (std::any_of(t.begin(), t.end(), [](char c) { return (c >= '0' && c <= '9') || c == '/'; })) continue;
    out.push_back(t);
  }
  return out;
}

double jaccard(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.empty() && b.empty()) return 0.0;
  const std::set<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
  size_t both = 0;
  for (const auto& w : sa) both += sb.count(w);
  const size_t any = sa.size() + sb.size() - both;
  return any == 0 ? 0.0 : static_cast<double>(both) / static_cast<double>(any);
}

std::string joined(const std::vector<std::string>& words) {
  std::string out;
  for (const auto& w : words) out += w;
  return out;
}

}  // namespace

std::string foldName(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (size_t i = 0; i < name.size();) {
    const auto c = static_cast<unsigned char>(name[i]);
    if (c < 0x80) {
      if (c >= 'A' && c <= 'Z')
        out += static_cast<char>(c - 'A' + 'a');
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/')
        out += static_cast<char>(c);
      else
        out += ' ';
      ++i;
      continue;
    }
    uint32_t cp = 0;
    size_t len = 1;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1Fu;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0Fu;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07u;
      len = 4;
    }
    if (i + len > name.size()) break;
    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(name[i + k]) & 0x3Fu);
    out += foldCodePoint(cp);
    i += len;
  }
  return out;
}

StopTraits stopTraits(const std::string& name) {
  StopTraits t;
  auto words = tokens(foldName(name));
  // Ranks first, so "Mixtur 4 fach" does not read as a 4'.
  static const std::set<std::string> rankWords{"fach", "f", "fache", "rangs", "rang", "rgs", "rg", "rks",
                                               "ranks", "rank", "st", "sterk", "fila", "file", "ch",
                                               "chorig", "chor"};
  std::vector<std::string> rest;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& w = words[i];
    if (isDigits(w) && i + 1 < words.size() && rankWords.count(words[i + 1]) != 0 && w.size() <= 2) {
      t.ranks = std::stoi(w);
      ++i;
      continue;
    }
    // "4fach", "5rgs".
    size_t d = 0;
    while (d < w.size() && w[d] >= '0' && w[d] <= '9') ++d;
    if (d > 0 && d <= 2 && d < w.size() && rankWords.count(w.substr(d)) != 0) {
      t.ranks = std::stoi(w.substr(0, d));
      continue;
    }
    rest.push_back(w);
  }

  // The footage: the last number that is one, a fraction joined to the whole
  // number before it ("2 2/3").
  for (size_t i = 0; i < rest.size(); ++i) {
    double v = 0.0, f = 0.0;
    if (isDigits(rest[i]) && rest[i].size() <= 2) {
      v = std::stod(rest[i]);
      if (i + 1 < rest.size() && fraction(rest[i + 1], f) && f < 1.0) {
        v += f;
        ++i;
      }
    } else if (!fraction(rest[i], v)) {
      continue;
    }
    if (knownFootage(v)) t.footage = v;
  }

  const std::string all = compact(foldName(name));
  for (const auto& fw : kFamilyWords)
    if (all.find(fw.word) != std::string::npos) {
      t.family = fw.family;
      break;
    }
  if (t.family == ToneFamily::Unknown && t.footage > 0.0 && !unisonFootage(t.footage))
    t.family = ToneFamily::Mutation;
  // A mixture's ranks in roman numerals: "Mixtur IV", "Cornet V".
  if (t.ranks == 0 && (t.family == ToneFamily::Mixture || t.family == ToneFamily::Cornet))
    for (const auto& w : rest)
      if (const int r = romanValue(w); r >= 2) {
        t.ranks = r;
        break;
      }
  if (t.family == ToneFamily::Unknown && t.ranks >= 2) t.family = ToneFamily::Mixture;
  return t;
}

double familyCloseness(ToneFamily a, ToneFamily b) {
  if (a == b) return 1.0;
  if (a == ToneFamily::Unknown || b == ToneFamily::Unknown) return 0.35;
  if (b < a) std::swap(a, b);
  using F = ToneFamily;
  static const std::map<std::pair<F, F>, double> near{
      {{F::Principal, F::OpenFlute}, 0.45},  {{F::Principal, F::StoppedFlute}, 0.25},
      {{F::Principal, F::String}, 0.35},     {{F::Principal, F::Celeste}, 0.3},
      {{F::Principal, F::Mutation}, 0.25},   {{F::OpenFlute, F::StoppedFlute}, 0.6},
      {{F::OpenFlute, F::String}, 0.25},     {{F::OpenFlute, F::Celeste}, 0.3},
      {{F::OpenFlute, F::Mutation}, 0.3},    {{F::StoppedFlute, F::Mutation}, 0.3},
      {{F::String, F::Celeste}, 0.6},        {{F::ChorusReed, F::SoloReed}, 0.5},
      {{F::ChorusReed, F::Regal}, 0.4},      {{F::SoloReed, F::Regal}, 0.6},
      {{F::Mixture, F::Mutation}, 0.35},     {{F::Mixture, F::Cornet}, 0.45},
      {{F::Mutation, F::Cornet}, 0.6},
      // A cornet is built of flute ranks; an unda maris is often a flute
      // celeste.
      {{F::OpenFlute, F::Cornet}, 0.3},      {{F::StoppedFlute, F::Cornet}, 0.3},
      {{F::StoppedFlute, F::Celeste}, 0.25},
  };
  const auto it = near.find({a, b});
  return it == near.end() ? 0.1 : it->second;
}

const char* toneFamilyName(ToneFamily f) {
  switch (f) {
    case ToneFamily::Principal: return "principal";
    case ToneFamily::OpenFlute: return "open flute";
    case ToneFamily::StoppedFlute: return "stopped flute";
    case ToneFamily::String: return "string";
    case ToneFamily::Celeste: return "celeste";
    case ToneFamily::ChorusReed: return "chorus reed";
    case ToneFamily::SoloReed: return "solo reed";
    case ToneFamily::Regal: return "regal";
    case ToneFamily::Mixture: return "mixture";
    case ToneFamily::Mutation: return "mutation";
    case ToneFamily::Cornet: return "cornet";
    case ToneFamily::Percussion: return "percussion";
    case ToneFamily::Unknown: break;
  }
  return "unknown";
}

const char* tabKindName(ConsoleTabKind k) {
  switch (k) {
    case ConsoleTabKind::Coupler: return "coupler";
    case ConsoleTabKind::Tremulant: return "tremulant";
    case ConsoleTabKind::Piston: return "piston";
    case ConsoleTabKind::Stop: break;
  }
  return "stop";
}

bool tabKindFromName(const std::string& s, ConsoleTabKind& out) {
  for (auto k : {ConsoleTabKind::Stop, ConsoleTabKind::Coupler, ConsoleTabKind::Tremulant, ConsoleTabKind::Piston})
    if (s == tabKindName(k)) {
      out = k;
      return true;
    }
  return false;
}

// ------------------------------------------------------------ matching

namespace {

// Both known: the same or not at all. One unknown: a doubt.
double sameOrNot(int a, int b, double unknown) {
  if (a < 0 || b < 0) return unknown;
  return a == b ? 1.0 : 0.0;
}

}  // namespace

double matchScore(const ConsoleTab& tab, const ConsoleTab& control) {
  if (tab.kind != control.kind) return 0.0;
  const std::string fa = foldName(tab.label), fc = foldName(control.label);
  const auto wa = nameWords(fa), wc = nameWords(fc);
  const double alike = jaccard(wa, wc);
  const bool sameName = !wa.empty() && joined(wa) == joined(wc);
  switch (tab.kind) {
    case ConsoleTabKind::Stop: {
      const double keyboard = sameOrNot(tab.keyboard, control.keyboard, 0.7);
      if (keyboard == 0.0) return 0.0;
      const StopTraits ta = stopTraits(tab.label), tc = stopTraits(control.label);
      // Both stated: the same or never. Neither: nothing against it, as with
      // two mixtures named by their ranks. One: a doubt.
      double footage = 1.0;
      if (ta.footage > 0.0 && tc.footage > 0.0) {
        if (std::abs(ta.footage - tc.footage) > 0.02) return 0.0;
      } else if (ta.footage > 0.0 || tc.footage > 0.0) {
        footage = 0.6;
      }
      const double family = sameName ? 1.0 : familyCloseness(ta.family, tc.family);
      const double ranks = ta.ranks > 0 && tc.ranks > 0 && ta.ranks != tc.ranks ? 0.85 : 1.0;
      return keyboard * footage * family * ranks * (0.85 + 0.15 * (sameName ? 1.0 : alike));
    }
    case ConsoleTabKind::Coupler: {
      const double onto = sameOrNot(tab.keyboard, control.keyboard, 0.6);
      const double from = sameOrNot(tab.fromKeyboard, control.fromKeyboard, 0.6);
      if (onto == 0.0 || from == 0.0 || tab.shift != control.shift) return 0.0;
      const double score = onto * from * (0.85 + 0.15 * alike);
      return sameName ? std::max(score, 0.9) : score;
    }
    case ConsoleTabKind::Tremulant: {
      const double keyboard = sameOrNot(tab.keyboard, control.keyboard, 0.5);
      if (keyboard == 0.0) return 0.0;
      const double score = keyboard * (0.85 + 0.15 * alike);
      return sameName ? std::max(score, 0.9) : score;
    }
    case ConsoleTabKind::Piston: {
      if (compact(fa) == compact(fc) && !compact(fa).empty()) return 1.0;
      std::string da, dc;
      for (char ch : fa)
        if (ch >= '0' && ch <= '9') da += ch;
      for (char ch : fc)
        if (ch >= '0' && ch <= '9') dc += ch;
      if (da.empty() || da != dc) return 0.0;
      const double keyboard = sameOrNot(tab.keyboard, control.keyboard, 0.7);
      return keyboard * (0.6 + 0.15 * alike);
    }
  }
  return 0.0;
}

std::vector<ProfileMatch> matchProfile(const std::vector<ConsoleTab>& tabs,
                                       const std::vector<ConsoleTab>& controls) {
  std::vector<ProfileMatch> pairs;
  for (size_t t = 0; t < tabs.size(); ++t)
    for (size_t c = 0; c < controls.size(); ++c) {
      const double s = matchScore(tabs[t], controls[c]);
      if (s >= kWeakestMatch) pairs.push_back({t, c, s, s >= kStrongMatch});
    }
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const ProfileMatch& a, const ProfileMatch& b) { return a.score > b.score; });
  std::vector<bool> tabUsed(tabs.size(), false), controlUsed(controls.size(), false);
  std::vector<ProfileMatch> out;
  for (const auto& p : pairs) {
    if (tabUsed[p.tab] || controlUsed[p.control]) continue;
    tabUsed[p.tab] = controlUsed[p.control] = true;
    out.push_back(p);
  }
  // The stops left over, by sound: a Diapason with no principal left on its
  // keyboard is offered the Gedackt at its pitch. Keyboard and footage still
  // rule (matchScore is 0 across them); the family only has to be near.
  std::vector<ProfileMatch> near;
  for (size_t t = 0; t < tabs.size(); ++t) {
    if (tabUsed[t] || tabs[t].kind != ConsoleTabKind::Stop) continue;
    const ToneFamily ft = stopTraits(tabs[t].label).family;
    for (size_t c = 0; c < controls.size(); ++c) {
      if (controlUsed[c] || controls[c].kind != ConsoleTabKind::Stop) continue;
      const double s = matchScore(tabs[t], controls[c]);
      if (s <= 0.0 || familyCloseness(ft, stopTraits(controls[c].label).family) < kRelatedSound) continue;
      near.push_back({t, c, s, false, true});
    }
  }
  std::stable_sort(near.begin(), near.end(),
                   [](const ProfileMatch& a, const ProfileMatch& b) { return a.score > b.score; });
  for (const auto& p : near) {
    if (tabUsed[p.tab] || controlUsed[p.control]) continue;
    tabUsed[p.tab] = controlUsed[p.control] = true;
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const ProfileMatch& a, const ProfileMatch& b) { return a.tab < b.tab; });
  return out;
}

// ------------------------------------------------------------ the organ

namespace {

// The division a keyboard plays: its hint, the division listing it, or the
// division its keys reach unconditionally.
Id keyboardDivisionOf(const OrganModel& m, Id keyboard) {
  const auto kb = m.keyboards.find(keyboard);
  if (kb != m.keyboards.end() && kb->second.primaryDivisionHint != 0) return kb->second.primaryDivisionHint;
  for (const auto& [id, d] : m.divisions)
    if (std::find(d.keyboardIds.begin(), d.keyboardIds.end(), keyboard) != d.keyboardIds.end()) return id;
  for (const auto& a : m.keyActions)
    if (a.conditionSwitchId == 0 && static_cast<Id>(a.sourceKeyboard) == keyboard && !a.destIsKeyboard &&
        a.destDivision != 0)
      return static_cast<Id>(a.destDivision);
  return 0;
}

bool pedalWord(const std::string& t) {
  return t == "p" || t == "ped" || t == "pd" || t.rfind("pedal", 0) == 0 || t.rfind("pedaal", 0) == 0 ||
         t == "pedale" || t == "pedalier" || t == "pedalwerk";
}

// Roman numerals I to VI, or "manual 2".
int manualWord(const std::vector<std::string>& words, size_t i) {
  const int r = romanValue(words[i]);
  if (r >= 1 && r <= 6) return r;
  if ((words[i] == "manual" || words[i] == "man") && i + 1 < words.size() && isDigits(words[i + 1]) &&
      words[i + 1].size() == 1)
    return std::stoi(words[i + 1]);
  return -1;
}

// Short forms of division names, as they are printed on couplers.
const std::map<std::string, std::vector<std::string>>& abbreviations() {
  static const std::map<std::string, std::vector<std::string>> a{
      {"gt", {"great"}},           {"sw", {"swell", "schwellwerk"}},
      {"ch", {"choir"}},           {"pos", {"positi"}},
      {"so", {"solo"}},            {"hw", {"hauptwerk", "hoofdwerk"}},
      {"ow", {"oberwerk"}},        {"rp", {"ruckpositiv", "rueckpositiv", "rugwerk"}},
      {"bw", {"brustwerk", "borstwerk"}}, {"go", {"grandorgue", "grandorgano", "grandeorgue"}},
      {"rec", {"recit"}},          {"sch", {"schwellwerk"}},
      {"bomb", {"bombarde"}},      {"ech", {"echo"}},
  };
  return a;
}

// Each keyboard a name refers to, in the order it names them.
std::vector<int> keyboardRefs(const std::vector<std::string>& words,
                              const std::vector<std::pair<std::string, int>>& divisions) {
  std::vector<std::pair<std::string, int>> divs;
  for (const auto& [n, kb] : divisions)
    if (kb >= 0) divs.emplace_back(compact(foldName(n)), kb);
  std::vector<int> refs;
  for (size_t i = 0; i < words.size(); ++i) {
    const auto& w = words[i];
    int kb = -1;
    if (pedalWord(w)) {
      kb = 0;
    } else if (const int man = manualWord(words, i); man > 0) {
      kb = man;
      if (words[i] == "manual" || words[i] == "man") ++i;
    } else if (w.size() >= 2 && !isDigits(w)) {
      std::vector<std::string> names{w};
      if (const auto it = abbreviations().find(w); it != abbreviations().end())
        names.insert(names.end(), it->second.begin(), it->second.end());
      for (const auto& candidate : names) {
        for (const auto& [div, dkb] : divs)
          if (!div.empty() && (div == candidate || (candidate.size() >= 3 && div.rfind(candidate, 0) == 0))) {
            kb = dkb;
            break;
          }
        if (kb >= 0) break;
      }
    }
    if (kb >= 0) refs.push_back(kb);
  }
  return refs;
}

}  // namespace

int divisionKeyboard(const OrganModel& m, Id divisionId) {
  if (divisionId == 0) return -1;
  // The organ's channel convention: 1 the pedal, 2 the first manual.
  int best = 0;
  for (const auto& [id, kb] : m.keyboards)
    if (kb.assignmentCode > 0 && keyboardDivisionOf(m, id) == divisionId && (best == 0 || kb.assignmentCode < best))
      best = kb.assignmentCode;
  if (best > 0) return best - 1;
  const auto d = m.divisions.find(divisionId);
  if (d == m.divisions.end()) return -1;
  // A CODM organ numbers its manuals.
  const bool numbered = std::any_of(m.divisions.begin(), m.divisions.end(),
                                    [](const auto& kv) { return kv.second.manualNumber > 0; });
  if (numbered) return d->second.manualNumber;
  const auto words = tokens(foldName(d->second.name));
  for (size_t i = 0; i < words.size(); ++i) {
    if (pedalWord(words[i])) return 0;
    if (const int man = manualWord(words, i); man > 0) return man;
  }
  return -1;
}

int keyboardNumber(const OrganModel& m, Id keyboardId) {
  const auto kb = m.keyboards.find(keyboardId);
  if (kb != m.keyboards.end() && kb->second.assignmentCode > 0) return kb->second.assignmentCode - 1;
  return divisionKeyboard(m, keyboardDivisionOf(m, keyboardId));
}

bool couplerKeyboards(const OrganModel& m, Id switchId, int& onto, int& from, int& shift) {
  std::vector<Id> frontier{switchId};
  std::set<Id> seen{switchId};
  for (int hop = 0; hop < 4 && !frontier.empty(); ++hop) {
    for (Id sw : frontier)
      for (const auto& a : m.keyActions) {
        // A unison off is a key action live while its switch is off: not a
        // coupler.
        if (a.conditionSwitchId != sw || !a.conditionWhenEngaged) continue;
        const int o = keyboardNumber(m, static_cast<Id>(a.sourceKeyboard));
        const int f = a.destIsKeyboard ? keyboardNumber(m, static_cast<Id>(a.destKeyboard))
                                       : divisionKeyboard(m, static_cast<Id>(a.destDivision));
        if (o < 0 && f < 0) continue;
        onto = o;
        from = f;
        shift = a.midiIncrement;
        return true;
      }
    std::vector<Id> next;
    for (Id sw : frontier)
      for (const auto& l : m.switchLinkages)
        if (l.sourceSwitchId == sw && seen.insert(l.destSwitchId).second) next.push_back(l.destSwitchId);
    frontier.swap(next);
  }
  return false;
}

namespace {
// A name's words with "II/I" taken apart.
std::vector<std::string> refWords(const std::string& name) {
  std::string f = foldName(name);
  std::replace(f.begin(), f.end(), '/', ' ');
  return tokens(f);
}
}  // namespace

int couplerShiftFromName(const std::string& name) {
  int s = 0;
  for (const auto& w : refWords(name)) {
    if (w == "16" || w.rfind("sub", 0) == 0 || w.rfind("unteroktav", 0) == 0 || w == "grave") s = -12;
    if (w == "4" || w.rfind("super", 0) == 0 || w.rfind("octav", 0) == 0 || w.rfind("oktav", 0) == 0 ||
        w == "octaaf" || w == "ottava" || w == "aigu")
      s = 12;
  }
  return s;
}

bool couplerKeyboardsFromName(const std::string& name,
                              const std::vector<std::pair<std::string, int>>& divisions, int& onto,
                              int& from, int& shift) {
  const auto words = refWords(name);
  const int s = couplerShiftFromName(name);
  const auto refs = keyboardRefs(words, divisions);
  if (refs.size() >= 2) {
    from = refs[0];
    onto = refs[1];
    shift = s;
    return true;
  }
  // "Swell Octave": the keyboard onto itself, an octave up.
  if (refs.size() == 1 && s != 0) {
    from = onto = refs[0];
    shift = s;
    return true;
  }
  return false;
}

int keyboardFromName(const std::string& name, const std::vector<std::pair<std::string, int>>& divisions) {
  const auto refs = keyboardRefs(refWords(name), divisions);
  return refs.empty() ? -1 : refs.front();
}

// ------------------------------------------------------------ the profile

std::vector<MidiBinding> ConsoleProfile::bindingsOf(size_t tab) const {
  std::vector<MidiBinding> out;
  for (const auto& b : messages.bindings())
    if (b.targetKind == MidiTargetKind::Switch && b.targetId == targetOf(tab)) out.push_back(b);
  return out;
}

void ConsoleProfile::setBindingsOf(size_t tab, const std::vector<MidiBinding>& bindings) {
  messages.setBindingsFor(MidiTargetKind::Switch, targetOf(tab), bindings);
}

void ConsoleProfile::removeTab(size_t tab) {
  if (tab >= tabs.size()) return;
  std::vector<std::vector<MidiBinding>> all;
  for (size_t i = 0; i < tabs.size(); ++i) all.push_back(bindingsOf(i));
  messages.removeBindings([](const MidiBinding& b) { return b.targetKind == MidiTargetKind::Switch; });
  tabs.erase(tabs.begin() + static_cast<std::ptrdiff_t>(tab));
  all.erase(all.begin() + static_cast<std::ptrdiff_t>(tab));
  for (size_t i = 0; i < all.size(); ++i) setBindingsOf(i, all[i]);
}

std::string ConsoleProfile::toText() const {
  std::ostringstream out;
  out << "# Masterpiece console profile\n";
  out << "name " << name << "\n";
  // The label last: it can hold spaces.
  for (const auto& t : tabs)
    out << "tab " << tabKindName(t.kind) << " " << t.keyboard << " " << t.fromKeyboard << " " << t.shift << " "
        << t.label << "\n";
  out << "map\n" << messages.toText();
  return out.str();
}

bool ConsoleProfile::fromText(const std::string& text) {
  name.clear();
  tabs.clear();
  messages = MidiMap();
  std::istringstream in(text);
  std::string line;
  bool any = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "map") {
      std::ostringstream rest;
      rest << in.rdbuf();
      return messages.fromText(rest.str()) || any;
    }
    if (line.rfind("name ", 0) == 0) {
      name = line.substr(5);
      any = true;
    } else if (line.rfind("tab ", 0) == 0) {
      std::istringstream f(line.substr(4));
      std::string kind;
      ConsoleTab t;
      if (!(f >> kind >> t.keyboard >> t.fromKeyboard >> t.shift) || !tabKindFromName(kind, t.kind)) continue;
      std::getline(f, t.label);
      const auto first = t.label.find_first_not_of(' ');
      t.label = first == std::string::npos ? std::string() : t.label.substr(first);
      tabs.push_back(std::move(t));
      any = true;
    }
  }
  return any;
}

std::string ConsoleProfile::stamp() const {
  // FNV-1a over the saved form.
  uint64_t h = 1469598103934665603ull;
  for (const char c : toText()) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

}  // namespace mp
