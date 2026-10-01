# Command line

Masterpiece runs without any options: launch it and open an organ from the
window. The options below are for opening an organ directly, for reporting a
problem, for scripted recordings, and for measuring what an organ costs to load.
`Masterpiece --help` prints a short list of them.

```
Masterpiece [options] [organ file]
```

On Windows the program is `Masterpiece.exe` in the install folder. On macOS it
is inside the app bundle: `/Applications/Masterpiece.app/Contents/MacOS/Masterpiece`.
On Linux it is `masterpiece`.

Paths are relative to the folder the command runs in. A path with spaces needs
quotes. Options may come in any order, unless a section below says otherwise.

- [Opening an organ](#opening-an-organ)
- [Information](#information)
- [Logging and reporting problems](#logging-and-reporting-problems)
- [MIDI input](#midi-input)
- [Playing, registering and recording](#playing-registering-and-recording)
- [Loading and memory](#loading-and-memory)
- [Examples](#examples)

## Opening an organ

### `<organ file>`

An organ to open at startup. It can be a definition (`.Organ_Hauptwerk_xml`,
`.CustomOrgan_Hauptwerk_xml`), a GrandOrgue `.organ` file, or a package (`.rar`,
`.orgue`). It is the same as `--odf`, and it is what **Open with Masterpiece**
passes from a file manager. The file has to exist; anything else is ignored.

When Masterpiece is already running and the system hands it a file this way (on
macOS, Finder's **Open With**), it opens that organ in the running window.

### `--odf <file>`

Open this organ at startup. It accepts the same files as above. When both
are given, `--odf` wins.

### `--gui-only`

Build the whole console and read no audio. The organ appears in a second or
two and is silent. It is the quickest way to check how a set's console looks,
or to tell whether a problem is in the definition or in the samples. In this
mode the crash-recovery prompt and the first-run wizard are skipped.

### `--console-page <n>`

Show this console page once the organ is loaded, counting from 1. It is useful
for sets that put their jambs or their pistons on pages of their own.

### `--organ-root <folder>`

The folder that holds the organ's `OrganInstallationPackages`, for an
installation laid out in a way that the definition's own path doesn't reveal:
for example, a definition copied somewhere else, or samples moved to another
drive.

## Information

### `--help`, `-h`, `/?`

Print the short list of options and quit.

### `--version`

Print the version and quit.

## Logging and reporting problems

Masterpiece always keeps a log, even without any option. It is
`masterpiece.log` in the app's data folder:

| System | Folder |
|---|---|
| Windows | `%APPDATA%\Masterpiece` |
| macOS | `~/Library/Masterpiece` |
| Linux | `~/.config/Masterpiece` |

The log from the run before is kept beside it as `masterpiece.previous.log`, so
a crash doesn't lose the session that crashed. **Settings → Log** shows the
current log in the app. These two files are the most useful thing to attach to
a bug report.

### `--log <file>`

Write the log to this file instead. It is set up before anything else, so it
covers the whole load, including the timing of each stage.

### `--log-midi`

Log every MIDI message and what became of it: which manual, stop or control
it reached, and, if nothing sounded, why. Use it when a keyboard or a piston
does nothing, or does the wrong thing.

### `--log-releases`

Describe every key release: which of the pipe's releases played, how long that
release is, how many releases the pipe has to choose from, where in the release
it started, and how it joined the attack. Use it for a click, a noise or a
wrong-sounding tail when keys are let go.

### `--record-midi <file>`

Record everything played in the session as a MIDI file, saved when Masterpiece
quits. It helps with a fault that can't be reproduced on demand, such as a stuck
note. Play until it happens, then quit, and the moment is in the last seconds of
the file. `--play-midi` replays the file exactly.

## MIDI input

### `--virtual-midi [name]`

macOS and Linux only. Publish a MIDI input port of Masterpiece's own, so that
another program (a sequencer, a notation editor, a virtual keyboard) can play
the organ without a cable or a loopback driver. The port is called
`Masterpiece` unless a name is given. On Windows the system offers no way to
do this. A loopback driver such as loopMIDI does the same job, and Masterpiece
picks up its port like any other device.

## Playing, registering and recording

These options let a script load an organ, draw stops, play one or more MIDI
files, and record what the organ produced. That makes recordings and
comparisons repeatable.

### `--play-midi <file>`

Once the organ has loaded, play this MIDI file through it. Playback starts after
1.5 seconds of silence, so the start isn't cut off. Masterpiece quits when the
music ends, unless `--stay-open` or `--draw-only` is given.

The option can be given more than once. Each file is a **take**, and the takes
play one after another. Before each take, every key is released and every stop
is retired, so each take registers from scratch. The log records the stops
drawn for each take, and the wall-clock time in milliseconds at which each take
starts. With that, a screen recording of a whole session can be cut back into
takes.

### `--record-audio <file>`

Record the organ's output to this WAV file while the current take plays. The
recording is taken inside the program, not from the sound card, so no other
sound gets in and nothing is lost if the machine stutters. It starts exactly
with the music. With several takes, put a `--record-audio` after each
`--play-midi`.

### `--draw-stops <all | n | ids>`

The stops to draw for a take:

- `all`: every stop.
- a number, such as `4`: the first that many stops in the organ's stop list.
  Stop lists are usually ordered by division, so on most organs `4` means four
  pedal stops.
- a comma-separated list of stop ids, such as `12,15,31`: exactly those stops.
  The ids are the `StopID` values in the organ definition. An id the organ
  doesn't have is reported in the log and skipped.

A `--draw-stops` that comes after a `--play-midi` applies to the next take, so
it may be written on either side of its file.

### `--registration <name>`

Draw stops chosen from their names, for an organ that has no preset written for
it:

| Name | What it draws |
|---|---|
| `plenum` | principal chorus and mixture: preludes and fugues. This is also what any unknown name gives. |
| `tutti` (or `all`) | everything |
| `chorale` | one flute and a quiet pedal |
| `trio` | two contrasting manuals and a light bass |
| `solo` | a reed or a mixture against a soft accompaniment |

Like `--draw-stops`, it applies to the next take when it comes after a
`--play-midi`. A list of stop ids takes priority over a registration, and a
registration over `all` or a number.

### `--preload-drawn`

Load only the ranks that the drawn stops use. On a large set, this cuts trying
a registration from minutes to seconds. It needs a `--draw-stops` list of ids,
and is ignored otherwise (the log says so). Every stop that isn't drawn is
silent for the rest of the session.

### `--preload-ranks <ids>`

Load exactly these ranks, given as a comma-separated list of rank ids, such as
`2,4,14`. It is meant for organs whose stops reach their pipes through pallets.
On those organs the drawn stops name no ranks, so `--preload-drawn` can't
narrow the load.

### `--draw-only`

Load the organ, draw the stops chosen with `--draw-stops` or `--registration`,
and then hand the console over: nothing plays and Masterpiece stays open. It
is the way to open an organ with a registration already drawn.

### `--stay-open`

Stay open after the last take has played. Without it, Masterpiece quits when
the music ends. On a large set, staying open saves loading the organ again.

## Loading and memory

These options set how the next organ is loaded. They are the same settings as
the **Loading** tab of the organ's settings. A value given on the command line
takes precedence over the saved settings for that run, except for `--cache`:
a cache mode saved in the settings still applies. The log line
`memory config:` shows the values in effect.

### `--storage <int16 | int24 | float32>`

How each sample frame is held in memory. The default is `int24`. `int16` uses
a third less memory and is enough for most sets. `float32` uses the most
memory and holds the samples exactly as decoded.

### `--load-mono <on | off>`

Fold stereo samples to one channel as they load. It halves the memory, but the
stereo image is lost. `on`, `1`, `true` and `yes` turn it on; anything else
turns it off. The default is `off`.

### `--load-rate <Hz>`

Convert the samples to this sample rate as they load, such as `48000`. `0`
keeps them as recorded, which is the default. Converting a 96 kHz set to
48 kHz halves its memory.

### `--stream-releases <on | off>`

Keep only the start of each release tail in memory, and read the rest from
disk as it plays. It saves a large share of the memory on sets with long
releases, and needs a reasonably fast disk. The default is `off`.

### `--preload-head <frames>`

The minimum number of frames held in memory at the start of every sample. `0`,
the default, holds whole files.

### `--cache <off | single | per-organ>`

The sample cache keeps decoded samples on disk, so the next load of the same
organ is much faster.

- `single` (the default): one cache, replaced when a different organ is loaded.
- `per-organ`: one cache for each organ. This uses more disk space and keeps
  every organ fast.
- `off`: no cache.

Any value other than `off` or `per-organ` means `single`. A load in which some
samples failed to read writes no cache, so a damaged installation is never
cached as complete.

## Examples

Open an organ:

```
Masterpiece "C:\Hauptwerk\Organs\Friesach.Organ_Hauptwerk_xml"
```

Check how a console looks, without loading any samples, on its second page:

```
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" --gui-only --console-page 2
```

Collect everything needed for a report about notes that don't sound:

```
Masterpiece --log report.log --log-midi --record-midi report.mid
```

Load an organ with a plenum drawn and play it yourself:

```
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" --registration plenum --draw-only
```

Record two pieces on two registrations. Only the drawn ranks are loaded, and
the samples are held as 16-bit mono:

```
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" ^
  --storage int16 --load-mono on --preload-drawn ^
  --draw-stops 12,15 --play-midi chorale.mid --record-audio chorale.wav ^
  --draw-stops 12,15,31,40 --play-midi fugue.mid --record-audio fugue.wav
```

(On macOS and Linux, end the lines with `\` instead of `^`.)
