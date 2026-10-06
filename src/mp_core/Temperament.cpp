#include "Temperament.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace mp {

// Historical temperaments, as cents of deviation from 12-TET, index 0 = C.
// Each table is DERIVED from the temperament's fifth-chain definition (how
// much each fifth along Eb-Bb-F-C-G-D-A-E-B-F#-C#-G# is narrowed, in
// fractions of the Pythagorean or syntonic comma) rather than transcribed
// from a printed table, so the numbers follow from the tuning rule itself.
// Regenerate with build-scripts/gen-temperaments.py.
static const std::vector<Temperament> kTemperamentLib{
    // The 12-TET reference: every offset is zero by definition.
    {"Equal",
     {0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000,
      0.000, 0.000, 0.000}},

    // Werckmeister 1691 "correct temperament no. 1": the fifths C-G, G-D, D-A
    // and B-F# each narrowed by a quarter Pythagorean comma.
    {"Werckmeister III",
     {0.000, -9.775, -7.820, -5.865, -9.775, -1.955, -11.730, -3.910,
      -7.820, -11.730, -3.910, -7.820}},

    // Kirnberger 1779: C-G, G-D, D-A and A-E each narrowed by a quarter syntonic
    // comma, leaving the schisma on F#-C#.
    {"Kirnberger III",
     {0.000, -9.775, -6.843, -5.865, -13.686, -1.955, -9.776, -3.422,
      -7.820, -10.265, -3.910, -11.731}},

    // Vallotti 1754: the six natural fifths F-C-G-D-A-E-B each narrowed by a
    // sixth of a Pythagorean comma; the remaining six stay pure.
    {"Vallotti",
     {0.000, -5.865, -3.910, -1.955, -7.820, 1.955, -7.820, -1.955,
      -3.910, -5.865, 0.000, -9.775}},

    // Young 1799 second temperament: Vallotti shifted one fifth sharp, so C-G
    // through B-F# carry the sixth-comma tempering.
    {"Young II",
     {0.000, -9.775, -3.910, -5.865, -7.820, -1.955, -11.730, -1.955,
      -7.820, -5.865, -3.910, -9.775}},

    // Quarter-comma meantone: every fifth in the chain narrowed by a quarter
    // syntonic comma, giving pure major thirds and the wolf on G#-Eb.
    {"Meantone 1/4 comma",
     {0.000, -23.951, -6.843, 10.265, -13.686, 3.422, -20.529,
      -3.422, -27.373, -10.265, 6.843, -17.108}},

    // Pure fifths throughout the chain; the Pythagorean comma piles up as the
    // wolf between G# and Eb.
    {"Pythagorean",
     {0.000, 13.685, 3.910, -5.865, 7.820, -1.955, 11.730, 1.955,
      15.640, 5.865, -3.910, 9.775}},

    // Silbermann sixth-comma meantone: every fifth narrowed by a sixth of a
    // syntonic comma, a milder meantone than the quarter-comma tuning.
    {"Silbermann 1/6 comma",
     {0.000, -11.406, -3.259, 4.888, -6.518, 1.629, -9.776, -1.629,
      -13.035, -4.888, 3.259, -8.147}},
};

const std::vector<Temperament>& temperamentLibrary() {
  return kTemperamentLib;
}

const Temperament* findTemperament(const std::string& name) {
  // Case-insensitive search
  auto lowerName = name;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  for (const auto& t : kTemperamentLib) {
    auto lowerLib = t.name;
    std::transform(lowerLib.begin(), lowerLib.end(), lowerLib.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lowerLib == lowerName) {
      return &t;
    }
  }
  return nullptr;
}

