// Masterpiece audio processor — JUCE AudioProcessor + APVTS.
// Standalone + VST3/AU/LV2 from ONE codebase (no Projucer, CMake only).
// ODF parse (background) -> immutable OrganModel -> AudioGraph (bus list, rank->bus, IR sends).
// User data (combinations/voicing/MIDI/favourites) as ValueTree + separate files per organ + global.
#pragma once
#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../mp_core/OrganModel.h"
#include "../mp_control/Control.h"
#include "../mp_control/MidiMap.h"
#include "../mp_control/Combinations.h"
#include "../mp_control/StageSwitches.h"
#include "../mp_control/PlayerCombinations.h"
#include "../mp_control/PedalCoupling.h"
#include "../mp_control/LcdPanel.h"
#include "../mp_control/WindSolver.h"
#include "../mp_control/SwitchNetwork.h"
#include "../mp_core/Temperament.h"
#include "../mp_core/Perspectives.h"
#if MP_ENABLE_DSP
#include "../mp_dsp/Dsp.h"
#endif

#include "Convolver.h"
#include "Metronome.h"
#include "OutputLimiter.h"
#include "BassManager.h"
#include "AudioRecorder.h"
#include "MidiArchive.h"
#include "MidiRecorder.h"
#include "ScorePlayer.h"
#include "SplitKeyboardHost.h"
#include "SampleLibrary.h"
#include "MixerConfig.h"
#include "VoicingSet.h"
#include "RankEq.h"
#include "LiveLevels.h"
#include "VoicingEq.h"
#include "VoicingEqPresets.h"
#include "Favourites.h"
#include "../mp_core/OdfLoader.h"
#include "../mp_sampler/StreamingEngine.h" // ParallelConfig
#include "../mp_sampler/VoiceEngine.h"

#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace mp {

struct AudioGraphConfig {
  EngineSwitch engineSwitch; // runtime DSP toggle (ADR-005)
  SampleLoadMode loadMode = SampleLoadMode::Auto;
  int numBuses = 2; // simple routing default; full mixer expands (M4)
  // Voice ceiling. The perf budget is written against 500 (laptop) to 2000
  // (console); the pool is allocated once at prepareToPlay.
  int maxVoices = 1536;
  // Voice-render threading (ADR-012). renderThreads 0 = auto (cores - 1).
  ParallelConfig parallel;
};

class MasterpieceProcessor : public juce::AudioProcessor {
public:
  // The playing state -- switches, controls, voices, pistons -- belongs to
  // the audio thread, which works on it inside processBlock. Everything that
  // changes it from elsewhere (the window, a MIDI-learn panel, a script)
  // takes the lock JUCE already holds around every audio block, so the two
  // never run at once: the window moving a shoe while a note engaged a
  // switch crashed on a 12-core Mac (#120). Re-entrant, so a method that
  // holds it may call another that takes it, and the audio thread may call
  // these from inside its own block. The window waits at most one block.
  struct AudioLock {
    explicit AudioLock(MasterpieceProcessor& p) : owner(p), lock(p.getCallbackLock()) {}
    // What was changed under the lock is handed to the window as the lock is
    // let go: see publishShown().
    ~AudioLock() { owner.publishShown(); }
    MasterpieceProcessor& owner;
    const juce::ScopedLock lock;
  };

  MasterpieceProcessor();
  ~MasterpieceProcessor() override {
    // A load on another thread reads and writes this object to the end: it
    // stops before anything here goes.
    stopLoad(15000);
    serviceArchive(true);  // quitting saves the archive's take in progress
    alive_->store(false);
    writeBlockLog();
  }

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  // The device is stopping, most often to change its buffer size: the engine
  // comes to rest before prepareToPlay rebuilds it. Left running, the workers,
  // the streaming thread and hundreds of sounding voices met the rebuild, and
  // on a loaded Linux machine the heap was corrupted (#267).
  void releaseResources() override;
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  // Any number of discrete outputs up to 32, and no input: a mixer pair with
  // device channels of its own plays on them when the output has them.
  static constexpr int kMaxOutputChannels = 32;
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  // The output channels the mixer asks for: one past the highest device
  // channel a bus names, and two at the least.
  int outputChannelsWanted() const;
  // A pair's bass management: the crossover frequency (40-200 Hz, 0 for
  // off) and the device channel of the subwoofer, counted from 0. Kept with
  // the pairs, in the organ's settings and in the saved defaults.
  void setBusBass(int busId, float crossoverHz, int subChannel);
  // Told on the message thread's side when that number changes, so the
  // program that owns the device can open the channels.
  std::function<void()> onOutputChannelsWanted;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return "Masterpiece"; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override { return 5.0; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return "Default"; }
  void changeProgramName(int, const juce::String&) override {}
  // A plugin's project keeps the organ it played, and reopens it: through
  // the editor's load, with its dialog, while the plugin's window is open
  // (editorLoader), or on the message thread before the window exists.
  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;
  // Set by the plugin's editor. False when the editor is gone.
  std::function<bool(const juce::File&)> editorLoader;

  // Organ loading. `loadOrgan` is synchronous and is what the headless
  // renderer and the tests use; `loadOrganAsync` runs the same work off the
  // message thread so the UI stays responsive on a 19 GB set.
  struct LoadResult {
    bool ok = false;
    std::string error;
    OdfDiagnostics diagnostics;
    SampleLoadReport samples;
    int stopsEngaged = 0;
    // Stopped at the memory limit rather than failing on its own.
    bool outOfMemory = false;
    // Stopped at the memory limit, and kept: what was read plays, the rest
    // of the organ is silent. samples.loaded of samples.wanted arrived.
    bool incomplete = false;
    // The licence was asked about during this load (licenceAsker), so the
    // caller need not ask again whatever the answer was.
    bool licenceAsked = false;
  };
  // Asked on the loading thread once the definition is read and before a
  // sample is, when the set needs a licence not yet confirmed: whether the
  // player holds it. Blocks until answered. Unset -- the renderer, the tests
  // -- the licensed samples are left out, as before (#164: asking after the
  // load and then loading again read the organ twice).
  std::function<bool(const std::string& organ, const std::string& publisher)> licenceAsker;
  // Asked on the loading thread for a package a composite plays that is not
  // in the organ's folder, the folders in it or beside it (#289, #522): the
  // player may point to its archive, or the folder that holds it, and the
  // choice is kept for the organ. `start` is where a chooser opens: the folder
  // last picked for this organ, or the one that holds the organ's. Returns the
  // path, or nothing to play without it. Blocks until answered. Unset -- the
  // renderer, the tests -- nothing is asked.
  std::function<std::string(const std::string& package, const std::string& name, const std::string& start)>
      packageAsker;
  // `graphicsOnly` builds the whole model and console and reads not one byte
  // of audio: the artwork, the jamb, the drawn manuals and the switch network
  // all come from the ODF, and only `loadAll` touches the sample tree. It is
  // what lets a console be drawn — or screenshotted, or smoke-tested in CI —
  // in a second or two on a machine that has no sample set at all. The organ
  // is silent by construction; nothing is stubbed to make it so.
  LoadResult loadOrgan(const juce::File& odfFile,
                       int64_t maxFramesPerSample = 0,
                       bool graphicsOnly = false);
  void loadOrganAsync(const juce::File& odfFile);

  // An organ still in its RAR packages. Indexes them and unpacks the small
  // files -- definitions, artwork -- into a folder of its own, once; later
  // calls reuse it. Returns the organ definitions found there. Loading one of
  // them is an ordinary load whose samples come out of the archives.
  // --- versions of one organ -------------------------------------------
  // Producers ship one organ as several definitions: a perspective each
  // (Lemmer near, far, surround), or full and light. These are the
  // definitions beside this one -- in its package, or in its folder.
  static juce::Array<juce::File> organVersions(const juce::File& odf);
  // A package remembers which of its definitions was opened last, in the
  // folder Masterpiece unpacked it into -- never in an installed sample set,
  // which is not ours to write to. None: an empty File.
  static void rememberDefinition(const juce::File& odf);
  static juce::File rememberedDefinition(const juce::Array<juce::File>& definitions);

  juce::Array<juce::File> openPackagedOrgan(const juce::File& archive, juce::String& error);
  // Called on the message thread before showing the archive dialog, so a
  // Cancel click cannot be cleared later by the worker starting up.
  void beginPackageLoad() {
    loadProgress_.cancelled.store(false, std::memory_order_release);
    loadProgress_.beginPhase(LoadProgress::Phase::OpeningArchive);
  }

  // Load only the ranks these stops need, on the NEXT load.
  //
  // Not a lighter organ, an incomplete one: every stop outside the list is
  // silent afterwards and nothing will warn you. It is here because loading is
  // what makes trying anything slow -- minutes on a large set -- and a test
  // that draws four stops should not read four hundred ranks. Empty (the
  // default) loads the whole instrument.
  void setPreloadStops(std::vector<Id> stops) { preloadStops_ = std::move(stops); }
  // Load exactly these ranks and no others. For testing one rank of a large
  // set in seconds; overrides setPreloadStops.
  void setPreloadRanks(std::vector<Id> ranks) { preloadRanks_ = std::move(ranks); }

  // Where a load has got to, and how to stop it. The progress object is read
  // by a UI timer while the loader's worker threads write it, which is why
  // everything in it is atomic.
  const LoadProgress& loadProgress() const { return loadProgress_; }
  // True while loadOrgan() runs, on whatever thread; the model is the
  // loader's then. organLoads() moves on each time one ends.
  bool organLoading() const { return organLoading_.load(std::memory_order_acquire); }
  int organLoads() const { return organLoads_.load(std::memory_order_acquire); }
  // Late blocks since the program started, without taking the worst block
  // as takeAudioLoad() does.
  int64_t lateBlocksTotal() const { return lateBlocks_.load(std::memory_order_relaxed); }
  int soundingVoices() const { return soundingVoices_.load(std::memory_order_relaxed); }
  // A stop held off because a demo ships no pipework for it. Set at load.
  bool stopLocked(Id stopId) const { return lockedStops_.count(stopId) > 0; }
  // Ask the running load to stop. Returns immediately; the load ends at the
  // next file boundary and reports itself cancelled.
  void cancelLoad() {
    loadProgress_.cancelled.store(true, std::memory_order_release);
    juce::Logger::writeToLog("load: cancel requested");
  }
  // Cancels a load running on another thread and waits for it to end, for
  // the program quitting in the middle of one. False if it still ran after
  // `timeoutMs`.
  bool stopLoad(int timeoutMs);
  const OrganModel& organModel() const { return model_; }
  const SampleLibrary& sampleLibrary() const { return samples_; }

  // The on-screen keyboard plays through the same path as a MIDI device: its
  // events are merged into the incoming buffer at the top of processBlock, so
  // there is exactly one note path rather than a second one for the mouse.
  // Device input is mirrored back into this state (display only, never
  // re-injected), so the drawn manuals and the piano strip light up for an
  // external console too.
  juce::MidiKeyboardState& keyboardState() { return keyboardState_; }
  // Whether this key of this manual is down: played on it by a keyboard bound
  // to it, by its channel, or on the screen. Any thread.
  bool keyDownOnKeyboard(Id keyboard, int note) const {
    if (note < 0 || note > 127) return false;
    const auto slot = (static_cast<size_t>(keyboard) & 63u) * 2u + static_cast<size_t>(note >> 6);
    return (keysDown_[slot].load(std::memory_order_relaxed) >> (note & 63)) & 1u;
  }

  // Let go of every key on every channel, the way a console's cancel does.
  //
  // A piece can stop with notes still down -- a file that ends on a held
  // chord, or playback halted part-way -- and an organ pipe has no decay to
  // hide it: it simply keeps speaking. That is a cipher, and it sounds
  // through the silence and into whatever plays next.
  //
  // Asked for here, done on the audio thread: the MIDI path must not be fed
  // from the message thread, so this raises a flag that the next block acts
  // on.
  void releaseAllKeys() {
    releaseAll_.store(true, std::memory_order_release);
    split_.panic();
    // A score being played would put the next chord straight back down.
    scorePlayer_.stop();
  }

  // Stops in a stable order for the console: by division, then by id, so the
  // jamb does not reshuffle between loads.
  // control: a coupler or a tremulant listed with the stops. It plays, and
  // holds no samples of its own to leave out of a load.
  struct StopEntry { Id stopId = 0; Id divisionId = 0; std::string name; bool playable = false; bool control = false; };
  std::vector<StopEntry> stopList() const;
  bool stopEngaged(Id stopId) const { return engagedStops_.count(stopId) != 0; }
  // The same two answers for the window, which asks from its own thread while
  // the audio thread plays: stopEngaged() and switchEngaged() read the sets
  // the audio thread rewrites when a stop or a piston arrives over MIDI, and
  // a set read while it is rewritten is a crash waiting for its moment. These
  // read a copy made for the window each time the sets change.
  bool shownStopEngaged(Id stopId) const {
    const std::lock_guard<std::mutex> lock(shownMutex_);
    return shownStops_.count(stopId) != 0;
  }
  bool shownSwitchEngaged(Id switchId) const {
    const std::lock_guard<std::mutex> lock(shownMutex_);
    return shownSwitches_.count(switchId) != 0;
  }

  // Draw every stop the organ has. Blunt, but it is what a "does this make
  // sound at all" check wants, and it is how the renderer gets a full organ.
  int engageAllStops();

  juce::AudioProcessorValueTreeState& apvts() { return apvts_; }

  // The organ's own level for a layer, as a linear gain factor.
  //
  // This is also how a set switches between its tremulant and non-tremulant
  // recordings. Every pipe of a tremmed set carries two layers, and the organ
  // mutes one of them: Nancy silences 488 that way, Azzio 537 of 996. A set
  // that ignored this would sound both variants at once.
  //
  // Not every linkage that feeds these controls is understood yet —
  // BinaryOperationCode 7 is rejected by the loader, InvertSourceControlValue
  // is unparsed, LinkTypeCode uninterpreted — but the crossfade itself is
  // built from plain conditional linkages, which are. mp-render --levels
  // reports what each control resolves to, so the remaining gaps stay
  // measurable.
  //
  // Read at voice start, never per sample: a level is a control-rate thing
  // and a note already sounding keeps the gain it began with, exactly as a
  // pipe does when someone moves a fader.
  float layerLevel(const PipeLayer& layer) const {
    if (layer.ampScalingControlId == 0) return 1.0f;
    if (model_.continuousControls.count(layer.ampScalingControlId) == 0)
      return 1.0f;
    return static_cast<float>(controls_.normalised(layer.ampScalingControlId));
  }

  // The same answers the voice engine uses, for tools that want to report on
  // them without starting a note. Shared rather than reimplemented: a
  // diagnostic that computes the figure its own way will eventually disagree
  // with the engine, and then it is worse than having none.
  float layerLevelFor(const PipeLayer& layer) const { return layerLevel(layer); }

  // The organ's own output trim as a linear gain — the producer's calibration
  // so two sets recorded at different levels play at a comparable loudness.
  float organTrimGain() const { return organTrimGain_; }
  // Ignore that calibration. For measuring what it actually does: rendering
  // the same organ with and without it is the only way to show the figure
  // reaches the audio rather than merely being parsed. Set before loading.
  void setOrganTrimEnabled(bool on) { applyOrganTrim_ = on; }
  double detuneOffsetHzFor(const PipeLayer& layer, double targetHz) const {
    if (layer.pitchControlId == 0) return 0.0;
    return detunedTargetHz(targetHz, detuneControlValue(layer),
                           detuneCentre(layer),
                           layer.pitchSensitivityHzPerUnit) -
           targetHz;
  }

  // A console action a mapped piston asked for, or None if none is waiting.
  // Reading takes it: the editor collects on its timer and acts on the
  // message thread, which is the only thread allowed to touch a component.
  MidiTargetKind takeConsoleAction() {
    return static_cast<MidiTargetKind>(
        pendingConsoleAction_.exchange(0, std::memory_order_acq_rel));
  }
  // A recorder button pressed on the console: 1 Record, 2 Play, 0 none.
  // Reading takes it. The audio thread only raises it; the recorder is
  // started and stopped on the message thread (pressRecorderButton).
  int takeRecorderRequest() { return pendingRecorder_.exchange(0, std::memory_order_acq_rel); }
  // What a console's recorder button does, on the message thread. Record
  // stops a take being made; otherwise a take that was never saved is saved
  // into `folder` first -- a button cannot answer the question the window
  // asks -- and a new take begins. Play plays the take, or stops it. Returns
  // the file the take was saved to, or an empty file.
  juce::File pressRecorderButton(bool record, const juce::File& folder);
  // Where a console's Record button keeps the takes it saves:
  // Documents/Masterpiece/Recordings.
  static juce::File recordingsFolder();
  // The last take a console button saved, for the Recorder window.
  juce::File lastButtonSave() const { return lastButtonSave_; }
  // The take as a file for other programs, a sequencer or a notation
  // program: one track per keyboard, the console's own messages left out,
  // the notes in pairs, and the registration as markers when `markers`.
  // The plain save is the recorder's own and stays as it is.
  bool saveTakeForSequencer(const juce::File& file, bool markers) const;

  // Output level, for a meter. The audio thread does the decay itself and
  // publishes a single value per channel: a reader that polled more slowly
  // than the blocks arrive would otherwise miss every peak between looks.
  // Post-fader and post-metronome — what the meter shows is what leaves.
  float outputPeak(int channel) const {
    return outPeak_[channel & 1].load(std::memory_order_relaxed);
  }

