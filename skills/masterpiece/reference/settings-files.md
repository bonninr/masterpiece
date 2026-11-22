# Settings files

Some settings have no API object: the audio outputs and their device
channels, the crossovers, the output EQ, and the MIDI mappings. They live in
text files of the settings folder, one setting per line.

## Rules

1. Close Masterpiece first. It writes these files when it quits and when a
   setting changes, so an edit made while it runs is lost.
2. Copy the file before editing it. Tell the player where the copy is.
3. Change only the lines the task needs; keep every other line as it is.
4. Start Masterpiece again and check the result in the window or with the
   API. A line the program cannot read has no effect, so check every change.
5. When this page and the program disagree, the program's reader and writer
   in the source code decide (named below).

## Where the files are

| System | Settings folder |
|---|---|
| Windows | `%APPDATA%\Masterpiece` |
| macOS | `~/Library/Masterpiece` |
| Linux | `~/.config/Masterpiece` |
| Linux, Flatpak | `~/.var/app/io.github.bonninr.Masterpiece/config/Masterpiece` |

| File | Holds |
|---|---|
| `settings.mpglobal` | General settings, the API keys, the favourites, and the audio outputs (the same for every organ) |
| `organs/<organ>.mporgan` | One organ's settings: loading, voicing, rank routes, perspectives left out, tuning |
| `midi/<organ>.mpmidi` | One organ's MIDI mapping |
| `midi/console.mpmidi` | The player's pistons, sequencer and setter, on every organ |
| `combinations/<organ>.mpcomb` | Stored pistons and sequencer frames |

`<organ>` is the organ's name with letters and digits kept, then `-` and the
organ's unique id (or `p` and a hash of its path). List the folder to find
it. The manual's page `manual/src/guide/files.md` describes every file.

## The API keys (`settings.mpglobal`)

| Line | Meaning |
|---|---|
| `api 1` | Serve the web console and the control API |
| `apiwrite 1` | Allow changes from the network |
| `apilocal 1` | This computer only (127.0.0.1) |
| `apiport 8080` | The port |
| `apitoken <text>` | The token changes need |

## Audio outputs (`settings.mpglobal`)

Written by `MasterpieceProcessor::mixerLines()` and read by
`MasterpieceProcessor::loadGlobalDefaults()` in
`src/mp_audio/MasterpieceProcessor.cpp`.

- `buses 1:0,1 2:2,3 3m:4` — the outputs. Each entry is
  `<output>[m]:<device channels>[><outputs it feeds>]`. Device channels count
  from 0: `2:2,3` plays output 2 on channels 3 and 4; `3m:4` is output 3 in mono
  on channel 5. `1:0,1>3` also feeds output 3; `>3@-6.0` at −6 dB.
- `busname <output> <name>` — an output's name, as the Outputs tab and the API
  `to` property show it.
- `busfilt <output> <high-pass Hz> <type> <low-pass Hz> <type> <delay ms> <invert>`
  — the crossover of one output. A frequency of 0 is off. Types: 0 Butterworth
  12 dB, 1 Butterworth 24 dB, 2 Linkwitz-Riley 12 dB, 3 Linkwitz-Riley 24 dB.
- `buseq <output> <bands>` — the output's ten-band EQ, in the voicing EQ's
  line format.

A rank goes to an output through the API (`PUT /routes/{rank}/value`), not
through these lines. A route names an output from 1 in the order of `buses`.

## MIDI mapping (`midi/<organ>.mpmidi`)

Written by `MidiMap::toText()` and read by `MidiMap::fromText()` in
`src/mp_control/MidiMap.cpp`. The file starts with comment lines that name
every field. The main kinds of line:

```
# <source> <channel> <number> <target> <id> <latching> <invert> <trigger> <low> <high> <device> [| debounce=<ms> exact bit=<0-6>]
note 3 36 switch 12 1 0 toggle 0 127 any
cc 1 7 control 701 0 0 momentary 0 127 any
pgm 1 5 switch 8002 0 0 on 0 127 Johannus_MIDI
manual <keyboard> <channel> <low key> <high key> <transpose> <low velocity> <high velocity> <ignore velocity> <short octave> <debounce ms> <device>
send <target> <id> <message> <channel> <number> <low> <high>
```

- Sources: `note`, `cc`, `pgm`, `sysex`, `rpn`, `nrpn`. Channels 1–16; 0 is any channel.
- Targets: `switch` (a stop, coupler, tremulant or piston), `control` (a shoe),
  `keyboard`, and the console commands (`stepper-next`, `stepper-prev`, …).
- `<id>` is the organ's own id of the switch or control: the `odfId` the API
  describes.
- Triggers: `toggle`, `momentary`, `on`, `off`, `toggle-both`.
- `<device>` is `any`, or the MIDI input's name with `_` for each space.
- `manual` lines put a MIDI channel on a keyboard; `send` lines drive the
  console's lights and displays.

Check a new mapping with `--log-midi` (see `command-line.md`): the log names
what each incoming message reached, or why it reached nothing.

## Organ settings (`organs/<organ>.mporgan`)

Written by `MasterpieceProcessor::saveSettings()` and read by
`MasterpieceProcessor::loadSettingsFor()`. Voicing, routes and live levels in
this file all have API objects; use the API for them. Edit this file only for
a setting the API and the window both lack, and read the writer first.
