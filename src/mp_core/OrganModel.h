// Masterpiece — immutable organ model (2026 contemporary design).
// Inspired by GrandOrgue 2004 structures, rebuilt for JUCE/C++20.
// ODF parse (background) -> OrganModel (immutable, sharable) -> AudioGraph.
// User data (combinations/voicing/MIDI) lives OUTSIDE this model (ValueTree).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mp {

using Id = int; // Hauptwerk object IDs are ints; 0 = none/invalid in most tables.

enum class OdfType { Unknown, Full, Codm };

struct EngineSwitch {
  // Play each pipe at the pitch it sounded on the original instrument
  // (EnablePlayingAtOriginalOrganPitch) rather than at a tempered pitch
  // derived from the keyboard. Requires Pitch_OriginalOrgan_PitchHz; pipes
  // that lack it fall back to the tempered path.
  bool playAtOriginalOrganPitch = false;
  // Runtime DSP mode. Full DSP is OPTIONAL at runtime:
  // slow machines use SimpleWav, hi-end enables FullVoicing + WindModel.
  bool simpleWavOnly = false;   // bypass wind/trem/enclosure/voicing
  bool enableWindModel = true;  // false = InfiniteVolume fast path
  bool enableTremulant = true;
  bool enableEnclosure = true;
  bool enableVoicing = true;
};

enum class SampleLoadMode { Auto, Preloaded, Streaming };
// Auto: preload if RAM allows, else stream. Same code path (HISE lesson:
// preload head + stream tail; 128GB machine never touches disk in play).

// One package a set is played from. Hauptwerk installs sample content under
// OrganInstallationPackages/<id zero-padded to 6>, and an ODF references files
// relative to that directory rather than to itself.
struct InstallationPackage {
  Id packageId = 0;
  std::string name;
  std::string supplierName;
  bool presentOnDisk = false;
};

struct SampleRef {
  Id sampleId = 0;
  Id installationPackageId = 0; // which package holds the file
  std::string packageId;   // RequiredInstallationPackage (legacy string form)
  std::string fileName;    // e.g. "036-C.wav"
  bool encrypted = false;  // .hbw/.hbx detected -> report, don't load (v1 legal rule)
  double pitchHz = 0.0;    // Pitch_ExactSamplePitch (method code 4)
  int midiNote = -1;       // Pitch_NormalMIDINoteNumber (method code 3)
  int rankBasePitch64ftHarmonicNum = 8; // tempered path base
  // Which of the fields above the SET says to believe: 0 none, 1 the file's
  // own metadata, 2/5 tremulant, 3 the note + harmonic, 4 the exact Hz.
  // -1 means the row declared no code at all. Reading the fields without
  // this is guesswork -- see resolveSamplePitch() in Temperament.h.
  int pitchMethodCode = -1;
  // Filled in after the file is opened, not by the ODF parser: the note the
  // WAV's own smpl chunk says it sounds, fractional, concert pitch. Negative
  // until known, and still negative for a file that declares none.
  double fileMidiNote = -1.0;
  // What the sample actually holds, in Hz, once the code above has been
  // obeyed. 0 means "nothing declares one" -- play the file as it is.
  double resolvedPitchHz = 0.0;
};

struct AttackSample {
  Id id = 0;
  SampleRef sample;
  // Selection matrix (M2.1, first-match in file order): note sounds when
  // velocity <= velHigh, timeSinceCloseMs >= minTimeSinceCloseMs and
  // continuous-control value <= ctsHigh. HW stores ceilings only.
  int velHigh = 127;
  int minTimeSinceCloseMs = 0;
  int ctsHigh = 127;
  // Load range (raw codes + values; type semantics unverified — OdfEdit
  // lists the codes as unknown, so ranges are preserved, not applied).
  int loadStartType = 0, loadStartValue = 0;
  int loadEndType = 0, loadEndValue = 0;
};

struct ReleaseSample {
  Id id = 0;
  SampleRef sample;
  int velHigh = 127;
  int64_t holdTimeMsHigh = INT32_MAX;
  // Attack context: which played-attack situation this release belongs to.
  int attackVelHigh = 127;
  int attackCtsHigh = 127;
  int ctsHigh = 127;
  bool scaleAmplitude = true;
  Id preferLinkedAttackId = 0; // ReleaseSelCriteria_PreferThisRelForAttackID
  int loadStartType = 0, loadStartValue = 0;
  int loadEndType = 0, loadEndValue = 0; // raw; type semantics unverified (see AttackSample)
  double releaseCrossfadeMs = 10.0;
  bool phaseAlign = false;
};

