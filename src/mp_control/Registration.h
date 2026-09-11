// Choosing stops on an organ you have never seen.
//
// A demonstration that draws every stop plays every piece as full organ, which
// is right for a toccata and wrong for everything else. A chorale prelude
// wants one flute and a quiet pedal; a trio wants two contrasting manuals and
// a light bass. Drawing "the first four stops" is no better: stop lists are
// ordered by division, so on most organs that is four pedal stops and silence
// from the manuals.
//
// So stops are chosen by what they ARE, which means reading their names. Organ
// builders name stops in their own language and have done for four centuries,
// but the vocabulary is small and stable: Prestant, Principal, Montre and
// Diapason are the same rank in four countries. That is what this classifies.
//
// Deliberately JUCE-free and header-only: it is string matching over the
// model, decided once before a note sounds, and the fast loop should be able
// to check it without audio.
#pragma once
#include "../mp_core/OrganModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

enum class StopFamily {
  Unknown,
  Principal,  // the organ's backbone: Prestant, Montre, Octaaf, Diapason
  Flute,      // Holpijp, Gedackt, Bourdon, Rohrfloete, Nachthorn
  String,     // Gamba, Salicional, Viola, Celeste - narrow and quiet
  Reed,       // Trompet, Fagot, Kromhoorn, Bombarde, Hautbois
  Mixture,    // Mixtuur, Scherp, Cymbel, Plein Jeu, Cornet, Sesquialter
  Effect,     // Tremulant, Zimbelstern, bells: never part of a registration
};

// Case- and accent-insensitive enough for stop names. Not a general Unicode
// fold: it exists so that "Rohrfloete" and "Rohrflote" match the same word.
inline std::string normaliseStopName(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(std::tolower(c)));
      continue;
    }
    // UTF-8 two-byte Latin-1 supplement: fold the vowels builders actually
    // use, so Rohrfloete and the accented spelling match the same word.
    if (c == 0xC3 && i + 1 < in.size()) {
      const unsigned char d = static_cast<unsigned char>(in[++i]);
      if (d >= 0xA0 && d <= 0xA5) out.push_back('a');
      else if (d >= 0xA8 && d <= 0xAB) out.push_back('e');
      else if (d >= 0xAC && d <= 0xAF) out.push_back('i');
      else if (d >= 0xB2 && d <= 0xB6) out.push_back('o');
      else if (d >= 0xB9 && d <= 0xBC) out.push_back('u');
      else out.push_back(' ');
      continue;
    }
    // Anything else is a separator as far as word matching is concerned. The
    // Polish letters land here and stay unfolded, which costs nothing: the
    // words below are matched on their stems, and Pryncypal is only accented
    // on its last letter.
    out.push_back(' ');
  }
  return out;
}

inline StopFamily classifyStop(const std::string& rawName) {
  const std::string n = normaliseStopName(rawName);
  auto has = [&n](const char* w) { return n.find(w) != std::string::npos; };

  // Order matters. A Cornet is a mixture even though it sounds reedy, and a
  // Quintaton is a flute even though it is named after a quint.
  if (has("tremul") || has("tremol") || has("zimbelst") || has("cymbelst") ||
      has("stern") || has("glocken") || has("bell") || has("nachtigal"))
    return StopFamily::Effect;

  if (has("mixtu") || has("scherp") || has("scharf") || has("cymbel") ||
      has("zimbel") || has("plein jeu") || has("fourniture") || has("cornet") ||
      has("sesquialt") || has("rauschpfeife") || has("acuta") ||
      has("ripieno") || has("mikstu") || has("kornet"))
    return StopFamily::Mixture;

  if (has("trompet") || has("trumpet") || has("fagot") || has("bombarde") ||
      has("bazuin") || has("posaune") || has("dulcian") || has("kromhoorn") ||
      has("krummhorn") || has("cromorne") || has("clarin") || has("clairon") ||
      has("hautbo") || has("hobo") || has("oboe") || has("schalmei") ||
      has("vox humana") || has("voix humaine") || has("voce umana") ||
      has("regal") || has("krumhorn") || has("krumm") || has("kromm") ||
      has("tromba") || has("puzon") || has("szalmaja"))
    return StopFamily::Reed;

  if (has("gamba") || has("salicional") || has("viola") || has("viole") ||
      has("violon") || has("violoncell") || has("celeste") ||
      has("unda maris") || has("aeoline") || has("dolce") || has("dulciana"))
    return StopFamily::String;

  if (has("prestant") || has("praestant") || has("principa") ||
      has("prinzipal") || has("montre") || has("octaaf") || has("octave") ||
      has("oktave") || has("oktav") || has("oktawa") || has("oktawbas") ||
      has("pryncypa") || has("ottava") || has("doublette") ||
      has("diapason") || has("choralbas"))
    return StopFamily::Principal;

  if (has("fluit") || has("flute") || has("flote") || has("floete") ||
      has("holpijp") || has("gedekt") || has("gedackt") || has("gedact") ||
      has("bourdon") || has("subbas") || has("soubasse") || has("quintat") ||
      has("quintad") || has("rohr") || has("roer") || has("nachthorn") ||
      has("nasard") || has("nazard") || has("quinte") || has("sifflo") ||
      has("waldflo") || has("woudfluit") || has("spitzflo") || has("blockflo") ||
      has("cor de nuit") || has("terz") || has("tierce") || has("tercja") ||
      has("larigot") || has("flet") || has("flaut") || has("gemshorn") ||
      has("kwint") || has("amabile") || has("dolcan"))
    return StopFamily::Flute;

  return StopFamily::Unknown;
}