bool parseScala(const std::string& text, Temperament& out, std::string& error) {
  std::vector<std::string> lines;
  {
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
      if (i == text.size() || text[i] == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] != '!') lines.push_back(line);
        line.clear();
      } else {
        line += text[i];
      }
    }
  }
  // Blank lines count as content only for the description, which may be empty.
  if (lines.empty()) {
    error = "the file is empty";
    return false;
  }
  auto trim = [](std::string s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    const auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
  };
  const std::string description = trim(lines[0]);
  std::vector<std::string> rest;
  for (size_t i = 1; i < lines.size(); ++i)
    if (!trim(lines[i]).empty()) rest.push_back(trim(lines[i]));
  if (rest.empty()) {
    error = "no note count";
    return false;
  }
  const int count = std::atoi(rest[0].c_str());
  if (count != 12) {
    error = "a keyboard temperament has 12 notes; this scale has " + rest[0];
    return false;
  }
  if (static_cast<int>(rest.size()) < 1 + count) {
    error = "the scale lists fewer than 12 notes";
    return false;
  }
  std::vector<double> cents;
  for (int i = 1; i <= count; ++i) {
    // Only the first token counts: anything after it is a comment.
    std::string tok = rest[static_cast<size_t>(i)];
    const auto space = tok.find_first_of(" \t");
    if (space != std::string::npos) tok = tok.substr(0, space);
    double c = 0.0;
    if (tok.find('.') != std::string::npos) {
      c = std::atof(tok.c_str());
    } else {
      const auto slash = tok.find('/');
      const double num = std::atof(tok.substr(0, slash).c_str());
      const double den = slash == std::string::npos ? 1.0 : std::atof(tok.substr(slash + 1).c_str());
      if (num <= 0.0 || den <= 0.0) {
        error = "cannot read the note \"" + tok + "\"";
        return false;
      }
      c = 1200.0 * std::log2(num / den);
    }
    cents.push_back(c);
  }
  if (std::abs(cents.back() - 1200.0) > 0.5) {
    error = "the scale does not repeat at the octave";
    return false;
  }
  out.name = description.empty() ? "Scala" : description;
  out.centsOffset12.assign(12, 0.0);
  for (int i = 1; i < 12; ++i)
    out.centsOffset12[static_cast<size_t>(i)] = cents[static_cast<size_t>(i - 1)] - 100.0 * i;
  return true;
}

double pipeTargetHz(int midiNote, int rankBasePitch64ftHarmonicNum, double basePitchHz,
                    double deviationCents, const Temperament& t, int transposeSemitones) {
  // MIDI 69 (A4) is the anchor: with harmonic 8 and basePitchHz, yields basePitchHz.
  // harmonic N scales frequency by N/8 relative to 8' reference pitch.
  // Sounding note is midiNote + transposeSemitones; its pitch class selects temperament offset.

  const double refNote = 69.0;  // A4, concert A
  double soundingNote = static_cast<double>(midiNote + transposeSemitones);
  double semisFromRef =
      soundingNote - refNote + 12.0 * std::log2(static_cast<double>(rankBasePitch64ftHarmonicNum) / 8.0);
  double cents = 100.0 * semisFromRef + deviationCents;

  // Apply temperament offset for the sounding pitch class (mod 12, always non-negative).
  if (t.centsOffset12.size() == 12) {
    int pitchClass = static_cast<int>(soundingNote) % 12;
    if (pitchClass < 0) pitchClass += 12;
    cents += t.centsOffset12[static_cast<size_t>(pitchClass)];
  }

  return basePitchHz * std::pow(2.0, cents / 1200.0);
}

double playbackRatio(double targetHz, double recordedHz) {
  if (recordedHz <= 0.0) return 1.0;
  return targetHz / recordedHz;
}

double detunedTargetHz(double targetHz, int controlValue, double centre,
                       double sensitivityHzPerUnit) {
  if (targetHz <= 0.0 || sensitivityHzPerUnit == 0.0) return targetHz;
  const double fromCentre = controlValue - centre;
  if (fromCentre == 0.0) return targetHz;
  const double moved = targetHz + fromCentre * sensitivityHzPerUnit;
  // Half the target is far beyond any detuning an organ builder means, and
  // stops a bad sensitivity from dropping a rank an octave or through zero.
  return moved < targetHz * 0.5 ? targetHz * 0.5 : moved;
}

