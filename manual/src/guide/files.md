# Files and folders

Masterpiece keeps its own files in one settings folder in the user's profile and writes a few files next to an organ's installation. This page lists every file and folder the program reads or writes, where each lives on each system, what it holds, and what happens when it is deleted. It ends with what the program sends over a network: the once-a-day update check, and nothing else.

## The settings folder

| System | Settings folder |
|---|---|
| Windows | `%APPDATA%\Masterpiece` (`C:\Users\<name>\AppData\Roaming\Masterpiece`) |
| macOS | `~/Library/Masterpiece` |
| Linux | `~/.config/Masterpiece` |
| Linux (Flatpak) | The same path inside the application's sandbox storage |
| Android | `Masterpiece` in the app's private storage, which other apps cannot reach |
| iPhone and iPad | `Masterpiece` in the app's private storage |

The folder is hidden on Windows (`AppData`) and on macOS (`Library`). **Settings > General settings > Log > Open the log folder** opens it on a desktop system.

The standalone program and the plugins share this folder, so both see the same settings, combinations and MIDI mappings. On a phone or tablet, uninstalling the app removes it.

The Flatpak sandbox gives the program access to the home folder, `/media`, `/run/media` and `/mnt`, so organs on those paths open without further steps. Other paths are outside the sandbox.

### Layout

```
Masterpiece/
  settings.mpglobal            global settings and favourites
  audio.xml                    audio device state (standalone program)
  window.txt                   main window position (standalone program)
  dialogs.txt                  dialog sizes
  jack-connections.txt         JACK MIDI connections (Linux)
  split-keyboard.txt           split keyboard presets and controls
  masterpiece.log              this session's log (standalone program)
  masterpiece.previous.log     the previous session's log
  organs/                      <organ>.mporgan, <organ>.mppanels
  midi/                        <organ>.mpmidi, console.mpmidi
  combinations/                <organ>.mpcomb, <organ>.<set>.mpcomb
  split/                       <organ>.mpsplit
  cache/                       samples.mpcache, samples-<organ>.mpcache
  Packaged/<id>/               organs unpacked from their archives
  Portable/                    copies that keep an organ playable without its files
  Standard noises/             generated stand-ins for a set's standard noises
  Organs/                      packages copied into the app (Android)
  Linked/                      packages read where they are (Android)
```

### How a file is named for an organ

Per-organ files are named by the organ's identity, so renaming or moving the sample set keeps its files.

- An organ that declares an identity is `<name>-<UniqueOrganID>`. `<name>` is the organ's name with letters and digits kept, spaces, hyphens and underscores turned into hyphens, cut to 48 characters.
- An organ that declares none is `<name>-p<16 hexadecimal digits>`, a hash of the definition's full path in lower case. Moving such an organ starts it with fresh files.
- Files written by older versions under the definition's own file name are adopted and rewritten under the new name at the next save.

## Global files

### `settings.mpglobal`

A text file, one `key value` per line, lines starting with `#` ignored. It holds the settings that apply to every organ and the favourites. The first start of the standalone program shows the first-run wizard while this file does not exist; a plugin load creates it.

