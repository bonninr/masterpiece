# Architecture

This page describes how Masterpiece is put together: the modules under `src/` and which of them are free of JUCE, the path from an organ file to sound, the switch network that models a console's wiring, the threads and the rules the audio thread follows, and the vector engine. After reading it you can find where a behaviour lives and what a change there may and may not do.

## Modules

Masterpiece is one engine and two front ends: the standalone app (`apps/MasterpieceApp`) and the plugins (`apps/MasterpiecePlugin`). Both drive the same `MasterpieceProcessor`.

| Module | Folder | JUCE | Content |
|---|---|---|---|
| `mp_core` | `src/mp_core` | none | The organ model (`OrganModel.h`), the loader for Hauptwerk-format definitions (`OdfLoader`), the compiler for compact definitions (`CodmCompiler`), the GrandOrgue importer (`GrandOrgueImport`), temperaments and pitch (`Temperament`), perspectives (`Perspectives`), case-insensitive path lookup (`PathCase`), UTF-8 path helpers (`Utf8Path.h`), keyboard geometry (`KeyboardLayout.h`), load progress (`LoadProgress.h`) |
| `mp_archive` | `src/mp_archive` | none | `OrganArchive`: RAR packages and GrandOrgue `.orgue` packages read in place |
| `mp_sampler` | `src/mp_sampler` | none | `VoiceEngine` (voices, stealing, interpolation, release streaming, worker threads), `SimdRun` (vector inner loops), attack and release selection (`StreamingEngine`) |
| `mp_control` | `src/mp_control` | none | `SwitchNetwork`, key flow (`Control`), `CombinationSystem`, `PlayerCombinations`, `StageSwitchBank`, `WindSolver`, `MidiMap`, `MidiDeviceMap`, `LcdPanel`, `Registration` |
| `mp_split` | `src/mp_split` | none | The split keyboard: `SplitKeyboard` (the engine) and `SplitText` (its files) |
| `mp_dsp` | `src/mp_dsp` | `juce_dsp` | Tremulant LFO and enclosure filter |
| `mp_audio` | `src/mp_audio` | yes | `MasterpieceProcessor`, `SampleLibrary`, sample cache, WavPack format, convolution reverb, metronome, recorders, split keyboard host, translations |
| `mp_ui` | `src/mp_ui` | yes | The console, settings windows, panels, loading dialog, mobile layout |

`mp_core`, `mp_archive`, `mp_sampler`, `mp_control` and `mp_split` include no JUCE header. Their CMake files link JUCE modules when JUCE targets exist, but no source in them uses one. This boundary is what lets the test suite run most of the engine against synthesised tones in seconds. Several headers in `mp_audio` are JUCE-free too and are tested directly: `AudioGraph.h`, `MixerConfig.h`, `VoicingSet.h`, `Favourites.h` (and `VoicingEq.h`).

The dependency direction is `mp_core` at the bottom; `mp_control`, `mp_sampler`, `mp_split` and `mp_archive` above it; `mp_dsp` and `mp_audio` above those; the executables on top.

`mp_ui` is an `INTERFACE` target. Its sources are compiled into `mp_audio`, because `MasterpieceProcessor::createEditor()` is a member of a class declared there; an editor in a separate library layered above it made the linker drop the processor's vtable entries. `mp_ui` remains so that link lines of the app and the plugin keep working.

### mp_core

`OrganModel` is the in-memory form of an organ: ranks and their pipes, stops, switches and their linkages, divisions, keyboards, key actions, combinations, enclosures, tremulants, wind compartments, continuous controls, display pages and image sets, and the sample and package tables. The loader fills it and the rest of the program reads it. After a load the processor still edits it in two places (pruning releases the player left out, and marking releases that hold nothing).

`OdfLoader` reads three kinds of definition:

