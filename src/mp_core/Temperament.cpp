#include "Temperament.h"
#include <algorithm>
#include <cctype>
#include <cmath>

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
                                     double concertAHz) {
  const double aHz = (concertAHz > 0.0) ? concertAHz : 440.0;

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
      if (in.exactHz > 0.0) return {in.exactHz, PitchRoute::ExactHz};
      break;
    default:
      break;
  }

  // No code, or the declared route had nothing in it. Take whatever is
  // actually present, most precise first: an explicit frequency, then the
  // file's own metadata, then the tempered fields.
  if (in.exactHz > 0.0) return {in.exactHz, PitchRoute::ExactHz};
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