struct PipeLayer {
  Id layerId = 0;
  double gainDb = 0.0;
  double loopCrossfadeMs = 8.0;
  int loopStartFrames = -1; // -1 = from WAV cue
  int loopEndFrames = -1;
  std::vector<AttackSample> attacks;
  std::vector<ReleaseSample> releases;
  int optimalChannel = 0; // AudioOut_OptimalChannelFormatCode (M2.1 stored, M4 routed)
  int optimalResolution = 0; // AudioOut_OptimalSampleResolutionCode
  // The control that scales this layer's amplitude. EVERY layer names one —
  // it is how an organ's own level sliders reach the pipework. A noise layer
  // points at its audio group's noise level; a pipe layer at that group's
  // mixed level for its division. The player moves a percentage slider, a
  // double linkage combines it with the group level, and the answer lands
  // here. 0 means the layer plays at its declared gain and nothing else.
  Id ampScalingControlId = 0;
  // Detuning, as a control and a rate. An organ drifts out of tune pipe by
  // pipe, never as a block, so a set declares a control per division and zone
  // and gives each pipe its own sensitivity — Nancy has 245 such controls and
  // a different rate on almost every layer. offsetHz = value * sensitivity.
  //
  // Both sit at zero until a player asks for detuning, so a set that ships
  // this costs nothing until it is used.
  Id pitchControlId = 0;
  double pitchSensitivityHzPerUnit = 0.0;
  // How hard the key was struck changes how loud the pipe speaks. The organ
  // states the attenuation at the softest touch; full velocity is unattenuated.
  // Half the corpus declares one, and without it every note plays at one
  // level, which is what a tracker action is NOT. `invert` swaps the sense
  // for the sets whose couplers or second touch need it (AmpLvl_Invert...).
  // The raw sign is kept as the file writes it — producers disagree (+5 on
  // Alessandria, -6 on Giubiasco) and the magnitude is the attenuation.
  double velSensMaxAttenDb = 0.0; // AmpLvl_VelocitySensitivityMaxAttenuationDecibels
  bool invertVelocitySens = false;
  // How far THIS layer's tremulant depth is adjusted from the pipe's own.
  // The depth belongs to the chest (TremulantWaveformPipe); these are the
  // per-layer trims on top of it, and applying them per layer is what keeps a
  // flute and a reed on the same tremulant wobbling differently.
  double tremAmpDepthAdjustDb = 0.0;    // AmpLvl_TremulantModDepthAdjustDecibels
  double tremPitchDepthAdjustPct = 100.0; // PitchLvl_TremulantModDepthAdjustPercent
  // M2+: enclosure/trem/wind depth, EQ, AudioOut codes, reverb-tail truncation.
  double enclosureDepth01 = 1.0;
  double tremDepthDb = 0.0;
  int audioOutCode = 0;
};

struct Pipe {
  Id pipeId = 0;
  int midiNote = 60; // mapped division input note
  std::vector<PipeLayer> layers;
  double baseTuningDeviationCents = 0.0;
  // What this pipe does to the wind. It draws air from one compartment and
  // exhausts into another, at a declared mass flow rate; that rate is the
  // single most important number in the wind model, because it is the
  // difference between a 32' Bombarde and a 2' Piccolo and it varies by more
  // than an order of magnitude across a rank.
  Id windSourceCompartmentId = 0;
  Id windOutputCompartmentId = 0;
  double windMassFlowKgPerSec = 0.0;
  double windRefPressureInches = 1.0;
  double windAirConsumption = 0.0; // legacy CODM field, kept for that path
  // Footage as a 64' harmonic number (8 = 8' unison, 16 = 4', 4 = 16'). The
  // temperament solver needs it per pipe, because a rank can be transposed.
  int basePitch64ftHarmonicNum = 8;
  // What this pipe actually sounded on the original instrument
  // (Pitch_OriginalOrgan_PitchHz). HW can play a set at its own historical
  // pitch instead of a tempered one, which is the whole point of sampling a
  // specific organ; 0 means the file does not say.
  double originalOrganPitchHz = 0.0;
  // The switch whose state opens this pipe's pallet
  // (ControllingPalletSwitchID). An organ can wire its pipework entirely
  // through the switch network -- key switch, through the stop's switch, to
  // the pallet -- and declare no StopRank at all; the pipe then speaks while
  // this switch is engaged and at no other time. 0 means the file names none.
  Id palletSwitchId = 0;
};

struct Rank {
  Id rankId = 0;
  std::string name;
  std::vector<Pipe> pipes;
  bool percussive = false;
  bool randomNoteZero = false; // noise ranks
  // M2.4: key, stop, blower and tracker action noises are ordinary ranks with
  // their own envelopes; they are flagged so the engine can route and gate
  // them separately (and so Options::includeKeyNoises can drop them).
  bool isNoise = false;
  Id noiseTriggerSwitchId = 0;
};