- Full definitions (`.Organ_Hauptwerk_xml`).
- Compact definitions (`.CustomOrgan_Hauptwerk_xml`), which `CodmCompiler` completes with the fixed defaults a full definition would carry.
- GrandOrgue definitions (`.organ`). `convertGrandOrgue()` translates the INI-style file into an equivalent full definition as an XML string in memory, never written to disk, and the loader parses that. Nothing after this step knows the organ came from a `.organ` file. Where the two formats differ, the importer follows GrandOrgue's semantics and records what it approximated or left out as notes, which the loader reports as warnings starting with `GrandOrgue import:`.

The parse uses pugixml and loads the whole document into memory.

Unknown tables and attributes produce warnings and are kept; they do not fail the load. `OdfDiagnostics` collects errors, warnings, skipped items and a long list of validation findings (dangling ids, missing sample files, unmapped codes, release branches with no attack, and others). The loader only errors on malformed XML or fatal id faults.

`Perspectives` groups ranks by the microphone position named in their rank names (`Flute 8 (close)`, `Principal 8 Rear`); a GrandOrgue set has no perspectives, and its windchest groups stand in for them.

### mp_archive

`OrganArchive` finds the archives that belong to a chosen file (the file itself, its volumes and its sibling packages), reads their headers and serves their contents. RAR can only be read front to back, so samples are delivered in archive order, one pass per archive, and never looked up one at a time. The small files (definition, artwork, package definitions) are unpacked once into `<application data>/Masterpiece/Packaged/<identity>/`, where the loader finds an ordinary organ; the samples stay in the archives and are decompressed into memory when needed. An index (`archive-index.txt`) is saved beside the unpacked files so a solid archive is walked once.

libarchive reads ZIP (the `.orgue` format), RAR 5 and non-solid RAR 4. unarr reads solid RAR 4, which libarchive refuses. Encrypted archives are refused with a message. A platform can install a hook (`setArchiveOpenHook`) for packages it opens through descriptors; Android uses it for files chosen in the system picker.

### mp_sampler