  // Console input: move a swell shoe / crescendo wheel. Safe to call from the
  // message thread; the audio thread reads the resulting positions per block.
  void setContinuousControl(Id controlId, int value);
  // The control a PLAYER moves to set this one: follow the unconditional
  // linkages that feed it upstream to one nothing feeds. An enclosure's
  // shutters are often driven from its pedal, and the pedal from the shoe
  // drawn on the console; setting the shutters directly is undone by the
  // next propagate, and leaves both drawn shoes where they were.
  Id playerControlFor(Id controlId) const;

  // Draw or retire a stop. Which stops are engaged decides which pipes a key
  // press sounds, so this is the other half of the console.
  void setStopEngaged(Id stopId, bool engaged);

  // Flip a switch (drawstop, coupler, tremulant, blower). Switches with noise
  // ranks attached fire them: that mechanical sound is most of what makes a
  // sampled organ feel like an instrument rather than a synthesiser.
  void setSwitchEngaged(Id switchId, bool engaged);
  // Switches latch, so there is one state and this is it. Moving one travels
  // through the organ's own wiring; see SwitchNetwork.
  bool switchEngaged(Id switchId) const;
  const SwitchNetwork& switchNetwork() const { return switches_; }
  // The switch a PLAYER would move to change this one: the drawn knob at the
  // top of the wiring, or the switch itself when nothing feeds it. Engaging an
  // internal node directly leaves the knob out and the organ inconsistent —
  // and a general cancel, which works by turning the logical switch off, then
  // has nothing to push back.
  Id playerSwitchFor(Id switchId) const;

  // --- pistons ----------------------------------------------------------
  // Combinations are the player's own, not the organ builder's, so they are
  // saved beside the MIDI map in the user's data and never written back into
  // the sample set.
  CombinationSystem& combinations() { return combinations_; }
  const CombinationSystem& combinations() const { return combinations_; }
  juce::File combinationFileFor(const juce::File& odf) const;

  // --- combination sets -------------------------------------------------
  // Several named registrations for one organ: a set for a recital, another
  // for a service, a third someone else left. One file each, so copying a set
  // between organs is copying a file and deleting one cannot corrupt another.
  //
  // The empty name is the default set, and is what every organ has had until
  // now — so an organ with no sets keeps working and its file keeps its name.
  const std::string& combinationSetName() const { return combinationSet_; }
  // Saves the set now live before switching, then loads the new one. Returns
  // false if the switch happened but nothing was there to load.
  bool switchCombinationSet(const std::string& name);
  // The sets this organ already has, by name, in order. The default set is
  // reported as an empty string.
  std::vector<std::string> combinationSets() const;
  // Write the live registrations out under another name, leaving the current
  // set alone. How "save a copy before I change everything" is spelled.
  bool copyCombinationSetTo(const std::string& name) const;
  bool deleteCombinationSet(const std::string& name) const;
  bool saveCombinations() const;
  bool loadCombinations();
  // Capture can happen on the audio thread — a piston is a MIDI message like
  // any other — and the audio thread must not write a file. So a capture only
  // raises a flag, and the message thread does the writing.
  bool combinationsNeedSaving() const {
    return combinationsDirty_.load(std::memory_order_acquire);
  }
  // Writes the file if anything was captured since the last call. Message
  // thread only.
  bool saveCombinationsIfDirty();
  // Setter mode: with this on, pressing a piston stores what is drawn instead
  // of recalling what was stored. An organ that has a setter switch of its own
  // drives this from the console; this is for one that has not, and for a UI
  // button.
  void setCaptureMode(bool on) {
    // The organ's own Set follows the player's (#248): its switch puts the
    // combinations into capture, so the console lights as the window does.
    if (setterSwitchId_ != 0 && switchEngaged(setterSwitchId_) != on) {
      setSwitchEngaged(setterSwitchId_, on);
      return;
    }
    const AudioLock audio(*this);
    combinations_.setCaptureMode(on);
  }

  // --- tuning: the player's, over the organ's own -----------------------
  // Temperament, pitch and transposer, per organ. Each applies to the notes
  // played after it changes: a held chord keeps the pitch it started with,
  // as it would on a real organ being retuned under the player's hands.
  //
  // The temperament: empty for the organ's own, a name from
  // temperamentLibrary(), or "scala:<path>" for a Scala file.
  bool setTemperament(const std::string& choice, std::string* error = nullptr,
                      bool remember = true);
  const std::string& temperamentChoice() const { return temperamentChoice_; }
  // The name of the temperament that sounds, for display.
  std::string temperamentName() const;
  // The next or previous in the list: the organ's own, then the library.
  void stepTemperament(int direction);
  // A in Hz; 0 is the organ's own pitch.
  void setMasterPitchHz(double hz);
  double masterPitchSetting() const { return masterPitchHz_.load(std::memory_order_relaxed); }
  // What A sounds at now, and what it would be at the organ's own pitch.
  double masterPitchHz() const;
  double nativePitchHz() const { return model_.basePitchHz > 0.0 ? model_.basePitchHz : 440.0; }
  // Keys, not samples: +2 plays the pipes two keys up. Clamped to an octave
  // either way.
  void setTranspose(int semitones);
  int transpose() const { return transpose_.load(std::memory_order_relaxed); }

  // Unevenness, saved for the organ and off by default. Pipe detune puts each
  // pipe up to this many cents off true, the same pipe by the same amount at
  // every note: heard from the next note, nothing done while it is 0. Wind
  // sway moves the pitch of each division slowly by up to this many cents,
  // as an organ's wind does: heard on the pipes started while it is on,
  // nothing done while it is 0. 0 to 10 cents each.
  static constexpr float kMaxUnevennessCents = 10.0f;
  void setPipeDetuneCents(float cents);
  float pipeDetuneCents() const { return pipeDetuneCents_.load(std::memory_order_relaxed); }
  void setWindSwayCents(float cents);
  float windSwayCents() const { return windSwayCents_.load(std::memory_order_relaxed); }
  // How far, -1 to 1, a pipe stands off true under pipe detune.
  static double pipeDetuneOffset(Id pipeId);
  bool captureMode() const { return combinations_.captureMode(); }

  // --- the crescendo, and everything else a shoe position moves -----------
  // Move a continuous control and let it fire whatever thresholds it crosses.
  // This is the crescendo: one control, a step per row, each firing a
  // registration. Use it rather than setting the control directly whenever the
  // move comes from a player.
  void setControlValue(Id controlId, int value);
  const StageSwitchBank& stageSwitches() const { return stages_; }
  // The wind. Read-only from outside: what the pressure is doing is a result,
  // not a setting, and the only control over it is EngineSwitch::enableWindModel.
  const WindSolver& wind() const { return wind_; }
  // How far the wind model's deviation from nominal is scaled. 1 = the
  // physics as solved; higher exaggerates it so it can be HEARD and judged.
  // Not a tone control: leaving it above 1 makes the organ lie about its own
  // wind system.
  void setWindDepth(double d) { windDepth_ = d < 0.0 ? 0.0 : d; }
  double windDepth() const { return windDepth_; }

  // --- the player's own pistons ----------------------------------------
  // Generals, divisionals, cancels and a stepper on every organ, whatever its
  // file declares (see PlayerCombinations). The organ's own pistons keep
  // working beside them. Both answer the same setter: hold it, press a piston,
  // and the piston stores what is drawn.
  const PlayerCombinations& playerCombinations() const { return player_; }
  // The piston or sequencer step the player chose last, whatever has been
  // drawn since: where a momentary Set stores (#276). Choose General 3, draw
  // the registration, press Set.
  struct StoreTarget {
    enum class Kind { None, General, Divisional, Step };
    Kind kind = Kind::None;
    Id divisionId = 0;
    int n = 0;
  };
  StoreTarget storeTarget() const;
  // "General 3", "Pedal divisional 2", "Step 12"; empty when none is chosen.
  std::string storeTargetName() const;
  // Store what is drawn into it. False when nothing has been chosen yet.
  bool storeIntoTarget();
  void pressGeneral(int n);
  void pressGeneralCancel();
  void pressDivisional(Id divisionId, int n);
  void pressDivisionalCancel(Id divisionId);
  // "Pedal combinations coupled" (PedalCoupling.h): while on, the coupled
  // manual's divisionals recall, store and cancel the Pedal's of the same
  // number with their own. The program's, kept for every organ; off by
  // default. The two divisions it ties are worked out at each load; both 0
  // when the organ has no pedal or no manual with divisionals.
  bool pedalCombinationsCoupled() const { return pedalCoupled_.load(); }
  void setPedalCombinationsCoupled(bool on) {
    pedalCoupled_.store(on);
    writeGlobalFile();
  }
  CoupledDivisions coupledDivisions() const { return coupledDivisions_; }
  // A change made on the audio thread (a MIDI button) raises a flag; the
  // window's save tick writes the global file.
  bool saveGlobalIfDirty() {
    if (!globalDirty_.exchange(false, std::memory_order_acq_rel)) return false;
    writeGlobalFile();
    return true;
  }
  // How many generals, and divisionals per division, the window shows.
  // Saved with the combination set.
  void setPlayerPistonCounts(int generals, int divisionals);

  // --- the registration sequencer ---------------------------------------
  // A long list of frames of its own -- a recital's worth -- walked by two
  // thumb pistons. Not in the organ file; the player maps it.
  //
  // With the setter held, stepping CAPTURES into the frame it lands on, which
  // is how a registration is built for a piece: hold the setter and walk
  // forwards, setting each frame as you go.
  bool stepperNext();
  bool stepperPrev();
  bool stepperGoto(int frame);
  // The longer moves (#298). First and last are the ends of the frames that
  // hold anything. A jump of several frames stops at those ends, as stepping
  // does; with the setter on it may run past the last one, as stepping may.
  // False when there is nowhere to go.
  bool stepperFirst();
  bool stepperLast();
  bool stepperJump(int frames);
  // A digit button: 1 to 9 go to that frame, and 0 to the tenth. Like the
  // frame prompt, it stops at the last frame that holds anything unless
  // capture is on. False when there is nowhere to go.
  bool stepperDigit(int digit);
  // Open a frame before the current one, or take the current one out; the
  // frames after it move along.
  bool stepperInsertFrame();
  bool stepperDeleteFrame();
  void stepperRewind() { player_.rewind(); }
  int continuousControlValue(Id controlId) const {
    return controls_.value(controlId);
  }

  // MIDI mapping. The player's console is not the organ's, so this is edited by
  // learning and saved beside the organ rather than inside it. The audio thread
  // only ever reads it; edits come from the message thread while it is safe to
  // do so (a mapping change between blocks is not worth a lock-free structure).
  // --- keyboards and channels ------------------------------------------
  // The organ's playable keyboards, in a stable order.
  const std::vector<Id>& playableKeyboards() const {
    return couplers_.inputKeyboards();
  }
  std::string keyboardName(Id keyboardId) const {
    return couplers_.keyboardName(keyboardId);
  }
  // The assignment code the key-flow walk RESOLVED for this keyboard, which is
  // not always the one the organ file states: exposed so a diagnostic can tell
  // the two apart.
  int assignmentCodeOf(Id keyboardId) const {
    return couplers_.assignmentCodeFor(keyboardId);
  }
  // Which keyboard a MIDI channel plays. Unset channels follow Hauptwerk's own
  // default assignment (code 1 is the pedal, 2 the first manual, ...), and
  // fall back to the preferred manual — the widest compass when declared,
  // else the unenclosed manual shipping the most pipework — so an unmapped
  // keyboard always speaks.
  // Learn a manual by PLAYING it: press the lowest key you want, then the
  // highest. Two presses give the device, the channel and the range in one
  // go, and the transpose falls out of where the organ's own compass starts —
  // which is the whole of what a player would otherwise type in by hand.
  void beginKeyboardLearn(Id keyboardId);

  // Which of the player's keyboards is meant, found by playing it: the next
  // key pressed on a console (not on screen) is taken, not played, and its
  // channel is kept. How a manual piston is told which keyboard it moves
  // (#90), without a menu of channel numbers.
  void beginKeyPick() {
    pickedChannel_.store(0, std::memory_order_relaxed);
    keyPick_.store(true, std::memory_order_release);
  }
  void cancelKeyPick() { keyPick_.store(false, std::memory_order_release); }
  // The player has set which keyboard plays what, by hand: that is where the
  // keyboards start from now, and where General Cancel puts them back.
  void keepRouting() { pistonRouted_.store(false); }

  // --- the MIDI window ------------------------------------------------------
  // Listen: the next press from a console -- a note, a controller, a program
  // change -- is taken rather than acted on, and kept for the window to fill a
  // row with. GrandOrgue's "Listen for Event".
  void beginMidiListen() {
    heardReady_.store(false, std::memory_order_relaxed);
    midiListen_.store(true, std::memory_order_release);
  }
  void cancelMidiListen() { midiListen_.store(false, std::memory_order_release); }
  bool midiListening() const { return midiListen_.load(std::memory_order_acquire); }
  // The message that answered, once. False while none has.
  bool takeHeardMidi(MidiSource& source, int& value) {
    if (!heardReady_.exchange(false, std::memory_order_acq_rel)) return false;
    source = heard_;
    value = heardValue_;
    return true;
  }
  // Everything one object listens for, sends and answers to on the computer
  // keyboard, replaced at once and saved. Taken under the audio lock: the
  // audio thread reads all three every block.
  void setObjectBindings(MidiTargetKind kind, Id id, const std::vector<MidiBinding>& rows);
  void setObjectSends(MidiTargetKind kind, Id id, const std::vector<MidiSend>& rows);
  void setObjectShortcuts(MidiTargetKind kind, Id id, const std::vector<KeyShortcut>& rows);
  // A computer key pressed on the console. True when a shortcut used it.
  bool applyKeyShortcut(const std::string& key);
  bool keyPicking() const { return keyPick_.load(std::memory_order_acquire); }
  // The channel of the key that answered, or 0 while none has.
  int pickedChannel() const { return pickedChannel_.load(std::memory_order_acquire); }
  void cancelKeyboardLearn() { keyboardLearn_ = 0; }
  Id keyboardLearning() const { return keyboardLearn_; }
  // The lowest key has arrived and the highest is awaited, so the prompt can
  // say which of the two presses it wants next.
  bool keyboardLearnHasLowKey() const {
    return keyboardLearn_ != 0 && keyboardLearnLow_ >= 0;
  }

  Id keyboardForChannel(int channel, int deviceId = 0) const;
  // The keyboard the fallback piano plays, and the one an unassigned channel
  // falls back to. See mp_control::defaultKeyboard.
  Id preferredKeyboard() const { return defaultKeyboard(model_, couplers_); }
  // False when the organ draws no manual key-by-key (its manuals are backdrop
  // photos), and the fallback piano is then the only mouse-playable keys.
  bool hasDrawnManuals() const { return mp::hasDrawnManuals(model_); }
  // Assign a manual to a channel, and optionally to one console. The simple
  // case of the full manual receiver in MidiMap: whole compass, no transpose,
  // full velocity. Anything more is set through midiMap().addKeyboardBinding().
  // `exclusive` decides what happens to the manuals already on that channel.
  // Learning one from a pressed key takes it from them: a player teaching a
  // rig one keyboard at a time means this manual, not both. A channel chosen
  // by hand shares it, which is how one keyboard is made to play two
  // divisions at once.
  // The player's own console, for every organ: which channel the pedal and
  // each manual come in on, by role -- 0 the pedal, 1 the first manual and
  // so on, as each organ numbers its keyboards. An organ with no channels
  // saved for it starts from this; one with its own keeps them. Empty means
  // each organ uses its own default channels.
  const std::map<int, int>& defaultConsole() const { return defaultConsole_; }
  // Takes the loaded organ's current assignment as the default, and saves it.
  void useChannelsAsDefaultConsole();
  void clearDefaultConsole();
  // The role a keyboard plays on a console: 0 the pedal, 1 the first manual,
  // -1 none.
  int consoleRoleOf(Id keyboardId) const;

  void setKeyboardForChannel(int channel, Id keyboardId, int deviceId = 0,
                             bool exclusive = true) {
    midiMap_.removeKeyboardBindingsFor(keyboardId);
    if (exclusive) midiMap_.releaseChannel(channel, deviceId, keyboardId);
    if (keyboardId != 0 && channel > 0) {
      MidiMap::KeyboardBinding b;
      b.channel = channel;
      b.deviceId = deviceId;
      b.keyboardId = keyboardId;
      midiMap_.addKeyboardBinding(b);
    }
    midiMapDirty_.store(true, std::memory_order_release);
  }
  void clearChannelAssignments() { midiMap_.clearKeyboardBindings(); }
  const std::vector<MidiMap::KeyboardBinding>& channelAssignments() const {
    return midiMap_.keyboardBindings();
  }
  // The channel that reaches a given keyboard, for the console's own drawn
  // manuals: clicking a drawn key has to arrive as if played there.
  int channelForKeyboard(Id keyboardId) const;
  // What a DRAWN manual shows and sends, which is stricter than the above.
  // The organ's default channel for a keyboard no longer reaches it once the
  // player gives that channel to another keyboard -- a pedal left on its
  // default 1 after Manual I was mapped to 1 -- and a drawn key must then
  // neither light for notes that play the other manual nor send its clicks
  // to it. 0 means no channel reaches this keyboard.
  int litChannelForKeyboard(Id keyboardId) const;
  int clickChannelForKeyboard(Id keyboardId) const;
  // True when the organ declared no key flow, so every division sounds on
  // every key and couplers do nothing. Worth telling the player.
  bool keyFlowMissing() const { return couplers_.usingFallback(); }