struct StopRankEntry {
  Id rankId = 0;
  int firstMappedDivisionNote = 36;
  int numMappedNotes = 61;
  int midiIncrement = 0;
  Id alternateRankId = 0;
  bool retriggerOnAlternate = false;
};

struct Stop {
  Id stopId = 0;
  std::string name;
  Id divisionId = 0; // Stop.DivisionID — which division this stop draws (M1.3)
  int defaultAsgnCode = 0; // 20xx-30xx determines capture division + jamb sort
  std::vector<StopRankEntry> ranks;
  Id controllingSwitchId = 0;
  // Hint_PrimaryAssociatedRankID: the rank this stop draws, for the sets —
  // and every demo set that ships part of its pipework — that declare no
  // StopRank rows at all. The reference converters follow it; without it the
  // stop draws and plays nothing. Gathered into `ranks` at load when it is
  // safe to do so; see the loader for the one case where it is not.
  Id hintPrimaryRankId = 0;
};

// One edge of the key-flow graph: keys played on `sourceKeyboard` also reach
// `destKeyboard` or `destDivision`, transposed by `midiIncrement`.
//
// This is the whole coupling mechanism. A manual's own keys reach its division
// through an unconditional edge; a coupler is the same edge gated on a
// drawstop's switch. There is no separate "coupler" object in a Hauptwerk
// organ, which is why coupling cannot be modelled as a fixed matrix.
// A key of a keyboard, as a SWITCH. This is how a set whose manuals are part
// of a photographed backdrop still makes them playable: each key is its own
// drawn, clickable switch, and the organ says which keyboard and which note it
// is. Nancy draws 300 of them this way; without this they read as drawstops
// and clicking one toggles a picture and sounds nothing.
struct KeyboardKeyRef {
  Id keyboardId = 0;
  int midiNote = 60;
};

struct KeyAction {
  Id id = 0;
  int sourceDivision = 0, destDivision = 0;
  int sourceKeyboard = 0, destKeyboard = 0;
  bool destIsKeyboard = true;
  Id conditionSwitchId = 0;
  // Whether the action is live while the condition switch is ENGAGED (the
  // usual sense: a drawn coupler couples) or while it is disengaged, which is
  // how a unison-off is written.
  bool conditionWhenEngaged = true;
  // The window of source keys this edge carries. 0 keys means "all of them":
  // plenty of organs leave it unstated.
  int firstSourceNote = 0;
  int numKeys = 0;
  int midiIncrement = 0;
  int actionType = 1; // full matrix M3 (pizz/reit/traps/custom 10000+)
  int effectCode = 0;

  bool carries(int midiNote) const {
    if (numKeys <= 0) return true;
    return midiNote >= firstSourceNote && midiNote < firstSourceNote + numKeys;
  }
};

// One wire of the organ's switch network: the state of `sourceSwitchId`
// drives the state of `destSwitchId`, optionally gated on a third switch.
//
// This is how a Hauptwerk console is actually wired. The drawstop a player
// clicks is rarely the switch anything reads: it drives an internal node that
// the couplers, the combinations and the pipework consult. Lemmer's "Pedaal
// koppel" is switch 1006, and everything that cares about it looks at switch
// 10101, "CouplerNode_1006", one linkage away.
struct SwitchLinkage {
  Id sourceSwitchId = 0;
  Id destSwitchId = 0;
  Id conditionSwitchId = 0;
  // Which state of the source (and of the condition) makes the link fire.
  bool sourceWhenEngaged = true;
  bool conditionWhenEngaged = true;
  // What to do to the destination when the link fires, and when it does not.
  // 1 and 4 engage it; 2 and 7 disengage it. The four combinations organs
  // actually use are 1/2 and 4/7 (a plain follow) and 7/4, or 1/2 with an
  // inverted source (an inverting follow) — which is exactly what the pair of
  // codes plus sourceWhenEngaged expresses, with no special cases.
  int engageAction = 1;
  int disengageAction = 2;
};

struct Division {
  Id divisionId = 0;
  std::string name;
  int manualNumber = 0; // 0=pedal, 1..6
  std::vector<Id> keyboardIds; // linked via Keyboard Hint_*AssociatedDivisionID (M1.3)
  std::vector<KeyAction> keyActions;
};

