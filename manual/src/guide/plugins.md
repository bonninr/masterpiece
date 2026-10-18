# Plugins in a DAW

Masterpiece is also built as a VST3, an LV2 and an Audio Unit (AU) plugin, so that a digital audio workstation (DAW) can host an organ as an instrument track. This page covers where each plugin is installed, how a host loads it, how an organ is chosen inside it, what it stores in the host's project, which MIDI and audio connections it offers, and what differs from the standalone program.

The standalone program is the main product. The plugins use the same engine, the same console and the same files, and add the limits that come with living inside a host.

## Formats and installation

| Format | Systems | Delivered as |
|---|---|---|
| VST3 | Windows, macOS, Linux | `Masterpiece.vst3`: in the Windows portable `.zip`, in the macOS `.zip`, and installed to `/usr/lib/vst3` by the PC Debian package |
| LV2 | Linux | `Masterpiece.lv2`, installed to `/usr/lib/lv2` by the PC Debian package |
| AU | macOS | `Masterpiece.component`, in the macOS `.zip` beside `Masterpiece.app` |

The Windows setup program (`masterpiece-<version>-windows-setup.exe`) installs the standalone program only. To use the VST3 on Windows, take `Masterpiece.vst3` from the portable `.zip` and copy it to a folder that the host scans for VST3 plugins. On macOS, copy the plugins from the `.zip` to the folders the host scans. After a copy, rescan the plugins in the host.

The macOS build is unsigned; see [Installing Masterpiece](installing.md).

The plugins are not built for these packages:

- The Flatpak bundle (the build sets `MP_BUILD_PLUGIN=OFF`).
- The Raspberry Pi 32-bit and 64-bit Debian packages (their build presets build no plugins).
- Android, iPhone and iPad.

### Plugin identity

| Property | Value |
|---|---|
| Name | Masterpiece |
| Manufacturer code | `Mspc` |
| Plugin code | `Msp1` |
| Bundle identifier | `org.masterpiece.player` |
| LV2 URI | `https://masterpiece.audio/plugins/masterpiece`, an identifier that hosts compare; no program contacts this address |
| Category | Instrument (a synthesiser) |

## Loading it in a host

Add Masterpiece as an instrument on a new track, as for any sampled instrument. The host shows one stereo output and a MIDI input. After a rescan, a host lists it under the manufacturer name Masterpiece.

The plugin window is the console of the standalone program. It opens at 1280 by 760 pixels and can be resized; the host decides whether a plugin window may change size. A new instance shows **No organ loaded.** in the status line.

## Choosing an organ

Open **File > Open...** in the plugin window. The window lists the organs saved as favourites, with a button for the file dialog; with no favourite yet, the file dialog opens at once. The plugin accepts the same files as the standalone program: Hauptwerk-format definitions, GrandOrgue `.organ` files, and `.rar` and `.orgue` packages. See [Opening an organ](opening.md).

When an organ has several definitions beside it (perspectives, or full and light versions), **File > Other versions of this organ** switches between them.

While an organ loads, the load dialog is on screen and the plugin outputs silence. MIDI notes that reach the plugin during the load are dropped, because they were played on the organ being replaced.

