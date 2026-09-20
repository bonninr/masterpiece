// Masterpiece audio processor — JUCE AudioProcessor + APVTS.
// Standalone + VST3/AU/LV2 from ONE codebase (no Projucer, CMake only).
// ODF parse (background) -> immutable OrganModel -> AudioGraph (bus list, rank->bus, IR sends).
// User data (combinations/voicing/MIDI/favourites) as ValueTree + separate files per organ + global.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../mp_core/OrganModel.h"
#include "../mp_control/Control.h"
#include "../mp_control/MidiMap.h"
#include "../mp_control/Combinations.h"
#include "../mp_control/StageSwitches.h"
#include "../mp_control/Stepper.h"
#include "../mp_control/LcdPanel.h"
#include "../mp_control/WindSolver.h"
#include "../mp_control/SwitchNetwork.h"
#include "../mp_core/Temperament.h"
#if MP_ENABLE_DSP
#include "../mp_dsp/Dsp.h"
#endif

#include "Convolver.h"
#include "Metronome.h"
#include "AudioRecorder.h"
#include "MidiRecorder.h"
#include "SampleLibrary.h"
#include "MixerConfig.h"
#include "VoicingSet.h"
#include "Favourites.h"
#include "../mp_core/OdfLoader.h"
#include "../mp_sampler/StreamingEngine.h" // ParallelConfig
#include "../mp_sampler/VoiceEngine.h"