  // --- tagged MIDI input -------------------------------------------------
  // A message together with the console it came from. The host's merged MIDI
  // buffer has no device in it, so anything that needs to tell two keyboards
  // apart has to come in this way instead. Called from each device's own
  // callback thread; allocation-free and lock-free, because a MIDI callback is
  // as real-time as the audio one.
  void pushMidi(int deviceId, const juce::MidiMessage& msg);
  // Report every incoming MIDI message and what became of it, to the log.
  // For diagnosing "my console does not play it": the answer is always one of
  // a handful of things -- the message never arrived, it was swallowed by a
  // mapping, it reached a manual that no stop is drawn on, or it sounded --
  // and none of them can be told apart from outside. Writing to the log from
  // the audio thread is not real-time safe, so this is a diagnostic to turn
  // on deliberately (--log-midi), not something left running.
  void setMidiLogging(bool on) { logMidi_.store(on, std::memory_order_release); }
  bool midiLogging() const { return logMidi_.load(std::memory_order_acquire); }
  int midiMapRepairedOnLoad() const { return midiMapRepaired_; }
  // Register a console and get its id. Message thread, at device setup.
  // The identifier is the one the system gives the device (MidiDeviceInfo),
  // passed only where it lasts across runs (MidiDevices.h).
  int registerMidiDevice(const juce::String& name, const juce::String& identifier = {}) {
    const int id = midiMap_.devices().claim(name.toStdString(), identifier.toStdString());
    // The number the MIDI log calls it by ("midi: in dev=3"), next to its name
    // and the name its mappings know it by (#217).
    juce::Logger::writeToLog("midi: device " + juce::String(id) + " = " + name +
                             (juce::String(midiMap_.devices().nameFor(id)) != name
                                  ? " (mapped as " + juce::String(midiMap_.devices().nameFor(id)) + ")"
                                  : juce::String()) +
                             (identifier.isNotEmpty() ? " [" + identifier + "]" : juce::String()));
    return id;
  }
  const MidiDeviceMap& midiDevices() const { return midiMap_.devices(); }

  // An input the app opens itself, beside the system's own: its JACK MIDI
  // port and a virtual port. The system's inputs are switched on and off in
  // the MIDI settings through the audio device manager, which knows nothing
  // of these, so they are switched here, and remembered with the general
  // settings (#225). Message thread.
  int registerOwnMidiInput(const juce::String& name);
  juce::StringArray ownMidiInputs() const { return ownInputs_; }
  // Whether an input plays, by name: any input, the system's included. The
  // system's are opened by the audio device manager, whose saved state lists
  // only the inputs that are on, so a switched-off one could not be told from
  // a new one and every input came back on at the next start (#230). The
  // inputs switched off are kept here, with the general settings.
  bool midiInputEnabled(const juce::String& name) const { return !inputsOff_.contains(name); }
  void setMidiInputEnabled(const juce::String& name, bool on);
  // Only the inputs switched off, from the general settings: wanted before the
  // inputs are opened, which comes before the rest of those settings is read.
  void readMidiInputSwitches();

  MidiMap& midiMap() { return midiMap_; }
  const MidiMap& midiMap() const { return midiMap_; }
  // One keyboard playing the whole organ (#358). Message thread, apart from
  // the two calls in the MIDI handling.
  SplitKeyboardHost& splitKeyboard() { return split_; }
  const SplitKeyboardHost& splitKeyboard() const { return split_; }
  // Persisted next to the organ definition, per organ: a player's console
  // mapping is theirs, and must never be written into a licensed sample set.
  // --- which organ is this? ----------------------------------------------
  // Everything a player configures — the MIDI map, the combinations, the load
  // settings — is saved per organ, so the organ needs an identity that is
  // stable across the things a player actually does to their sample library.
  //
  // Hauptwerk already answers this: `Identification_UniqueOrganID` in the
  // _General table. It is part of the organ, so it survives moving the set to
  // another drive, renaming the folder, or reinstalling it — none of which a
  // path can survive. That is the primary key.
  //
  // GrandOrgue takes the other approach: it hashes the normalised absolute ODF
  // path and names the .cmb after that. Simple and needs nothing from the
  // file, but move the organ and the settings are orphaned. It is used here
  // only as the FALLBACK, for a set that declares no unique id — a CODM organ,
  // or one somebody wrote by hand.
  //
  // The name is carried alongside so the folder is readable rather than a
  // wall of hex.
  std::string organKey() const;
  // The same, for an organ that has not been loaded yet: reads only the
  // _General table, which is the first thing in the file.
  static std::string organKeyFor(const juce::File& odf);

  // Where one of an organ's files lives, adopting the older filename-based
  // name when that is what is on disk.
  juce::File organFile(const juce::File& odf, const juce::String& folder,
                       const juce::String& extension) const;
  // Where it is WRITTEN, which is always the current naming.
  juce::File organFileForSaving(const juce::String& folder,
                                const juce::String& extension) const;
  juce::File midiMapFileFor(const juce::File& odf) const;

  // --- per-organ settings ------------------------------------------------
  // How this organ should be LOADED — resident format, streaming, preload
  // head — plus the DSP switches and the master volume. These belong to the
  // organ and not to the program: a 19 GB set wants streaming and 16-bit, a
  // one-manual village organ does not, and a player should not have to set
  // that again every time they open it.
  //
  // Read at the START of loadOrgan, because the load options decide how the
  // samples are read and cannot be applied afterwards. Saved to the player's
  // own data, never into the sample set.
  juce::File settingsFileFor(const juce::File& odf) const;
  bool saveSettings() const;
  // Record every stop, coupler and switch that is not at its default as this
  // organ's starting state, and save the organ's settings.
  bool saveCurrentState();
  // Applies whatever the file holds. Call before reading any audio.
  bool loadSettingsFor(const juce::File& odf);

  // The same keys one tier up: where a newly opened organ STARTS before its
  // own file, if it has one, is applied on top. Some of these settings are
  // really about the machine rather than the instrument — whether this disk
  // wants streaming, whether this much RAM wants 16-bit — and re-choosing
  // them per organ is answering the same question repeatedly.
  //
  // A floor, never an override: an organ that has been configured keeps what
  // it was given.
  juce::File globalSettingsFile() const;
  bool saveGlobalDefaults();
  bool loadGlobalDefaults();
  // The player's language, from the global file, before any window exists:
  // the labels a window is built with are translated when it is built.
  void applyChosenLanguage();

  // What to reopen when the program starts with no organ named. Held with the
  // global defaults because it belongs to the program, not to any one organ.
  juce::File lastOrgan() const;
  bool reopenLastOrgan() const { return reopenLastOrgan_; }
  // Whether General Cancel also puts every keyboard back on the manual it
  // started on, after pistons moved it (#90). Off by default, as in Hauptwerk
  // and GrandOrgue, whose General Cancel leaves the master couplers alone.
  bool cancelResetsKeyboards() const { return cancelResetsKeyboards_.load(); }
  // Whether the combinations window stays above every other program's
  // windows, not only above the console (#130). The program's, not an organ's.
  bool combinationsOnTop() const { return combinationsOnTop_; }
  void setCombinationsOnTop(bool on) {
    combinationsOnTop_ = on;
    writeGlobalFile();
  }
  // Whether Set lets go once a piston clicked in the combinations window
  // has stored, so the next click recalls instead of overwriting a
  // combination by mistake (#130). Off by default: a console's own Set
  // button stays down until it is pressed again.
  bool setOffAfterStore() const { return setOffAfterStore_; }
  void setSetOffAfterStore(bool on) {
    setOffAfterStore_ = on;
    writeGlobalFile();
  }
  // Whether the player's pistons and sequencer leave the bass and melody
  // couplers alone: not stored, not recalled (PlayerCombinations). Off by
  // default. The program's, for every organ.
  bool leaveBassAndMelody() const { return leaveBassAndMelody_; }
  void setLeaveBassAndMelody(bool on) {
    const AudioLock audio(*this);
    leaveBassAndMelody_ = on;
    player_.setLeaveBassAndMelody(on);
    writeGlobalFile();
  }
  // Whether the organ loaded has a bass or melody coupler a player moves.
  bool hasBassOrMelodyCoupler() const;
  // Whether the drawn manuals show the keys being played. Drawing them is
  // the window's largest cost while the organ is played fast; off, the
  // console neither looks at the keys nor draws them.
  bool showPlayedKeys() const { return showPlayedKeys_.load(std::memory_order_relaxed); }
  void setShowPlayedKeys(bool on) {
    showPlayedKeys_.store(on, std::memory_order_relaxed);
    writeGlobalFile();
  }
  // Whether a page smaller than half the first one fills the window in the
  // main window, as it does in a window of its own. Off as it has always
  // been: a console drawn small sat at its own size beside the first page.
  bool smallPagesFill() const { return smallPagesFill_.load(std::memory_order_relaxed); }
  void setSmallPagesFill(bool on) {
    smallPagesFill_.store(on, std::memory_order_relaxed);
    writeGlobalFile();
  }
  // The faster audio engine: vector runs (AVX2 on a PC, NEON on ARM) where
  // the processor has them. On by default since 0.7.11; off, the per-frame
  // path of the earlier versions plays. Takes effect at once.
  // MASTERPIECE_SIMD=0 in the environment still forces it off.
  bool fasterEngine() const { return fasterEngine_.load(); }
  // What a program that has never been told starts with.
  static bool fasterEngineByDefault() { return true; }
  void setFasterEngine(bool on) {
    fasterEngine_.store(on);
    {
      const AudioLock lock(*this);
      voices_.setSimd(on);
    }
    juce::Logger::writeToLog(juce::String("audio engine: ") +
                             (voices_.simd() == simd::Isa::None
                                  ? "per-frame"
                                  : juce::String("faster (") + simd::isaName(voices_.simd()) + ")"));
    writeGlobalFile();
  }
  // What it found on this machine: "AVX2", "NEON", or empty when the faster
  // engine cannot run here (or is off).
  juce::String fasterEngineUnit() const {
    return voices_.simd() == simd::Isa::None ? juce::String() : juce::String(simd::isaName(voices_.simd()));
  }
  // Real-time render threads, off by default while it is tried. The render
  // workers that share each block with the audio thread are ordinary threads
  // otherwise; on, each becomes real-time the way its system offers it (see
  // RealtimeThread.h), and on macOS and iOS joins the audio device's
  // workgroup. On Linux the audio thread itself too, where JUCE's ALSA
  // thread is an ordinary one. Takes effect at each thread's next share.
  bool realtimeThreads() const { return realtimeThreads_.load(); }
  void setRealtimeThreads(bool on);
  // False on Android, which gives an app none.
  static bool realtimeThreadsSupported();
  // How it is done here: "MMCSS Pro Audio", "SCHED_FIFO", "rtkit", "the
  // realtime portal", "Mach time constraint"; empty where the system gives
  // this user no real-time rights (Linux without them).
  static juce::String realtimeMethod();
  // How many render workers are real-time now, and how many in a workgroup.
  static int realtimeWorkers();
  static int workgroupWorkers();
  void audioWorkgroupContextChanged(const juce::AudioWorkgroup& workgroup) override;
  void setCancelResetsKeyboards(bool on) {
    cancelResetsKeyboards_.store(on);
    writeGlobalFile();
  }
  // Stops and couplers moved while keys are held leave those notes as they
  // are: only the keys pressed afterwards play the new registration. Off,
  // the default, a held note gains and loses pipes as on a console with
  // electric action. For a player who changes registration on a held chord
  // the way a key switch changes a sound (GrandOrgue #1833).
  bool stopsLeaveHeldNotes() const { return stopsLeaveHeldNotes_.load(std::memory_order_relaxed); }
  void setStopsLeaveHeldNotes(bool on) {
    stopsLeaveHeldNotes_.store(on, std::memory_order_relaxed);
    writeGlobalFile();
  }
  // A sustain pedal, controller 64 on any channel, for a player directing a
  // choir: while it is down, keys let go keep sounding until it is lifted.
  // Off by default; a controller 64 learned for something else keeps doing
  // that.
  bool sustainPedal() const { return sustainPedal_.load(std::memory_order_relaxed); }
  void setSustainPedal(bool on) {
    sustainPedal_.store(on, std::memory_order_relaxed);
    writeGlobalFile();
  }
  // The noise filter for learning. A console with a failing contact sends
  // notes of a few milliseconds that nobody played, and the first of them was
  // learned in place of the key the player pressed. With this on, a key
  // counts for a learn only once it has been held a quarter second; a shorter
  // note is dropped. Controllers, program changes and system exclusive have
  // no length and are taken at once. Off by default: some consoles' pistons
  // send a note that lasts only a few milliseconds.
  static constexpr double kLearnHoldMs = 250.0;
  bool learnNoiseFilter() const { return learnNoiseFilter_.load(std::memory_order_acquire); }
  void setLearnNoiseFilter(bool on) {
    learnNoiseFilter_.store(on, std::memory_order_release);
    writeGlobalFile();
  }
  // Keep portable speakers from going to sleep (#55). A battery speaker on a
  // 3.5 mm cable switches its amplifier off after a few seconds of silence and
  // takes a second or two to wake on the next note, swallowing it. With this
  // on, a 20 Hz tone too low and too quiet to hear runs under the organ, so
  // the line is never silent. The level is the player's: how much signal
  // wakes an amplifier differs from one model to the next.
  //
  // Program-wide, in the global file. 0 means off; otherwise dBFS, -80..-40.
  void setSpeakerKeepAlive(float levelDb);
  float speakerKeepAlive() const { return keepAliveDb_.load(std::memory_order_relaxed); }
  void setReopenLastOrgan(bool on);
  // Once a day the application reads the newest version's number from the
  // project's site and offers it when it is newer (ui::checkForUpdate). On
  // unless the player turns it off, here or in the offer itself.
  bool updateCheckEnabled() const { return updateCheck_; }
  void setUpdateCheckEnabled(bool on);

  // The control API and web console (src/mp_api, protocol/control.yaml),
  // run by the standalone program. Off by default; the settings live here so
  // they are kept with the others, and the program is told when they change.
  struct RemoteSettings {
    bool enabled = false;
    int port = 8080;
    bool allowChanges = true;
    std::string token;
    // The quit and power-off commands; off by default.
    bool allowPowerOff = false;
    // The page, the API and OSC on this computer only (127.0.0.1).
    bool thisComputerOnly = false;
    // The same protocol over MIDI System Exclusive, on the MIDI inputs and
    // the console's output, as this unit number (0-126).
    bool sysex = false;
    int sysexUnit = 0;
    // And over OSC, on this UDP port.
    bool osc = false;
    int oscPort = 9000;
  };
  const RemoteSettings& remoteSettings() const { return remote_; }
  void setRemoteSettings(const RemoteSettings& s) {
    remote_ = s;
    writeGlobalFile();
    if (onRemoteSettingsChanged) onRemoteSettingsChanged();
  }
  // Set by the program that runs the server: told of a change, and asked for
  // the server's state and its addresses for the settings.
  std::function<void()> onRemoteSettingsChanged;
  std::function<juce::String()> remoteStatus;
  // Set by the program: save and quit, and with true shut the computer down.
  std::function<void(bool powerOff)> onPowerRequest;

  // The ready lamp: a message on the console's MIDI output that lights a lamp
  // once an organ is loaded and playable, blinks while one loads and while a
  // quit or power-off button is held, and goes out with no organ. For a
  // console with no screen. Off by default; kept with the global settings.
  struct ReadyLamp {
    MidiSourceKind kind = MidiSourceKind::None;  // Note or ControlChange
    int channel = 1;                             // 1..16
    int number = 0;                              // the note or the controller
  };
  ReadyLamp readyLamp() const;
  void setReadyLamp(const ReadyLamp& lamp);
  // What the lamp shows this block: 1 lit, 0 dark.
  int readyLampState() const { return lampState_.load(std::memory_order_relaxed); }
  // The message that sets it: a note on at velocity 127 or off, a controller
  // at 127 or 0. False when the lamp is off.
  static bool readyLampMessage(const ReadyLamp& lamp, bool lit, RawMidi& out);
  // How long the program must have run before a quit or power-off button is
  // obeyed, how long it is held, and the window for two presses of a button
  // whose message has no release.
  static constexpr double kPowerArmSeconds = 120.0;
  static constexpr double kPowerHoldSeconds = 3.0;
  static constexpr double kPowerTwiceSeconds = 2.0;
  // On the message thread, from the window's timer: carries out what a MIDI
  // button asked for on the audio thread -- a temperament, a quit or a power
  // off (through onPowerRequest) -- and logs a button pressed before it was
  // armed.
  void serviceMidiRequests();
  // Whether a day has passed since the last check, and its time noted.
  bool updateCheckDue() const;
  void markUpdateChecked();
  // Cleared when the processor goes: for work queued on the message thread.
  std::weak_ptr<std::atomic<bool>> aliveToken() const { return alive_; }

  // Crash guard. While the guard is on, a load writes the organ into the
  // global file as "running", and only a clean exit takes it out again. An
  // entry still there at the next start means the program died with that
  // organ loading or loaded -- and reopening it automatically would crash it
  // straight away again, before the player could do anything about it. The
  // application turns the guard on; tools and tests leave it off, since they
  // never exit the way the application does.
  void setCrashGuard(bool on) { crashGuard_ = on; }
  void clearRunningOrgan();
  // Marks the loaded organ as running again, after clearRunningOrgan: a phone
  // or tablet clears the mark when the app goes to the background, where the
  // system may end it without a word, and sets it again on the way back.
  void markRunningOrgan();
  // The organ the previous session died with, read by loadGlobalDefaults.
  juce::File crashedOrgan() const { return crashedOrgan_; }
  // Takes the last session's mark out of the settings once it has been read
  // and acted on. Otherwise a session that loads nothing and does not exit
  // cleanly either (a phone ending it in the background) left it there, and
  // every later start warned about the same crash and skipped the organ.
  void forgetCrash() {
    if (crashedOrgan_.getFullPathName().isNotEmpty()) writeGlobalFile();
  }