| Key | Holds | Set with |
|---|---|---|
| `reopenlastorgan` | `1` to reopen the last organ at startup. Default off. | **Reopen the last organ when Masterpiece starts** |
| `updatecheck` | `0` turns the once-a-day update check off; written only then. | **Check once a day for a newer version** |
| `updatecheckedat` | The time of the last update check, in seconds since 1970. | The update check |
| `lastorgan` | Path of the last organ that loaded completely. | Automatic, after each load |
| `running` | Path of the organ being loaded or loaded; the crash guard's mark. Removed at a clean exit. | Automatic (standalone program) |
| `memlimit` | Memory limit for samples, in MB. Absent: 80% of the computer's memory. | **Memory limit for samples** |
| `polyphony` | Polyphony limit in voices. Absent: the default. | **Polyphony limit** |
| `fasterengine` | `1` or `0`. Default `1`. | **Faster audio engine** |
| `realtimethreads` | `1`. Written only while on. | **Real-time render threads (trial)** |
| `loadticks` | `1` for a tap at each 10% of a load. | **Tap at each 10% while loading an organ** |
| `speakerkeepalive` | Level in dB, -80 to -40, of the 20 Hz tone. Absent: off. | **Keep portable speakers awake** |
| `language` | Language code. Absent: the system language. | **Settings > General settings > Language** |
| `cancelresetskeyboards`, `learnnoisefilter` | `1` when on. | **Settings > General settings > MIDI** |
| `combinationsontop`, `setoffafterstore` | `1` when on. | **Always on top**, **Set turns off after storing** in the Combinations window |
| `showkeys` | `0` when played keys are hidden. | **View > Show played keys** |
| `cachewhileplaying` | `1` saves the cache in silence while the organ plays. | **Save the sample cache** |
| `midiinputoff` | Name of a MIDI input that is switched off; one line per input. | The inputs list in **General settings > MIDI** |
| `consolechannel` | A console role and its MIDI channel, set as the player's own console. | **General settings > MIDI** |
| `midioutput` | Name of the MIDI output port. The port opens again at the next start; a port that is not plugged in keeps its name. | **MIDI output** in **General settings > MIDI** |
| `midifeedback` | `1` when on. | **Send stop changes back to the console** |
| `metronome` | `<bpm> <beats> <level>`: tempo, beats per bar and level. Written when the Metronome tab closes. The click itself starts silent. | **General settings > Metronome** |
| `lcd` | `<on> <display> <width> <line1> <line2> <line3> <line4> <prefix>`: the console display setup. | **General settings > Display** |
| `defaulteq` | `<on> <gain> <band 1> ... <band 10>`: the EQ that every organ without an EQ of its own starts with. Absent while the default is off and flat. | **Save as default** in **Organ settings > Voicing EQ** |
| `library` | A folder of sample sets Masterpiece has seen or been given; one line each. The standard library folder of Hauptwerk-format installations in the home folder, `~/Hauptwerk/HauptwerkSampleSetsAndComponents`, is added when it exists. | Automatic |
| `cachedir` | Folder of the sample cache. Absent: the `cache` folder here. | **Cache folder** |
| `packagedir` | Package folder where composites look for the organs they play. | **Package folder** |
| `openfolder`, `dialogfolder <kind> <path>` | Where the file dialogs opened last. | Automatic |
| `favourite <kind> <slot> <name> \| <target>` | The favourites of organs, temperaments and combination sets. | **File > Open...**, the favourites window |
| `storage`, `mono`, `releases`, `releaselength`, `releasecutoff`, `rate`, `cache`, `stream`, `streamhead`, `streamheadpct`, `preload`, `simple`, `wind`, `tremulant`, `enclosure`, `voicing`, `originalpitch`, `buses`, `groups` and others | The defaults that an organ without its own settings starts with. | **Save as default** in **Organ settings > Loading** |

The standalone program rewrites the file at once when most of these change. The plugins read it at each organ load.

Deleting `settings.mpglobal` resets every general setting, empties the favourites and the library list, turns the first-run wizard on again, and removes the crash mark. Organs keep their own files.

### `audio.xml`

The audio device state: driver, device, sample rate, buffer size and channels, as the audio device manager writes it. Written when the standalone program quits. A saved state that no longer matches the hardware is dropped and the defaults return. Deleting it resets the audio device. The plugins do not use it.

### `window.txt` and `dialogs.txt`

`window.txt` holds the position and state of the main window. `dialogs.txt` holds the size each kind of dialog was last left at, one `kind=width height` line each. A window position that no screen shows is moved onto a screen at the next start. Deleting either resets the layout.

### `jack-connections.txt`

On Linux, the JACK ports connected to Masterpiece's JACK MIDI input, one port name per line, restored at the next start. Deleting it drops the connections.

### `split-keyboard.txt`

The split keyboard's presets, triggers and the chosen source keyboard. See [Split keyboard](split-keyboard.md). Deleting it removes the presets.

## Per-organ files

All files below are in the settings folder, named `<organ>` as described above.

| File | Holds | Written |
|---|---|---|
| `organs/<organ>.mporgan` | The organ's settings: loading choices, engine switches, master gain, reverb impulse response, voicing and the organ's own voicing EQ (the `eq` line, present once **Save for this organ** has been pressed on the **Voicing EQ** tab), mixer routes, stops and perspectives left out, tuning, window places, the registration saved with **Save state**, and the controls and switches the organ marks to be remembered. | **Save for this organ**, **Save state**, the volume slider, and when controls move |
| `organs/<organ>.mppanels` | The organ's panels (**View** menu panels). | When a panel changes |
| `midi/<organ>.mpmidi` | The organ's MIDI mapping: manuals, stops, shoes, divisionals. | When the mapping changes |
| `midi/console.mpmidi` | The player's console on every organ: stepper, generals, setter. | With the organ's MIDI mapping |
| `midi/<organ>.mpmidi.before-repair` | A copy of a mapping from version 0.3.7 or earlier, kept when the program repaired manuals that shared a channel. | Once, at the repair |
| `combinations/<organ>.mpcomb` | The default combination set: the organ's pistons as the player set them, and the player's generals, divisionals and stepper frames (lines prefixed `player`). | When a piston is stored |
| `combinations/<organ>.<set>.mpcomb` | A named combination set. | **Save as new set...** |
| `split/<organ>.mpsplit` | The organ's split keyboard layout. | When the layout changes |

