# Command line

The Masterpiece program takes options that open an organ directly, write a log for a bug report, record a session, play MIDI files through an organ for repeatable recordings, and set how an organ is loaded. This page lists every option, with its default and its effect. The program starts without any option; the options only change what happens at startup.

```
Masterpiece [options] [organ file]
```

| System | How to start the program |
|---|---|
| Windows | `Masterpiece.exe` in the install folder |
| macOS | `/Applications/Masterpiece.app/Contents/MacOS/Masterpiece` |
| Linux (Debian, Ubuntu, Raspberry Pi OS) | `masterpiece` |
| Linux (Flatpak) | `flatpak run io.github.bonninr.Masterpiece [options]` |

Android, iPhone and iPad have no command line. The plugins take no options.

Rules for all options:

- A relative path is relative to the folder the command runs in. A path with spaces needs quotes.
- Options may come in any order, except where a section says that position matters.
- Options with a value take the next word as the value.
- An unknown word is ignored.

`Masterpiece --help` prints a short list of the options to standard output and quits.

- [Opening an organ](#opening-an-organ)
- [Information](#information)
- [Logging and reporting problems](#logging-and-reporting-problems)
- [MIDI input](#midi-input)
- [Playing, registering and recording](#playing-registering-and-recording)
- [Loading and memory](#loading-and-memory)
- [Examples](#examples)

## Opening an organ

### `<organ file>`

An organ to open at startup. The file is a definition (`.Organ_Hauptwerk_xml`, `.CustomOrgan_Hauptwerk_xml`, or the version 3 spellings `.Organ.Hauptwerk.xml` and `.CustomOrgan.Hauptwerk.xml`), a GrandOrgue `.organ` file, or a package (`.rar`, `.orgue`). The program takes the first word that is not an option, names an existing file and has one of these endings. Other words are ignored. This is the argument that **Open with Masterpiece** passes from a file manager.

When another instance is already running and the system hands it a file (on macOS, Finder's **Open With**), the running window opens that organ.

### `--odf <file>`

Opens this organ at startup. It accepts the same files as `<organ file>`. When both are given, `--odf` is used. The file is looked up in the working folder; a missing file produces a load error in the status line and the log.

Without either, the program opens the last organ when **Reopen the last organ when Masterpiece starts** is on, and starts with no organ otherwise.

### `--gui-only`

Builds the whole console and reads no audio. The organ appears in a second or two and is silent. Use it to check how a set's console looks, or to decide whether a problem lies in the definition or in the samples.

In this mode the first-run wizard is skipped, the first-load **Organ settings** step is skipped, and the crash guard does not mark the organ as running.

### `--console-page <n>`

Shows console page `n` once the organ has loaded, counting from 1. A value outside the organ's pages is ignored. Use it for sets that put jambs or pistons on pages of their own.

### `--organ-root <folder>`

The folder that holds the organ's `OrganInstallationPackages`. Use it when the definition's own path does not lead to the samples: a definition copied elsewhere, or samples moved to another drive. The same setting exists as **Organ folder** in **Organ settings > Loading**.

Give an absolute path. The value is read as written: quotes around it are kept, and a relative path is not resolved against the working folder.

## Information

### `--help`, `-h`, `/?`

Prints the short list of options and quits.

### `--version`

Prints `Masterpiece <version>` and quits.

## Logging and reporting problems

Every session writes a log without any option. The files are in the settings folder:

| System | Settings folder |
|---|---|
| Windows | `%APPDATA%\Masterpiece` |
| macOS | `~/Library/Masterpiece` |
| Linux | `~/.config/Masterpiece` |
| Linux (Flatpak) | The same path inside the application's sandbox storage |

At the start of the next session, `masterpiece.log` becomes `masterpiece.previous.log`, so a crash keeps its log. **Settings > General settings > Log** shows the path of the current log and opens its folder. Attach both files to a bug report.

### `--log <file>`

Writes the log to this file. The log starts before anything else, so it covers the whole load, including the time of each stage. With `--log`, the standard `masterpiece.log` is not written and the previous log is not rotated.

### `--log-midi`

Logs every MIDI message and what became of it: the manual, stop or control it reached and, when nothing sounded, the reason. It also logs the MIDI inputs present and, after each load, the playable keyboards with their channels. Use it when a keyboard or a piston does nothing or does the wrong thing. The same switch is the **Log every MIDI message and what became of it** box in **General settings > Log**; both last until the program quits.

### `--log-releases`

Describes every key release: which of the pipe's releases played, how long that release is, how many releases the pipe has, where in the release it started and how it joined the attack. Use it for a click, a noise or a wrong-sounding tail when keys are released. The same switch is the **Log every key release** box in **General settings > Log**.

### `--log-touches`

Logs every press with the component that took it, and every change of the modal window. Use it to find what a tap that does nothing landed on, mostly on a touch screen. The option is not in the `--help` list.

### `--record-midi <file>`

Records everything played in the session as a MIDI file, saved when the program quits. It helps with a fault that cannot be repeated on demand, such as a stuck note: play until it happens, then quit, and the moment is in the last seconds of the file. `--play-midi` replays the file exactly.

## MIDI input

### `--virtual-midi [name]`

Publishes a MIDI input port of Masterpiece's own, so that another program (a sequencer, a notation editor, a virtual keyboard) can play the organ without a cable. The port is called `Masterpiece` unless a name follows. The name must not start with `--`.

Available on macOS and Linux. Windows offers no way to publish a port; a loopback driver such as loopMIDI does the same job, and Masterpiece opens its port like any other device. On Linux the program also offers a JACK MIDI port when a JACK server runs.

## Playing, registering and recording

These options load an organ, draw stops, play one or more MIDI files and record what the organ produced, so that recordings and comparisons repeat exactly. Masterpiece quits when the last file has played, unless `--stay-open` or `--draw-only` is given.

### `--play-midi <file>`

Plays this MIDI file through the organ once it has loaded. Playback starts 1.5 seconds after the organ is ready, so the beginning is not cut off.

Give the option more than once for several **takes**, played one after another. Before each take every key is released and every stop is retired, so each take registers from scratch. The log records the stops drawn for each take and the wall-clock time in milliseconds at which each take starts, so that a screen recording of a whole session can be cut into takes.

A take whose MIDI file is missing or unreadable is logged and skipped.

### `--record-audio <file>`

Records the organ's output to this 24-bit WAV file while the take plays. The recording is made inside the program, so no other sound gets into it and nothing is lost if the machine stutters. It starts with the music. With several takes, put a `--record-audio` after each `--play-midi`. The output is stereo at the audio device's sample rate. The folder is created when it does not exist.

### `--draw-stops <all | n | ids>`

The stops to draw for a take:

| Value | Stops drawn |
|---|---|
| `all` | Every stop. |
| a number, such as `4` | The first that many stops of the organ's stop list. The list is ordered by division, so on most organs `4` draws four pedal stops. |
| a comma-separated list, such as `12,15,31` | Exactly those stops, by `StopID` in the organ definition. An id the organ does not have is logged and skipped. |

A `--draw-stops` that follows a `--play-midi` applies to the next take, so it can stand on either side of its file.

### `--registration <name>`

Draws stops chosen from their names, for an organ that has no preset.

| Name | What it draws |
|---|---|
| `plenum` | Principal chorus and mixture: preludes and fugues. Any unknown name also gives this. |
| `tutti` (or `all`) | Everything. |
| `chorale` | One flute and a quiet pedal. |
| `trio` | Two contrasting manuals and a light bass. |
| `solo` | A reed or a mixture against a soft accompaniment. |

Like `--draw-stops`, a `--registration` that follows a `--play-midi` applies to the next take. A list of stop ids takes priority over a registration, and a registration over `all` or a number.

### `--preload-drawn`

Loads only the ranks that the drawn stops use. On a large set this reduces the time to try a registration from minutes to seconds. It needs a `--draw-stops` list of ids and is ignored otherwise (the log says so). Every stop that is not drawn is silent for the session. A load of this kind writes its own cache, which a later full load does not use.

### `--preload-ranks <ids>`

Loads exactly these ranks, given as comma-separated rank ids such as `2,4,14`. Use it for organs whose stops reach their pipes through pallets: the drawn stops name no ranks there, so `--preload-drawn` cannot narrow the load.

### `--draw-only`

Loads the organ, draws the stops chosen with `--draw-stops` or `--registration`, and hands the console over. Nothing plays and Masterpiece stays open. Use it to open an organ with a registration already drawn.

### `--stay-open`

Stays open after the last take has played. Staying open saves loading a large organ again.

## Loading and memory

These options set how the next organ is loaded. They are the settings of **Organ settings > Loading**. A value given here takes precedence over the saved settings for that run, except `--cache`: a cache mode saved in the organ's settings still applies. The log line `memory config:` shows the values in effect.

| Option | Values | Default | Effect |
|---|---|---|---|
| `--storage` | `int16`, `int24`, `float32` | `int24` | How each sample frame is held in memory. `int16` uses a third less than `int24` and is enough for most sets. `float32` uses the most and holds the samples as decoded. Another value is logged and ignored. |
| `--load-mono` | `on`, `off` | `off` | Folds stereo samples to one channel as they load. Halves the memory and removes the stereo image. `on`, `1`, `true` and `yes` switch it on; any other value switches it off. |
| `--load-rate` | a rate in Hz, such as `48000`; `0` | `0` (as recorded) | Converts the samples to this rate as they load. Converting a 96 kHz set to 48 kHz halves its memory. |
| `--stream-releases` | `on`, `off` | `off` | Keeps only the start of each release tail in memory and reads the rest from disk while it plays. Saves a large share of the memory on sets with long releases and needs a fast disk. Accepts the same words as `--load-mono`. |
| `--preload-head` | a number of frames | `0` (whole files) | The minimum number of frames held in memory at the start of every sample. |
| `--cache` | `off`, `single`, `per-organ` | `single` | The sample cache keeps decoded samples on disk, so the next load of the same organ is much faster. `single` keeps one cache and replaces it when a different organ loads. `per-organ` keeps one cache per organ and uses more disk. `off` writes none. Any other value means `single`. |

A load in which some samples failed to read writes no cache, so a damaged installation is never cached as complete.

## Examples

Open an organ:

```
Masterpiece "C:\Organs\Friesach.Organ_Hauptwerk_xml"
```

Check how a console looks on its second page, with no samples loaded:

```
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" --gui-only --console-page 2
```

Collect what a report about notes that do not sound needs:

```
Masterpiece --log report.log --log-midi --record-midi report.mid
```

Load an organ with a plenum drawn and play it by hand:

```
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" --registration plenum --draw-only
```

Record two pieces on two registrations. Only the drawn ranks load, and the samples are held as 16-bit mono:

```
Masterpiece --odf "Friesach.Organ_Hauptwerk_xml" ^
  --storage int16 --load-mono on --preload-drawn ^
  --draw-stops 12,15 --play-midi chorale.mid --record-audio chorale.wav ^
  --draw-stops 12,15,31,40 --play-midi fugue.mid --record-audio fugue.wav
```

The `^` ends a line in the Windows command prompt. On macOS and Linux, end the lines with `\`.