// Pitch in feet, from the name: "Prestant 8", "Nasard 2 2/3", "Quinte 1 1/3".
// Returns 0 when the name states none, which is how mixtures usually read.
inline double stopFootage(const std::string& rawName) {
  const std::string n = normaliseStopName(rawName);

  // Compound pitches first: the leading digit of "2 2/3" must not be read as a
  // plain 2, which would make a quint look like a fifteenth.
  static const struct { const char* text; double feet; } compound[] = {
      {"10 2/3", 10.667}, {"5 1/3", 5.333}, {"2 2/3", 2.667},
      {"1 3/5", 1.6}, {"1 1/3", 1.333},
  };
  for (const auto& c : compound)
    if (n.find(c.text) != std::string::npos) return c.feet;

  static const struct { const char* text; double feet; } plain[] = {
      {"32", 32.0}, {"16", 16.0}, {"8", 8.0}, {"4", 4.0}, {"2", 2.0}, {"1", 1.0},
  };
  for (const auto& t : plain) {
    const std::string s = t.text;
    const size_t p = n.find(s);
    if (p == std::string::npos) continue;
    // The digits must stand alone. "II st." is not two feet, and a roman
    // numeral or a rank count must not be mistaken for a pitch.
    const bool leftOk =
        p == 0 || !std::isdigit(static_cast<unsigned char>(n[p - 1]));
    const size_t after = p + s.size();
    const bool rightOk =
        after >= n.size() || !std::isdigit(static_cast<unsigned char>(n[after]));
    if (leftOk && rightOk) return t.feet;
  }
  return 0.0;
}

// A mutation sounds a pitch that is not the note played: a third or a fifth
// above it. Two of them and a unison make a cornet; one on its own makes a
// sour note, which is why every recipe here keeps them out unless asked.
inline bool isMutation(double feet) {
  for (double f : {32.0, 16.0, 8.0, 4.0, 2.0, 1.0})
    if (std::abs(feet - f) < 0.01) return false;
  return feet > 0.0;
}

enum class Registration {
  Tutti,    // everything: a toccata, a final flourish
  Plenum,   // principal chorus + mixture: preludes and fugues
  Chorale,  // one flute, quiet pedal: an Orgelbuechlein prelude
  Trio,     // two contrasting manuals and a light bass
  Solo,     // a reed or mixture against a soft accompaniment
};

struct StopChoice {
  Id stopId = 0;
  Id divisionId = 0;
  std::string name;
  StopFamily family = StopFamily::Unknown;
  double feet = 0.0;
};

// Every stop the organ declares, classified, in a stable order so the same
// organ registers the same way twice.
inline std::vector<StopChoice> classifyStops(const OrganModel& model) {
  std::vector<StopChoice> out;
  out.reserve(model.stops.size());
  for (const auto& [id, stop] : model.stops) {
    StopChoice c;
    c.stopId = id;
    c.divisionId = stop.divisionId;
    c.name = stop.name;
    c.family = classifyStop(stop.name);
    c.feet = stopFootage(stop.name);
    out.push_back(std::move(c));
  }
  std::sort(out.begin(), out.end(), [](const StopChoice& a, const StopChoice& b) {
    if (a.divisionId != b.divisionId) return a.divisionId < b.divisionId;
    return a.stopId < b.stopId;
  });
  return out;
}

