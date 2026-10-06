<p align="center">
  <img width="500" alt="Masterpiece, virtual pipe organ" src="screenshots/logo.png" />
</p>

### Pipe organ sample player for Hauptwerk-format and GrandOrgue sample sets.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/openpipesorg)

https://github.com/user-attachments/assets/f9e38610-aaf4-4b95-8d1c-e8f7f04b7d77

<!-- hero: the demonstration clip goes on the next line, as a github.com/user-attachments URL -->
**[Watch the demonstration](https://bonninr.github.io/masterpiece/#hear)** — thirty-three works on nine organs, recorded from the application's own output · [programme and credits](ATTRIBUTION.md#music)  
**[Watch "New tested instruments"](https://bonninr.github.io/masterpiece/#hear-2)** — thirty works on fifteen organs, filmed from the running console · [programme and credits](https://bonninr.github.io/masterpiece/attribution-2.html)

[![Windows](https://img.shields.io/badge/Download-Windows%20installer-0078D6?logo=windows&logoColor=white)](https://github.com/bonninr/masterpiece/releases/download/v0.7.4/masterpiece-0.7.4-windows-setup.exe)
[![macOS Apple silicon](https://img.shields.io/badge/Download-macOS%20Apple%20silicon-000000?logo=apple&logoColor=white)](https://github.com/bonninr/masterpiece/releases/download/v0.7.4/masterpiece-0.7.4-macos-arm64.zip)
[![macOS Intel](https://img.shields.io/badge/Download-macOS%20Intel-000000?logo=apple&logoColor=white)](https://github.com/bonninr/masterpiece/releases/download/v0.7.4/masterpiece-0.7.4-macos-x86_64.zip)
[![Debian and Ubuntu](https://img.shields.io/badge/Download-Debian%20%2F%20Ubuntu%20.deb-A81D33?logo=debian&logoColor=white)](https://github.com/bonninr/masterpiece/releases/download/v0.7.4/masterpiece-0.7.4-linux-amd64.deb)
[![Raspberry Pi 64-bit](https://img.shields.io/badge/Download-Raspberry%20Pi%2064--bit%20.deb-A22846?logo=raspberrypi&logoColor=white)](https://github.com/bonninr/masterpiece/releases/download/v0.7.4/masterpiece-0.7.4-linux-arm64.deb)
[![Raspberry Pi 32-bit](https://img.shields.io/badge/Download-Raspberry%20Pi%2032--bit%20.deb-A22846?logo=raspberrypi&logoColor=white)](https://github.com/bonninr/masterpiece/releases/download/v0.7.4/masterpiece-0.7.4-linux-armhf.deb)

Portable archives for every platform, and the plugins on their own, are on the
[releases page](https://github.com/bonninr/masterpiece/releases/latest).

[![build](https://github.com/bonninr/masterpiece/actions/workflows/build.yml/badge.svg)](https://github.com/bonninr/masterpiece/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/bonninr/masterpiece?include_prereleases&label=release&color=c8a97e)](https://github.com/bonninr/masterpiece/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![JUCE 9](https://img.shields.io/badge/JUCE-9-8DC63F?logo=juce&logoColor=white)](https://juce.com/)
[![CMake](https://img.shields.io/badge/CMake%20%2B%20Ninja-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![platforms](https://img.shields.io/badge/Windows%20%7C%20macOS%20%7C%20Linux%20%7C%20Raspberry%20Pi-555?logo=linux&logoColor=white)](#building)
[![formats](https://img.shields.io/badge/Standalone%20%7C%20VST3%20%7C%20LV2%20%7C%20AU-6b4f9e)](#building)
[![licence](https://img.shields.io/badge/licence-GPL--3.0--only-blue)](LICENCE)

---

Masterpiece plays sampled pipe organs. It reads Hauptwerk-format sample sets
and GrandOrgue organs, draws each instrument's own console, and plays it from
the mouse, a MIDI keyboard or a full console. It is an independent
implementation in C++20 and JUCE, licensed GPL-3.0-only.

- Standalone application, VST3 and LV2 plugins, and an Audio Unit on macOS
- Windows, macOS (Apple silicon and Intel), Linux, Raspberry Pi (64- and 32-bit),
  Android and iPhone/iPad
- Tested on 37 published sample sets, from a 9-register Flentrop to Nancy's
  65 stops on four manuals

## Features

**Sample sets**
- Hauptwerk-format definitions, full and compact, read directly
- GrandOrgue `.organ` definitions, converted as they load, console included
- Sets played straight from their packages: RAR 4 and 5 (solid, multi-part,
  multi-volume) and GrandOrgue `.orgue`
- WAV and WavPack samples, any rate; optional conversion to the device rate on load
- Sets that require a publisher's licence load once you confirm you hold it

**Console**
- The set's own artwork: drawstops, keys, pedals, shoes, jamb pages, several
  console sizes where a set ships them
- Couplers, sub- and super-octaves, unison off, reversible pistons, crescendo
  and alternate ranks, all through the set's own switch network
- A stop list grouped by division, for sets drawn across many jambs or shipped as text only
- Any page in its own window, remembered per organ

**Sound**
- Attack, crossfaded sustain loop, and a release chosen by how long the key was
  held, with a crossfade into it
- Wind model: chests, bellows and valves solved as a system; a full registration
  lowers the pressure, and pitch and level follow it
- Tremulants per pipe and per chest; enclosures with level and filter per box
- Hermite interpolation; 1536 voices, stealing releases first
- Optional vectorized engine, AVX2, SSE2 or NEON, 3 to 5 times faster on 512
  voices (experimental, off by default)

**Memory and loading**
- 24-bit resident (bit-identical to the files) or 16-bit; stereo can be folded
  to mono
- Release tails streamed from disk through per-voice ring buffers; keep the
  first second or 25, 50 or 75% of each release in memory
- Leave out stops, perspectives or single ranks; left-out stops take their
  tremulant ranks with them
- Decoded sample cache: a second load reads one cache file
- A memory limit, 80% of RAM by default; a load that reaches it stops and says what to change
- Optionally keep an organ playable after its installation files are removed

**Combinations and registration**
- Player's own generals (up to 100), divisionals per manual, general cancel,
  and a 999-frame stepper, on any organ, beside the organ's own pistons
- Named combination sets, saved per organ
- Optional: Set turns off after storing a piston with the mouse

**Tuning**
- The organ's temperament, equal, or Werckmeister III, Kirnberger III,
  Vallotti, Young II, quarter-comma meantone, Pythagorean, Silbermann
  sixth-comma; Scala `.scl` files
- Pitch in Hz (415, 440, 442 one click away); transposer ±12 semitones
- Voicing: level and tuning per rank and per pipe, with A/B sets to compare

**MIDI and consoles**
- All inputs at once, each mapped to a manual by channel; key range, transpose,
  velocity window, short octaves, debounce, split keyboards
- MIDI learn for drawstops, pistons, shoes, pages, stepper and transposer;
  MIDI out lights a console's drawstops
- Jamb text displays over system exclusive
- A virtual MIDI input on macOS and Linux

**Recording and practice**
- MIDI recording of notes, stops and shoes, replayable through another registration
- Audio recording alongside; metronome in the organ's own audio stream
- Convolution reverb for sets recorded dry

## Limitations

- Encrypted samples (`.hbw`, `.hbx`) are skipped and listed in the load report
- GrandOrgue divisional couplers are skipped on import

## Measured

Friesach, 44 stops, 17 GB, 12,148 files ([method and full figures](PERFORMANCE.md)):

| Resident format | Memory |
|---|---|
| 32-bit float | 21.6 GB |
| 24-bit | 16.2 GB |
| 16-bit, releases streamed | 4.65 GB |
| 16-bit mono, releases streamed | 2.33 GB |

Opening: 77.1 s at 32-bit float, 25.9 s at 16-bit mono streamed, 18.9 s from the
sample cache.
Streaming assumes SSD-class storage; the status line says when a disk falls behind.

## Tested organs

Each set below is a separate, freely published library: 23 by
[Piotr Grabowski](https://piotrgrabowski.pl/), 14 by
[Augustine's Virtual Organs](https://hauptwerk-augustine.info/). Full credits
in [ATTRIBUTION.md](ATTRIBUTION.md).

<details>
<summary>Screenshots of all 37</summary>

| | |
|:--:|:--:|
| **Lipiny** — a historic case, drawstops lettered in Fraktur | **Melcer Chamber Music Hall** |
| ![Lipiny](screenshots/Lipiny.jpg) | ![Melcer](screenshots/MelcerChamberMusicHall.jpg) |
| A. Volkmann, 1898 · 25 stops · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/lipiny/) | chamber organ · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/melcer-chamber-music-hall/) |
| **Azzio** | **Kraków, St. John Cantius** |
| ![Azzio](screenshots/Azzio.jpg) | ![Kraków](screenshots/CracowStJohnCantius.jpg) |
| Mascioni, 2016 · 12 stops · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/azzio/) | 40 stops, three manuals · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/cracow-st-john-cantius/) |
| **Długa Kościelna** | **Raszczyce** |
| ![Długa Kościelna](screenshots/DlugaKoscielna.jpg) | ![Raszczyce](screenshots/Raszczyce.jpg) |
| 22 stops · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/dluga-koscielna/) | Vermeulen, 1965 · 21 stops · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/raszczyce/) |
| **Strassburg** | **Friesach** — three manuals, jambs on their own pages |
| ![Strassburg](screenshots/Strassburg.jpg) | ![Friesach](screenshots/Friesach.jpg) |
| C. Werner, 1743 · 20 stops · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/strassburg/) | Eisenbarth, 2000 · 44 stops · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/friesach/) |
| **Nancy** — four manuals; the keys are part of the photograph, and the drawstops are colour-coded by division | **Lemmer** — a Flentrop under the saints, with the recording perspective on the case |
| ![Nancy](screenshots/Nancy.jpg) | ![Lemmer](screenshots/Lemmer.jpg) |
| 65 stops, four manuals · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/nancy/) | Flentrop, 1977–78 · 9 registers · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Lemmer.php) |
| **Giubiasco** | **Alessandria** — pipes reached through pallet switches |
| ![Giubiasco](screenshots/Giubiasco.jpg) | ![Alessandria](screenshots/Alessandria.jpg) |
| complete set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/giubiasco/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/alessandria/) |
| **Pinerolo** | **Ermelo** |
| ![Pinerolo](screenshots/Pinerolo.jpg) | ![Ermelo](screenshots/Ermelo.jpg) |
| demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/pinerolo/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/ermelo/) |
| **Święta Lipka** — pipes reached through pallet switches | **Nitra** |
| ![Święta Lipka](screenshots/SwietaLipka.jpg) | ![Nitra](screenshots/Nitra.jpg) |
| demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/swieta-lipka/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/nitra/) |
| **Erfurt-Büßleben** | **Erfurt, Predigerkirche** — pipes reached through pallet switches |
| ![Erfurt-Büßleben](screenshots/ErfurtBussleben.jpg) | ![Erfurt, Predigerkirche](screenshots/ErfurtPredigerkirche.jpg) |
| demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/erfurt-bussleben/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/erfurt-predigerkirche/) |
| **Obervellach** | **Düren** |
| ![Obervellach](screenshots/Obervellach.jpg) | ![Düren](screenshots/Duren.jpg) |
| demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/obervellach/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/duren/) |
| **Chorzów, St Hedwig of Silesia** | **Goch** |
| ![Chorzów, St Hedwig of Silesia](screenshots/Chorzow.jpg) | ![Goch](screenshots/Goch.jpg) |
| demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/chorzow-sw-jadwiga-slaska/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/goch/) |
| **Oloron-Sainte-Marie** | **Bégard** |
| ![Oloron-Sainte-Marie](screenshots/OloronSainteMarie.jpg) | ![Bégard](screenshots/Begard.jpg) |
| demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/oloron-sainte-marie/) | demonstration set · [sample set by Piotr Grabowski](https://piotrgrabowski.pl/begard/) |
| **Nagyrákos** — an Angster organ | **Hungarian village organs** — the organs of two village churches in one set |
| ![Nagyrákos](screenshots/AngsterNagyrakos.jpg) | ![Hungarian village organs](screenshots/HungarianVillage.jpg) |
| Angster, 1938 · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Angster_Nagyrakos.php) | composite set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Hungarian_village.php) |
| **Zalalövő** — a modern baroque organ | **Császár** — a Luber organ |
| ![Zalalövő](screenshots/Zalalovo.jpg) | ![Császár](screenshots/LuberCsaszar.jpg) |
| Aquincum · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Zalalovo.php) | F. X. Luber, 1793 · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Luber_Csaszar.php) |
| **Vasvár, Dominican church** | **Kézdimartonfalva** — a Dutch harmonium |
| ![Vasvár, Dominican church](screenshots/Vasvar.jpg) | ![Kézdimartonfalva](screenshots/DutchHarmonium.jpg) |
| demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Vasvar.php) | harmonium · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Dutch_harmonium.php) |
| **Szombathely** — an Eule organ | **Hajós** — an Angster organ |
| ![Szombathely](screenshots/EuleSzombathely.jpg) | ![Hajós](screenshots/AngsterHajos.jpg) |
| Eule · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Eule_Szombathely.php) | Angster · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Angster_Hajos.php) |
| **Székesfehérvár, Franciscan church** — a Mauracher organ | **Székesfehérvár, Cistercian church** — an Angster organ |
| ![Székesfehérvár, Franciscan church](screenshots/MauracherSzekesfehervar.jpg) | ![Székesfehérvár, Cistercian church](screenshots/AngsterCistercian.jpg) |
| Mauracher · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Mauracher_Fehervar.php) | Angster · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Angster__Cistercian.php) |
| **Ják Abbey** — an Angster organ | **Monor, Reformed church** — a Rieger organ |
| ![Ják Abbey](screenshots/AngsterJak.jpg) | ![Monor, Reformed church](screenshots/RiegerMonor.jpg) |
| Angster, 1902 · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Angster_Jak.php) | Rieger · demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Rieger_Monor.php) |
| **Budapest, Church of the Holy Spirit** |  |
| ![Budapest, Church of the Holy Spirit](screenshots/HolySpiritBudapest.jpg) |  |
| demonstration set · [sample set by Augustine's Virtual Organs](https://hauptwerk-augustine.info/Holy_Spirit.php) |  |

</details>

## Installing

**Windows.** `masterpiece-<version>-windows-setup.exe` installs with a Start
menu entry; the portable `.zip` runs from any folder and includes the VST3 plugin.

**Debian, Ubuntu, Raspberry Pi OS.**

```
sudo apt install ./masterpiece-0.7.4-linux-amd64.deb     # PC
sudo apt install ./masterpiece-0.7.4-linux-arm64.deb     # Raspberry Pi OS, 64-bit
sudo apt install ./masterpiece-0.7.4-linux-armhf.deb     # Raspberry Pi OS, 32-bit
```

Runs as `masterpiece`; the PC package also installs the plugins to
`/usr/lib/vst3` and `/usr/lib/lv2`. Other distributions: the `.tar.gz` archives.

**macOS.** Move `Masterpiece.app` to Applications. The build is unsigned; if
Gatekeeper reports it as damaged, run
`xattr -cr /Applications/Masterpiece.app` and open it with right-click, Open.

**Android, iPhone and iPad.** The `.apk` and `.ipa` on the releases page are
installed directly; see the [0.7.0 release notes](https://github.com/bonninr/masterpiece/releases/tag/v0.7.0).

## Running

**Open** takes a definition, a GrandOrgue `.organ` file, or a RAR or `.orgue`
package. File managers offer *Open with Masterpiece*; `Masterpiece <file>`
does the same from a terminal.

| Option | |
|---|---|
| `--odf <file>` | Load this organ at startup |
| `--gui-only` | Draw the console only, silent, in a second or two |
| `--log <file>` | Write a log, with load timings |
| `--log-midi` | Log every MIDI message and where it went |
| `--record-midi <file>` | Record the session, saved on quit |
| `--play-midi <file>` | Play a MIDI file through the organ |
| `--virtual-midi [name]` | Publish a MIDI input (macOS, Linux) |

All options: `Masterpiece --help` and [COMMAND-LINE.md](COMMAND-LINE.md).
Each session writes `masterpiece.log` in the settings folder; attach it to bug reports.

**Audio on Windows:** WASAPI exclusive or ASIO for low latency; shared mode adds
noticeable delay. **On Linux:** ALSA and JACK (including PipeWire's JACK);
JACK is listed when a JACK library is installed.

## Building

C++20 (MSVC 2022, GCC 12+, Clang 14+), CMake 3.22+, Ninja. JUCE, pugixml and
the archive libraries are fetched by CMake.

- **Debian, Ubuntu, Raspberry Pi OS:**
  ```bash
  sudo apt-get install -y build-essential git cmake ninja-build pkg-config \
    libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
    libxcomposite-dev libfreetype6-dev libfontconfig1-dev libglu1-mesa-dev \
    mesa-common-dev libxi-dev libjack-jackd2-dev
  ```
- **macOS:** `xcode-select --install`, then `brew install cmake ninja`.
- **Windows:** Visual Studio 2022 Build Tools with *Desktop development with
  C++*; build from the *x64 Native Tools* prompt.

```bash
cmake --preset dev
cmake --build --preset dev
```

CI presets: `ci-linux`, `ci-macos`, `ci-windows`, `ci-linux-arm` (32-bit
Raspberry Pi cross-build).

**Tests** run on the small organs in `tests/fixtures`, in seconds:

```
cmake --build --preset dev --target mp_tests
build/dev/tests/mp_tests --no-perf
```

**Releasing:** `cmake/set-version.sh <version>`, merge, then tag `v<version>`;
the release workflow builds and publishes every platform.

## Layout

```
mp_core      definition loader, GrandOrgue import, validator, temperaments
mp_sampler   voice engine, release streaming
mp_control   key flow, couplers, switch network, pistons, stepper, crescendo, wind
mp_archive   RAR and .orgue packages, read in place
mp_dsp       enclosure filters, tremulant modulation
mp_audio     audio processor, sample storage, cache, routing
mp_ui        console, settings, MIDI learn
```

`mp_core`, `mp_sampler`, `mp_control` and `mp_archive` are plain C++20 and
pugixml, so the musical path is tested against synthesised tones.

## Credits

Written with four projects as references:
[GrandOrgue](https://github.com/GrandOrgue/GrandOrgue) (voice engine, release
crossfades), [OdfEdit](https://github.com/GrandOrgue/OdfEdit) (the definition
format), [rusty-pipes](https://github.com/dividebysandwich/rusty-pipes) (samples,
loops, file formats) and [HISE](https://github.com/christophhart/HISE)
(streaming through per-voice buffers). Sample sets and MIDI sequences:
[ATTRIBUTION.md](ATTRIBUTION.md).

## Licence

GPL-3.0-only, with an additional permission for combining it with JUCE and
UnRAR under their own licences: [`LICENCE`](LICENCE), [`COPYING`](COPYING).
Contributing: [`CONTRIBUTING.md`](CONTRIBUTING.md).

The Windows build includes ASIO support, used under the GPL option of the
Steinberg ASIO SDK. ASIO is a trademark of Steinberg Media Technologies GmbH.
Hauptwerk is a trademark of its owner. Masterpiece is an independent project.