  // The most memory an organ's samples may take, in megabytes. 0 means the
  // default, 80% of this machine's memory. A load that would pass it stops
  // cleanly and says so, rather than running the machine out of memory.
  int memoryLimitSettingMB() const { return memoryLimitMB_; }
  void setMemoryLimitMB(int mb);
  // The most voices that sound at once: the size of the voice pool, chosen
  // by the player; 0 means the default (defaultPolyphony()). Past it a new
  // pipe takes the place of one dying away, or of the quietest. Fewer voices
  // is a thinner tutti on a full organ, more than the machine can render is
  // a glitch. A larger pool is built at once, the organ staying loaded; a smaller
  // number applies at once.
  int polyphonyLimit() const { return polyphony_; }
  // The voices built now, which a larger choice rebuilds at once.
  int voicePoolSize() const { return voices_.poolSize(); }
  // Where a release tail ends: decibels below full scale, 0 for never. Saved
  // with the organ; -90 unless the player chose: at -80 the tails of a
  // final chord, each below it alone, ended together audibly.
  int releaseCutoffDb() const { return releaseCutoffDb_; }
  void setReleaseCutoffDb(int db) {
    releaseCutoffDb_ = db < 0 ? db : 0;
    voices_.setReleaseCutoff(releaseCutoffDb_ < 0 ? std::pow(10.0f, releaseCutoffDb_ / 20.0f) : 0.0f);
  }
  void setPolyphonyLimit(int voices);
  // What this computer is given when the player has chosen nothing: 725
  // voices a processor thread, at least 1,536; 1,536 when releases stream.
  int defaultPolyphony() const;
  static int maxPolyphonySetting() { return 16384; }
  int voicePool() const { return polyphony_ > 0 ? polyphony_ : defaultPolyphony(); }
  // What a pool of this many voices holds in memory with the streaming
  // choice as it stands; part of the memory limit, with the samples.
  int64_t voicePoolBytes(int voices) const;
  // The same ceiling for this session only, saved nowhere: for tools that
  // must not rewrite the player's settings.
  void overrideMemoryLimitMB(int mb) { memoryLimitOverrideMB_ = mb > 0 ? mb : 0; }
  static int defaultMemoryLimitMB();
  int64_t memoryLimitBytes() const;
  // Audible load progress: a swift tap at each 10% of a load. Off unless
  // asked. Global, never per organ: it suits the room, not the instrument.
  bool loadTicks() const { return loadTicks_.load(std::memory_order_acquire); }

  // Where the sample cache is written. A cache is as large as the organs
  // played through it, so a machine with a small fast disk and a large slow
  // one has to be told which to use. An empty file means the default place,
  // beside the other settings; setting one saves the choice at once.
  juce::File cacheDirectory() const;
  void setCacheDirectory(const juce::File& dir);
  static juce::File defaultCacheDirectory();
  // The test suite's own cache folder, set before any processor is made. The
  // cache is one file per folder, replaced as organs change, so a test organ
  // loaded through the player's folder replaced the player's cache.
  static void setCacheDirectoryForTests(const juce::File& dir);
  juce::File cacheDirectorySetting() const { return cacheDir_; }
  // A folder the player keeps packages in, for a composite that plays other
  // organs' packages (#289): searched, with the folders directly in it, after
  // the organ's own folder. Empty: none. Set is written at once; use is for
  // this session only, so the tests leave the machine's settings alone.
  juce::File packageDirectory() const { return packageDir_; }
  void setPackageDirectory(const juce::File& dir);
  void usePackageDirectory(const juce::File& dir) { packageDir_ = dir; }
  void setLoadTicks(bool on);
  // The language the program speaks: "" follows the system, "en" is English,
  // anything else is a code from availableLanguages(). The windows are built
  // with their words already in place, so the choice is applied once, when the
  // global file is first read. A change is saved at once and is heard at the
  // next start.
  juce::String languageSetting() const { return language_; }
  void setLanguageSetting(const juce::String& code);
  // Session-only form of the above: flips the switch without writing the
  // global file. Headless tools use this so a measurement render never
  // carries the progress taps, and without changing the player's preference
  // behind their back.
  void setLoadTicksSession(bool on) {
    loadTicks_.store(on, std::memory_order_release);
  }
  // Remembering which organ was open must not quietly promote that organ's
  // settings to everyone's defaults, so this rewrites the file around the
  // defaults already in it rather than around the live state.
  void setLastOrgan(const juce::File& odf);
  // The folder the last organ was chosen from in Open, where the next Open
  // starts (#251). Empty, or a folder that is gone: the system's default.
  juce::File openFolder() const { return openFolder_.isDirectory() ? openFolder_ : juce::File(); }
  void setOpenFolder(const juce::File& folder);
  // Where each kind of file dialog was last used -- "ir", "midi",
  // "recording", "scala" -- so the next one starts there (#323). Empty, or
  // a folder that is gone: the system's default.
  juce::File dialogFolder(const juce::String& kind) const {
    const auto it = dialogFolders_.find(kind);
    return it != dialogFolders_.end() && it->second.isDirectory() ? it->second : juce::File();
  }
  // The folder of `picked` (a file, or a folder chosen as one).
  void rememberDialogFolder(const juce::String& kind, const juce::File& picked);
  // Raised whenever something a settings file holds is changed, so the message
  // thread can write it without the audio thread touching a disk.
  void markSettingsDirty() { settingsDirty_.store(true, std::memory_order_release); }

  // --- which stops to load (Organ settings, Stops) -----------------------
  // The stops a player has chosen to leave out of this organ, saved with it.
  // Takes effect on the next load: everything is loaded except the ranks
  // that ONLY left-out stops use, so a rank a loaded stop shares, noises and
  // pallet-wired ranks always come.
  const std::set<Id>& excludedStops() const { return excludedStops_; }
  void setExcludedStops(std::set<Id> stops) {
    excludedStops_ = std::move(stops);
    markSettingsDirty();
  }
  // The perspectives -- microphone positions -- to leave out of this organ,
  // saved with it like the stops. A set recorded from three or four places
  // holds the whole organ that many times over; leaving one out saves its
  // share of the memory and loses nothing else, where leaving out stops
  // loses stops.
  std::map<std::string, std::vector<Id>> perspectives() const { return perspectivesOf(model_); }
  const std::set<std::string>& excludedPerspectives() const { return excludedPerspectives_; }
  void setExcludedPerspectives(std::set<std::string> names) {
    excludedPerspectives_ = std::move(names);
    markSettingsDirty();
  }
  // What the last load left out, so a change can be told from what is loaded.
  const std::set<std::string>& perspectivesLeftOut() const { return perspectivesLeftOut_; }
  // Single ranks to leave out, saved with the organ like the stops (#53): the
  // finest choice, for what neither a stop nor a perspective can say -- the
  // rear ranks of a few stops only, or a set's "tremmed" copies. A rank left
  // out is left out whichever stop plays it.
  // Keep this organ playable with its installation files unmounted: its own
  // sample cache, plus a copy of its definition and console pictures, which
  // the next open uses when the originals are not there (#90). Saved with the
  // organ.
  bool keepsPortableCopy() const { return keepPortable_; }
  void setKeepPortableCopy(bool on) {
    keepPortable_ = on;
    markSettingsDirty();
  }
  // Where the copy of an organ's definition is, when one was made; an empty
  // File otherwise.
  static juce::File portableCopyFor(const juce::File& originalOdf);
  const std::set<Id>& excludedRanks() const { return excludedRanks_; }
  void setExcludedRanks(std::set<Id> ranks) {
    excludedRanks_ = std::move(ranks);
    markSettingsDirty();
  }
  // What the last load left out of those, to tell a change from what is loaded.
  const std::set<Id>& ranksLeftOut() const { return ranksLeftOut_; }
  // Whether a stop's audio came with the last load. A left-out stop still
  // draws and records in pistons -- it simply makes no sound -- and the
  // console dims it so it is not taken for a broken one.
  bool stopLoaded(Id stopId) const { return unloadedStops_.count(stopId) == 0; }
  const std::unordered_set<Id>& unloadedSwitches() const { return unloadedSwitches_; }
  // The stop a drawn switch stands for when that stop makes no sound, and
  // why: its ranks ship no pipes in this set (a demo's locked stops), or the
  // player left it out of the load. For the console to say so.
  struct Unavailable {
    Id stopId = 0;
    bool noPipes = false;
  };
  const Unavailable* unavailableFor(Id switchId) const {
    const auto it = unavailable_.find(switchId);
    return it == unavailable_.end() ? nullptr : &it->second;
  }
  // Memory figures before a load (SampleLibrary::shapeJobs and friends):
  // the jobs from the model on the message thread, readShape() on a worker,
  // and the bytes at whatever the engine settings are now.
  std::vector<SampleLibrary::ShapeJob> sampleShapeJobs() const;
  int64_t residentBytesFor(const SampleLibrary::SampleShape& shape) const {
    return samples_.residentBytes(shape);
  }
  // The organ file loaded now, for loading it again.
  const juce::File& loadedOrganFile() const { return loadedOdf_; }
  // Whether a player can reach this stop: it has a drawstop on the console,
  // or the organ draws no console at all and the stop list is the console.
  // Sets keep machinery in stops nobody draws -- key-action noises, coupler
  // and tremulant effects -- which Organ settings lists apart.
  bool stopDrawn(Id stopId) const;
  // The samples a stop's ranks play, for adding the estimate up per stop.
  std::vector<Id> samplesOfStop(Id stopId) const;
  std::vector<Id> samplesOfRanks(const std::vector<Id>& rankIds) const;
  // Where the player keeps the combinations window on this organ, and whether
  // it was open. Per organ, because a console with its own pistons drawn
  // wants it closed and one with none wants it open. w 0 means never placed.
  // Message thread only.
  struct WindowPlace {
    int x = 0, y = 0, w = 0, h = 0;
    bool open = false;
  };
  const WindowPlace& combinationsWindowPlace() const { return combWindow_; }
  // The console pages the player keeps in windows of their own, and where:
  // a second or third screen set up once comes back the same way with the
  // organ. Per organ, like the combinations window.
  struct PagePlace {
    int page = 0;
    int x = 0, y = 0, w = 0, h = 0;
    // The console layout the window shows -- a set's portrait jambs on a
    // portrait screen -- or -1 to follow the main window's.
    int layout = -1;
    // Whether the window was without a title bar.
    bool borderless = false;
    bool operator==(const PagePlace&) const = default;
  };
  const std::vector<PagePlace>& pageWindowPlaces() const { return pageWindows_; }
  void setPageWindowPlaces(std::vector<PagePlace> places) {
    if (places == pageWindows_) return;
    pageWindows_ = std::move(places);
    markSettingsDirty();
  }
  // Audio blocks answered with silence because an organ was being loaded
  // (see EngineSuspension). For tests and diagnostics.
  uint64_t blocksSkippedForLoad() const { return blocksSkippedForLoad_.load(); }
  void setCombinationsWindowPlace(const WindowPlace& p) {
    combWindow_ = p;
    markSettingsDirty();
  }
  bool saveSettingsIfDirty();
  // The fader alone, raised from the UI on every drag and flushed on the same
  // timer as the rest. Deliberately its own flag and its own writer rather
  // than routing through markSettingsDirty()/saveSettings(): that pair
  // rewrites the whole per-organ file from the LIVE engine state, which would
  // let a Settings-dialog change the player only "kept" for the session ride
  // along on the next tick of the volume slider. This one touches nothing but
  // the "gain" line.
  void markMasterGainDirty() { masterGainDirty_.store(true, std::memory_order_release); }
  bool saveMasterGainIfDirty();
  bool saveMasterGain() const;
  // What the ORGAN asks to keep from one session to the next -- its controls
  // and switches marked RememberStateFromLastLoad: Nancy's audio-group and
  // noise levels, its "start blower on load" switch. Moving one used to be
  // written only if something else saved the file afterwards, so a level set
  // on the organ's own settings page was back at its default after a restart
  // (#53).
  //
  // Raised on any thread when a control or switch moves; written on the
  // message thread once the player has stopped for a second, so dragging a
  // slider is not a file write per frame. Like the gain, it rewrites only its
  // own lines ("control", "switch"), never the rest of the file from the live
  // state.
  void markRememberedStateMoved() {
    if (hasRememberedState_)
      // Never 0, which means "nothing waiting"; never ahead of the clock
      // either, or the wait would wrap round and read as long over.
      rememberedMovedAtMs_.store(std::max(juce::Time::getMillisecondCounter(), 1u),
                                 std::memory_order_release);
  }
  bool saveRememberedStateIfSettled(uint32_t quietMs = 1000);
  // Now, whatever the wait: on closing, and before another organ replaces
  // this one.
  bool saveRememberedStateIfPending();
  // The publisher's licence, for a set whose samples ask for one (ADR-003).
  // Masterpiece cannot check it; the player says whether they hold one that
  // lets them play the set here, once per organ, and the answer is saved
  // with it. Takes effect on the next load. Encrypted files are another
  // matter and are never loaded, whatever this says.
  bool licenceConfirmed() const { return licenceConfirmed_; }
  void setLicenceConfirmed(bool on) {
    if (licenceConfirmed_ == on) return;
    licenceConfirmed_ = on;
    markSettingsDirty();
  }
  // Who licenses the set, as its packages name them; empty when they do not.
  std::string licencePublisher() const {
    for (const auto& [id, pkg] : model_.packages)
      if (!pkg.supplierName.empty()) return pkg.supplierName;
    return {};
  }
  // A mapping learned on the audio thread, written here.
  bool saveMidiMapIfDirty();
  // Every save that is waiting, now: before an export, so the archive holds
  // what the player sees.
  void saveEverythingPending();
  // Settings were restored or copied into the settings folder
  // (SettingsArchive.h), and what is in memory is older. Nothing is saved
  // until the next organ load has read them; the saves waiting are dropped.
  void holdSavesUntilReload();
  void releaseSaves() { holdSaves_.store(false, std::memory_order_release); }
  bool savesHeld() const { return holdSaves_.load(std::memory_order_acquire); }
  // The folder the player's settings live in.
  static juce::File settingsRoot();
  bool saveMidiMap() const;
  bool loadMidiMap();

  // --- practice and session tools -------------------------------------
  // All three run inside the audio callback, because anything that has to
  // stay in time with the organ cannot live outside it.
  Metronome& metronome() { return metronome_; }
  // The metronome's tempo, beats and level, kept in the general settings when
  // its tab closes. Whether it ticks is not kept: it starts silent.
  void rememberMetronome() { writeGlobalFile(); }
  MidiRecorder& recorder() { return recorder_; }
  // The score panel's playing, merged in where the recorder's is.
  ScorePlayer& scorePlayer() { return scorePlayer_; }
  // The recorder's take saved and read with the consoles it was played on,
  // by their names.
  bool saveTake(const juce::File& file);
  bool loadTake(const juce::File& file);

  // Everything played, kept (E2): off by default. A take begins with the
  // first note and ends after `silenceSeconds` with no MIDI input; the
  // archive folder is kept under `maxMegabytes` and `maxDays`.
  struct ArchiveSettings {
    bool on = false;
    int silenceSeconds = 30;
    int maxMegabytes = 500;
    int maxDays = 90;
  };
  ArchiveSettings archiveSettings() const {
    return {archiveOn_.load(std::memory_order_acquire), archiveSilence_, archiveMegabytes_, archiveDays_};
  }
  void setArchiveSettings(const ArchiveSettings& s, bool save = true);
  // Documents/Masterpiece/Recordings/Archive, or the tests' own.
  juce::File archiveFolder() const;
  void setArchiveFolderForTests(const juce::File& folder) { archiveFolder_ = folder; }
  // Message thread, from the window's timer: writes a take that has ended,
  // or the take in progress when `now`, and prunes the folder. Returns the
  // file written.
  juce::File serviceArchive(bool now = false);
  // Captures what leaves, minus the metronome. See AudioRecorder.
  AudioRecorder& audioRecorder() { return audioRecorder_; }
  Convolver& convolver() { return convolver_; }

  // The peak limiter on the final output: after the master fader, before
  // the device. Off by default, and while it is off the audio thread does
  // not call it at all, so the output is what the engine made. The three
  // settings are kept in the general settings.
  bool limiterEnabled() const { return limiter_.enabled(); }
  void setLimiterEnabled(bool on);
  float limiterCeilingDb() const { return limiter_.ceilingDb(); }
  void setLimiterCeilingDb(float db);
  float limiterReleaseMs() const { return limiter_.releaseMs(); }
  void setLimiterReleaseMs(float ms);
  // The limiter itself, for a test and for a readout.
  OutputLimiter& limiter() { return limiter_; }