`VoiceEngine` makes the sound. It is described under [Threads](#threads) and [The vector engine](#the-vector-engine). `selectAttack` and `selectRelease` implement the selection matrices: an attack is the first one in file order whose velocity, time-since-close and continuous-control ceilings the strike satisfies; a release is the one with the tightest hold-time ceiling among those that match the key-off context, preferring releases linked to the attack that played.

`DiskProbe` (a storage-tier classifier), `SampleHandle` and `StreamConfig` are declared in this module and exercised by tests. The application does not call them: release streaming runs inside `VoiceEngine` and `SampleLibrary`.

### mp_control

| Class | Job |
|---|---|
| `SwitchNetwork` | The organ's switches and the wires between them; see [The switch network](#the-switch-network) |
| `CouplerMatrix` (`Control.h`) | Key flow: which divisions a key press reaches and at what transposition |
| `ContinuousControlBank` (`Control.h`) | The position of every shoe, wheel and generic control, and the linkages that let one drive another, settled by repeated relaxation |
| `StageSwitchBank` | Shoe positions that fire switches: crescendo steps, blower start, delays. Each row is a threshold crossing, so sweeping a control fires every threshold it passes in order |
| `CombinationSystem` | The organ's own pistons: recall and capture of a combination as a list of switch states |
| `PlayerCombinations` | The player's generals, divisionals, cancels and stepper, the same on every organ, saved in the player's data |
| `WindSolver` | Pressures of the wind compartments, advanced once per block |
| `MidiMap`, `MidiDeviceMap` | What a player's console does to the organ, and which physical device a message came from |
| `LcdPanel` | System-exclusive text for a console's display |
| `Registration.h` | Choosing stops on an unfamiliar organ by classifying stop names into families |

Combination recall never moves a switch itself. It returns the changes, and the processor applies them through `SwitchNetwork`, so the organ's own wiring follows. What a player captures is saved in the player's data beside the MIDI map and never written into the sample set.

`WindSolver` solves compartment volumes, pipe mass-flow rates, links and valves: flow through an opening goes as the square root of the pressure difference, and a compartment's pressure follows from the air in it. A reservoir counts as its swallowed volume (frame area times travel). The board's mass, damping and spring are not modelled. The solver lives in `mp_control` because it is control-rate state that advances once per block and touches no sample.

### mp_split

A single keyboard playing a whole organ. The split sits between the MIDI inputs and the organ. It re-sends the notes of one keyboard as notes of its own input, named `Split keyboard`, on channels 11 to 14 (one per role: lower, upper, solo, pedal). The player's MIDI map sends each of those channels to a manual like any keyboard's. The module works on notes and channels alone. `SplitKeyboardHost` in `mp_audio` carries messages in and out and keeps the files (`split/<organ>.mpsplit` and `split-keyboard.txt`).

### mp_dsp

`TremulantLfo` and `EnclosureFilter` use JUCE's `juce::dsp` classes. The enclosure is a `StateVariableTPTFilter` whose cutoff and gain follow a smoothed shutter position. `MP_ENABLE_DSP=OFF` drops the module at configure time; the `EngineSwitch` flags in the model switch each feature off at run time per organ.

### mp_audio

`MasterpieceProcessor` is a `juce::AudioProcessor` and owns everything: the model, the switch network, the control bank, the combinations, the wind solver, the voice engine, the sample library, the MIDI map, the mixer and the files. `SampleLibrary` turns the model's sample rows into resident buffers and optional streaming tails. `WavPackFormat` is a JUCE audio format registered ahead of the WAV reader; it recognises WavPack streams by content, because GrandOrgue sets name them `.wav`, and exposes the `smpl` and `cue` chunks under the names JUCE's WAV reader uses. `Convolver` is the room (impulse-response) reverb, `Metronome` and the two recorders run inside the audio callback or hand blocks to a background writer, and `Translations` embeds the language files.

### Applications

`apps/MasterpieceApp/Main.cpp` owns the audio device and the MIDI inputs and runs `AudioProcessorPlayer` over the processor. Each MIDI input has a `DeviceRoute` that tags a message with its device and pushes it into the processor's queue. `JackMidi.cpp` adds a JACK MIDI input port on Linux, opened only when a JACK server already runs. `MasterpiecePlugin` is one function, `createPluginFilter`.

## The load path

`MasterpieceProcessor::loadOrgan` does the work and `loadOrganAsync` runs it on a JUCE thread. `LoadProgress` (atomic, allocation-free) carries the phase, counts and cancel flag to the dialog's timer on the message thread.

| Phase (`LoadProgress::Phase`) | What happens |
|---|---|
| `OpeningArchive`, `ReadingArchive`, `ExtractingArchive` | `openPackagedOrgan`: for a `.rar` or `.orgue`, find and index the archives and unpack the small files |
| `ReadingDefinition` | Remember the previous organ's state, load the organ's saved settings, then parse the definition (`OdfLoader::load`; a `.organ` file is converted first) |
| `BuildingWind` | Reset the combination system and the wind solver, assign chest indices |
| `WiringConsole` | Reset the stage table, run the start-up controls, settle the wind, index the linkages and the stops |
| `FindingSamples` | Free the previous organ's audio, list the samples the pipework references, look for them on disk and in the cache |
| `LoadingSamples` | Decode samples on worker threads |
| `SavingCache` | Write the sample cache when it is set to be saved before the organ plays |
| `Preparing` | Rebuild the voice pool and everything derived from the model; load the MIDI map and combinations saved for the organ |
| `BuildingConsole` | Decode artwork on the message thread |
| `Done`, `Cancelled`, `Failed` | End states |

The details that matter when changing the path:

- The model is parsed first, then published, then the audio is read, so a note arriving during a load resolves pipes that merely have no sound yet.
- `EngineSuspension` sets `engineSuspended_` and waits for a running block to leave. From then until `loadOrgan` returns, `processBlock` outputs silence and drops incoming MIDI. The audio thread therefore touches none of the structures the load rebuilds.
- Only the samples the pipework references are read, which can be far fewer than the rows of the Sample table: a demo set lists the whole instrument and ships part of it.
- A load can run with `graphicsOnly`, which builds the model and console and reads no audio (`--gui-only`).
- The memory budget is the memory limit less the voice pool; a load that reaches it stops and reports `outOfMemory`.
- `SampleLibrary::loadAll` decodes on a pool of threads (by default twice the core count, clamped to 4 to 32) because decoding is bound by input and output. Samples held in archives are read by one producer thread per archive, front to back, into a bounded queue (512 MB) that the decoder threads drain.
- The result is published as a new immutable generation behind one atomic pointer. Old generations are freed by `retireOldGenerations()` on the message thread with audio stopped.
- The sample cache is written on a thread of its own after publishing, unless the player chose to save before the organ plays. A load that was cancelled, stopped at the memory limit or had failed files is never cached.

### From a key press to a voice

1. A MIDI message arrives on a device thread and is pushed into a fixed ring (`TaggedMidi`, 2,048 slots of 256 bytes) tagged with its device id. The on-screen keyboard and a MIDI file playback are merged into the same buffer at the top of `processBlock`, so the mouse and a console share one note path.
2. `handleMidi` matches the message in `MidiMap` and acts: a note on a manual, a switch, a control, a piston, a console action.
3. A note goes through `CouplerMatrix::expandInto`, a breadth-first walk over the organ's key actions from the source keyboard. A manual's own keys reach their division through an ungated edge, and a coupler is the same edge gated on a switch. The walk carries the accumulated transposition, is depth-bounded and remembers where it has been.
4. For each division reached, `resolvePipesInto` maps the note through every engaged stop's rank window to the pipes that sound.
5. `startPipeLayers` starts one voice per layer: it selects the attack, computes the playback ratio from the temperament and the sample's own pitch, applies velocity attenuation, the player's voicing, the wind chest and tremulant of the pipe and the layer's loop settings, and calls `VoiceEngine::startVoice`.
6. A key release calls `noteOff`, which selects a release from the key-off context and crossfades from the attack to it over the release's own crossfade length.

Organs whose pipework hangs off pallet switches reach their pipes through the switch network (`palletMoved`) and skip the key path; the voice is the same.

## The switch network

`src/mp_control/SwitchNetwork.h` opens with a long explanation. The essentials:

On a console the switch a player clicks is rarely the one anything reads. A coupler's drawn knob is one `SwitchLinkage` away from the node that key actions look at, and a large organ declares thousands of such wires. If the wires are ignored, a coupler moves on screen and couples nothing, a piston lights and registers nothing, and the crescendo is inert.

- **Switches latch and changes propagate.** A switch keeps its state until something moves it. Moving it fires its outgoing wires, which move their destinations, until nothing changes. A model that recomputed every switch from its inputs cannot express a general cancel, which pushes the stop knobs out by turning the logical stop switches off and letting the drawn knobs follow through the reverse wires.
- **Loops terminate** because propagation stops when a switch is already in the state it is being set to. A drawn knob and the logical switch behind it drive each other this way.
- **Each wire has an engage action and a disengage action.** When the source (and the wire's condition switch, if there is one) is in the state that fires it, the wire does its engage action; when it stops firing, its disengage action. Codes 1 and 4 engage, 2 and 7 disengage, and engage code 3 toggles the destination on each firing (a reversible piston) and does nothing when the wire stops firing. Together these cover a plain follow (1 and 2, or 4 and 7), a reversible (3 and 7) and an inverting wire (7 and 4). Codes outside these are recorded in `OdfDiagnostics::unmappedLinkageCodes` for review.
- **An idle wire asserts nothing.** An organ whose pipework hangs off pallet switches carries one wire per tremulant state on each pallet; with the tremulant off, only one fires. Applying the idle wire's disengage action closed the pallet in the same instant the key opened it.
- **A destination is the OR of the wires firing into it.** A disengage is applied only when no other firing wire asserts engagement. Mirrors are excluded from that count: when a wire runs a to b and another runs b to a, they are copies of one control (a stop drawn on several pages), and each would otherwise hold the other on.
- **`holdOff`** lists switches nothing may engage, for a demo set's locked stops whose ranks have no pipes. A piston, the crescendo, a wire or a click that would draw one leaves it in.
- `lastChanges()` lists every switch that moved during the last `set()`, so a noise belongs to the switch that actually moved. `lastChangeRanAway()` reports that propagation hit its limit, which a file with flip-flopping wiring can cause.
- `reset()` builds the indices and starts every switch at its declared default; every wire firing at that state asserts its destination, so an organ that ships with its blower running comes up that way.

The processor keeps the player-facing switch for each stop by walking the reversed linkages to find the drawn one upstream. A switch wired straight into the nodes of several stops is a registration aid (a tutti, a general) and counts as the knob of none of them.

## Threads

| Thread | Work |
|---|---|
| Audio thread | `processBlock`: merges MIDI, resolves notes, advances controls and wind, renders the voices, applies the room, the fader, the recorder, the metronome and the meter |
| Render workers | Optional extra threads that render a share of the voices each block |
| Streaming thread | One thread in `VoiceEngine` that refills the ring buffer of every voice playing a streamed release tail |
| Loading threads | `loadOrganAsync` thread, `SampleLibrary` decoder pool and archive producers, the cache writer |
| MIDI threads | One per input device, owned by JUCE or the driver; they tag and enqueue only |
| Message thread | Window, timers, settings, anything that touches a component |
| Background writers | The audio recorder's `ThreadedWriter`, and JUCE's loader for the impulse response |

### Render workers

`VoiceEngine::setRenderThreads` starts the workers when the voice pool is built (`buildVoicePool`), never inside `render()`. The audio thread renders one share itself and waits on the rest. Below `minVoicesPerThread * threads` active voices (default 32 per thread) rendering stays inline. By default the thread count is the usable cores minus one (the cores the process may run on, found with `sched_getaffinity` on Linux), at most 8. Shares are fixed and summed in a fixed order on the audio thread, so the result is repeatable but not bit-identical to inline rendering, because the float additions group differently. Workers wait on a ticket for a few microseconds before they sleep on a condition variable, because the renders of consecutive blocks follow each other more closely than a thread can be woken.

### The voice pool

The pool is allocated once in `prepare()`. The default size is 725 voices per usable core, at least 1,536 and at most 16,384 (`maxPolyphonySetting`); with release streaming on, 1,536. A voice started when the pool is full takes the place of one dying away, then the quietest, and a note that just started is never stolen. Releasing voices are stolen longest-decayed first. A release tail ends once it has stayed below the cutoff level for a twentieth of a second.

### Release streaming

An attack's sustain loop must be resident, because the note dies without it. A release is played once, from its start, never looped, so only its first frames stay in memory and the rest is read from disk into a per-voice ring (two seconds by default, three quarters of a megabyte per voice at 48 kHz). The streamer thread keeps each armed ring ahead of its voice. A voice that outruns the streamer plays silence and counts an underrun, which the status line reports. The ring is single producer, single consumer: the streamer writes and publishes `filled`, the audio thread reads it. A generation counter stops a fill in flight for one voice from landing in the ring of the next.

### Rules for the audio thread

`processBlock` and everything it calls follow these:

- **No allocation, no locking, no disk.** Scratch vectors (`KeyFlowScratch`, the resolve and recall scratch buffers) are sized at load and keep their capacity. Threads start in `prepare`, not in `render`.
- **The sample store is read through one atomic pointer.** A note-on sees the whole old generation or the whole new one.
- **MIDI arrives through the fixed ring.** The write index is atomic; a slot is read only after its `ready` stamp says it is complete. An overflow drops a message; blocking a MIDI thread is worse.
- **The playing state belongs to the audio thread.** Switches, controls, voices and pistons change from other threads only under `MasterpieceProcessor::AudioLock`, which takes the callback lock JUCE holds around every block. It is re-entrant, and the window waits at most one block. When the lock is released, the audio thread publishes a copy of the engaged stops and switches for the window (`publishShown`) with a `try_lock`: if the window is reading, the next block tries again.
- **The window reads copies.** `shownStopEngaged` and `shownSwitchEngaged` read the published copy; `stopEngaged` and `switchEngaged` read the live sets and are for the audio thread.
- **Hand-offs from the message thread use flags or mailboxes.** `releaseAllKeys()` raises a flag that the next block acts on; the split keyboard's layouts travel through mailboxes; coefficients for the voicing equaliser are computed on the message thread and taken with a `try_lock`, so a busy lock keeps the previous filter for a block.
- **Anything that rebuilds the engine stops it first.** `releaseResources()` quiesces the render workers and the streamer before `prepare` rebuilds the pools; a device change on a loaded machine otherwise corrupted the heap.
- **Control-rate state advances once per block.** The wind, the controls and the stage switches never run per sample. The tremulant is the exception in effect: it is passed to the voice as a ramp (start value and step per frame) because at a 256-frame block a constant per block would step audibly.

## The vector engine

Most of a voice's life is a straight run: the key is held, the cursor is well inside the resident sample and short of the loop end, no crossfade is running and no tremulant moves the pitch. For those frames `SimdRun.cpp` interpolates and mixes several frames at a time. Loop wraps, crossfades, release fades, streamed tails and the tremulant stay on the ordinary per-frame path, which is also the whole engine on a CPU without a vector unit.

| `simd::Isa` | Unit |
|---|---|
| `Sse2` | Every x86-64 processor; four frames at a time |
| `Avx2` | x86-64 with AVX2, checked at start-up; eight frames at a time |
| `Neon` | ARM: always present on 64-bit, required by the 32-bit build |
| `None` | The per-frame path only |

`simd::detect()` picks the best unit. Setting `MASTERPIECE_SIMD=0` in the environment forces the per-frame path. The vector code reads the three resident formats (float, 16-bit, 24-bit) and gives the same answer as the per-frame reader: positions advance one frame at a time as there, and the Hermite interpolation uses the same operations in the same order, with no fused multiply-add. `VoiceEngine.cpp` and `SimdRun.cpp` are compiled with `-ffp-contract=off` on every compiler but MSVC, because Apple Clang and GCC would otherwise fuse a multiply and an add into one rounding. A test compares each unit that the machine offers against the per-frame path.

The setting **Faster audio engine** (`fasterEngine_`) is on for a program that has never been told otherwise: `fasterEngineByDefault()` returns `true`. It is saved in the global settings file.

Resident audio is held as `Pcm24` (three bytes, little-endian), `int16_t` with a per-file scale folded into the voice gain, or `float`. `renderVoiceFrom<T>` is instantiated per type so the inner loop never tests which one it reads.

## Front ends and platforms

| Front end | Notes |
|---|---|
| Standalone app | Owns the audio device (WASAPI and ASIO on Windows, ALSA and JACK on Linux, CoreAudio on macOS) and the MIDI inputs |
| Plugins | VST3, AU and LV2; the processor's `getStateInformation` stores the path of the loaded organ definition and `setStateInformation` loads it again, so the rest of the state lives in Masterpiece's own per-organ files |
| Phones and tablets | The app only. `mp_ui/Mobile.*` provides full-screen windows for touch; on iOS, `IosAccess.mm` reads picked documents in place |

The plugin and the app create the same editor through `MasterpieceProcessor::createEditor()`, and that editor (`mp_ui/Ui.cpp`) calls `openPackagedOrgan` to open RAR and `.orgue` packages.