Deleting a per-organ file resets that part for that organ. Deleting `organs/<organ>.mporgan` makes the standalone program open **Organ settings** again before the organ's next first load. Deleting a `.mpcomb` file removes the stored pistons and cannot be undone; copy the file first if the registrations matter.

### Files next to an organ's samples

When a composite organ plays packages that are not in its folder, Masterpiece records where it found them in the organ's root folder, the folder that holds `OrganInstallationPackages` (for an organ opened from packages, its folder under `Packaged`):

| File | Holds |
|---|---|
| `package-folders.txt` | One line per package: its number, a tab, and the folder the player chose. |
| `joined-packages.txt` | Archives of other organs that supply packages, one path per line. |
| `joined-<id>.txt` | The file index of such an archive, so that it is read once. |

Deleting them makes the next load search again and, if needed, ask for the packages again. For an installed Hauptwerk-format organ these files are written inside the installation folder. Masterpiece writes nothing else there.

## The sample cache

`cache/samples.mpcache` is the decoded samples of the last organ loaded. With **Sample cache** set to **One cache per organ (uses more disk)**, each organ has `samples-<organ>.mpcache`. A cache is read only when the organ's definition (size and modification time) and every loading setting that changes the stored bytes match; any difference rebuilds it. A file ending in `.part` is a cache being written; the program moves it into place when it is whole.

The folder is the `cache` folder of the settings folder, or the **Cache folder** chosen in **Organ settings > Loading**. If the chosen folder cannot be created, the usual folder is used and **Run the checks** reports it.

Deleting a cache file is safe: the next load of the organ decodes the samples again and writes a new cache, which takes as long as the first load. Caches can reach several gigabytes. An organ set to **Keep this organ playable without its installation files** depends on its cache when its drive is unplugged; deleting that cache ends this until the organ loads completely again with its drive connected.

## Organs opened from packages

### `Packaged/<id>/`

When a `.rar` or `.orgue` package opens, Masterpiece unpacks the definition and the console artwork into `Packaged/<id>`, where `<id>` is derived from the archives themselves, so the same packages open the same folder from any location. The samples stay in the archives and are read in place.

| File in the folder | Holds |
|---|---|
| `masterpiece-archive.txt` | The path of the archive. It exists only after an unpack finished without damage. |
| `archive-index.txt` | The list of files in the archives and where each one starts. |
| `chosen-definition.txt` | The definition last opened, when the packages hold several. |
| The unpacked definitions and artwork | Everything except the samples. |

Deleting a folder under `Packaged` is safe while the original archives exist: the next open indexes and unpacks them again, which can take several minutes for a large set. The organ's settings are in `organs/` and stay. A folder without `masterpiece-archive.txt` counts as an unpack that did not finish and is rebuilt. The count of these folders is one line in **Run the checks**.

### `Portable/`

Created for organs with **Keep this organ playable without its installation files**. `Portable/<organ>/` holds a copy of everything under the organ's root except the audio files (the definition, the console pictures and masks) and a `stamp.txt`; `Portable/index.txt` maps each original definition to its copy, one `original<TAB>copy` per line. When the original is missing, the program opens the copy and plays from the organ's cache. Deleting the folder removes the option until the next complete load.

### `Standard noises/`

Generated 16-bit stereo WAV files that stand in for a set's standard key, stop and blower noises when the set does not ship them. They are created when needed and are safe to delete.

## Phones and tablets

| Folder | Holds |
|---|---|
| `Organs/` | On Android, packages copied into the app when the player picks them. A package picked again with the same size is reused. |
| `Linked/` | On Android, links to packages read where they are, with `documents.txt` listing the documents to link again at the next start. |

On iPhone and iPad, packages come through the Files app and are read where they are. Deleting these folders on Android removes the copies; open the packages again to restore them. See [Phones and tablets](mobile.md).

## Logs

| File | Holds |
|---|---|
| `masterpiece.log` | The current session: version, audio device, each load stage with its time, MIDI events when logging is on, warnings and failures. |
| `masterpiece.previous.log` | The session before. |