// Which division is the pedal? The one whose stops sit lowest: a pedal is
// built on 16 and 8 foot where a manual is built on 8 and 4. Guessing from the
// NAME fails across languages and on organs that number their divisions.
inline Id guessPedalDivision(const std::vector<StopChoice>& stops) {
  std::unordered_map<Id, double> sum, count;
  for (const auto& s : stops) {
    if (s.feet <= 0.0 || s.family == StopFamily::Effect) continue;
    sum[s.divisionId] += s.feet;
    count[s.divisionId] += 1.0;
  }
  if (sum.size() < 2) return 0;  // nothing to be lower THAN

  Id best = 0;
  double bestAvg = 0.0;
  double total = 0.0;
  for (const auto& entry : sum) {
    const double avg = entry.second / count[entry.first];
    total += avg;
    if (avg > bestAvg) { bestAvg = avg; best = entry.first; }
  }

  // A pedal is not merely the lowest division, it is decisively lower than the
  // manuals: built on 16 and 8 where they are built on 8 and 4. Requiring only
  // "lowest" would nominate a division on any organ, including a single-manual
  // one with no pedal stops at all, and the recipes would then spend their
  // bass stop on a manual.
  const double others = (total - bestAvg) / static_cast<double>(sum.size() - 1);
  return (bestAvg >= 8.0 && bestAvg > others) ? best : 0;
}

