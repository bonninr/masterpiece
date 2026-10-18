# Introduction

Masterpiece is a free, open-source pipe organ sample player. It plays
Hauptwerk-format sample sets and GrandOrgue organs, each with its own console,
wiring, wind and swell boxes, on Windows, macOS, Linux, Raspberry Pi, Android,
iPhone and iPad, as a standalone program and as VST3, LV2 and AU plugins. It is
part of [OpenPipes](https://openpipes.org), an open-source virtual pipe organ
platform, and is published under the GPL-3.0 licence.

![The Nancy demo in Masterpiece](images/console-nancy.jpg)

This manual has two parts. The **user guide** covers installing the program,
opening sample sets, playing, connecting a console, audio, and the memory and
loading choices that decide which organs a computer can hold, followed by a
complete reference of every setting. The **developer guide** covers building
Masterpiece from source, its architecture, its tests and its performance
figures.

| | |
|---|---|
| Sample set formats | Hauptwerk-format organs (full, compact and composite definitions), GrandOrgue organs (`.organ` definitions and `.orgue` packages) |
| Packages | Installed folders, or straight from `.rar` packages, including solid, multi-part and multi-volume sets; a solid RAR 4 set split into volumes is the exception |
| Samples | WAV and WavPack, at any sample rate |
| Systems | Windows, macOS (Apple silicon and Intel), Linux (x86-64, Flatpak, Debian packages), Raspberry Pi (64-bit and 32-bit), Android, iPhone and iPad |
| Plugins | VST3, LV2, AU |
| Engine | Hand-written AVX2, SSE2 and NEON vector code; 725 voices per processor thread by default, up to 16,384 |
| Memory | Samples held at 24 or 16 bits or folded to mono, release tails streamed from disk, and a sample cache for fast loads |
| Control | A MIDI channel per manual, MIDI learn for every stop, piston and swell shoe (SysEx included), generals, divisionals and a 999-frame stepper, temperaments, pitch and a transposer |

## Where to start

- [Installing Masterpiece](guide/installing.md) for the download for your
  system.
- [Opening an organ](guide/opening.md) for sample sets in folders and in their
  packages.
- [MIDI and consoles](guide/midi.md) to connect keyboards, pistons and shoes.
- [Memory, loading and streaming](guide/memory.md) and
  [Small, medium and large organs](guide/sizing.md) before loading a large set.
- [Troubleshooting and caveats](guide/troubleshooting.md) when something does
  not sound as expected.

Questions and ideas are welcome in the
[discussions](https://github.com/bonninr/masterpiece/discussions), and
problems in the [issues](https://github.com/bonninr/masterpiece/issues) of the
repository on GitHub.