double temperedPlaybackRatio(int midiNote, int harmonicNum64ft, double basePitchHz,
                             double deviationCents, const Temperament& t, int transposeSemi) {
  // Return pipeTargetHz / basePitchHz; the pitch ratio relative to base reference.
  double target = pipeTargetHz(midiNote, harmonicNum64ft, basePitchHz, deviationCents, t, transposeSemi);
  return target / basePitchHz;
}

double originalPitchRatio(double samplePitchHz, double targetPitchHz) {
  // Original-organ-pitch mode: directly return the ratio of target to recorded frequency.
  if (samplePitchHz <= 0.0) return 1.0;
  return targetPitchHz / samplePitchHz;
}

double centsBetween(double aHz, double bHz) {
  // Cents between two frequencies: 1200 * log2(aHz / bHz).
  // Returns 0.0 if either frequency is <= 0.
  if (aHz <= 0.0 || bHz <= 0.0) return 0.0;
  return 1200.0 * std::log2(aHz / bHz);
}

bool pipeHzInRange(double hz) {
  return hz >= kMinPipeHz && hz <= kMaxPipeHz;
}

const char* pitchRouteName(PitchRoute r) {
  switch (r) {
    case PitchRoute::NoTuning:     return "declared-none";
    case PitchRoute::FileMetadata: return "file-metadata";
    case PitchRoute::Tempered:     return "tempered-fields";
    case PitchRoute::ExactHz:      return "exact-hz";
    case PitchRoute::Tremulant:    return "tremulant";
    case PitchRoute::Filename:     return "filename";
    case PitchRoute::NoisePlaceholder: return "noise-placeholder";
    case PitchRoute::Unresolved:   break;
  }
  return "unresolved";
}

int midiNoteFromFileName(const std::string& fileName) {
  // The bare name, after either separator: sets are written on Windows and
  // read here, and a set that says "Rank\036-c.wav" means the same file as
  // one that says "Rank/036-c.wav".
  const size_t cut = fileName.find_last_of("/\\");
  const std::string base =
      (cut == std::string::npos) ? fileName : fileName.substr(cut + 1);

  size_t n = 0;
  while (n < base.size() && n < 3 &&
         std::isdigit(static_cast<unsigned char>(base[n])))
    ++n;
  if (n == 0) return -1;
  // A digit immediately after the run means the leading digits are part of a
  // longer number and not a note: "1234-c.wav" is not note 123.
  if (n < base.size() && std::isdigit(static_cast<unsigned char>(base[n])))
    return -1;

  const int note = std::stoi(base.substr(0, n));
  return (note >= 0 && note <= 127) ? note : -1;
}

namespace {
double noteToHz(double midiNote, double concertAHz) {
  return concertAHz * std::pow(2.0, (midiNote - 69.0) / 12.0);
}
} // namespace

