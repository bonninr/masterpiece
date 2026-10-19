# Audio, room and mixer

This page covers everything between the organ and the loudspeakers: the audio device and its latency, the signal chain, the output pairs of the mixer, the impulse-response room, the player's voicing of ranks and pipes, the master level, the metronome, and what to do when the sound breaks up. Use it to set up a device for low latency, to place a dry organ in a room, and to find the cause of dropouts.

The controls described here are on the **Audio**, **Room**, **Metronome**, **Mixer** and **Voicing** tabs of **Settings > General settings**. **Settings > Audio and MIDI...** opens the same window on the **Audio** tab. The memory and loading choices that decide how many voices a machine can render are in **Settings > Organ settings > Loading**; see [Memory, loading and streaming](memory.md) and [Small, medium and large organs](sizing.md).

## The signal chain

Each audio block passes through these stages, in this order.

| Stage | What it does | Bypassed by Simple WAV |
|---|---|---|
| Voices | Every sounding pipe is rendered, assigned to a mixer pair (see [Output routing](#output-routing-and-the-mixer)) and, where its rank is enclosed, filtered by its swell shades | Shades and tremulants are bypassed; the voices play |
| Voicing EQ | The ten-band equaliser of **Organ settings > Voicing EQ** | Yes |
| Room | The impulse-response convolution of the **Room** tab | Yes |
| Organ trim and master level | The organ's own output trim and the master fader, applied as one multiplication | No |
| Audio recorder | Captures the signal at this point, so a recording does not contain the metronome | No |
| Metronome, load-progress taps, speaker keep-alive tone | Added after the fader | No |
| Level meter | Reads the finished block | No |

The **Simple WAV only (bypass all DSP)** box in **Organ settings > Loading** (also the **No DSP** box in the top bar) switches off wind model, tremulants, enclosures, voicing EQ, convolution and detuning. The per-rank and per-pipe voicing of the **Voicing** tab, the organ trim and the master level stay active, because each is a single multiplication or ratio taken when a note starts. The per-rank and per-pipe voicing follows the **Voicing** box of **Organ settings > Loading**; see [Relation to Voicing EQ](#relation-to-voicing-eq).

## The audio device

Open **Settings > Audio and MIDI...**. The tab is the device selector with two additions below it: a latency line and the **Keep portable speakers awake** control.

![The Audio tab](../images/general-audio.png)

### Driver types

The **Audio device type** box lists the driver types the system offers. It appears when more than one type exists.

| System | Driver types | Notes |
|---|---|---|
| Windows | **Windows Audio**, **Windows Audio (Low Latency Mode)**, **Windows Audio (Exclusive Mode)**, **DirectSound**, **ASIO** | ASIO is compiled into every Windows build and lists the ASIO drivers installed on the machine. With an ASIO driver the **Control Panel** button opens the interface's own panel. |
| macOS | **CoreAudio** | |
| iPhone and iPad | **iOS Audio** | |
| Linux, Raspberry Pi | **ALSA**, **JACK** | JACK is compiled into the Linux builds and listed when the JACK library (`libjack.so.0`) is present, which includes the JACK layer of PipeWire. The JACK client is named `Masterpiece`. |
| Android | **Android Oboe** | Oboe prefers AAudio, which Android provides from version 8.0 (API 26), the lowest version the app supports. |

### Selector controls

| Control | What it does | Takes effect |
|---|---|---|
| **Audio device type** | Chooses the driver type | At once |
| **Output** (**Device** on drivers that join inputs and outputs) | Chooses the output device | At once |
| **Active output channels** | Lists the device's output channels as stereo pairs. The program opens the device with two output channels. | At once |
| **Sample rate** | The rates the device offers | At once |
| **Audio buffer size** | The buffers the device offers, each shown as `N samples (X ms)` | At once |
| **Test** | Plays a test tone on the output | At once |
| **Control Panel**, **Reset Device** | Shown for drivers that have their own control panel (ASIO) | |
| **Active MIDI inputs**, **MIDI Output** | The MIDI ports; see [MIDI and consoles](midi.md) | |

The selector offers one to eight output channels and no input channels. The advanced options are visible. A change to device, rate or buffer size stops the device and starts it again; sounding notes stop while the engine rebuilds its voice pool.

### Latency

The line under the selector reads: *Key to speaker: about X ms, of which the buffer is Y ms and the output Z ms, as the device reports it.* X is the sum of the buffer length and the output latency the driver reports. The line updates whenever the device changes. The convolution of the room is partitioned with a head at the audio block size, so it adds no latency of its own.

The keyboard-to-sound time also contains the delay of the MIDI interface and of the console, which the line does not include.

| Goal | Choice |
|---|---|
| Lowest latency | A native low-latency driver (ASIO on Windows, CoreAudio on macOS, ALSA or JACK on Linux) with a small buffer. At 48 kHz, 128 to 256 samples gives 2.7 to 5.3 ms of buffer. |
| Safety | A buffer of 256 to 512 samples. The first-run wizard recommends this range for most machines. |
| Large organs, many stops, convolution | The larger buffer. A buffer that is too small makes late blocks (see [Audio dropouts](#audio-dropouts)). |

A first start, and any start where the saved setup failed, sets the buffer to the available size nearest 10 ms (480 samples at 48 kHz). A setup that the player saved is left as it is.

### What happens at start and when the device changes

- The setup (driver, device, rate, buffer, channels) is written to `audio.xml` in the `Masterpiece` folder of the user's application data when the program exits. A crash loses the changes of that session.
- At start the saved setup is opened. If it fails to open, or opens with no active output channel, the factory default device is used and the reason is written to the log.
- A device that opens below 44.1 kHz (some virtual outputs default to 8 kHz) is moved to 48 kHz, or to 44.1 kHz when 48 kHz is missing.
- A MIDI input that another program holds is retried every three seconds.
- The latency line follows every change the device manager reports and is empty while no device is open.

The program renders at the device's sample rate. A sample set recorded at another rate is converted as it loads when **Sample rate** in **Organ settings > Loading** is set to 48 kHz or 44.1 kHz; with **As recorded** each voice is resampled during play.

### Keep portable speakers awake

| Field | Value |
|---|---|
| Name | **Keep portable speakers awake**, with a level slider beside it |
| What it does | Adds a 20 Hz sine under the organ so that a battery speaker on a cable never sees silence, switches itself off, and swallows the first notes while it wakes. The tone is added after the recorder, so recordings do not contain it. |
| Default | Off |
| Range | The slider runs from -80 dB to -40 dB in 1 dB steps and is shown at -60 dB when the box is off |
| Takes effect | At once |
| Stored | Program-wide, as `speakerkeepalive` in `settings.mpglobal` |
| Advice | Raise the level when the speaker still sleeps; lower it when the tone is audible |

### The audio engine

**Organ settings > Loading > Faster audio engine** renders several samples per instruction with the processor's vector unit. The box label shows what was found: **Faster audio engine: using AVX2**, **using SSE2** or **using NEON**, or **not available on this processor**.

| Unit | Processors |
|---|---|
| AVX2 | x86-64 processors with AVX2, checked at start |
| SSE2 | Every x86-64 processor without AVX2, four frames at a time |
| NEON | ARM processors |

| Field | Value |
|---|---|
| Default | On |
| Takes effect | At once; a chord can be played with the box on and off to compare. The state is written to `settings.mpglobal` as `fasterengine` |
| Stored | Program-wide |
| Environment | `MASTERPIECE_SIMD=0` forces the per-frame path whatever the box says |
| Advice | Leave it on. It gives more polyphony for the same CPU. Turn it off to rule it out when something sounds wrong. The log records the active path as `audio engine: faster (AVX2)` or `audio engine: per-frame`. |

### Real-time render threads

A large organ is rendered by the audio thread and several render threads that share each block. The audio thread is a real-time thread; the render threads are ordinary threads, and the system may run other work first, or put them on slower cores, while the audio thread waits for them. **Organ settings > Loading > Real-time render threads (trial)**, beside **Faster audio engine**, makes them real-time threads the way each system offers it.

| System | What the box does |
|---|---|
| Windows | Registers each render thread with MMCSS as "Pro Audio", the class Windows gives audio threads, and asks the system never to slow it down to save power. On Intel processors since the 12th generation, that keeps it off the efficiency cores. |
| Linux, Raspberry Pi | SCHED_FIFO at priority 40, or the highest the user may have, for the render threads and for the audio thread when it is an ordinary thread (ALSA). A thread JACK has made real-time keeps the priority JACK gave it. Needs real-time rights: membership of the `audio` group with an `rtprio` limit, as most audio distributions set up. Without them nothing changes. |
| macOS, iOS | The time-constraint policy for each render thread, and a place in the audio device's workgroup, so the system runs them with the audio thread, to its deadline, on the performance cores of Apple silicon. |
| Android | Not available; the box is hidden. |

| Field | Value |
|---|---|
| Default | Off, while it is tried |
| Takes effect | At once: each thread changes at its next share. The state is written to `settings.mpglobal` as `realtimethreads` |
| Stored | Program-wide |
| Log | `real-time render threads: on (MMCSS Pro Audio)` and the like when it is turned on; each **late blocks** line adds how many render threads are real-time and how many are in the audio workgroup |
| Advice | Try it with a heavy registration and compare the **late blocks** count in the status bar with the box on and off. Reports are collected in the discussion on GitHub. |

## Output routing and the mixer

**Settings > General settings > Mixer** assigns each rank of the loaded organ to one of up to eight pairs. The pairs are rendered separately, each with the enclosure processing of its ranks, and mixed together into the stereo output of the device. No sample set declares an audio routing, so the layout is the player's, in the way a MIDI mapping is.

![The Mixer tab](../images/general-mixer.png)

| Control | What it does | Default | Takes effect |
|---|---|---|---|
| **Output pairs** | **1 (stereo)** and **2 pairs** to **8 pairs**. Lowering the count folds ranks that pointed at a removed pair back to **Pair 1**. | 1 | At once for notes started afterwards |
| Status line | `N rank(s), M pair(s)`; when something is wrong it adds `X unrouted, Y stale` | | |
| **Spread ranks evenly** | Assigns the ranks in rank-number order to the pairs in turn, so that the routing is audible straight away | | At once |
| **All to pair 1** | Puts every rank on **Pair 1** | | At once |
| **Save for this organ** | Writes the organ's settings file, including the routes | | At once |
| Rank list | One row per rank, labelled with its number and name, with a **Pair** box | **Pair 1** | At once |

A rank is routed when a note starts, so notes already sounding keep the pair they began on. A rank with no entry plays through the first pair, so an organ is audible before the Mixer is opened. The pair of a rank is independent of its enclosure: a Swell rank stays behind the Swell shades whichever pair carries it.

### Stored

| Part | Where | Scope |
|---|---|---|
| The pairs (`buses`) | The organ's `.mporgan` file; also the defaults file when **Save as default** is pressed in **Organ settings > Loading** | Read for every organ that has none of its own |
| Which rank uses which pair (`route`) | The organ's `.mporgan` file | Per organ |

Changes on this tab are written when **Save for this organ** is pressed, or by any later save of the organ's settings, which writes the live state of everything.

The sum of the pairs is the whole organ, so splitting the ranks over more pairs leaves the total unchanged.

## The room

**Settings > General settings > Room** places the organ in a room by convolution with an impulse response (IR). Most sample sets are recorded in the room they live in, and several sets ship close, far and rear perspectives that are the room. Use the Room tab for a dry set or for headphones. A wet fraction far below what a reverb plug-in would use is usually enough.

![The Room tab](../images/general-room.png)

| Name | What it does | Default | Range or options | Takes effect |
|---|---|---|---|---|
| **Impulse-response reverb** | Switches the room on | Off | On, off | At once |
| **Load IR...** | Opens a file chooser for WAV, AIFF or FLAC files, and switches the room on when the file loads | No IR | | At once |
| **Clear** | Removes the IR; the label reads **No IR loaded** | | | At once |
| IR label | **IR:** and the file name minus its extension | | | |
| **Wet** | The share of the convolved signal in the output | 0.25 | 0 to 1 in steps of 0.01 | At once |

How the IR is used:

- **Mix.** The output is the wet signal times **Wet** plus the dry signal times 1 minus **Wet**, an equal-gain crossfade. At 0 the room is bypassed.
- **Channels.** A mono file feeds both sides. A two-channel file is an ordinary stereo room. A file with four or more channels is read as true stereo: channels 1 and 2 carry the left input to the left and right outputs, channels 3 and 4 the right input to the left and right outputs.
- **Level.** The file is scaled by its total energy with one gain for all channels so that a true-stereo room keeps its balance. Leading silence is trimmed.
- **Sample rate.** When the chosen file is named `<room>-48000Hz.wav` (a rate in hertz before the extension) and a sibling for the device's rate exists, that sibling is loaded. The file's own sample rate is passed to the convolution engine with its samples.
- **Position.** The room follows the voicing EQ and precedes the master fader. Simple WAV bypasses it. Convolution is the most expensive stage of the audio callback; the engine uses partitioned convolution with a 256-sample head.

The chosen file, the wet fraction and the on/off state belong to the loaded organ: they are written to its `.mporgan` file as the `reverb` line, saved automatically, and restored when that organ opens. **Save as default** in **Organ settings > Loading** copies them to the defaults that organs without settings start from. The file is stored as a path, so the IR must stay where it was chosen.

If a file cannot be read, a warning titled **The impulse response was not loaded** gives the reason, either *The file could not be read where it is.* or *&lt;name&gt; is not an impulse response Masterpiece can read (WAV, AIFF or FLAC).* The same reason goes to the log (`impulse response not loaded: <path>: <reason>`). The previous IR and the state of the box stay as they were. When an organ's saved IR is missing at load time, the log records `reverb: the impulse response saved with this organ was not found: <path>` and an IR loaded earlier stays in use, with the wet fraction and the on/off state saved with the organ.

## Voicing

**Settings > General settings > Voicing** is the voicer's tab: a level and a tuning for a whole rank or for one pipe, in two sets for comparison.

![The Voicing tab](../images/general-voicing.png)

| Name | What it does | Default | Range or options |
|---|---|---|---|
| **Rank** | The rank to adjust, listed as its number and name | First rank | Every rank of the loaded organ |
| **Applies to** | **The whole rank** or **One pipe** | **The whole rank** | |
| **MIDI note** | The pipe, by its MIDI note; enabled with **One pipe** | 60 | 0 to 127 |
| **Level** | The gain of the rank or pipe | 0 dB | -12 to +12 dB in steps of 0.1 |
| **Tuning** | The pitch offset | 0 cents | -50 to +50 cents in steps of 0.1 |
| **A / B** | Makes the other of two complete voicing sets the live one | Set A live | |
| **Copy to other** | Copies the live set to the other slot | | |
| **Reset this** | Sets level and tuning of the chosen rank or pipe to 0 | | |
| **Reset everything** | Clears the live set | | |
| **Save for this organ** | Writes the organ's settings file | | |

The line under the buttons reads `Slot A  -  N rank(s), M pipe(s) adjusted  -  takes effect on the next note`. A pipe that does not exist on the chosen rank shows **This rank has no pipe at that note**.

- A rank adjustment and a pipe adjustment add.
- Level is a gain and tuning a pitch ratio, both taken once when a note starts. Notes already sounding keep what they began with. Both work with Simple WAV on and cost nothing while a note sounds.
- Voicing is stored per organ, as `voicingadj` lines for both slots and the live slot (`voicingslot`), because a rank number has no meaning in another instrument. The tab changes the live set at once; **Save for this organ** writes it.
- The file also holds brightness and balance values for each adjustment. The tab has no control for them and the engine does not apply them.

### Relation to Voicing EQ

The **Voicing EQ** tab of **Organ settings** is a different tool. It is a ten-band equaliser for the whole organ (31.5 Hz to 16 kHz, plus an output gain, up to 12 dB per band), off by default, applied before the room. It can be kept for every organ (**Save as default**) or for one organ (**Save for this organ**). The **Voicing** tab of General settings changes single ranks and pipes in level and pitch and is stored per organ. See [Organ settings](organ-settings.md#voicing-eq) for the equaliser.

The **Voicing** box among the switches of **Organ settings > Loading** is saved with the organ. With the box off, the organ plays as recorded: the per-rank and per-pipe level and tuning of the **Voicing** tab are not applied. The adjustments stay in the voicing sets and apply again when the box is ticked. A change reaches the notes that start afterwards.

## Level

| Control | Where | Range | Default | Stored |
|---|---|---|---|---|
| Master volume slider | Top bar of the main window | -40 dB to +24 dB in steps of 0.1 dB; -40 dB is silence | 0 dB | Per organ, as `gain`; written on its own, apart from the other settings |
| Organ trim | Not adjustable | The organ definition's own output trim, limited to -24 dB to +24 dB | The definition's value | Not stored |
| Level meter | Top bar | 14 lamps per channel, from -48 dB; lamps 10 to 12 amber, 13 and 14 red | | |

The organ trim is the producer's calibration, which brings sets recorded at different levels to comparable loudness. It is applied under Simple WAV as well. The meter shows the finished output, metronome included; it rises instantly and falls with a time constant of 0.3 s.

The wind model's depth has no control in the program; the engine uses the physics as solved (depth 1).

## The metronome

**Settings > General settings > Metronome** adds a click to the output.

![The Metronome tab](../images/general-metronome.png)

| Name | What it does | Default | Range |
|---|---|---|---|
| **Metronome** | Switches the click on | Off | On, off |
| **Tempo** | Beats per minute | 90 bpm | 20 to 300 in steps of 1 |
| **Beats per bar** | The bar length; the first beat of each bar is accented | 4 | 0 to 12 in steps of 1; 0 gives no accent |
| **Level** | The click volume | 0.5 | 0 to 1 in steps of 0.01 |

A label below the sliders reads **Beat** and the number of the beat in the bar while the click runs. All four controls act at once. The tempo, the beats per bar and the level are kept in the global settings file as `metronome <bpm> <beats> <level>`, written when the Metronome tab closes, and return at the next start. The click starts silent at each start.

The click is a short decaying sine (a time constant of about 9 ms): 1000 Hz on ordinary beats and 1500 Hz at 100% strength on the accented first beat, with ordinary beats at 70% strength. It is added after the master fader, so the master level does not change it, and after the audio recorder, so recordings do not contain it. It sounds on both channels.

> [!NOTE]
> The **Metronome** box shows whether the click is running each time the tab opens. Clear it to stop the click.

## Audio dropouts

A late block is an audio block that took longer to compute than the audio in it lasts. Each late block is a click or a gap in the sound. The engine counts blocks on the audio thread and reports the figures below.

### The figures the program shows

| Figure | Where | Meaning |
|---|---|---|
| `voices N` | Status line of the main window | Voices sounding now |
| `dropped N` | Status line | Note starts that found no free voice at all |
| `no audio N` | Status line | Note starts whose samples were not resident |
| `late blocks N` | Status line | Blocks that took longer than their audio since the program started |
| `streamed releases fell behind Nx: turn off "Stream release tails" in Organ settings` | Status line | The disk did not deliver a streamed release in time; the release went quiet partway |
| `Resident samples: ...` | **Organ settings > Loading**, under the controls | Memory held, with `N streamed, M MB left on disk` and `DISK TOO SLOW: N underrun(s)` when the disk fell behind |
| `Voice pool: N voices, M MB` | Same readout | The size of the voice pool and its memory |
| `audio: N late block(s) of M; the slowest took P% of its time` | The log, at most once every five seconds | A figure of 100% or more is a late block |

The log is `masterpiece.log` in the `Masterpiece` folder of the application data (the previous session is `masterpiece.previous.log`). The **Open the log folder** button on the **Log** tab of General settings opens that folder.

While an organ loads, the engine answers with silence and drops notes that arrive.

### Causes and remedies

| Cause | Sign | What to change |
|---|---|---|
| Buffer too small | Late blocks with few voices | Raise the buffer size on the **Audio** tab, one step at a time |
| Polyphony | `voices` high on full organ, `late blocks` rising, or `dropped` above 0 | **Organ settings > Loading > Polyphony limit**. The default is 725 voices per usable processor core and at least 1,536 (1,536 when release streaming is on); the box offers 256 to 10,240 voices. A lower value keeps a small computer from falling behind; a higher value raises CPU use. Past the limit a new pipe takes the place of one that is dying away, or of the quietest. Each choice shows the memory its voices take; a lower limit applies at once and a higher one rebuilds the pool at once. |
| Processor | Late blocks on every busy passage | Turn on **Faster audio engine**. Turn off the room, or switch **Simple WAV only** on to find out whether the DSP stages are the cause. Use **Release tails end below** (default -90 dB; at -80 dB the last decay of a full chord can end audibly early), **Releases** and **Release length** to end sounding tails sooner. |
| Convolution | Dropouts after the room is switched on | A shorter IR, or **Clear**; the room is the costliest stage |
| Disk streaming | `DISK TOO SLOW` or the status line message about streamed releases | Raise the share each release holds in memory (**Hold 25%** to **Hold 75% of each release**), or turn off **Stream release tails from disk** and load the organ again; see [Memory, loading and streaming](memory.md) |
| Memory | Dropouts together with heavy disk use | Use the **Recommended** or **Smallest** memory profile, or a smaller set of ranks; see [Small, medium and large organs](sizing.md) |
| Other programs | Irregular late blocks | A native low-latency driver, and the larger buffer |

The voice-rendering threads are chosen automatically: the number of usable processor cores minus one (one thread when there are two or fewer cores), at most eight. The program has no control for the count.

The sample cache writing "in silence, while it plays" waits for silence and takes no voices from the organ; **Save the sample cache** in **Organ settings > Loading** chooses between this and writing before the organ plays.

## Platform notes

- **Plug-ins.** The host owns the audio device and the MIDI ports. **Settings > General settings...** in the plug-in editor opens General settings without the **Audio** and **MIDI** tabs, so the device selector of this page is not available there. See [Plugins in a DAW](plugins.md).
- **Phones and tablets.** The same selector is used with the driver types above; see [Phones and tablets](mobile.md).
- **Raspberry Pi.** The faster audio engine uses the NEON unit.