// Pick the stops for a registration, as stop ids to engage.
//
// Every recipe is written the way a player would describe it, and degrades
// gracefully: an organ with no reed still gets a solo line, an organ with one
// manual still gets a trio that sounds like something.
inline std::vector<Id> chooseRegistration(const OrganModel& model,
                                          Registration style) {
  const auto stops = classifyStops(model);
  const Id pedal = guessPedalDivision(stops);

  std::vector<Id> chosen;
  auto take = [&chosen](const StopChoice* s) {
    if (s != nullptr) chosen.push_back(s->stopId);
  };

  auto free = [&chosen](const StopChoice& s) {
    return std::find(chosen.begin(), chosen.end(), s.stopId) == chosen.end();
  };

  // A stop at a given pitch, from the first family in the preference list that
  // has one.
  //
  // Pitch is the requirement and family is only the preference, which is the
  // opposite of how it reads. Try it the other way round and an organ whose
  // main manual happens to have no 8 foot flute registers a chorale prelude on
  // a 4 foot one and plays the whole thing an octave high -- it would much
  // rather have the 8 foot principal that was sitting right there.
  //
  // Mutations are excluded unless a mutation is what was asked for. A Nasard
  // is a flute at 2 2/3 foot, so by pitch-nearest alone it is a perfectly good
  // answer to "give me a flute"; drawn on its own it sounds like a mistake,
  // because a mutation is a colour laid over a unison and never a voice.
  auto pickAt = [&stops, &free](Id div, double wantFeet,
                                std::initializer_list<StopFamily> prefer)
      -> const StopChoice* {
    for (StopFamily fam : prefer)
      for (const auto& s : stops)
        if (s.divisionId == div && s.family == fam && free(s) &&
            std::abs(s.feet - wantFeet) < 0.01)
          return &s;

    // Nothing at that pitch anywhere in the preferred families: take the
    // nearest unison-or-octave pitch instead, still in preference order.
    const StopChoice* best = nullptr;
    double bestErr = 1e9;
    for (StopFamily fam : prefer) {
      for (const auto& s : stops) {
        if (s.divisionId != div || s.family != fam || !free(s)) continue;
        if (s.feet <= 0.0 || isMutation(s.feet)) continue;
        const double err = std::abs(s.feet - wantFeet);
        if (err < bestErr) { bestErr = err; best = &s; }
      }
      if (best != nullptr) return best;
    }
    return nullptr;
  };

  // A mixture states no pitch, so it is chosen by family alone.
  auto pickMixture = [&stops, &free](Id div) -> const StopChoice* {
    for (const auto& s : stops)
      if (s.divisionId == div && s.family == StopFamily::Mixture && free(s))
        return &s;
    return nullptr;
  };

  // Manual divisions, biggest first. The main manual is the one carrying the
  // most stops, which is true of every organ that has a main manual.
  std::unordered_map<Id, int> perDiv;
  for (const auto& s : stops)
    if (s.divisionId != pedal && s.family != StopFamily::Effect)
      ++perDiv[s.divisionId];
  std::vector<std::pair<Id, int>> manuals(perDiv.begin(), perDiv.end());
  std::sort(manuals.begin(), manuals.end(), [](const std::pair<Id, int>& a,
                                               const std::pair<Id, int>& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });

  const Id main = manuals.empty() ? 0 : manuals[0].first;
  const Id second = manuals.size() > 1 ? manuals[1].first : main;

  switch (style) {
    case Registration::Tutti:
      for (const auto& s : stops)
        if (s.family != StopFamily::Effect) chosen.push_back(s.stopId);
      break;

    case Registration::Plenum:
      // The principal chorus: 8, 4, 2 and a mixture crowning it. This is the
      // sound a prelude and fugue is written for. Flutes stand in where the
      // chorus is incomplete, which on a small organ it usually is.
      take(pickAt(main, 8.0, {StopFamily::Principal, StopFamily::Flute}));
      take(pickAt(main, 4.0, {StopFamily::Principal, StopFamily::Flute}));
      take(pickAt(main, 2.0, {StopFamily::Principal, StopFamily::Flute}));
      take(pickMixture(main));
      if (pedal != 0) {
        take(pickAt(pedal, 16.0, {StopFamily::Flute, StopFamily::Principal}));
        take(pickAt(pedal, 8.0, {StopFamily::Principal, StopFamily::Flute}));
      }
      break;

    case Registration::Chorale:
      // One 8 foot flute, and a pedal that supports without competing. The
      // Orgelbuechlein is written for exactly this and sounds wrong on more.
      take(pickAt(main, 8.0,
                  {StopFamily::Flute, StopFamily::Principal, StopFamily::String}));
      if (pedal != 0)
        take(pickAt(pedal, 16.0, {StopFamily::Flute, StopFamily::Principal}));
      break;

    case Registration::Trio:
      // Two manuals that can be told apart, and a bass that stays out of the
      // way. Contrast is the point: principal against flute.
      take(pickAt(main, 8.0, {StopFamily::Principal, StopFamily::Flute}));
      take(pickAt(second, 8.0, {StopFamily::Flute, StopFamily::String,
                                StopFamily::Principal}));
      if (pedal != 0)
        take(pickAt(pedal, 16.0, {StopFamily::Flute, StopFamily::Principal}));
      break;

    case Registration::Solo: {
      // A reed singing over a quiet accompaniment; a mixture if the organ has
      // no reed to sing with.
      const StopChoice* voice = pickAt(main, 8.0, {StopFamily::Reed});
      if (voice != nullptr && voice->family != StopFamily::Reed) voice = nullptr;
      take(voice != nullptr ? voice : pickMixture(main));
      take(pickAt(second, 8.0, {StopFamily::Flute, StopFamily::String,
                                StopFamily::Principal}));
      if (pedal != 0)
        take(pickAt(pedal, 16.0, {StopFamily::Flute, StopFamily::Principal}));
      break;
    }
  }

  // A registration that chose nothing is a silent organ, which looks exactly
  // like a broken one. Fall back to any single real stop.
  if (chosen.empty()) {
    for (const auto& s : stops)
      if (s.family != StopFamily::Effect) { chosen.push_back(s.stopId); break; }
  }
  return chosen;
}

inline const char* registrationName(Registration r) {
  switch (r) {
    case Registration::Tutti: return "tutti";
    case Registration::Plenum: return "plenum";
    case Registration::Chorale: return "chorale";
    case Registration::Trio: return "trio";
    case Registration::Solo: return "solo";
  }
  return "?";
}

inline const char* familyName(StopFamily f) {
  switch (f) {
    case StopFamily::Principal: return "principal";
    case StopFamily::Flute: return "flute";
    case StopFamily::String: return "string";
    case StopFamily::Reed: return "reed";
    case StopFamily::Mixture: return "mixture";
    case StopFamily::Effect: return "effect";
    case StopFamily::Unknown: break;
  }
  return "unknown";
}

// Unknown text falls back to plenum: a wrong-but-musical registration beats
// refusing to play.
inline Registration registrationFromName(const std::string& s) {
  if (s == "tutti" || s == "all") return Registration::Tutti;
  if (s == "chorale") return Registration::Chorale;
  if (s == "trio") return Registration::Trio;
  if (s == "solo") return Registration::Solo;
  return Registration::Plenum;
}

}  // namespace mp
