// Temperament / pitch solver (M2.2 musical parity).
// Per-pipe tempered (BaseTuningSchemeCode, RankBasePitch64ftHarmonicNum,
// BaseTuningDeviation, random detune) vs original-organ (SpecificationMethodCode,
// PitchHz) vs sample metadata/filename. Global temperament modes + transposer.
#pragma once
#include <string>
#include <vector>

namespace mp {

struct Temperament {
  std::string name;
  std::vector<double> centsOffset12; // size 12, semitone offsets from equal temperament; index 0 = C
};

// Built-in temperament library; case-insensitive lookup.
const std::vector<Temperament>& temperamentLibrary();
const Temperament* findTemperament(const std::string& name);

// Absolute sounding frequency (Hz) for a pipe.
// MIDI 69 @ harmonic 8, 440 Hz base, zero deviation, Equal temperament => 440.0 Hz.
double pipeTargetHz(int midiNote, int rankBasePitch64ftHarmonicNum, double basePitchHz,
                    double deviationCents, const Temperament& t, int transposeSemitones = 0);

// Resample ratio: targetHz / recordedHz. Returns 1.0 if recordedHz <= 0.
double playbackRatio(double targetHz, double recordedHz);

// A pipe's target pitch after detuning. An organ goes out of tune pipe by
// pipe, so a set declares a control per division and zone and gives each pipe
// its own sensitivity in Hz per control unit.
//
// The control is BIPOLAR: the offset is measured from the middle of its
// travel, not from zero. Every sensitivity a real set declares is positive —
// Nancy has 3056 of them and not one is negative — while half its layers name
// a "...DetPos" control and half a "...DetNeg" one. The sign therefore comes
// from which side of centre the control sits, and the pair spreads pipes
// above and below true pitch as detuning is asked for. Read the other way,
// with the offset measured from zero, every pipe goes sharp together and the
// whole organ sits about ten cents high while claiming to be in tune.
//
// `centre` is the middle of the control's declared range, so a control at
// rest detunes nothing.
//
// Clamped to half the target: detuning is drift, and a pathological
// sensitivity must not be able to transpose or invert a rank.
double detunedTargetHz(double targetHz, int controlValue, double centre,
                       double sensitivityHzPerUnit);

// Tempered playback ratio: pipeTargetHz / basePitchHz (for resampling relative to base).
double temperedPlaybackRatio(int midiNote, int harmonicNum64ft, double basePitchHz,
                             double deviationCents, const Temperament& t, int transposeSemi = 0);

// Original-pitch mode bypasses temperament math (EnablePlayingAtOriginalOrganPitch).
double originalPitchRatio(double samplePitchHz, double targetPitchHz);

// Helper: cents between two frequencies. 1200*log2(aHz/bHz); 0.0 if either <= 0.
double centsBetween(double aHz, double bHz);

// Pipe frequency bounds.
inline constexpr double kMinPipeHz = 8.0;      // 64' C is ~8.18 Hz
// A 1 3/5' Tierce (64ft harmonic 40) genuinely sounds above 30 kHz at the top
// of its compass — the Lemmer set has six such pipes, and they are correct, not
// broken. The ceiling is here to catch an IMPOSSIBLE pitch (a mis-declared
// footage, which lands in the megahertz), not an inaudible one, so it sits
// above what real mutation ranks reach. Whether such a pipe should sound at all
// is a playback decision, not a validity one.
inline constexpr double kMaxPipeHz = 40000.0;  // above audible top of a 1' rank
bool pipeHzInRange(double hz);

} // namespace mp