  // MIDI OUT. A physical console lights its own drawstops from what the organ
  // sends back, so engaging a stop on screen has to reach the hardware.
  void setMidiOutput(juce::MidiOutput* out) { midiOut_ = out; }
  // Page turns for a score app (ScorePageMessage): sent on the MIDI output
  // with the next audio block, from any thread. The message is kept in the
  // global settings.
  void turnScorePage(bool forward) {
    (forward ? scorePagesForward_ : scorePagesBack_).fetch_add(1, std::memory_order_acq_rel);
  }
  ScorePageMessage scorePageMessage() const { return unpackScorePage(scorePage_.load(std::memory_order_acquire)); }
  void setScorePageMessage(const ScorePageMessage& m);
  // How many page turns have gone out since the program started; for tests,
  // which have no MIDI output to read.
  int scorePagesTurned() const { return scorePagesTurned_.load(std::memory_order_acquire); }
  juce::MidiOutput* midiOutput() const { return midiOut_; }
  // The console's output port, opened by name and kept in the general
  // settings: lamps, displays and key echo reach the console with any window
  // open or none, and the port opens again at the next start (Main.cpp). An
  // empty name closes it. False when no port of that name is there; the name
  // is kept for when the console is plugged in.
  bool openMidiOutput(const juce::String& name);
  const juce::String& midiOutputName() const { return midiOutputName_; }
  // Echo switch changes to the console. Off by default: a console that echoes
  // what it just sent can latch itself into a loop. Kept in the general
  // settings.
  void setMidiFeedbackEnabled(bool on) {
    if (midiFeedback_ == on) return;
    midiFeedback_ = on;
    writeGlobalFile();
  }
  bool midiFeedbackEnabled() const { return midiFeedback_; }

  // Console LCD panels. Configured by the player, because the framing belongs
  // to their hardware and not to the organ — see LcdPanel.h.
  LcdPanels& lcdPanels() { return lcd_; }
  // The console display as the player set it up on the Display tab, kept in
  // the general settings: what a line shows is the box's item number, 1 for
  // blank to 8 for the combination set. The display follows the organ with
  // any window open or none (the main window pumps it).
  struct LcdSetup {
    bool on = false;
    int displayNumber = 0;
    int width = 32;
    juce::String prefix = "7D";
    std::array<int, 4> lines{2, 3, 4, 7};  // organ, temperament, pitch, crescendo
  };
  const LcdSetup& lcdSetup() const { return lcdSetup_; }
  void setLcdSetup(const LcdSetup& setup);
  // What the panels are showing. Shared with the settings preview rather than
  // reassembled there: a preview that computes its own idea of the state will
  // eventually disagree with the hardware, and then it is worse than none.
  LcdState lcdState() const;

  // --- the mixer -------------------------------------------------------
  // Whose configuration this is: the player's, not the organ's. No sample set
  // declares a routing object at all (see MixerConfig.h).
  // --- voicing ---------------------------------------------------------
  // The player's per-rank and per-pipe adjustments. Two slots plus a live
  // flag, because voicing is done by comparing a change against what was
  // there before; from memory the comparison always flatters whichever was
  // heard last.
  // --- favourites ------------------------------------------------------
  // Numbered slots a player can reach without a file dialog. GLOBAL, not per
  // organ: the whole point is to get to a different organ, so storing them
  // inside the organ you are leaving would be useless.
  Favourites& favourites() { return favourites_; }
  const Favourites& favourites() const { return favourites_; }
  // Put the organ now loaded on a slot, or on the first free one when slot is
  // 0. Returns the slot used, or 0 when the bank is full or nothing is loaded.
  int addCurrentOrganToFavourites(int slot = 0);

  // The organ's voicing EQ (Organ settings, Voicing EQ): off unless the
  // player turns it on for this organ. Applied at once; saved with the organ.
  const VoicingEqSettings& voicingEq() const { return voicingEqSettings_; }
  void setVoicingEq(const VoicingEqSettings& s) {
    voicingEqSettings_ = s;
    voicingEq_.configure(s, sampleRate_);
    ++voicingEqChanges_;
  }
  // Moves on with every change, so a panel open on it can follow a preset
  // recalled from MIDI.
  int voicingEqChanges() const { return voicingEqChanges_; }
  // Every organ starts from the default EQ -- the speakers and the room are
  // the same whatever is played -- until one is saved for it. Saved for the
  // organ, the EQ is its own, On box included, whatever it is.
  bool saveVoicingEqForOrgan();
  // The EQ as it is now, for every organ that has none of its own.
  bool saveVoicingEqAsDefault();
  // This organ back to the default EQ, its own one forgotten.
  bool resetVoicingEqToDefault();
  const VoicingEqSettings& defaultVoicingEq() const { return defaultEq_; }
  bool organHasOwnVoicingEq() const { return organOwnsEq_; }

  // The attack's emphasis (Organ settings, Voicing EQ): each pipe begins
  // `db` louder, or softer when negative, and comes to its own level in a
  // straight line over `ms`. 0 dB is off, and then no voice does anything
  // for it. Saved with the organ; heard from the next note.
  float attackDb() const { return attackDb_.load(std::memory_order_relaxed); }
  int attackMs() const { return attackMs_.load(std::memory_order_relaxed); }
  void setAttack(float db, int ms) {
    attackDb_.store(juce::jlimit(-12.0f, 12.0f, db), std::memory_order_relaxed);
    attackMs_.store(juce::jlimit(5, 300, ms), std::memory_order_relaxed);
    markSettingsDirty();
  }

  // The EQ's presets, program-wide: ten of the program's and the player's own
  // (VoicingEqPresets.h). Recalling one sets the EQ at once and turns it on,
  // as moving the sliders does; Save for this organ and Save as default keep
  // it as before. Each has an id that never changes, for MIDI and for any
  // control from outside.
  const VoicingEqPresets& voicingEqPresets() const { return eqPresets_; }

  // Live levels (LiveLevels.h): a fader for each rank and each division of
  // the organ loaded, both in id order as the control API lists them, and
  // one for each output pair. 0..127, 127 as voiced; all at full after a
  // load. Set from any thread; ignored while an organ loads.
  int liveRankLevel(int index) const { return liveLevels_.rank(index); }
  int liveDivisionLevel(int index) const { return liveLevels_.division(index); }
  int livePairLevel(int index) const { return liveLevels_.pair(index); }
  void setLiveRankLevel(int index, int value) {
    if (!organLoading()) liveLevels_.setRank(index, value);
  }
  void setLiveDivisionLevel(int index, int value) {
    if (!organLoading()) liveLevels_.setDivision(index, value);
  }
  void setLivePairLevel(int index, int value) {
    if (!organLoading()) liveLevels_.setPair(index, value);
  }
  int liveRankCount() const { return liveLevels_.rankCount(); }
  int liveDivisionCount() const { return liveLevels_.divisionCount(); }
  // A rank's or a division's place in those lists; -1 for none.
  int liveRankIndexOf(Id rankId) const {
    const auto it = liveRankIndex_.find(rankId);
    return it == liveRankIndex_.end() ? -1 : it->second;
  }
  int liveDivisionIndexOf(Id divisionId) const {
    const auto it = liveDivisionIndex_.find(divisionId);
    return it == liveDivisionIndex_.end() ? -1 : it->second;
  }
  bool recallVoicingEqPreset(int id);
  bool recallVoicingEqPresetNamed(const juce::String& name);
  // The next (1) or the one before (-1); returns the id recalled, or 0.
  int stepVoicingEqPreset(int step);
  // The preset the EQ is now, or 0 when it is none of them or it is off.
  int currentVoicingEqPreset() const;
  // The last one recalled or saved, which Update, Rename and Delete act on.
  int lastVoicingEqPreset() const { return lastEqPreset_; }
  // The player's own: a new one from the EQ as it is now (returns its id),
  // the EQ as it is now stored into one, a new name, or gone. The program's
  // ten cannot be changed.
  int saveVoicingEqPreset(const juce::String& name);
  bool updateVoicingEqPreset(int id);
  bool renameVoicingEqPreset(int id, const juce::String& name);
  bool deleteVoicingEqPreset(int id);
  // A preset a MIDI button asked for, collected on the message thread: an id,
  // -1 for the next, -2 for the one before, or 0 when none is waiting. Reading
  // takes it.
  int takeVoicingEqPresetRequest() { return pendingEqPreset_.exchange(0, std::memory_order_acq_rel); }
  VoicingAB& voicing() { return voicing_; }
  // After the voicing's EQ has changed -- a rank's or a division's
  // brightness or bass, a swap of A and B, a reset: hands it to the audio
  // (RankEq.h). Message thread.
  void applyVoicingEq() {
    const AudioLock audio(*this);
    pushVoicingEq();
  }
  const VoicingAB& voicing() const { return voicing_; }

  MixerConfig& mixer() { return mixer_; }
  const MixerConfig& mixer() const { return mixer_; }
  // Rebuild the dense bus indexing after the config changes. Must be called
  // before the next block, and never from the audio thread.
  void refreshMixerBuses();
  // Load or drop each bus's impulse response to match the config. Slow (it
  // reads and re-plans), so it is called when the mixer changes, never per
  // block, and never from the audio thread.
  void refreshBusReverbs();
  int mixBusCount() const { return static_cast<int>(mixBusOrder_.size()); }
  BusId mixBusAt(int denseIndex) const {
    return denseIndex >= 0 && denseIndex < static_cast<int>(mixBusOrder_.size())
               ? mixBusOrder_[static_cast<size_t>(denseIndex)]
               : BusId{0};
  }
  // Capture each mixer bus separately as the normal callback runs.
  //
  // A hook rather than a second render path: the per-bus signal has to come
  // from the same voices, the same wind and the same shades as the audio
  // anyone actually hears, and a parallel path would drift from it. When set,
  // each bus is rendered into its own buffer AND summed into the output as
  // usual, so nothing about the callback changes.
  //
  // What lands here is PRE-convolver and pre-master-fader: it is the bus
  // signal, which is where a per-bus IR will eventually sit. Caller owns the
  // buffers and must size them to the block; pass nullptr to stop capturing.
  // Message thread only, and not while audio is running.
  void setMixBusCapture(std::vector<juce::AudioBuffer<float>>* perBus) {
    mixBusCapture_ = perBus;
  }
  // Gathers what the panels show and queues whatever changed. Call from the
  // MESSAGE thread, on a timer: it builds strings, which has no business on
  // the audio thread, and nothing a panel shows moves faster than a person
  // can read it anyway. Returns how many messages were queued.
  int pumpLcdPanels();
  // A System Exclusive message to the console's output, sent with the next
  // audio block as the LCD lines are. Dropped when no output is open.
  void sendSysexOut(std::vector<uint8_t> bytes);
  // Re-sends every line regardless, for a console plugged in mid-session and
  // for the settings page's test button.
  int refreshLcdPanels();

  // --- memory / streaming ----------------------------------------------
  // How much of each sample is preloaded. The head is a MINIMUM: it is always
  // extended to cover the sustain loop, because a sample whose loop is missing
  // does not sustain. 0 loads whole files.
  // Whether the program couplers are offered: the ones the player asks
  // for, between the manuals at 16', 8' and 4'. Off by default; when on,
  // they are real switches and everything else works with them.
  void setProgramCouplers(bool on) { programCouplers_ = on; }
  bool programCouplers() const { return programCouplers_; }
  // The program couplers this organ has, in the order they were numbered.
  const std::vector<CouplerMatrix::ProgramCoupler>& programCouplerList() const {
    return programCouplersList_;
  }

  void setPreloadHeadFrames(int64_t frames) {
    preloadHead_ = frames;
  }
  int64_t preloadHeadFrames() const { return preloadHead_; }

  // The resident sample format, which is the other half of the memory
  // question and the larger half: the preload head decides how much of each
  // file is held, this decides what a held frame costs. Int16 halves it.
  // Applies to the next load, not the one already resident.
  void setSampleStorage(SampleStorage s) {
    samples_.setStorage(s);
  }
  SampleStorage sampleStorage() const { return samples_.storage(); }

  // Keys named here are ignored when a settings file is read, so a value the
  // player stated explicitly is not quietly replaced by a stored one.
  //
  // The order of a load makes this necessary rather than merely tidy: the
  // command line is applied at startup, and loadSettingsFor() runs inside
  // loadOrgan(), so without this the file always wins and the flag looks
  // broken rather than overridden.
  void overrideSetting(const juce::String& key) {
    overridden_.addIfNotAlreadyThere(key);
  }
  bool isOverridden(const juce::String& key) const {
    return overridden_.contains(key);
  }

  // Fold a stereo set to one channel while loading. Halves everything, and
  // gives up the recording's stereo image to do it. Applies to the next load.
  void setLoadMono(bool on) { samples_.setLoadMono(on); }
  // Which releases are loaded, and how long each is kept: at the next load.
  SampleLibrary::ReleaseChoice releaseChoice() const { return samples_.releaseChoice(); }
  void setReleaseChoice(SampleLibrary::ReleaseChoice c) { samples_.setReleaseChoice(c); }
  int releaseMaxMs() const { return samples_.releaseMaxMs(); }
  void setReleaseMaxMs(int ms) { samples_.setReleaseMaxMs(ms); }
  bool loadMono() const { return samples_.loadMono(); }

  // --- sparse loading -----------------------------------------------------
  // How many notes of a rank are loaded: 1 for every note, 2 for every second
  // note, 3 for every third. A note that is not loaded plays its nearest
  // loaded neighbour's samples, retuned to its own pitch, so the samples it
  // would have played are never read.
  //
  // Saved with the organ, and it takes effect at the next load.
  void setSparseEvery(int every) {
    const int v = every < 1 ? 1 : (every > 3 ? 3 : every);
    if (v == sparseEvery_) return;
    sparseEvery_ = v;
    samples_.setSparseEvery(v);
    markSettingsDirty();
  }
  int sparseEvery() const { return sparseEvery_; }

  // The sample cache: a previous load of this organ at these settings, kept
  // so the next one is a read instead of twelve thousand decodes. One file by
  // default, replaced as organs change, because these run to gigabytes.
  void setCacheMode(SampleLibrary::CacheMode m) { samples_.setCacheMode(m); }
  SampleLibrary::CacheMode cacheMode() const { return samples_.cacheMode(); }
  int64_t cacheBytesRead() const { return samples_.cacheBytesRead(); }
  int64_t cacheBytesWritten() const { return samples_.cacheBytesWritten(); }
  bool cacheWriting() const { return samples_.cacheWriting(); }
  double cacheWriteFraction() const { return samples_.cacheWriteFraction(); }
  int64_t cacheBytesDone() const { return samples_.cacheBytesDone(); }
  int64_t cacheBytesTotal() const { return samples_.cacheBytesTotal(); }
  // When the sample cache of a new load is written. Off (the default): the
  // load writes it before the organ can be played, as GrandOrgue does, and
  // nothing competes with the sound. On: the organ plays at once and the
  // cache is written whenever nothing sounds.
  bool cacheWhilePlaying() const { return cacheWhilePlaying_.load(); }
  void setCacheWhilePlaying(bool on) {
    cacheWhilePlaying_.store(on);
    writeGlobalFile();
  }
  // The player chose, in the load window, to open the organ without waiting
  // for its cache: the write is stopped and the next load reads the samples.
  void skipCacheSave() { skipCacheSave_.store(true); }
  // Whether a choice that decides what the sample cache holds differs from
  // the one the loaded organ was loaded with: closing Organ settings then
  // asks to rebuild the cache and load the organ again.
  bool cacheChoicesChangedSinceLoad() const {
    return !cacheChoicesAtLoad_.empty() && cacheChoices() != cacheChoicesAtLoad_;
  }

  // Convert sample data to this rate while loading; 0 keeps each file's own.
  // A 96 kHz set on a 48 kHz device is otherwise held at twice the size and
  // resampled once per voice. Applies to the next load.
  void setLoadSampleRate(double hz) { samples_.setLoadSampleRate(hz); }
  double loadSampleRate() const { return samples_.loadSampleRate(); }

  // Stream release tails from disk instead of holding them. Releases are
  // several seconds each, played once, straight through — the only samples in
  // an organ that stream well. Applies to the next load. See SampleLibrary.
  void setStreamReleases(bool on) {
    samples_.setStreamReleases(on);
  }
  bool streamReleases() const { return samples_.streamReleases(); }
  // Frames of a streamed release that stay resident, and how far ahead of each
  // voice the streamer runs.
  void setStreamHeadFrames(int64_t f) {
    samples_.setStreamHeadFrames(f);
  }
  // Or that share of each release, in percent; 0 is the fixed head alone.
  void setStreamHeadPercent(int percent) {
    samples_.setStreamHeadPercent(percent);
    markSettingsDirty();
  }
  int streamHeadPercent() const { return samples_.streamHeadPercent(); }
  void setStreamSeconds(double s) { voices_.setStreamSeconds(s); }
  // Zero unless the disk could not keep up, in which case a release went
  // silent partway and the player deserves to know.
  int64_t streamUnderruns() const { return voices_.streamUnderruns(); }
  // --log-releases: record every key release, and describe the ones recorded
  // since the last call, one line each. Message thread.
  // Also writes the lines to the log, a few times a second, while it is on;
  // from the command line or from General settings. Message thread.
  void setReleaseLogging(bool on);
  bool releaseLogging() const { return releaseLog_ != nullptr; }
  // Releases start in phase with their attack where the set asks for it.
  // Off only to measure against.
  void setReleasePhaseAlign(bool on) { voices_.setReleasePhaseAlign(on); }

  // A learned pedal's ends, from where it is now: a console whose swell shoe
  // only sends 55..127 still has to close the box (#90). Returns the value
  // taken, or -1 when the control is not mapped or nothing has arrived yet.
  int setControlPedalEnd(Id controlId, bool open);
  bool resetControlPedalRange(Id controlId);
  // The player's console mappings, kept for every organ (stepper, generals,
  // setter, page and tuning buttons).
  static juce::File consoleMidiFile();
  void applyConsoleMidi();
  // The value a controller last sent; -1 before it has sent any.
  int lastControllerValue(const MidiSource& source) const;
  std::vector<std::string> takeReleaseLog();