// The shapes a drawn manual is built from. Hauptwerk does not ship a keyboard
// as one picture: it ships one image per KEY SHAPE plus the horizontal
// advances between them, and the console assembles the manual. A set whose
// manual is drawn this way (Lemmer) shows an empty gap until this is rendered;
// a set whose manuals are part of a photographed backdrop (Nancy) does not.
//
// The shapes differ because a key is not a rectangle: a C or an F is notched
// on the right only, an E or a B on the left only, a D both sides, and the
// keys at either end of the compass are cut differently again.
struct KeyImageSet {
  Id keyImageSetId = 0;
  std::string name;
  // One image set per shape. 0 means the set does not define that shape, in
  // which case WholeNatural is the fallback.
  Id shapeCF = 0;
  Id shapeD = 0;
  Id shapeEB = 0;
  Id shapeG = 0;
  Id shapeA = 0;
  Id shapeWholeNatural = 0;
  Id shapeSharp = 0;
  Id shapeFirstKeyDA = 0; // bottom key of the compass, when it is a D or an A
  Id shapeFirstKeyG = 0;
  Id shapeLastKeyDG = 0; // top key, when it is a D or a G
  Id shapeLastKeyA = 0;
  // Which frame of each shape's image set is the pressed key and which the
  // released one. Sets routinely omit both; frame 1 is the released key and
  // frame 2 the pressed one, which is what Hauptwerk assumes.
  int indexEngaged = 2;
  int indexDisengaged = 1;

  // Horizontal advances in pixels. These do not reduce to one number: the gap
  // from a C to its C# is not the gap from that C# to the D.
  int spacingNaturalToNatural = 0;
  int spacingCFToSharp = 0;
  int spacingDAToSharp = 0;
  int spacingGToSharp = 0;
  int spacingSharpToDG = 0;
  int spacingSharpToEB = 0;
  int spacingSharpToA = 0;
};

struct Keyboard {
  Id keyboardId = 0;
  std::string name;
  int numKeys = 0;
  int firstMidiNote = 0;
  // Hauptwerk's own default MIDI assignment: 1 is the pedal, 2 the first
  // manual, 3 the second, and so on. It is what decides which channel plays
  // which manual before the player has mapped anything, and it is carried by
  // the organ's internal keyboards rather than by the drawn ones.
  int assignmentCode = 0;
  // The division this keyboard plays when no KeyAction says otherwise. Named a
  // hint by Hauptwerk, but on a full-size organ it is the only thing that
  // links a manual to its pipework short of walking the switch graph.
  Id primaryDivisionHint = 0;
  // Whether a player's MIDI can reach this keyboard. The organ's internal
  // keyboards and its drawn console manuals are marked N; the manuals a player
  // actually plays are marked Y or leave it unstated.
  bool accessibleForInput = true;
  // Console placement of a DRAWN manual. keyImageSetId 0 means this keyboard
  // is not drawn: a pedalboard usually is not, nor is an input-only keyboard.
  Id keyImageSetId = 0;
  Id displayPageId = 0;
  int dispLeftPx = 0, dispTopPx = 0;
  // The same manual on the set's other console layouts, with its own key
  // artwork: a manual drawn for a wide console is not the one drawn for a
  // narrow one. 0 means this layout does not draw the keyboard at all.
  Id altKeyImageSetId[3] = {0, 0, 0};
  int altLeftPx[3] = {0, 0, 0}, altTopPx[3] = {0, 0, 0};

  Id keyImageSetFor(int layout) const {
    if (layout <= 0 || layout > 3) return keyImageSetId;
    return altKeyImageSetId[layout - 1];
  }
  int dispLeftFor(int layout) const {
    return (layout <= 0 || layout > 3) ? dispLeftPx : altLeftPx[layout - 1];
  }
  int dispTopFor(int layout) const {
    return (layout <= 0 || layout > 3) ? dispTopPx : altTopPx[layout - 1];
  }
};

struct Switch {
  Id switchId = 0;
  std::string name;
  bool isCoupler = false;
  bool isTremulant = false;
  bool defaultEngaged = false;
  // A drawstop latches; a piston does not. A momentary switch releases itself
  // as soon as it has fired, which is what makes a piston a piston.
  bool latching = true;
  // Hauptwerk's own default assignment code. For a piston it doubles as the
  // link to the combination it fires: a combination of type 101 is fired by
  // the switch whose code is 101, and organs wire their pistons that way
  // rather than naming a switch on the combination.
  int asgnCode = 0;
  bool rememberState = false;
  bool clickable = true;
  // How this switch appears on the console: which drawn instance it owns, and
  // which frame of that instance's image set means on and off. This is the
  // whole mechanism behind a drawstop that moves when you click it.
  Id dispInstanceId = 0;
  int dispIndexEngaged = 0;
  int dispIndexDisengaged = 0;
};

struct CombinationElement {
  Id combinationId = 0;
  // What the piston moves, and what it looks at when capturing. Usually the
  // same switch; they differ when a combination drives something other than
  // the thing it remembers.
  Id controlledSwitchId = 0;
  Id capturedSwitchId = 0;
  // The state the organ shipped with. Capture replaces it, and it is saved to
  // the player's own data — never back into the sample set.
  bool storedEngaged = false;
  // Flip the stored state on the way out. A handful of elements are wired
  // this way so that one piston can pull some stops and cancel others.
  bool invertWhenActivating = false;
};

