# Command line

The manual page `manual/src/guide/command-line.md` is the full reference; this
page lists what scripted work uses. A value on the command line wins over the
saved settings for that run.

```
Masterpiece [options] [organ file]
```

The program is `Masterpiece.exe` on Windows,
`Masterpiece.app/Contents/MacOS/Masterpiece` on macOS and `Masterpiece` on
Linux.

## Opening an organ

- `<organ file>`, `--odf <file>` — a Hauptwerk `.Organ_Hauptwerk_xml` or
  `.CustomOrgan_Hauptwerk_xml`, a GrandOrgue `.organ`, or a package (`.rar`,
  `.orgue`).
- `--gui-only` — builds the console and reads no audio: silent, loads in
  seconds. For console and registration checks.
- `--console-page <n>` — shows console page n (from 1) after the load.
- `--organ-root <folder>` — the folder holding `OrganInstallationPackages`
  when the definition's path does not reveal it.

## Logs

- `--log <file>` — writes the log there.
- `--log-midi` — logs every MIDI message and what it reached, or why nothing
  sounded.
- `--log-releases` — logs each release: which one played, its length, its
  start, its join to the attack. For clicks and wrong tails.
- `--record-midi <file>` — records the MIDI played, saved on quit.
- Default log: `masterpiece.log` in the settings folder, and
  `masterpiece.previous.log` for the session before.

## Takes: registration, MIDI file, recording

- `--play-midi <file>` — plays a MIDI file after the load, then quits unless
  `--stay-open`. Repeatable: each one is a take. Before each take, keys are
  released and stops retired.
- `--record-audio <file>` — records the take to WAV inside the program: no
  sound card needed, nothing lost to dropouts. One after each `--play-midi`.
- `--draw-stops <all | n | ids>` — the stops of the next take: `all`, the first
  n, or organ-file stop ids (`12,15,31`).
- `--registration <name>` — `plenum`, `tutti`, `chorale`, `trio` or `solo`, for
  sets without presets.
- `--preload-drawn` — loads only the ranks of the drawn stops (needs a stop id
  list). A trial registration then loads in seconds.
- `--draw-only` — loads, draws, plays nothing, stays open.
- `--stay-open` — stays open after the last take.

## Loading and memory

- `--storage <int16|int20|int24|float32>` — sample storage; `int16` takes a
  third less memory than the default `int24`.
- `--load-mono <on|off>`, `--load-rate <Hz>`, `--stream-releases <on|off>`,
  `--preload-head <frames>`, `--cache <off|single|per-organ>` — as the Loading
  tab of Organ settings.

## MIDI

- `--virtual-midi [name]` (macOS, Linux) — publishes a MIDI input port, so
  another program plays the organ without a loopback driver.

## Examples

```bash
# Console only, page 2
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" --gui-only --console-page 2

# Two takes, two registrations, only the drawn ranks loaded
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" --preload-drawn \
  --draw-stops 12,15 --play-midi chorale.mid --record-audio chorale.wav \
  --draw-stops 12,15,31,40 --play-midi fugue.mid --record-audio fugue.wav

# Evidence for notes that do not sound
Masterpiece --log report.log --log-midi --record-midi report.mid --odf ORGAN
```