  // How the audio blocks keep time. A block that takes longer than the audio
  // it produces (its size over the sample rate) is late, and a late block is
  // a glitch the player hears. Counted on the audio thread, read anywhere;
  // reading takes the worst block since the last read.
  struct AudioLoad {
    int64_t blocks = 0;
    int64_t late = 0;
    double worstPercent = 0.0;  // the slowest block, as a share of its time
  };
  AudioLoad takeAudioLoad();

  // Where the organ was loaded from — the console needs it to resolve artwork
  // out of the same installation packages the audio comes from.
  const std::string& organRootDir() const { return organRootDir_; }

  // Where this organ's OrganInstallationPackages lives, when the definition's
  // own path does not lead there -- a folder linked in from another tree, for
  // instance. Empty means work it out from the path, which is the usual case.
  // Set before loading; saved with the organ's other settings.
  juce::File organRootOverride() const { return organRootOverride_; }

  // The folders this machine keeps sample libraries in: anything holding an
  // OrganInstallationPackages directory. A definition whose own path leads
  // nowhere near its audio -- both standard folders linked to unrelated
  // drives, which is what a Hauptwerk installation reorganised by hand looks
  // like -- is found by asking each of these whether it holds the packages
  // the definition names. Seeded with the standard location, added to by
  // every load that works, and saved with the other machine-wide settings.
  const std::vector<juce::File>& sampleLibraries() const { return libraries_; }
  void rememberSampleLibrary(const juce::File& root);
  void setOrganRootOverride(const juce::File& dir) { organRootOverride_ = dir; }
  // The next loads list the definition's fields nothing read, in
  // LoadResult::diagnostics.unreadFields. For mp-render --check.
  void setReportUnreadFields(bool on) { reportUnreadFields_ = on; }

  // Where the engine gets sample audio. Injected rather than owned, so the
  // preloaded and streaming backing stores share one voice path (ADR-004) and
  // tests can hand it a synthesised tone.
  void setSampleProvider(SampleProvider provider) {
    voices_.setSampleProvider(std::move(provider));
  }
  const EngineStats& voiceStats() const { return voices_.stats(); }

  // Runtime DSP toggles (ADR-005). DSP always ships; this is how a slow
  // machine turns it off without a rebuild.
  void setEngineSwitch(const EngineSwitch& sw) {
    {
      const AudioLock audio(*this);
      graph_.engineSwitch = sw;
      pushVoicingEq();
    }
    mirrorSimpleWavParameter();
  }
  const EngineSwitch& engineSwitch() const { return graph_.engineSwitch; }

  // The Loading tab's choices, kept for this session and this organ when the
  // Organ settings window closes without saving them (#90). Every load reads
  // the organ's settings file first, so without this the load that follows
  // the window put the old choices back: "Smallest" chosen, window closed,
  // and the organ loaded at full size. Loading another organ drops them.
  void keepLoadingChoiceForSession();

private:
  // Render each enclosure bus, filter it with its own shades, and sum into
  // `buffer`. One filter per enclosure, prepared at prepareToPlay; nothing is
  // allocated here.
  void renderBuses(juce::AudioBuffer<float>& buffer);
  // Sound one load-progress tap when a 10% threshold passes and render any
  // tap in flight. Audio thread only, after the recorder: taps are a
  // monitoring aid, not the performance.
  void maybeLoadTick(juce::AudioBuffer<float>& buffer);  void renderOneMixBus(juce::AudioBuffer<float>& dest, int mixBusFilter);
  // Which bus a pipe belongs to: its enclosure's index, or the unenclosed bus.
  int busForPipe(Id pipeId) const;
  // Which MIXER bus a pipe of this rank speaks through, as a dense index into
  // the per-bus buffers rather than a BusId (ids run to 1024; the buffers are
  // only as many as the player actually configured). Resolved at note-on
  // because a group allocation depends on the key.
  int mixBusForPipe(Id rankId, int midiNote) const;
  // Turn incoming MIDI into voice starts and stops. Runs on the audio thread,
  // so it must not allocate: the pipe list it walks is preallocated scratch.
  void buildVoicePool(int blockFrames);
  void handleMidi(const juce::MidiBuffer& midi);
  // Resampling ratio for one pipe playing one recorded sample: the pitch we
  // want over the pitch the file actually holds.
  // `layer` is needed as well as the pipe because detuning is declared per
  // layer: the control that drives it and the Hz it moves per control unit
  // both live there.
  // Decide what each sample file holds, after loading and before playing.
  // Obeys the set's Pitch_SpecificationMethodCode; writes the answer into
  // both the sample registry and the pipework's own copies.
  void resolveSamplePitches();
  double playbackRatioFor(const Pipe& pipe, const SampleRef& sample,
                          const PipeLayer& layer) const;
  // What a pipe must sound at, in the mode the organ plays in: at the
  // original instrument's own pitch, or at a tempered pitch from the
  // keyboard. `originalPitch` says whether the original pitch may be used:
  // a pipe that plays a neighbour's samples needs both pipes measured the
  // same way, and the mode needs both of them to say what they sounded at.
  double pipeTargetHzFor(const Pipe& pipe, const PipeLayer& layer,
                          bool originalPitch) const;
  // The pipe whose samples this one plays, when it plays a neighbour's
  // because its own were left out of the load. Null when it plays its own.
  const Pipe* borrowedNeighbour(const Pipe& pipe) const;
  // The pipes by id, for the sparse loading. Built at load, read on the
  // audio thread.
  void buildPipeIndex();
  // The sparse loading, worked out and put into the model: the pipes that are
  // not kept take a neighbour's layers, and the samples they would have
  // played are then read by nobody.
  void applySparseLoadingToModel();

  // Where this layer's detuning currently stands. Zero unless the organ
  // declares detuning AND a player has asked for some — and zero flat out
  // under simpleWavOnly, which is the switch a slow machine relies on.
  // The arithmetic itself is detunedTargetHz(), which is JUCE-free and
  // therefore testable without an organ.
  int detuneControlValue(const PipeLayer& layer) const {
    if (graph_.engineSwitch.simpleWavOnly || layer.pitchControlId == 0)
      return 0;
    return controls_.value(layer.pitchControlId);
  }

  // The middle of that control's travel, which is where "no detuning" sits.
  double detuneCentre(const PipeLayer& layer) const {
    const auto it = model_.continuousControls.find(layer.pitchControlId);
    if (it == model_.continuousControls.end()) return 0.0;
    return (it->second.minValue + it->second.maxValue) / 2.0;
  }
  // A key press on one of the organ's playable keyboards. The channel decides
  // which keyboard, and the key-flow graph decides which divisions it reaches
  // — a coupler is nothing more than an edge of that graph.
  void startNote(int channel, int midiNote, int velocity);
  void stopNote(int channel, int midiNote, int velocity);
  // The same, with the manual already decided. A mapped rig names it outright;
  // an unmapped one derives it from the channel.
  // `playedNote` is the key as played on that manual, before transposition;
  // -1 when it is `midiNote` itself. It is what the drawn manual lights.
  void startNoteOnKeyboard(Id keyboard, int noteKeyId, int midiNote,
                           int velocity, int playedNote = -1);
  void stopNoteByKey(int noteKeyId, int velocity);
  // A key let go: held over while the sustain pedal is down, released
  // otherwise. Audio thread.
  void keyUp(int noteKeyId, int velocity);
  // The pedal down or up; up releases every key it held over.
  void setSustainDown(bool down);
  // Notes are held per (channel, key): two manuals playing the same key are
  // two separate presses and one release must not silence both.
  static int noteKey(int channel, int midiNote) {
    return (channel << 8) | (midiNote & 0xff);
  }
  // Recall or capture the combination this switch fires, if it fires one.
  // Returns true when the switch was a piston, so the caller knows it has
  // already been dealt with.
  bool firePiston(Id switchId);
  // Recall a combination, or capture into it when the setter is held. Shared
  // by the pistons and the sequencer, so a frame the sequencer lands on
  // behaves exactly like the same piston pressed by hand.
  void fireCombination(Id combinationId);
  // Fire every noise rank wired to this switch. `engaged` picks the direction:
  // a drawstop makes one sound going in and a different one coming out.
  void triggerNoiseFor(Id switchId, bool engaged);

  OrganModel model_;
  CouplerMatrix couplers_;
  CombinationSystem combinations_;
  // Scratch for a recall, so pressing a piston on the audio thread does not
  // allocate. A general on a large organ moves a few hundred switches.
  std::vector<CombinationSystem::Change> recallScratch_;
  // The organ's own setter switch, if it has one, so holding it turns a
  // recall into a capture exactly as the console does.
  Id setterSwitchId_ = 0;
  std::atomic<bool> combinationsDirty_{false};
  std::atomic<bool> settingsDirty_{false};
  std::atomic<bool> midiMapDirty_{false};
  std::atomic<bool> masterGainDirty_{false};
  std::atomic<bool> holdSaves_{false};
  // Shoe positions that move switches: the crescendo, the blower, enclosure
  // noises. Needs the previous position, because each row is a crossing.
  PlayerCombinations player_;
  // Sized at load, like recallScratch_: a piston pressed on the audio thread
  // must not allocate.
  std::vector<PlayerCombinations::Change> playerScratch_;
  // The switches a player's registration is made of. Moving one by hand
  // means no piston describes what is drawn any more, so it unlights them.
  std::unordered_set<Id> registrationSwitches_;
  // True while a piston applies its registration, so the switches it moves do
  // not count as the player moving them.
  bool applyingPistons_ = false;
  std::atomic<bool> pedalCoupled_{false};
  std::atomic<bool> globalDirty_{false};
  CoupledDivisions coupledDivisions_;
  // The Pedal's divisional that goes with this one, while coupled; 0 for none.
  Id coupledPedalFor(Id divisionId) const {
    return pedalCoupled_.load() && coupledDivisions_.valid() && divisionId == coupledDivisions_.manual
               ? coupledDivisions_.pedal
               : 0;
  }
  void resetPlayerCombinations();
  void applyPlayerChanges();
  bool playerElementEngaged(const PlayerCombinations::Element& e) const;
  std::string combinationFileText() const;
  // The wind system. Advanced once per block on the audio thread, so its
  // tables are sized at load and it never allocates here.
  WindSolver wind_;
  // One entry per modelled windchest, in the solver's own order, handed to the
  // voice engine each block. Voices carry an index into it.
  std::vector<VoiceEngine::WindMod> windMods_;
  std::vector<Id> windOrder_;
  // Compartments that report their pressure to a continuous control, and
  // which control: the console's wind gauges hang off these.
  std::vector<std::pair<Id, Id>> windGauges_;
  std::unordered_map<Id, int> windIndexOf_;
  // Which windchest each pipe stands on, resolved at load so a note-on does
  // not search for it.
  std::unordered_map<Id, int> pipeWindIndex_;
  // The pipes by id, resolved at load like the wind chests: a pipe that plays
  // a neighbour's samples has to find that neighbour when it sounds, and a
  // note-on must not search.
  std::unordered_map<Id, const Pipe*> pipeById_;
  // How many notes of a rank are loaded: 1 for every note, 2 for every second
  // note, 3 for every third. Saved with the organ.
  int sparseEvery_ = 1;
  // Tremulants, in a stable order, and which one reaches each pipe. Both
  // resolved at load: a note-on must not search, and a voice started in one
  // block must still point at the right tremulant in the next.
  std::vector<Id> tremOrder_;
  std::unordered_map<Id, int> tremIndexOf_;
  std::vector<VoiceEngine::TremMod> tremMods_;
  // The live levels, and each rank's and division's place in them, resolved
  // at load like the wind.
  LiveLevels liveLevels_;
  std::unordered_map<Id, int> liveRankIndex_;
  std::unordered_map<Id, int> liveDivisionIndex_;
  // Sets a live level from a MIDI control: a rank or division by its id, a
  // pair by its number from 1.
  void setLiveLevelFromMidi(MidiTargetKind kind, Id targetId, int value);
  void advanceTremulants(int numFrames);
  // The wind sway: one slow wave per division and one for ranks in none,
  // with each rank's place resolved at load; advanced once a block.
  void advanceSway(int numFrames);
  std::unordered_map<Id, int> rankSwayIndex_;
  int swayChannels_ = 0;
  std::vector<double> swayPhaseA_, swayPhaseB_, swayRatios_;
  std::vector<float> windDemand_;
  // Advance the wind by one block: ask the engine what is drawing air, solve,
  // and hand the result back for the voices to sound through.
  void advanceWind(int numFrames);
  // Hand each gauge-driving compartment's pressure to its control.
  void publishWindPressures();
  StageSwitchBank stages_;
  std::vector<StageSwitchBank::Change> stageScratch_;
  // Where every staged control was, so a move can be told from a rest. Only
  // the handful of controls that actually drive switches are tracked, and the
  // control a player moves is often not one of them — Nancy's crescendo pedal
  // drives an "extension" control through a linkage, and that is the one with
  // the steps behind it.
  std::vector<std::pair<Id, int>> stageValues_;
  // Resolved once at load: for each switch, the drawn one upstream of it.
  std::unordered_map<Id, Id> playerSwitch_;
  // Stop -> the drawn knob that stands for it, for sets whose Stop points at
  // a switch nothing draws and no linkage drives.
  std::unordered_map<Id, Id> stopKnob_;
  // The reverse, drawn switch -> the stops it stands for. Clicking a knob
  // moves that switch; the stops follow through the wiring — except on sets
  // (Friesach) whose knobs are a separate chain with no linkage into the
  // stop's own switch, where nothing downstream moves and the stop would
  // stay silent behind a drawn picture. Built at load; read on the audio
  // thread, never written there.
  std::unordered_map<Id, std::vector<Id>> stopsBySwitch_;
  ContinuousControlBank controls_;
  VoiceEngine voices_;
  SampleLibrary samples_;
  std::vector<Id> preloadStops_;
  std::vector<Id> preloadRanks_;
  juce::StringArray overridden_;   // settings the command line has claimed
  juce::File clearedRunning_;      // the running mark, set aside in the background
  std::atomic<int64_t> audioBlocks_{0};
  std::atomic<int64_t> lateBlocks_{0};
  std::atomic<int> worstBlockPermille_{0};
  juce::MidiKeyboardState keyboardState_;
  // Raised by releaseAllKeys(), consumed at the top of the next block.
  std::atomic<bool> releaseAll_{false};
  // The organ's tuning, resolved once at load. Held by value so the audio
  // thread never chases a pointer into the model while it is being swapped.
  Temperament organTuning_;
  // What the player chose instead, resolved: null means the organ's own. The
  // library's entries live for the program's lifetime and a Scala file is
  // kept in scalaTunings_, never erased while an organ is loaded, so a note
  // starting on the audio thread never reads a temperament being replaced.
  std::atomic<const Temperament*> playerTuning_{nullptr};
  std::deque<Temperament> scalaTunings_;
  std::string temperamentChoice_;
  std::atomic<double> masterPitchHz_{0.0};
  std::atomic<int> transpose_{0};
  std::atomic<float> pipeDetuneCents_{0.0f};
  std::atomic<float> windSwayCents_{0.0f};
  const Temperament& activeTuning() const {
    const Temperament* t = playerTuning_.load(std::memory_order_acquire);
    return t != nullptr ? *t : organTuning_;
  }
  double pitchFactor() const {
    const double hz = masterPitchHz_.load(std::memory_order_relaxed);
    return hz > 0.0 ? hz / nativePitchHz() : 1.0;
  }
  int transposed(int midiNote) const {
    const int n = midiNote + transpose_.load(std::memory_order_relaxed);
    return n < 0 ? 0 : (n > 127 ? 127 : n);
  }
  std::unordered_set<Id> engagedStops_;
  // The window's copy of engagedStops_ and engagedSwitches_, and whether
  // either has changed since it was made. The copy is made by whoever changed
  // them -- at the end of the audio block, or as the audio lock is let go --
  // and only if the window is not reading at that instant: the audio thread
  // does not wait for it, and tries again at the next block.
  mutable std::mutex shownMutex_;
  std::unordered_set<Id> shownStops_;
  std::unordered_set<Id> shownSwitches_;
  bool shownStale_ = true;
  void publishShown();
  std::atomic<bool> logMidi_{false};
  int64_t loggedUnderruns_ = 0;
  // Each controller's last value plus one; 0 until it sends anything.
  std::array<std::atomic<int>, 128> lastController_{};
  std::array<std::atomic<int>, 16 * 128> lastControllerOnChannel_{};
  std::unique_ptr<juce::Timer> releaseLog_;
  // How many manual assignments the last load had to discard from a saved
  // mapping. Shown to the player, because a mapping that changes under them
  // without a word would cost more trust than the fault it fixes.
  int midiMapRepaired_ = 0;
  // The organ's switch wiring, and its resolved output. A drawstop rarely
  // drives anything directly: it drives an internal node, and everything else
  // reads that. `engagedSwitches_` is the network's answer, kept as a set
  // because the key-flow walk consults it on the audio thread.
  SwitchNetwork switches_;
  std::unordered_set<Id> engagedSwitches_;
  std::string organRootDir_;
  juce::File organRootOverride_;
  bool reportUnreadFields_ = false;
  std::vector<juce::File> libraries_;
  // The root that holds the packages this model names, or an empty file.
  juce::File libraryHolding(const OrganModel& model) const;
  void seedSampleLibraries();
  // A drawstop on the console IS a switch; clicking it must draw the stop, not
  // merely animate the picture. Built at load so the audio thread never
  // searches for it. One switch can control several stops: Azzio's
  // Sesquialtera hangs both of its stops on one (#285).
  std::unordered_map<Id, std::vector<Id>> stopBySwitch_;
  // Every switch a stop answers to -- its own, the player's, its knob. The
  // stop is drawn while any of them is engaged; it used to follow whichever
  // moved last, and a knob let go pushed in a stop its own switch still held.
  std::unordered_map<Id, std::vector<Id>> stopSources_;
  bool stopSourcesEngaged(Id stopId) const;
  std::map<int, int> defaultConsole_;  // role -> channel
  void applyDefaultConsole();
  // Switches that swap a stop's rank for its alternate and re-sound held
  // notes when they do, with the stops concerned; and the switch state just
  // before the latest change, which says what was sounding.
  std::unordered_map<Id, std::vector<Id>> alternateStopsBySwitch_;
  std::unordered_set<Id> previousSwitches_;
  // The switches as they were before this change, and where each held key
  // reached under them and under the new ones: what a coupler moved (#131).
  std::unordered_set<Id> heldFlowBefore_;
  std::vector<ExpandedNote> reflowBefore_, reflowAfter_;
  void reflowHeldNotes();
  MidiMap midiMap_;
  SplitKeyboardHost split_{midiMap_};
  Metronome metronome_;
  // The peak limiter on the final output. Off until the player turns it on;
  // while it is off the audio thread does not call it at all.
  OutputLimiter limiter_;
  MidiRecorder recorder_;
  ScorePlayer scorePlayer_;
  MidiArchive archive_;
  std::atomic<bool> archiveOn_{false};
  int archiveSilence_ = 30, archiveMegabytes_ = 500, archiveDays_ = 90;
  juce::File archiveFolder_;
  AudioRecorder audioRecorder_;
  Convolver convolver_;
  // The organ's declared output trim as a linear gain, resolved once at
  // load. 1.0 for a set that declares none.
  float organTrimGain_ = 1.0f;
  double windDepth_ = 1.0;
  LoadProgress loadProgress_;
  std::atomic<bool> organLoading_{false};
  std::atomic<int> organLoads_{0};
  bool applyOrganTrim_ = true;
  juce::MidiOutput* midiOut_ = nullptr; // ownedMidiOut_, or the application's
  std::unique_ptr<juce::MidiOutput> ownedMidiOut_;
  juce::String midiOutputName_;
  bool midiFeedback_ = false;
  LcdPanels lcd_;
  LcdSetup lcdSetup_;
  // lcd_ built from lcdSetup_; the setter and the settings reader both use it.
  void applyLcdSetup();
  // The player's mixer. Defaults to one stereo bus, which makes the whole
  // routing path a no-op until someone configures something.
  MixerConfig mixer_ = MixerConfig::stereoDefault();
  VoicingAB voicing_;
  VoicingEqSettings voicingEqSettings_;
  VoicingEqSettings defaultEq_;  // the general settings' `defaulteq`
  VoicingEqPresets eqPresets_;   // the general settings' `eqpreset` lines
  int lastEqPreset_ = 0;         // the last recalled, where Next goes on from
  int voicingEqChanges_ = 0;     // message thread only
  std::atomic<int> pendingEqPreset_{0};
  bool organOwnsEq_ = false;     // the organ's file has an `eq` line
  std::atomic<float> attackDb_{0.0f};
  std::atomic<int> attackMs_{40};
  VoicingEq voicingEq_;
  Favourites favourites_;
  // Empty means the organ's default set.
  std::vector<PagePlace> pageWindows_;
  std::string combinationSet_;
  WindowPlace combWindow_;
  std::set<Id> excludedStops_;
  std::set<std::string> excludedPerspectives_;
  std::set<std::string> perspectivesLeftOut_;
  std::set<Id> excludedRanks_;
  bool keepPortable_ = false;
  std::string loadedStamp_;  // what the sample cache is keyed to, for the copy
  static juce::File portableRoot();
  void writePortableCopy(const juce::File& odf);
  std::set<Id> ranksLeftOut_;
  std::unordered_set<Id> unloadedStops_;
  std::unordered_set<Id> unloadedSwitches_;
  std::unordered_map<Id, Unavailable> unavailable_;
  // The stops whose ranks have no pipes in this set: held in (holdOff).
  std::unordered_set<Id> lockedStops_;
  // Ranks that list pipes and ship none of their samples: a demo's, for the
  // stops it leaves out. They count as having no pipes.
  std::unordered_set<Id> unshippedRanks_;
  std::vector<BusId> mixBusOrder_;             // dense index -> BusId
  std::unordered_map<int, int> mixBusIndexOf_;  // BusId.value -> dense index
  std::vector<juce::AudioBuffer<float>>* mixBusCapture_ = nullptr;
  // One convolver per dense bus index, built only for buses that declare an
  // IR. unique_ptr because a Convolver holds an FFT plan and is neither cheap
  // nor movable, and most buses will never have one.
  std::vector<std::unique_ptr<Convolver>> busConvolvers_;
  // Per-bus scratch, needed only when a bus has its own room: without one the
  // buses sum straight into the output and cost nothing.
  juce::AudioBuffer<float> mixScratch_;
  // The device channels of each dense bus, left and right; -1 for a bus that
  // names none, which plays on the first two as every bus did before.
  std::vector<std::pair<int, int>> busOutputs_;
  // Where a bus with device channels of its own is rendered before it goes
  // there, and the stereo fold-down of a wider output for the recorder.
  juce::AudioBuffer<float> pairScratch_;
  juce::AudioBuffer<float> recordScratch_;
  int lastOutputChannelsWanted_ = 2;
  // Bass management per dense bus, built for the buses that have it on.
  std::vector<std::unique_ptr<BassManager>> busBass_;
  void refreshBusBass();
  // Whether any bus has its own channels, or bass managed to a channel the
  // output has: then the buses are rendered one by one to where they go.
  bool needsRoutedOutput(int numCh) const;
  // The channels of bus `index` on an output of `numCh` channels: its own
  // when the output has them, otherwise the first pair.
  std::pair<int, int> busOutputFor(int index, int numCh) const;
  bool anyBusOnItsOwnChannels(int numCh) const;
  // Built on the message thread, drained by the audio thread into outgoing_ so
  // there is one sender to the port. The audio thread takes this with
  // try_lock and simply waits a block if it is contended — an LCD line arriving
  // 10 ms late is invisible, and blocking for it would not be.
  std::mutex lcdQueueLock_;
  std::vector<SysexMessage> lcdQueue_;
  // Written only by the audio thread, read only by the meter. `held_` is the
  // audio thread's own running value and needs no synchronisation; the atomic
  // is the copy the UI is allowed to see.
  std::atomic<int> pendingConsoleAction_{0};
  // The ready lamp's setting, as atomics the audio thread reads every block,
  // and what it sent last (-1 nothing yet).
  std::atomic<int> lampKind_{0}, lampChannel_{1}, lampNumber_{0};
  std::atomic<int> lampState_{0};
  int lampSent_ = -1;
  uint64_t lampFrames_ = 0;
  // Whether an organ is loaded and playable, for the lamp.
  std::atomic<bool> organReady_{false};
  // The quit and power-off buttons, audio thread only: frames since the
  // program started rendering, the button held now and for how long, and
  // the last press of a button with no release.
  uint64_t powerClockFrames_ = 0;
  MidiTargetKind powerHeld_ = MidiTargetKind::None;
  uint64_t powerHeldSince_ = 0;
  MidiTargetKind powerLastPress_ = MidiTargetKind::None;
  uint64_t powerLastPressAt_ = 0;
  // Handed to the message thread: 1 quit, 2 power off; a temperament target
  // + 1; seconds left before the buttons are armed, for the log.
  std::atomic<int> pendingPower_{0};
  std::atomic<int64_t> pendingTemperament_{0};
  std::atomic<int> powerNotArmedSeconds_{0};
  void powerButton(MidiTargetKind kind, bool pressed, bool hasRelease);
  void pumpReadyLamp(int numFrames);
  std::atomic<int> pendingRecorder_{0};
  juce::File lastButtonSave_;
  std::atomic<float> outPeak_[2]{{0.0f}, {0.0f}};
  float peakHeld_[2] = {0.0f, 0.0f};
  // Per-block decay, worked out once in prepareToPlay. A host that varies its
  // block size shifts the fall slightly; nobody can see that in a lamp, and
  // it is not worth an exp() per callback to correct.
  float meterFall_ = 0.5f;