// A piston. `type` is Hauptwerk's CombinationTypeCode, which is two things at
// once: the small codes name a kind (1 master capture, 2 general, 3 divisional,
// 4 crescendo, 6 general cancel, 7 divisional cancel), and the 1xx/2xx/...
// families name a specific piston (101 is "General 01", 200 is "Pedal
// divisional cancel"). Lemmer numbers all ten of its generals that way.
struct Combination {
  Id combinationId = 0;
  int type = 0;
  std::string name;
  // The switch that fires it. Often unstated, in which case the piston is the
  // switch whose DefaultInputOutputSwitchAsgnCode equals this type code —
  // which is how an organ wires a console piston to "General 01" without
  // naming a switch anywhere.
  Id activatingSwitchId = 0;
  // What the piston is allowed to do. A "cancel" is simply a combination that
  // may disengage and not engage.
  bool canEngage = true;
  bool canDisengage = true;
  bool allowsCapture = true;
  std::vector<CombinationElement> elements;

  // Cancels every stop rather than recalling a registration.
  bool isCancel() const {
    return type == 6 || type == 7 || (type >= 100 && type % 100 == 0);
  }
};

struct Enclosure {
  Id enclosureId = 0;
  std::string name;
  Id continuousControlId = 0;
  // The filter the shades impose, as the engine's one-cutoff-one-gain model
  // takes it. Hauptwerk states these PER PIPE (EnclosurePipe's FiltParam...),
  // relative to each pipe's own pitch, so there is no single enclosure-level
  // number in the file: these are the medians of the pipes' values gathered at
  // load, which is the representative figure for a bus-level filter. Set from
  // the pipes when the set declares any; the numbers below are only the
  // fallback for a box that ships none.
  double closedFilterHz = 800.0, openFilterHz = 12000.0;
  double closedAttnDb = -24.0, openAttnDb = 0.0;
  bool filterParamsFromPipes = false;
  // Shade positions the ODF actually declares (EnclosurePipe rows). An
  // enclosure with none is inert — the validator reports it rather than
  // silently swallowing the swell pedal (query enclosure-without-shades).
  int numShades = 0;
};

// M2.4: a console input that sweeps rather than switches — swell shoes,
// crescendo wheels, generic assignable controls. Pipework and enclosures
// reference these by id; the runtime feeds them 0..127 from MIDI or the UI.
struct ContinuousControl {
  Id controlId = 0;
  std::string name;
  int defaultValue = 0;   // 0..127, the position at organ load
  int minValue = 0;
  int maxValue = 127;
  // HW distinguishes the control from what drives it; an unmapped type code
  // is preserved and reported, never guessed (ADR-002).
  int typeCode = 0;
  bool inverted = false;

  // The drawn thing a player actually moves. Only a minority of controls have
  // one — an organ declares a control for every audio-group level and every
  // detuning parameter, and draws the handful it means you to touch.
  Id imageSetInstanceId = 0;
  bool clickable = true;
  // Which way along the image is "more". Stated by the organ rather than
  // inferred, because a swell shoe and a level slider do not agree about it.
  bool clickingHigherIncreasesValue = true;
  // Whether this position should come back next time. The organ decides, and
  // it is not the same answer for everything: a noise level is a preference,
  // but a swell shoe and a crescendo must start where the organ says rather
  // than where they were left. Nancy marks 49 of its 55 drawn controls.
  bool rememberState = false;
};

// How a control's 0..127 shows on its image: a staircase of value bands, each
// naming the frame to draw. Keyed by IMAGE SET, not by control, so several
// controls drawn with the same artwork share one ladder — which is how an
// organ affords a hundred identical percentage sliders.
//
// Rows do NOT arrive in value order and must be sorted before use.
struct ContinuousControlImageStage {
  int highestValue = 0;  // top of the band, inclusive
  int imageIndex = 1;
};

// A shoe position that moves a switch. This is how a crescendo works: the
// pedal is one continuous control with a row per step, each naming the switch
// that fires that step's registration. It is also how a blower starts, how
// enclosure noises are triggered, and how an organ delays anything at load.
//
// Each row is a THRESHOLD CROSSING, not a state. The flags say what a crossing
// does, separately for rising and falling, which is what lets an organ give a
// blower hysteresis: Nancy starts hers when the value rises through 120 and
// stops her when it falls back through 126, with two rows naming one switch.
struct ContinuousControlStageSwitch {
  Id controlId = 0;
  int value = 0; // the threshold, in the control's own 0..127 domain
  Id controlledSwitchId = 0;
  bool engageWhenIncreasing = false;
  bool engageWhenDecreasing = false;
  bool disengageWhenIncreasing = false;
  bool disengageWhenDecreasing = false;
};