The standalone program moves `masterpiece.log` to `masterpiece.previous.log` at each start and replaces the older previous log, so two starts in a row leave only the later two sessions. `--log <file>` writes the log to the named file and bypasses both. The plugins write no log. Deleting the logs is safe. See [Troubleshooting and caveats](troubleshooting.md).

**Run the checks** writes a temporary file `.masterpiece-check` in the settings and cache folders to test them and deletes it at once. **Send the report and the log** on a phone writes `masterpiece-report.txt` in the system's temporary folder and shares it with the log files.

## Recordings and other files you choose

Masterpiece writes these where the player says.

| Output | Format | Chosen in |
|---|---|---|
| Audio recording | 24-bit WAV of the output after the master fader and before the metronome | **File > Recorder...**, Audio |
| MIDI performance | Standard MIDI file with notes, stops and shoes, with a tempo of 60 bpm (one quarter note per second), so that a take keeps its length | **File > Recorder...**, **Save MIDI...** |
| `--record-audio`, `--record-midi` | The same formats | The command line |

An existing file with the chosen name is replaced. The file dialogs reopen in the folder last used for each kind.

Masterpiece reads the organs the player opens, the MIDI files the player loads, and the impulse responses and Scala `.scl` files the player picks. The paths of an impulse response and a Scala file are stored in the organ's `.mporgan` file.

## Privacy

Masterpiece's one connection is the update check, from version 0.8.1.

- **The update check.** Once a day, ten seconds after the program starts, it reads `https://masterpieceorgan.com/version.txt`: the number of the newest version, a few bytes, which the site's publishing writes at every release. When that number is newer, the right of the status bar reads **Masterpiece 0.8.1 is out** (with the number) until 30 seconds after the organ has loaded, or for 30 seconds when no organ is loading, with **Download**, which opens the download page, **Never check again** and a close button. Nothing takes the focus or waits for an answer. The request carries nothing about the player or the organs; like any request for a web page, it reaches GitHub Pages, which hosts the site, with the computer's IP address. Masterpiece keeps only the time of the last check (`updatecheckedat`). A check that gets no answer within 5 seconds says nothing and is tried again the next day. The plugins never check, and neither does a scripted run (`--play-midi`, `--gui-only`, `--draw-only`, or the environment variable `MASTERPIECE_UPDATE_CHECK=0`). `MASTERPIECE_UPDATE_URL` reads another address, for a test server.
- **Turning it off.** **Check once a day for a newer version** on the Log tab of General settings, or **Never check again** in the notice (`updatecheck 0` in `settings.mpglobal`).
- **On Linux** the file is read with the system's `curl` command; without one, the check does nothing. The build switches off JUCE's web browser and `curl` features.
- **On Android** the app asks for the internet permission for this check, besides Bluetooth (to find Bluetooth MIDI devices), location up to Android 11 (a platform requirement for Bluetooth scanning, never read) and storage up to Android 12 (to open organ files).
- **The Flatpak bundle** has network access for this check.
- There is no account, no analytics and no crash reporting.
- The log records what the program did, such as the organ that loaded and the devices found. It leaves the computer only when the player attaches it to a report. On a phone, **Send the report and the log** opens the system's share sheet, and the player chooses the destination.
- The LV2 identifier `https://masterpiece.audio/plugins/masterpiece` is a name that hosts compare. No program opens it.

The privacy policy is at [masterpieceorgan.com/privacy.html](https://masterpieceorgan.com/privacy.html).

## What is safe to delete

| Item | Effect of deleting it |
|---|---|
| `masterpiece.log`, `masterpiece.previous.log` | None. |
| `cache/` files | The next load rebuilds the cache. |
| `Standard noises/` | Regenerated when needed. |
| `Packaged/<id>/` | Unpacked again at the next open; needs the original archives. |
| `Portable/` | An organ kept playable without its files needs a complete load again. |
| `window.txt`, `dialogs.txt` | Window and dialog sizes reset. |
| `audio.xml` | The audio device resets to the defaults. |
| `jack-connections.txt` | JACK connections must be made again. |
| `midi/<organ>.mpmidi`, `midi/console.mpmidi` | MIDI mappings must be made again. |
| `organs/<organ>.mporgan` | The organ's settings reset; its **Organ settings** open before its next first load. |
| `settings.mpglobal` | All general settings, favourites and libraries reset; the first-run wizard returns. |
| `combinations/*.mpcomb`, `split/*.mpsplit`, `split-keyboard.txt` | The stored pistons, splits and presets are lost for good. |

Close Masterpiece before deleting files in the settings folder: the program rewrites several of them as it runs and at exit.