  int64_t preloadHead_ = 0;
  // Whether the program couplers are offered. Off by default.
  bool programCouplers_ = false;
  // The program couplers this organ has, in the order they were numbered.
  std::vector<CouplerMatrix::ProgramCoupler> programCouplersList_;
  // Off unless asked for: reopening at start is what turns one crash into a
  // loop of them.
  bool reopenLastOrgan_ = false;
  bool updateCheck_ = true;
  RemoteSettings remote_;
  juce::int64 updateCheckedAt_ = 0;  // seconds since 1970, of the last check
  std::atomic<float> keepAliveDb_{0.0f};
  double keepAlivePhase_ = 0.0;  // audio thread only
  void addKeepAlive(juce::AudioBuffer<float>& buffer);
  bool crashGuard_ = false;
  juce::File runningOrgan_;
  juce::File crashedOrgan_;
  bool globalsReadOnce_ = false;
  int memoryLimitMB_ = 0;
  int polyphony_ = 0;
  int memoryLimitOverrideMB_ = 0;  // this session only; never written
  std::atomic<bool> loadTicks_{false};
  juce::String language_;  // empty: the system's
  juce::File cacheDir_; // empty: the default place
  juce::File packageDir_; // empty: none
  // Next 10% threshold to tap at, 10 through 100. Reset by whoever starts a
  // load and advanced by the audio thread, so both sides use an atomic and
  // neither waits on the other.
  std::atomic<int> loadTickNext_{10};
  // A tap in flight. Audio thread only: set when a threshold passes and
  // rendered into the buffer over the next blocks.
  int loadTickLeft_ = 0;
  double loadTickPhase_ = 0.0;
  double loadTickAmp_ = 0.0;
  // Seconds before another tap may start. A cached load crosses every
  // threshold in a few blocks; without this it machine-guns ten taps.
  double loadTickCooldown_ = 0.0;
  juce::File lastOrgan_;
  juce::File openFolder_;
  std::map<juce::String, juce::File> dialogFolders_;
  // One writer and one reader for the keys both settings tiers share, so the
  // global defaults and an organ's own file cannot drift apart.
  juce::String settingsBody() const;
  void applySettingsLine(const juce::String& key, const juce::String& val,
                         EngineSwitch& sw);
  // Control positions read from the organ's file, held until the bank exists.
  // loadSettingsFor runs at the top of loadOrgan, but controls_.reset() is
  // much further down and would wipe anything set before it — so these wait
  // and are applied on the far side of it.
  std::vector<std::pair<Id, int>> pendingControlValues_;
  // The same for remembered switches, applied once the switch network exists.
  std::vector<std::pair<Id, bool>> pendingSwitchStates_;
  // The "control" and "switch" lines for the organ's remembered state.
  juce::String rememberedStateLines() const;
  bool saveRememberedState() const;
  // The stops the player saved as this organ's starting state ("state" lines).
  // Written by saveCurrentState(), read back on load and applied with the other
  // remembered switches. An explicit snapshot, unlike the remembered switches,
  // which only the organ's own marks decide.
  std::vector<std::pair<Id, bool>> snapshotStates_;
  // The stops drawn when the state was saved ("stopon" lines).
  std::vector<Id> snapshotStops_;
  // Whether this organ remembers anything at all; most do not, and they never
  // pay for a write.
  bool hasRememberedState_ = false;
  bool licenceConfirmed_ = false;
  // When remembered state last moved, 0 for "nothing waiting".
  std::atomic<uint32_t> rememberedMovedAtMs_{0};
  // The defaults as they stand on disk, kept verbatim so that writing the
  // file for any other reason cannot rewrite them from whatever is loaded.
  juce::String globalBody_;
  bool writeGlobalFile() const;
  // Reused every block so the audio thread never allocates one.
  juce::MidiBuffer outgoing_;
  // Page turns asked for and not yet sent, and the message, packed so the
  // audio thread reads it in one load.
  std::atomic<int> scorePagesForward_{0}, scorePagesBack_{0}, scorePagesTurned_{0};
  std::atomic<uint32_t> scorePage_{packScorePage(ScorePageMessage{})};
  static uint32_t packScorePage(const ScorePageMessage& m) {
    return static_cast<uint32_t>(m.kind & 3) | static_cast<uint32_t>(m.channel & 31) << 2 |
           static_cast<uint32_t>(m.forward & 127) << 7 | static_cast<uint32_t>(m.back & 127) << 14;
  }
  static ScorePageMessage unpackScorePage(uint32_t v) {
    ScorePageMessage m;
    m.kind = static_cast<int>(v & 3);
    m.channel = static_cast<int>(v >> 2 & 31);
    m.forward = static_cast<int>(v >> 7 & 127);
    m.back = static_cast<int>(v >> 14 & 127);
    return m;
  }
  void emitScorePageTurns();
  // Tagged input, filled by the device callbacks and drained by the audio
  // thread. A fixed ring so neither side allocates; a single producer per slot
  // is not guaranteed (several devices push), so the write index is atomic and
  // the whole thing is sized to make an overflow implausible rather than
  // impossible — a dropped message beats a blocked MIDI thread.
  // Long enough for the system exclusive messages consoles send for their
  // pistons (Johannus: 11 bytes) and registrations (a Rodgers bitfield: about
  // 44), and fixed, so a MIDI thread never allocates to hand one over (#210).
  static constexpr int kTaggedMidiBytes = 256;
  struct TaggedMidi {
    int deviceId = 0;
    uint8_t bytes[kTaggedMidiBytes] = {};
    int size = 0;
    // Which write this slot holds, plus one, stored after the bytes: the
    // reader takes a slot only once this says it is complete. Several
    // consoles push from their own MIDI threads, and reading a slot between
    // a writer claiming it and filling it replayed whatever message had
    // been there a lap of the queue before.
    std::atomic<uint32_t> ready{0};
  };
  static constexpr int kMidiQueueSize = 2048;
  // Half a megabyte, so on the heap: a processor built on a thread's stack
  // (a 1 MB stack on Windows) would not have room for it.
  std::unique_ptr<TaggedMidi[]> midiQueue_ =
      std::make_unique<TaggedMidi[]>(kMidiQueueSize);
  std::atomic<uint32_t> midiWrite_{0};
  uint32_t midiRead_ = 0;
  // Drain the tagged queue into `midi` before it is handled, so device-aware
  // and host-delivered messages take exactly the same path afterwards.
  void drainTaggedMidi(std::vector<std::pair<int, juce::MidiMessage>>& out);
  std::vector<std::pair<int, juce::MidiMessage>> midiScratch_;
  // Where in the block each of midiScratch_ falls: the host's and a MIDI
  // file's messages carry their place; a console's arrive between blocks and
  // take the block's start. Voices started while one is handled begin there.
  std::vector<int> midiOffsets_;
  int eventOffset_ = 0;
  // The console the message being handled came from. Set as each message is
  // dispatched rather than threaded through every call, because only the note
  // path cares and threading it would touch a dozen signatures.
  int noteDeviceId_ = 0;
  // The channel of the message being handled, for the same reason.
  int noteChannel_ = 0;
  std::atomic<bool> keyPick_{false};
  std::atomic<int> pickedChannel_{0};
  // Keyboards moved by pistons, and where they were before (#90).
  std::atomic<bool> pistonRouted_{false};
  std::atomic<bool> cancelResetsKeyboards_{false};
  std::atomic<bool> stopsLeaveHeldNotes_{false};
  std::atomic<bool> sustainPedal_{false};
  // Audio thread: whether the pedal is down, and the keys let go under it.
  bool sustainDown_ = false;
  std::vector<int> sustainedKeys_;
  std::atomic<bool> learnNoiseFilter_{false};
  // Keys held while learning with the noise filter on, until one has been
  // held long enough. Audio thread only.
  struct HeldForLearn {
    bool used = false;
    MidiSource source;
    int value = 0;
    double sinceMs = 0.0;
  };
  std::array<HeldForLearn, 16> heldForLearn_{};
  bool learningFrom(int deviceId) const;
  void holdForLearn(const MidiSource& source, int value);
  void learnHeldKeys();
  void learnFromPress(const MidiSource& source, int value);
  bool combinationsOnTop_ = false;
  std::atomic<bool> cacheWhilePlaying_{false};
  int releaseCutoffDb_ = -90;
  // MASTERPIECE_BLOCKLOG=<file>: every block's stages timed, with its note-ons,
  // note-offs and voices, kept in memory made at start and written to the
  // file when the program closes. For finding what a late block spent its
  // time on; without the variable, a flag test per block.
  struct BlockRecord {
    float totalUs, preUs, midiUs, controlsUs, windUs, renderUs, dspUs, budgetUs;
    int noteOns, noteOffs, activeBefore, activeAfter, stolen;
  };
  std::vector<BlockRecord> blockLog_;
  size_t blockLogCount_ = 0;
  std::string blockLogPath_;
  double blockWindUs_ = 0.0;
  void writeBlockLog();
  std::atomic<bool> skipCacheSave_{false};
  // The cache-deciding choices, as one string to compare, and what the
  // loaded organ was loaded with.
  std::string cacheChoices() const;
  std::string cacheChoicesAtLoad_;
  // How many voices sounded in the last block, for the cache write that waits
  // for silence.
  std::atomic<int> soundingVoices_{0};
  bool setOffAfterStore_ = false;
  bool leaveBassAndMelody_ = false;
  std::atomic<bool> showPlayedKeys_{true};
  std::atomic<bool> smallPagesFill_{false};
  // See storeTarget(). Written where a piston or step is chosen, under the
  // audio lock or on the audio thread; read under the lock.
  StoreTarget storeTarget_;
  std::atomic<bool> fasterEngine_{fasterEngineByDefault()};
  // setRealtimeThreads(): the switch, the device's workgroup, the length of a
  // block, which the scheduler is told a share may take, and a count moved on
  // each change, which the audio thread compares with the one it last saw.
  std::atomic<bool> realtimeThreads_{false};
  std::mutex workgroupMutex_;
  juce::AudioWorkgroup workgroup_;
  std::atomic<double> blockSeconds_{0.0};
  std::atomic<uint32_t> realtimeGeneration_{0};
  uint32_t audioThreadGeneration_ = 0; // the audio thread's own
  void realtimeHere(); // on a render worker, from the engine's hook
  juce::StringArray ownInputs_, inputsOff_;
  // Per device id, whether its messages are dropped: read on MIDI threads.
  static constexpr int kMaxMutedDevices = 64;
  std::array<std::atomic<bool>, kMaxMutedDevices> deviceMuted_{};
  std::vector<MidiMap::KeyboardBinding> routingBeforePistons_;
  std::vector<Id> routeScratch_;
  void routeFromControl(const MidiAction& action);
  void restoreRouting();
  // Listen for the MIDI window. heard_ is written by the audio thread before
  // heardReady_ is released, and read by the window after it is acquired.
  std::atomic<bool> midiListen_{false};
  std::atomic<bool> heardReady_{false};
  MidiSource heard_;
  int heardValue_ = 0;
  // The value each send last went out with, by its place in the map's list;
  // -1 sends the current state on the next block. Cleared when sends change.
  std::vector<int> sentValues_;
  // Keys played on a manual that sends them on, by key id, so the note-off
  // goes out on the note the note-on did.
  std::unordered_map<int, std::pair<Id, int>> keySends_;
  void emitSends();
  void sendKey(Id keyboard, int midiNote, int velocity, bool on);
  // Where each held key switch came from, as (channel, device): a key can be
  // a switch with no note sounding, on an organ played through its pallets.
  std::unordered_map<int, std::pair<int, int>> heldKeySwitchOrigin_;
  // Release everything played from this channel of this device -- what All
  // Notes Off and its relatives mean, per the MIDI standard.
  void releaseChannel(int channel, int deviceId);
  // The manual being learned, and the first key pressed for it. 0 means not
  // learning; -1 for the note means the low key is still to come.
  Id keyboardLearn_ = 0;
  int keyboardLearnLow_ = -1;
  int keyboardLearnDevice_ = 0;
  int keyboardLearnChannel_ = 0;
  // Take a key press as part of learning a manual. Returns true when the
  // message was consumed and must not also play.
  bool learnKeyboardFrom(int deviceId, int channel, int note);
  // Reused every block: matching a key against the manual bindings must not
  // allocate on the audio thread.
  std::vector<MidiMap::KeyHit> keyHits_;
  // Wall clock for the current block, for debouncing chattering contacts.
  double blockTimeMs_ = 0.0;
  juce::File loadedOdf_;
  // Cleared on destruction: a project's organ load, queued on the message
  // thread, runs only while the processor is there.
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  struct LoadingChoice {
    juce::File organ;
    SampleStorage storage = SampleStorage::Float32;
    bool mono = false, stream = false;
    int64_t streamHead = 0, preload = 0;
    int streamHeadPercent = 0;
    // How many notes of a rank are loaded: 1 for every note, 2 for every
    // second, 3 for every third.
    int sparse = 1;
    // The program couplers: chosen on the same tab, and lost with the rest
    // when the organ's own settings are read again at the load.
    bool programCouplers = false;
    EngineSwitch engine;
  };
  std::optional<LoadingChoice> sessionLoading_;
  // Noise ranks keyed by the switch that fires them, built once at
  // prepareToPlay so a switch flip never walks every rank on the audio thread.
  std::unordered_map<Id, std::vector<Id>> noiseRanksBySwitch_;
  // Scratch reused every note-on so the audio thread never allocates.
  std::vector<ResolvedPipe> resolveScratch_;
  // (channel, key) -> the note id its voices were started under, so note-off
  // can find them again.
  // A key that is down, and enough about it to work out again what it should
  // be sounding. Needed because the registration can change under a held note:
  // on a real organ, drawing a stop while a key is down makes that rank speak,
  // and pushing it in silences it, without touching the key.
  struct HeldNote {
    uint64_t id = 0;      // groups this key's voices
    Id keyboard = 0;
    int midiNote = 60;
    int velocity = 64;
    // Where the press came from, so a channel's All Notes Off can find the
    // notes that are that channel's and leave every other manual alone.
    int channel = 0;
    int device = 0;
    int playedNote = 60;  // the key as played, for the drawn manual
    // Where it stood among the keys held on its keyboard when its voices were
    // last worked out (CouplerMatrix::kLowestHeld / kHighestHeld): what a
    // bass or melody coupler carried for it.
    int extreme = 0;
  };
  // Where a key stands among those held on its keyboard, itself included.
  int extremeFor(Id keyboard, int midiNote) const;
  // A key went down or up on this keyboard: the keys still held there whose
  // standing changed gain or lose what the bass and melody couplers carry.
  void reflowExtremes(Id keyboard);
  // A chord's keys arrive a few milliseconds apart, and each in turn was the
  // top of it: a melody coupler played them one after the other, an arpeggio,
  // and again on the way up (#355). The top and the bottom are taken again
  // once a manual's keys have been still this long.
  static constexpr double kExtremeSettleMs = 25.0;
  void scheduleExtremes(Id keyboard);
  void settleExtremes();
  std::vector<std::pair<Id, double>> extremesDue_;  // keyboard, when
  std::unordered_map<int, HeldNote> soundingNotes_;
  // Which keys are down on which manual, by manual id (low six bits) and key.
  // Written on the audio thread where a note starts or stops on a manual,
  // read by the drawn manuals, so a key lights on the manual it reached
  // whatever device or channel brought it (#240).
  std::array<std::atomic<uint64_t>, 64 * 2> keysDown_{};
  void setKeyDown(Id keyboard, int note, bool down);
  // The keys held on a channel whose manual a piston is switching, to strike
  // again on the new one. Reserved at prepare, so the switch does not allocate.
  std::vector<std::pair<int, HeldNote>> rerouteScratch_;
  // The registration the sounding notes were started with, owned by the audio
  // thread, so a change can be told from what is already playing.
  std::unordered_set<Id> appliedStops_;
  std::atomic<bool> stopsChanged_{false};
  std::vector<Id> stopDiffScratch_;
  std::unordered_set<Id> stopSetScratch_;