A host project reopens the organ that its instance played; see [What the plugin saves in the host's project](#what-the-plugin-saves-in-the-hosts-project). An instance in a new track starts with no organ and never reopens the last organ used elsewhere.

### Prepare the organ in the standalone program

The plugin reads the settings that the standalone program saved: the global defaults in `settings.mpglobal` and each organ's file in `organs/`. See [Files and folders](files.md). Open each organ once in the standalone program, choose its memory settings in **Organ settings > Loading** (see [Memory, loading and streaming](memory.md)), save them with **Save for this organ**, and the plugin loads the organ with them.

The plugin has no first-load settings step. The standalone program opens **Organ settings** before the first load of an organ; the plugin starts the load at once with the global defaults. An organ that has no settings file loads with 24-bit samples held resident, the format that needs the most memory, under a memory limit of 80% of the computer's memory.

## What the plugin saves in the host's project

The state that the plugin hands to the host holds one item: the full path of the definition file of the organ the instance has loaded. An instance with no organ stores no path.

When the project opens, the host hands the state back and the instance loads that organ again:

- With the plugin window open, the load runs through the window, as **File > Open...** does, with its load dialog.
- With no plugin window open, the load starts straight away without it.

An instance that already holds that organ does not load it again. A state with no path, or one that does not belong to Masterpiece, changes nothing.

| Item | Where it is kept |
|---|---|
| The organ's path | In the host's project. The organ opens again when the project opens. |
| Master gain | In the organ's file `organs/<organ>.mporgan`, line `gain`. It returns when the organ is opened again. |
| Stops drawn, couplers, pistons | Combinations in `combinations/<organ>.mpcomb`; the starting registration with the **Save state** button, which writes the organ's file. |
| MIDI mapping | `midi/<organ>.mpmidi` and `midi/console.mpmidi` |
| Last organ used | `lastorgan` in `settings.mpglobal`. The plugin writes this line on every successful load. |

Because the plugin and the standalone program share one folder, a project opened on a computer that has the same organ in the same place finds the same settings, combinations and MIDI mapping.

## Host parameters

The plugin offers two parameters for automation.

| Name | What it does | Default | Range | Takes effect |
|---|---|---|---|---|
| **Master Gain** | The master fader, after the organ's own output trim. | 1.0 (unity, 0 dB) | 0.0 to 16.0 as a linear gain; 16.0 is +24 dB | At once |
| **Simple WAV (no DSP)** | Bypasses all DSP, including the voicing EQ and the room reverb. Same switch as **No DSP** in the console's top bar. | Off | Off, on | At once |

The volume slider in the console's top bar moves **Master Gain** and covers -40 dB to +24 dB; -40 dB is silence. Loading an organ resets the fader to unity, then applies the `gain` saved for that organ.

## MIDI from the host

The plugin declares a MIDI input and produces no MIDI output.

- Notes and controllers from the host's MIDI stream reach the engine through the same handling as a hardware keyboard. The host's merged stream carries no device name, so the engine treats it as one unnamed source ("any device").
- A manual answers to the MIDI channel assigned to it in the organ's MIDI settings, or to the channel the organ defines. A mapping bound to one named keyboard applies to that hardware input and matches nothing from the host; a mapping bound to any device matches.
- With no mapping, a controller number acts as the control with that number, so a swell shoe can be driven from a controller out of the box.
- All-notes-off (controller 123) and all-sound-off (controller 120) release the notes of that channel.
- The split keyboard (**View > Split keyboard**) works on the host's notes.
- The plugin opens no MIDI device and offers no MIDI output port. The lights and displays of a console (**Send stop changes back to the console**, console displays) need a MIDI output that only the standalone program opens.

## Audio outputs and buses

| Property | Value |
|---|---|
| Inputs | None |
| Outputs | One stereo bus named **Out** |
| Reported tail length | 5 seconds |
| MIDI in / out | In only |

The engine renders the organ to this single stereo bus. The mixer's buses, set under **Settings > General settings > Mixer**, route ranks inside the engine and are summed to the stereo output; the plugin offers no separate output per division or per bus.

The plugin takes the sample rate and the block size from the host. A rate change in the host rebuilds the voice pool.

## What differs from the standalone program

| Area | Standalone program | Plugin |
|---|---|---|
| Audio device, rate, buffer | Chosen in **Settings > General settings > Audio** | Owned by the host |
| MIDI inputs | All enabled by default; selectable in **General settings > MIDI** | The host's MIDI stream only |
| MIDI output | Chosen in **General settings > MIDI** | None |
| Settings windows | **Organ settings...**, **General settings...** and **Audio and MIDI...** in the **Settings** menu | The **Settings** menu lists **Organ settings...** and **General settings...**. **General settings...** opens General settings without the **Audio** and **MIDI** tabs, because the host owns the audio device and the MIDI ports. **Organ settings...** opens Organ settings without its **MIDI** tab. **Audio and MIDI...** is absent. |
| First-run wizard | Shown at the first start | Not shown |
| Startup | Opens the last organ when **Reopen the last organ when Masterpiece starts** is on | Opens the organ stored in the host's project when a project opens, and no organ otherwise |
| Crash guard | Remembers the organ being loaded and writes a note to the log after a crash | Absent |
| Log | `masterpiece.log` and `masterpiece.previous.log` | No log file is written |
| Command-line options | All | None |
| Pages in their own windows | Any console page can open in a window of its own | Pages stay tabs in the plugin window |
| Window position | Remembered in `window.txt` | Decided by the host |
| `--virtual-midi`, JACK MIDI | Available on macOS and Linux | Not applicable |

The recorder (**File > Recorder...**), the **Combinations** window, the split keyboard, the on-screen keyboard and the tuning controls work as in the standalone program. The audio recorder captures the plugin's output after the master fader.

## Several instances

Each instance holds its own copy of the samples. Two instances that load the same organ use twice the memory; the memory limit is checked per load and does not count what other instances hold. Choose the 16-bit and streamed-release settings for organs that several instances share, and read [Small, medium and large organs](sizing.md) before loading more than one large set.

All instances share the files in the settings folder. Set **Sample cache** to **One cache per organ (uses more disk)** in the standalone program when instances load different organs; with **One cache, replaced as organs change**, each load replaces the cache of the other instance's organ.

## Limitations

- The host's project stores the organ's path and nothing else; the stops drawn and the settings come from the files in the settings folder.
- General settings has no **Audio** or **MIDI** tab and Organ settings has no **MIDI** tab, because the host owns the audio and MIDI devices.
- No log is written, so a fault inside a host leaves no `masterpiece.log`. Reproduce it in the standalone program and attach that log, as described in [Troubleshooting and caveats](troubleshooting.md).
- A console's lights and displays need the standalone program, which opens the MIDI output.