// A linkage drives one continuous control from another (shoe -> crescendo,
// or a doubled shoe), optionally scaled. Cycles are rejected by the
// validator, not followed at run time.
struct ContinuousControlLinkage {
  Id linkageId = 0;
  Id sourceControlId = 0;
  Id destControlId = 0;
  Id conditionSwitchId = 0;
  // Whether the linkage is live while the condition switch is engaged, or
  // while it is disengaged. Tremulant crossfades come in such pairs, one of
  // each sense, feeding the same control.
  bool conditionWhenEngaged = true;
  // InvertSourceControlValue is folded into these at load: the loader negates
  // the coefficient and adds 127, which mirrors the source within its range.
  // Nancy marks 203 of her 1097 linkages that way.
  double scale = 1.0;
  int offset = 0;
};

// Two controls combined into a third. This is how an organ builds a level out
// of several sliders: "AG 0 Noises 0" is the Close audio-group level TIMES the
// key-action noise level, renormalised by a coefficient of 1/127 so the
// product lands back in 0..127.
//
//   dest = destCoefficient * op(first*firstCoef + firstInc,
//                               second*secondCoef + secondInc)
//
// Operation codes, each confirmed against the organ's own names and declared
// defaults rather than guessed: 1 adds (a row named "SpeedSum"), 2 subtracts
// (127 - 127 = 0, which is the destination's stated default), 3 multiplies.
// Anything else is reported and leaves its destination alone (ADR-002).
struct ContinuousControlDoubleLinkage {
  Id destControlId = 0;
  Id firstControlId = 0;
  Id secondControlId = 0;
  int operationCode = 0;
  double firstCoefficient = 1.0;
  double firstIncrement = 0.0;
  double secondCoefficient = 1.0;
  double secondIncrement = 0.0;
  double destCoefficient = 1.0;
  double destIncrement = 0.0;
};

struct Tremulant {
  Id tremulantId = 0;
  std::string name;
  double engagedHz = 6.0, disengagedHz = 5.0;
  double startPercent = 0.0, stopPercent = 100.0;
  bool hasWaveform = false;
  Id waveformId = 0;
  double depthPercent = 0.0; // amplitude swing; 0 with the stop enabled is a fault
  Id controllingSwitchId = 0;
};

// One body of air in the organ's wind system: the blower's output, a
// reservoir, a windchest, or the open air the pipes exhaust into.
//
// Pressures are in inches of water above ambient, which is what organ builders
// and Hauptwerk both use. An infinite compartment is a boundary condition —
// the blower's intake and the room itself — and never moves.
// How much one tremulant moves one pipe. Hauptwerk states it per pipe, and it
// matters: a tremulant is wired to a chest, not to a rank, and it reaches the
// stops on that chest by different amounts. A flute wobbles more than a reed
// on the same wind.
struct TremulantPipeMod {
  Id tremulantId = 0;
  double ampDepthDb = 0.0;    // peak amplitude swing
  double pitchDepthPct = 0.0; // peak pitch swing, in percent of a semitone
};

struct WindCompartment {
  Id compartmentId = 0;
  std::string name;
  bool infiniteVolume = true; // a fixed pressure: the room, or the blower
  double volumeM3 = 0.0;
  double defaultPressureInches = 0.0;
  // A reservoir with a weighted board on it. The board's weight is what sets
  // the regulated pressure, its mass is why the pressure overshoots when a
  // chord lands, and the damping is why it settles.
  bool hasBellows = false;
  double bellowsMassKg = 0.0;
  double bellowsDamping = 0.0;
  // The board's frame and how far it travels. Width x length x extension is
  // the volume the reservoir can swallow before its pressure moves at all,
  // and that is the whole reason an organ has one.
  double bellowsWidthM = 0.0, bellowsLengthM = 0.0, bellowsExtensionM = 0.0;
  double sweptVolumeM3() const {
    return bellowsWidthM * bellowsLengthM * bellowsExtensionM;
  }
  // Reports its own pressure to a continuous control, which is how an organ
  // drives a wind gauge on its console.
  Id pressureOutputControlId = 0;
};

// A pipe between two compartments: the blower feeding a reservoir, a reservoir
// feeding a chest. May be valved, which is how an organ's wind is switched on.
struct WindCompartmentLink {
  Id firstCompartmentId = 0;
  Id secondCompartmentId = 0;
  std::string name;
  Id valveSwitchId = 0;
  bool valveOpenWhenEngaged = true;
  // A valve can be driven by a continuous control instead of a switch, and on
  // a real organ most are: that is how a reservoir regulates itself, its own
  // board extension closing the intake as it fills. Nancy drives all fifteen
  // of her wind valves this way.
  Id valveControlId = 0;
  int valveControlTypeCode = 0;
  // Flow through it at the reference pressure difference. Orifice flow goes
  // as the square root of the difference, so this one number scales the whole
  // curve.
  double massFlowKgPerSec = 0.0;
  double refPressureInches = 1.0;
};