#include <array>
#include <atomic>
#include <mutex>
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
  MasterpieceProcessor();
  ~MasterpieceProcessor() override = default;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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
  void getStateInformation(juce::MemoryBlock&) override {}
  void setStateInformation(const void*, int) override {}

  // Organ loading. `loadOrgan` is synchronous and is what the headless
  // renderer and the tests use; `loadOrganAsync` runs the same work off the
  // message thread so the UI stays responsive on a 19 GB set.
  struct LoadResult {
    bool ok = false;
    std::string error;
    OdfDiagnostics diagnostics;
    SampleLoadReport samples;
    int stopsEngaged = 0;
  };
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
  // Ask the running load to stop. Returns immediately; the load ends at the
  // next file boundary and reports itself cancelled.
  void cancelLoad() {
    loadProgress_.cancelled.store(true, std::memory_order_release);
    juce::Logger::writeToLog("load: cancel requested");
  }
  const OrganModel& organModel() const { return model_; }
  const SampleLibrary& sampleLibrary() const { return samples_; }

  // The on-screen keyboard plays through the same path as a MIDI device: its
  // events are merged into the incoming buffer at the top of processBlock, so
  // there is exactly one note path rather than a second one for the mouse.
  // Device input is mirrored back into this state (display only, never
  // re-injected), so the drawn manuals and the piano strip light up for an
  // external console too.
  juce::MidiKeyboardState& keyboardState() { return keyboardState_; }

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
  void releaseAllKeys() { releaseAll_.store(true, std::memory_order_release); }

  // Stops in a stable order for the console: by division, then by id, so the
  // jamb does not reshuffle between loads.
  struct StopEntry { Id stopId = 0; Id divisionId = 0; std::string name; bool playable = false; };
  std::vector<StopEntry> stopList() const;
  bool stopEngaged(Id stopId) const { return engagedStops_.count(stopId) != 0; }

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
  void setCaptureMode(bool on) { combinations_.setCaptureMode(on); }
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

  // --- the registration sequencer ---------------------------------------
  // One thumb piston that walks the organ's generals in order. Not wired in
  // the organ file — Hauptwerk provides it and the player maps it — so it is
  // driven from here and from MIDI, never from a drawstop.
  //
  // With the setter held, stepping CAPTURES into the frame it lands on, which
  // is how a registration is built for a piece: hold the setter and walk
  // forwards, setting each frame as you go.
  bool stepperNext();
  bool stepperPrev();
  bool stepperGoto(int frame);
  const Stepper& stepper() const { return stepper_; }
  void stepperRewind() { stepper_.rewind(); }
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
  void cancelKeyboardLearn() { keyboardLearn_ = 0; }
  Id keyboardLearning() const { return keyboardLearn_; }

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
  void setKeyboardForChannel(int channel, Id keyboardId, int deviceId = 0) {
    midiMap_.removeKeyboardBindingsFor(keyboardId);
    // Take the channel rather than share it. Without this the comment below
    // was untrue: three manuals ended up claiming channel 1 on a real saved
    // mapping, which made two of them unplayable and sent every manual to the
    // pedal.
    midiMap_.releaseChannel(channel, deviceId, keyboardId);
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
  int midiMapRepairedOnLoad() const { return midiMapRepaired_; }
  // Register a console and get its id. Message thread, at device setup.
  int registerMidiDevice(const juce::String& name) {
    return midiMap_.devices().idFor(name.toStdString());
  }
  const MidiDeviceMap& midiDevices() const { return midiMap_.devices(); }

  MidiMap& midiMap() { return midiMap_; }
  const MidiMap& midiMap() const { return midiMap_; }
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

  // What to reopen when the program starts with no organ named. Held with the
  // global defaults because it belongs to the program, not to any one organ.
  juce::File lastOrgan() const;
  juce::File loadedOdf() const { return loadedOdf_; }
  bool reopenLastOrgan() const { return reopenLastOrgan_; }
  void setReopenLastOrgan(bool on);
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
  juce::File cacheDirectorySetting() const { return cacheDir_; }

  // The root data directory for Masterpiece settings, cache, and installed organs.
  static juce::File dataDirectory();

  // Known / previously used ODF locations. Global, saved in settings.mpglobal.
  const std::vector<juce::File>& recentOrgans() const { return recentOrgans_; }
  void addRecentOrgan(const juce::File& odf);
  void removeRecentOrgan(const juce::File& odf);

  // Hidden / ignored organs (e.g. removed from list while keeping files on disk)
  const std::vector<juce::File>& hiddenOrgans() const { return hiddenOrgans_; }
  void hideOrgan(const juce::File& odf);
  void unhideOrgan(const juce::File& odf);
  bool isOrganHidden(const juce::File& odf) const;

  // Unload currently loaded organ
  void unloadOrgan();
  void setLoadTicks(bool on);
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
  // Raised whenever something a settings file holds is changed, so the message
  // thread can write it without the audio thread touching a disk.
  void markSettingsDirty() { settingsDirty_.store(true, std::memory_order_release); }
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
  // A mapping learned on the audio thread, written here.
  bool saveMidiMapIfDirty();
  bool saveMidiMap() const;
  bool loadMidiMap();

  // --- practice and session tools -------------------------------------
  // All three run inside the audio callback, because anything that has to
  // stay in time with the organ cannot live outside it.
  Metronome& metronome() { return metronome_; }
  MidiRecorder& recorder() { return recorder_; }
  // Captures what leaves, minus the metronome. See AudioRecorder.
  AudioRecorder& audioRecorder() { return audioRecorder_; }
  Convolver& convolver() { return convolver_; }

  // MIDI OUT. A physical console lights its own drawstops from what the organ
  // sends back, so engaging a stop on screen has to reach the hardware.
  void setMidiOutput(juce::MidiOutput* out) { midiOut_ = out; }
  juce::MidiOutput* midiOutput() const { return midiOut_; }
  // Echo switch changes to the console. Off by default: a console that echoes
  // what it just sent can latch itself into a loop.
  void setMidiFeedbackEnabled(bool on) { midiFeedback_ = on; }
  bool midiFeedbackEnabled() const { return midiFeedback_; }

  // Console LCD panels. Configured by the player, because the framing belongs
  // to their hardware and not to the organ — see LcdPanel.h.
  LcdPanels& lcdPanels() { return lcd_; }
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

  VoicingAB& voicing() { return voicing_; }
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
  // Re-sends every line regardless, for a console plugged in mid-session and
  // for the settings page's test button.
  int refreshLcdPanels();

  // --- memory / streaming ----------------------------------------------
  // How much of each sample is preloaded. The head is a MINIMUM: it is always
  // extended to cover the sustain loop, because a sample whose loop is missing
  // does not sustain. 0 loads whole files.
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
  bool loadMono() const { return samples_.loadMono(); }

  // The sample cache: a previous load of this organ at these settings, kept
  // so the next one is a read instead of twelve thousand decodes. One file by
  // default, replaced as organs change, because these run to gigabytes.
  void setCacheMode(SampleLibrary::CacheMode m) { samples_.setCacheMode(m); }
  SampleLibrary::CacheMode cacheMode() const { return samples_.cacheMode(); }
  int64_t cacheBytesRead() const { return samples_.cacheBytesRead(); }
  int64_t cacheBytesWritten() const { return samples_.cacheBytesWritten(); }

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
  void setStreamSeconds(double s) { voices_.setStreamSeconds(s); }
  // Zero unless the disk could not keep up, in which case a release went
  // silent partway and the player deserves to know.
  int64_t streamUnderruns() const { return voices_.streamUnderruns(); }

  // Where the organ was loaded from — the console needs it to resolve artwork
  // out of the same installation packages the audio comes from.
  const std::string& organRootDir() const { return organRootDir_; }

  // Where this organ's OrganInstallationPackages lives, when the definition's
  // own path does not lead there -- a folder linked in from another tree, for
  // instance. Empty means work it out from the path, which is the usual case.
  // Set before loading; saved with the organ's other settings.
  juce::File organRootOverride() const { return organRootOverride_; }
  void setOrganRootOverride(const juce::File& dir) { organRootOverride_ = dir; }

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
    graph_.engineSwitch = sw;
  }
  const EngineSwitch& engineSwitch() const { return graph_.engineSwitch; }

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
  void startNoteOnKeyboard(Id keyboard, int noteKeyId, int midiNote,
                           int velocity);
  void stopNoteByKey(int noteKeyId, int velocity);
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
  // Shoe positions that move switches: the crescendo, the blower, enclosure
  // noises. Needs the previous position, because each row is a crossing.
  Stepper stepper_;
  // The wind system. Advanced once per block on the audio thread, so its
  // tables are sized at load and it never allocates here.
  WindSolver wind_;
  // One entry per modelled windchest, in the solver's own order, handed to the
  // voice engine each block. Voices carry an index into it.
  std::vector<VoiceEngine::WindMod> windMods_;
  std::vector<Id> windOrder_;
  std::unordered_map<Id, int> windIndexOf_;
  // Which windchest each pipe stands on, resolved at load so a note-on does
  // not search for it.
  std::unordered_map<Id, int> pipeWindIndex_;
  // Tremulants, in a stable order, and which one reaches each pipe. Both
  // resolved at load: a note-on must not search, and a voice started in one
  // block must still point at the right tremulant in the next.
  std::vector<Id> tremOrder_;
  std::unordered_map<Id, int> tremIndexOf_;
  std::vector<VoiceEngine::TremMod> tremMods_;
  void advanceTremulants(int numFrames);
  std::vector<float> windDemand_;
  // Advance the wind by one block: ask the engine what is drawing air, solve,
  // and hand the result back for the voices to sound through.
  void advanceWind(int numFrames);
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
  juce::MidiKeyboardState keyboardState_;
  // Raised by releaseAllKeys(), consumed at the top of the next block.
  std::atomic<bool> releaseAll_{false};
  // The organ's tuning, resolved once at load. Held by value so the audio
  // thread never chases a pointer into the model while it is being swapped.
  Temperament organTuning_;
  std::unordered_set<Id> engagedStops_;
  std::atomic<bool> logMidi_{false};
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
  // A drawstop on the console IS a switch; clicking it must draw the stop, not
  // merely animate the picture. Built at load so the audio thread never
  // searches for it.
  std::unordered_map<Id, Id> stopBySwitch_;
  MidiMap midiMap_;
  Metronome metronome_;
  MidiRecorder recorder_;
  AudioRecorder audioRecorder_;
  Convolver convolver_;
  // The organ's declared output trim as a linear gain, resolved once at
  // load. 1.0 for a set that declares none.
  float organTrimGain_ = 1.0f;
  double windDepth_ = 1.0;
  LoadProgress loadProgress_;
  bool applyOrganTrim_ = true;
  juce::MidiOutput* midiOut_ = nullptr; // owned by the application
  bool midiFeedback_ = false;
  LcdPanels lcd_;
  // The player's mixer. Defaults to one stereo bus, which makes the whole
  // routing path a no-op until someone configures something.
  MixerConfig mixer_ = MixerConfig::stereoDefault();
  VoicingAB voicing_;
  Favourites favourites_;
  // Empty means the organ's default set.
  std::string combinationSet_;
  std::vector<BusId> mixBusOrder_;              // dense index -> BusId
  std::unordered_map<int, int> mixBusIndexOf_;  // BusId.value -> dense index
  std::vector<juce::AudioBuffer<float>>* mixBusCapture_ = nullptr;
  // One convolver per dense bus index, built only for buses that declare an
  // IR. unique_ptr because a Convolver holds an FFT plan and is neither cheap
  // nor movable, and most buses will never have one.
  std::vector<std::unique_ptr<Convolver>> busConvolvers_;
  // Per-bus scratch, needed only when a bus has its own room: without one the
  // buses sum straight into the output and cost nothing.
  juce::AudioBuffer<float> mixScratch_;
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
  std::atomic<float> outPeak_[2]{{0.0f}, {0.0f}};
  float peakHeld_[2] = {0.0f, 0.0f};
  // Per-block decay, worked out once in prepareToPlay. A host that varies its
  // block size shifts the fall slightly; nobody can see that in a lamp, and
  // it is not worth an exp() per callback to correct.
  float meterFall_ = 0.5f;

  int64_t preloadHead_ = 0;
  bool reopenLastOrgan_ = true;
  std::atomic<bool> loadTicks_{false};
  juce::File cacheDir_; // empty: the default place
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
  std::vector<juce::File> recentOrgans_;
  std::vector<juce::File> hiddenOrgans_;
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
  // The defaults as they stand on disk, kept verbatim so that writing the
  // file for any other reason cannot rewrite them from whatever is loaded.
  juce::String globalBody_;
  bool writeGlobalFile() const;
  // Reused every block so the audio thread never allocates one.
  juce::MidiBuffer outgoing_;
  // Tagged input, filled by the device callbacks and drained by the audio
  // thread. A fixed ring so neither side allocates; a single producer per slot
  // is not guaranteed (several devices push), so the write index is atomic and
  // the whole thing is sized to make an overflow implausible rather than
  // impossible — a dropped message beats a blocked MIDI thread.
  struct TaggedMidi {
    int deviceId = 0;
    uint8_t bytes[3] = {0, 0, 0};
    int size = 0;
  };
  static constexpr int kMidiQueueSize = 2048;
  std::array<TaggedMidi, kMidiQueueSize> midiQueue_{};
  std::atomic<uint32_t> midiWrite_{0};
  uint32_t midiRead_ = 0;
  // Drain the tagged queue into `midi` before it is handled, so device-aware
  // and host-delivered messages take exactly the same path afterwards.
  void drainTaggedMidi(std::vector<std::pair<int, juce::MidiMessage>>& out);
  std::vector<std::pair<int, juce::MidiMessage>> midiScratch_;
  // The console the message being handled came from. Set as each message is
  // dispatched rather than threaded through every call, because only the note
  // path cares and threading it would touch a dozen signatures.
  int noteDeviceId_ = 0;
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
  };
  std::unordered_map<int, HeldNote> soundingNotes_;
  // The registration the sounding notes were started with, owned by the audio
  // thread, so a change can be told from what is already playing.
  std::unordered_set<Id> appliedStops_;
  std::atomic<bool> stopsChanged_{false};
  std::vector<Id> stopDiffScratch_;
  std::unordered_set<Id> stopSetScratch_;

  // Start the voices one key press asks for, drawing only on `stops`. The
  // whole registration when a key is struck; only what just moved when a stop
  // changes under a key already down. Returns whether any pipe answered.
  bool startVoicesForKey(Id keyboard, int midiNote, int velocity,
                         uint64_t noteId, const std::unordered_set<Id>& stops);
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
  void fireMovedStages();
  // False while a load is rebuilding the stage table; see loadOrgan().
  std::atomic<bool> stagesReady_{false};
  // False until a load has finished; pallets start no voices before then.
  std::atomic<bool> palletsLive_{false};
  void palletMoved(Id switchId, bool engaged);
  // Pallet switch -> the pipes it opens, with their rank.
  std::unordered_map<Id, std::vector<std::pair<Id, const Pipe*>>> palletPipes_;
  // (keyboard, note) -> the switch that key IS, for organs that declare one.
  std::unordered_map<int, Id> keySwitchByKey_;
  // The key switches held down, by the same key id soundingNotes_ uses, so a
  // note-off finds the switch its note-on engaged.
  std::unordered_map<int, Id> heldKeySwitches_;
  // Open pallets and the note id their voices sound under. Reserved at load
  // to the number of pallets, so opening one does not allocate.
  std::unordered_map<Id, uint64_t> palletNotes_;
  // The strike velocity of the key that is moving the network now.
  int palletVelocity_ = 100;
  // The keyboard an unassigned channel falls back to when the organ declares
  // no assignment code. Resolved once at load.
  Id fallbackKeyboard_ = 0;
  // Reused every note-on: the key-flow walk must not allocate.
  KeyFlowScratch keyFlow_;
  std::vector<ExpandedNote> expandScratch_;
  uint64_t nextNoteId_ = 1;
  AudioGraphConfig graph_;
  juce::AudioProcessorValueTreeState apvts_;
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
  std::unordered_map<Id, dsp::TremulantLfo> tremulantLfos_;

#endif
  double sampleRate_ = 48000.0;
  // Remembered so a mixer change can re-plan a bus convolver without waiting
  // for the next prepareToPlay.
  int maxBlock_ = 512;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MasterpieceProcessor)
};

} // namespace mp