SamplePitchResult resolveSamplePitch(const SamplePitchInputs& in,
                                     double concertAHz,
                                     double organBasePitchHz) {
  const double aHz = (concertAHz > 0.0) ? concertAHz : 440.0;


  // What "the exact frequency" means when the set reaches for it. An
  // unpitched sample declares a PLACEHOLDER here rather than a pitch -- "in
  // case of noise sample, Pitch_ExactSamplePitch can be 100 or the value of
  // AudioEngine_BasePitchHz" (OdfEdit.py:9759). Believing it turns a tracker
  // click into a 9 Hz thud on a rank keyed from 1, and the engine already
  // plays the noise ranks it can IDENTIFY at their recorded pitch; this is
  // the same rule for the sets that ship no Noise table at all, as Friesach
  // does. Exactly 100.000 Hz sits between G2 and G#2, where no real pipe is
  // declared to four decimal places.
  //
  // The declaration beats the file: Friesach's key-action releases share one
  // silent Noises/BlankLoop.wav whose smpl chunk claims note 98.3, an
  // artefact of whatever wrote it, and obeying that transposed a blank by six
  // semitones. It is only consulted where the set actually asks for the exact
  // frequency -- a set that says "the pitch is in the file" is not talking
  // about this field at all.
  auto useExactHz = [&]() -> SamplePitchResult {
    // A pipe of that very pitch declares it honestly: the GrandOrgue demo's
    // A4 Bourdon is a G recording declared at 440 Hz and retuned up a tone,
    // and taking that for a placeholder played it as G (#154).
    const bool pipeAtIt = in.pipeNominalHz > 0.0 && in.exactHz > 0.0 &&
                          std::abs(std::log2(in.exactHz / in.pipeNominalHz)) < 1.0 / 12.0;
    const bool placeholder =
        std::abs(in.exactHz - 100.0) < 1e-9 ||
        (organBasePitchHz > 0.0 && !pipeAtIt &&
         std::abs(in.exactHz - organBasePitchHz) < 1e-9);
    if (placeholder) return {0.0, PitchRoute::NoisePlaceholder};
    return {in.exactHz, PitchRoute::ExactHz};
  };

  // The declared route first, and only the declared route. Falling through to
  // another field because the declared one is empty would undo the point of
  // reading the code: a set that says "the pitch is in the file" and ships a
  // file without metadata is telling us it does not know, and guessing from a
  // stale neighbouring field is worse than admitting that.
  switch (in.methodCode) {
    case 0:
      return {0.0, PitchRoute::NoTuning};
    case 2:
    case 5:
      return {0.0, PitchRoute::Tremulant};
    case 1:
      if (in.fileMidiNote >= 0.0)
        return {noteToHz(in.fileMidiNote, aHz), PitchRoute::FileMetadata};
      break;
    case 3:
      if (in.normalMidiNote >= 0) {
        // The harmonic number is half of this route, not decoration. A rank
        // declared at 1 1/3' (harmonic 48) sounds 31 semitones above the note
        // its pipes are keyed at, and reading the note without it puts the
        // whole rank two and a half octaves flat.
        const int harm = (in.rankBasePitch64ftHarmonicNum > 0)
                             ? in.rankBasePitch64ftHarmonicNum
                             : 8;
        const double note = static_cast<double>(in.normalMidiNote) +
                            12.0 * std::log2(static_cast<double>(harm) / 8.0);
        return {noteToHz(note, aHz), PitchRoute::Tempered};
      }
      break;
    case 4:
      if (in.exactHz > 0.0) return useExactHz();
      break;
    default:
      break;
  }

  // No code, or the declared route had nothing in it. Take whatever is
  // actually present, most precise first: an explicit frequency, then the
  // file's own metadata, then the tempered fields.
  if (in.exactHz > 0.0) {
    const auto r = useExactHz();
    if (r.route == PitchRoute::ExactHz) return r;
    // A placeholder with no code behind it says nothing either way; keep
    // looking rather than declaring the sample unpitched on its own.
  }
  if (in.fileMidiNote >= 0.0)
    return {noteToHz(in.fileMidiNote, aHz), PitchRoute::FileMetadata};
  if (in.normalMidiNote >= 0) {
    const int harm = (in.rankBasePitch64ftHarmonicNum > 0)
                         ? in.rankBasePitch64ftHarmonicNum
                         : 8;
    const double note = static_cast<double>(in.normalMidiNote) +
                        12.0 * std::log2(static_cast<double>(harm) / 8.0);
    return {noteToHz(note, aHz), PitchRoute::Tempered};
  }

  // Nothing declared anywhere. The file name is the last thing left, and it
  // is a guess: it fixes the gross case (one recording serving several pipes)
  // and cannot know the pipe's own few cents of detuning.
  const int fromName = midiNoteFromFileName(in.fileName);
  if (fromName >= 0)
    return {noteToHz(static_cast<double>(fromName), aHz), PitchRoute::Filename};

  return {0.0, PitchRoute::Unresolved};
}

} // namespace mp