struct ImageSetElement {
  int index = 0; // ImageIndexWithinSet (1-based frame selector)
  std::string name;
  std::string bitmapFile;
};

struct ImageSet {
  Id imageSetId = 0;
  std::string name;
  Id packageId = 0;
  // Declared size. Routinely ABSENT, in which case the bitmap decides — key
  // shapes and drawstops both leave it unstated. Treating a missing size as a
  // default box makes a drawstop clickable only in its top-left corner.
  int widthPx = 0, heightPx = 0;
  // The part of the image that responds to a click, relative to its top-left.
  // A drawstop's knob is not its whole rectangle. -1 means unstated, and then
  // the whole image is live.
  int clickLeftPx = -1, clickRightPx = -1, clickTopPx = -1, clickBottomPx = -1;
  std::string transparencyMaskFile;
  std::vector<ImageSetElement> elements;

  bool hasClickArea() const {
    return clickRightPx > clickLeftPx && clickBottomPx > clickTopPx;
  }
};

struct ImageSetInstance {
  Id instanceId = 0;
  std::string name;
  Id imageSetId = 0;
  int defaultImageIndex = 0;
  int layer = 0; // ScreenLayerNumber (paint order)
  int leftPx = 0, topPx = 0;
  // A backdrop is authored as one tile plus the rectangle it repeats over
  // (RightXPosPixelsIfTiling / BottomYPosPixelsIfTiling). Drawing it once
  // leaves a narrow strip of artwork and bare background everywhere else.
  // -1 means "not tiled": draw the image at its own size.
  int tileRightPx = -1, tileBottomPx = -1;
  bool tiles() const { return tileRightPx > leftPx && tileBottomPx > topPx; }

  // Alternate screen layouts. A set that ships for more than one console size
  // gives each drawn thing up to three more positions — and often a different
  // image set as well, because a stop knob drawn for a 1920-wide console is
  // not the one drawn for a 1024-wide one.
  //
  // Layout 0 is the fields above. A layout that names no image set is not
  // offered at all, which is how a set says "I only have one".
  struct Alternate {
    Id imageSetId = 0;
    int leftPx = 0, topPx = 0;
    int tileRightPx = -1, tileBottomPx = -1;
    bool tiles() const { return tileRightPx > leftPx && tileBottomPx > topPx; }
  };
  Alternate alternates[3];

  bool hasLayout(int layout) const {
    if (layout <= 0) return true;
    return layout <= 3 && alternates[layout - 1].imageSetId != 0;
  }
  // The image set, position and tiling for a layout, falling back to the
  // primary one. Callers ask for what they want and get something drawable.
  Id imageSetFor(int layout) const {
    return hasLayout(layout) && layout > 0 ? alternates[layout - 1].imageSetId
                                           : imageSetId;
  }
  int leftFor(int layout) const {
    return hasLayout(layout) && layout > 0 ? alternates[layout - 1].leftPx
                                           : leftPx;
  }
  int topFor(int layout) const {
    return hasLayout(layout) && layout > 0 ? alternates[layout - 1].topPx
                                           : topPx;
  }
  int tileRightFor(int layout) const {
    return hasLayout(layout) && layout > 0 ? alternates[layout - 1].tileRightPx
                                           : tileRightPx;
  }
  int tileBottomFor(int layout) const {
    return hasLayout(layout) && layout > 0 ? alternates[layout - 1].tileBottomPx
                                           : tileBottomPx;
  }
  bool tilesFor(int layout) const {
    return tileRightFor(layout) > leftFor(layout) &&
           tileBottomFor(layout) > topFor(layout);
  }
};

// How a piece of console text is drawn. A set that engraves its stop names
// rather than painting them into the artwork needs all of this, or its
// drawstops come out blank.
struct TextStyle {
  Id styleId = 0;
  std::string name;
  std::string faceWindows, faceMac, faceLinux;
  int sizePx = 10;
  int weightCode = 2; // 1 light, 2 normal, 3 bold
  bool italic = false, underline = false;
  int red = 0, green = 0, blue = 0;
  // 0 or 3 centre, 1 left, 2 right. The position is the anchor, not the
  // corner: centred text sits astride its XPosPixels.
  int hAlignCode = 0;
  // 0 centre, 1 top, 2 bottom.
  int vAlignCode = 1;
};