  // Start the voices one key press asks for, drawing only on `stops`. The
  // whole registration when a key is struck; only what just moved when a stop
  // changes under a key already down. Returns whether any pipe answered.
  // `extreme`: see HeldNote.
  bool startVoicesForKey(Id keyboard, int midiNote, int velocity,
                         uint64_t noteId, const std::unordered_set<Id>& stops, int extreme = 0);
  // Bring the sounding notes into line with the registration.
  void applyStopChangeToHeldNotes();
  // Every layer of one pipe, under `noteId`.
  bool startPipeLayers(const Pipe& pipe, Id rankId, int midiNote, int velocity,
                       uint64_t noteId);

  // Pallet-driven pipework. Some organs reach their pipes through the switch
  // network instead of through StopRank: a key is a switch, the key switch is
  // wired through the stop's switch to a pallet switch, and the pipe speaks
  // while its pallet is engaged. Alessandria, Erfurt and Swieta Lipka declare
  // no StopRank at all, and every set we have tested sounds its key and stop
  // action this way. The network already follows the wiring, so the pipes only
  // have to answer the pallet switches it moves.
  //
  // Only ranks that no StopRank reaches are indexed. A pipe reached both ways
  // would otherwise speak twice.
  void buildPalletIndex();
  // A composite definition's packages that come with other organs (#289):
  // looked for among the archives beside the organ, joined to its archive,
  // and remembered beside its files. Returns the packages found this way.
  std::set<std::string> joinPackagesFromElsewhere(std::shared_ptr<OrganArchive>& archive,
                                                  const std::string& root,
                                                  const std::string& archivePath);
  // The stops whose switch leads, through the wiring, to the pallet of a
  // pipe the set ships (#304): Nowa Ruda names no rank under any stop, and
  // its stops reach their pipes only that way.
  void findPalletStops();
  std::unordered_set<Id> palletStops_;
  // The stops whose switch works a coupler or a tremulant: a GrandOrgue set
  // lists its couplers and tremulants with the stops, with no ranks (#490).
  void findControlStops();
  std::unordered_set<Id> controlStops_;
  void fireMovedStages();
  // False while a load is rebuilding the stage table; see loadOrgan().
  std::atomic<bool> stagesReady_{false};
  // A load rebuilds the whole engine on the loading thread: the settings and
  // mixer routing, the model, the wind, the switches, the couplers, the
  // voices. The audio thread must touch none of it meanwhile -- a crash
  // report showed WindSolver::integrate() reading what WindSolver::reset()
  // was freeing. While suspended, processBlock outputs silence and returns at
  // once; the loader, having set the flag, waits for any block already
  // running to finish before it changes anything (EngineSuspension).
  //
  // Both are sequentially consistent: the audio thread counts itself in
  // BEFORE reading the flag, the loader sets the flag BEFORE reading the
  // count, so one of them always sees the other.
  std::atomic<bool> engineSuspended_{false};
  std::atomic<int> inAudioCallback_{0};
  std::atomic<uint64_t> blocksSkippedForLoad_{0};
  struct EngineSuspension {
    explicit EngineSuspension(MasterpieceProcessor& p);
    ~EngineSuspension();
    MasterpieceProcessor& proc;
  };
  // False until a load has finished; pallets start no voices before then.
  std::atomic<bool> palletsLive_{false};
  // Set with palletsLive_: the first block after a load opens the pallets of
  // switches that came up engaged -- a blower that runs from the start.
  std::atomic<bool> palletsOpenEngaged_{false};
  void palletMoved(Id switchId, bool engaged);
  // Pallet switch -> the pipes it opens, with their rank.
  std::unordered_map<Id, std::vector<std::pair<Id, const Pipe*>>> palletPipes_;
  // (keyboard, note) -> the switch that key IS, for organs that declare one.
  std::unordered_map<int, Id> keySwitchByKey_;
  // The same switches as a set, for the noise path: a key-action noise fired
  // by a KEY switch belongs to the strike and takes its velocity, while one
  // fired by a stop switch is a mechanical event at a fixed touch.
  std::unordered_set<Id> keySwitchIds_;
  // The key switches held down, by the same key id soundingNotes_ uses, so a
  // note-off finds the switch its note-on engaged.
  std::unordered_map<int, Id> heldKeySwitches_;
  // Open pallets and the note id their voices sound under. Reserved at load
  // to the number of pallets, so opening one does not allocate.
  std::unordered_map<Id, uint64_t> palletNotes_;
  // The strike velocity of the key that is moving the network now.
  int palletVelocity_ = 100;
  // A coupler carries a note to a division (or a keyboard) whose own key was
  // never pressed, so nothing opens the pipe there: the organ reaches its
  // pipework by pressing the key, and no key was pressed. The organ's own
  // answer is the switch that carries that note in - the division's input
  // switch, or the destination keyboard's key - and the coupler closes it the
  // way a key does.
  //
  // It closes it through a switch of the engine's own, wired to the organ's
  // switch the way the key is. The wiring then decides what opens, as it
  // does for a key, and a key and a coupler can hold the same input at once:
  // the network releases a destination only when no other wire into it is
  // still holding it.
  struct CoupledInputKey {
    Id couplerSwitch = 0;
    Id destination = 0;
    int midiNote = 60;
    bool operator<(const CoupledInputKey& other) const {
      if (couplerSwitch != other.couplerSwitch) return couplerSwitch < other.couplerSwitch;
      if (destination != other.destination) return destination < other.destination;
      return midiNote < other.midiNote;
    }
  };
  struct CoupledInput {
    Id own = 0;             // the engine's own switch
    Id input = 0;           // the division's input switch it closes
    bool rankless = false;  // no stop of the division names a rank
  };
  std::map<CoupledInputKey, CoupledInput> coupledInput_;
  // Which note holds which of the engine's switches, so the last note out
  // closes what it opened. Reserved at load: the audio thread adds to it.
  std::vector<std::pair<uint64_t, Id>> coupledHeld_;
  // The moves the couplers asked for, made once the walk that asked for them
  // is over. Moving a switch while keys are held reflows every held note, and
  // that must never run inside a loop over the held notes or their scratch.
  struct CoupledMove {
    Id switchId = 0;
    bool engaged = false;
    int velocity = 100;
  };
  std::vector<CoupledMove> coupledPending_;
  int coupledDepth_ = 0;
  bool coupledFlushing_ = false;
  // Held by every entry point that can ask for a move; the outermost one
  // makes the moves as it goes out of scope.
  struct CoupledScope {
    explicit CoupledScope(MasterpieceProcessor& p) : owner(p) { ++owner.coupledDepth_; }
    ~CoupledScope() {
      if (--owner.coupledDepth_ == 0) owner.flushCoupledInputs();
    }
    MasterpieceProcessor& owner;
  };
  // The engine's own switches start well above the ids an organ uses.
  static constexpr Id kCoupledSwitchBase = 0x40000000ull;
  void buildCouplerInputs();
  // True when a coupler carried the note and the engine's own switch closes
  // the division's input for it: the organ's wiring opens the pipe from
  // there, so the caller must not open it a second time.
  bool engageCoupledInput(const ExpandedNote& reached, uint64_t noteId, int velocity);
  void releaseCoupledInput(const ExpandedNote& reached, uint64_t noteId);
  void releaseCoupledInputs(uint64_t noteId);
  // Every coupled input let go at once: Panic, and the audio stopping.
  void releaseAllCoupledInputs();
  void flushCoupledInputs();
  void releaseCoupledSwitch(Id sw);
  // Division inputs (#345): the notes a division plays while a switch is
  // engaged, for the divisions the organ reaches that way from its keys.
  // Built after the key flow, which is told which keyboards those are.
  void buildDivisionInputs();
  // The program couplers: the switches, and what the matrix does with them.
  void buildProgramCouplers();
  void divisionInputMoved(Id switchId, bool engaged);
  std::unordered_map<Id, std::pair<int, int>> divisionInputBySwitch_;  // switch -> (division, note)
  // Inputs held and the note id their pipes sound under; reserved at load.
  std::unordered_map<Id, uint64_t> divisionInputNotes_;
  std::vector<ResolvedPipe> divisionInputScratch_;
  // The keyboard an unassigned channel falls back to when the organ declares
  // no assignment code. Resolved once at load.
  Id fallbackKeyboard_ = 0;
  // Reused every note-on: the key-flow walk must not allocate.
  KeyFlowScratch keyFlow_;
  std::vector<ExpandedNote> expandScratch_;
  uint64_t nextNoteId_ = 1;
  AudioGraphConfig graph_;
  juce::AudioProcessorValueTreeState apvts_;
  // The "simpleWavOnly" parameter as the audio thread last saw it. The
  // parameter is a host's handle on the no-DSP switch: when it moves, the
  // switch follows; when the switch is set here or read from an organ's file,
  // the parameter is brought in line (mirrorSimpleWavParameter).
  bool simpleParamSeen_ = false;
  void mirrorSimpleWavParameter();
#if MP_ENABLE_DSP
  // Keyed by enclosure id so a reload cannot mismatch filters and boxes.
  std::unordered_map<Id, dsp::EnclosureFilter> enclosureFilters_;
  // Bus layout, fixed at prepareToPlay: one bus per enclosure in a stable
  // order, then one final bus for everything unenclosed.
  std::vector<Id> busEnclosures_;
  std::unordered_map<Id, int> enclosureBusIndex_;
  int unenclosedBus_ = 0;
  // Scratch for one bus. Preallocated: the audio thread must not allocate.
  juce::AudioBuffer<float> busScratch_;
  // Every enclosure bus of a block side by side, numCh channels each, for
  // VoiceEngine::renderBuses(); and where each bus's channels begin.
  juce::AudioBuffer<float> busScratchAll_;
  std::vector<float* const*> busOuts_;
  // EQ per rank and division (RankEq.h): its group buses, numCh channels
  // each, after the enclosure buses; each rank's place in its tables; and
  // the ranks and divisions in that order.
  RankEq rankEq_;
  juce::AudioBuffer<float> eqScratch_;
  std::unordered_map<Id, int> rankEqIndex_;
  std::vector<Id> rankEqRanks_, rankEqDivisions_;
  void configureRankEq();
  // The live voicing's EQ to the audio, flat while the voicing or the DSP is
  // switched off. Under the audio lock, or while no block renders.
  void pushVoicingEq();
  std::unordered_map<Id, dsp::TremulantLfo> tremulantLfos_;

#endif
  double sampleRate_ = 48000.0;
  // Remembered so a mixer change can re-plan a bus convolver without waiting
  // for the next prepareToPlay.
  int maxBlock_ = 512;

  // The events of the take being played back, for this block: each with the
  // console it came from. The recorder fills it, the MIDI handling routes it.
  std::vector<MidiRecorder::Playback> recordedPlayback_;
  // Panic's releases for this block, sent after everything else in it.
  std::vector<juce::MidiMessage> panicNotes_;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterpieceProcessor)
};

} // namespace mp