struct TextInstance {
  Id textInstanceId = 0;
  std::string name;
  std::string text;
  Id styleId = 0;
  int xPx = 0, yPx = 0;
  // Word wrap happens inside this box when the set declares one.
  int boxWidthPx = 0, boxHeightPx = 0;
  // Text can be tied to a drawn thing, and then either carry its own absolute
  // position or one measured from that thing's top-left corner. A stop label
  // is written the second way, so it travels with its knob when the set is
  // drawn at another console size.
  Id attachedInstanceId = 0;
  bool posRelativeToInstance = false;
};

struct DisplayPage {
  Id pageId = 0;
  std::string name;
  std::vector<ImageSetInstance> instances;
  std::vector<TextInstance> texts;
};

// An organ's tuning, as declared by the ODF. Either the file supplies the
// twelve offsets outright, or it names one and we resolve it against the
// built-in library (see Temperament.h). A name we cannot resolve is reported,
// never silently replaced with equal temperament — that would retune the organ
// behind the player's back.
struct OrganTemperament {
  Id temperamentId = 0;
  std::string name;
  std::vector<double> centsOffset12; // empty until resolved
  bool resolved = false;             // false -> temperament-unknown-code
};

struct OrganModel {
  OdfType odfType = OdfType::Unknown;
  std::string organName;
  std::string church, builder;
  Id uniqueOrganId = 0;
  std::string organVersion;
  double basePitchHz = 440.0;
  // AudioOut_AmplitudeLevelAdjustDecibels on _General: the producer's
  // output trim, applied to everything the organ makes so sets recorded at
  // different levels play at a comparable loudness.
  double audioOutputTrimDb = 0.0;

  std::unordered_map<Id, Rank> ranks;
  std::unordered_map<Id, Stop> stops;
  std::unordered_map<Id, Switch> switches;
  // Ordered, because solving the network applies them in order and the answer
  // must not depend on a hash table's layout.
  std::vector<SwitchLinkage> switchLinkages;
  std::unordered_map<Id, Division> divisions;
  std::unordered_map<Id, Keyboard> keyboards; // M1.3: manuals/pedal + division links
  std::vector<KeyAction> keyActions; // M1.3: global key-flow list (matrix built in M3)
  // Switch id -> the key it is. Keyed by switch because that is what a console
  // click arrives as.
  std::unordered_map<Id, KeyboardKeyRef> keyboardKeys;
  std::unordered_map<Id, TextStyle> textStyles;
  std::unordered_map<Id, Combination> combinations;
  std::unordered_map<Id, Enclosure> enclosures;
  std::unordered_map<Id, Tremulant> tremulants;
  // Pipe -> what its tremulant does to it. Empty on an organ whose tremulants
  // are declared but never wired to pipework, which is a real and common case.
  std::unordered_map<Id, TremulantPipeMod> tremulantPipes;
  std::unordered_map<Id, WindCompartment> wind;
  std::vector<WindCompartmentLink> windLinks;
  // M2.3: which box encloses each pipe, from the EnclosurePipe rows. Kept as a
  // flat pipe->enclosure map rather than a list per enclosure because the
  // lookup that matters happens at note-on, once per sounding pipe.
  std::unordered_map<Id, Id> pipeEnclosure;
  std::unordered_map<Id, OrganTemperament> temperaments; // M2.2
  Id defaultTemperamentId = 0;                           // 0 = equal
  std::unordered_map<Id, ContinuousControl> continuousControls; // M2.4
  // Image set id -> its value/frame staircase, sorted ascending by value.
  std::unordered_map<Id, std::vector<ContinuousControlImageStage>>
      continuousControlStages;
  std::vector<ContinuousControlLinkage> controlLinkages;        // M2.4
  std::vector<ContinuousControlDoubleLinkage> controlDoubleLinkages;
  // M3: shoe positions that move switches. Ordered, because a sweep fires the
  // thresholds it crosses in the order it crosses them, and the last one to
  // fire decides the registration.
  std::vector<ContinuousControlStageSwitch> controlStageSwitches;
  std::unordered_map<Id, DisplayPage> displayPages; // M1.4: console pages
  std::unordered_map<Id, ImageSet> imageSets; // M1.4: shared control artwork
  std::unordered_map<Id, KeyImageSet> keyImageSets; // drawn manuals

  std::unordered_map<Id, SampleRef> samples; // Sample table registry (M1.1/M1.2)
  std::unordered_map<Id, InstallationPackage> packages; // where the audio lives

  // Unknown tables/attributes preserved for forward-compat (never hard-fail).
  std::vector<std::string> warnings;
  std::vector<std::string> unknownTables;

  bool hasEncryptedSamples = false;
  std::vector<std::string> encryptedFiles;
};

} // namespace mp
