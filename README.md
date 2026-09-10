# Masterpiece

A pipe organ sample player. It reads XML-defined sample sets, draws the
instrument's own console, and plays it.

![Friesach](screenshots/Friesach.jpg)

---

## What it is

Sampled pipe organs are distributed as large libraries: a recording of every
pipe, plus an XML definition describing the instrument around them — how the
console is drawn, which key reaches which pipe, what the couplers do, how the
wind system is built. Masterpiece reads those libraries and turns them back
into a playable instrument.

It is a fresh design rather than a fork. The console is the real one: the
artwork, the drawstop positions, the keyboards and the pedalboard all come out
of the set's own definition, so an organ looks and behaves like itself rather
than a generic mixer with the stop names changed.

It runs as a standalone application and as a VST3, AU or LV2 plugin — the same
engine either way.

---

## The console

The same program, reading different libraries.

| | |
|:--:|:--:|
| **Lipiny** — a historic case, drawstops lettered in Fraktur | **Melcer Chamber Music Hall** |
| ![Lipiny](screenshots/Lipiny.jpg) | ![Melcer](screenshots/MelcerChamberMusicHall.jpg) |
| **Azzio** | **Kraków, St. John Cantius** |
| ![Azzio](screenshots/Azzio.jpg) | ![Kraków](screenshots/CracowStJohnCantius.jpg) |
| **Długa Kościelna** | **Raszczyce** |
| ![Długa Kościelna](screenshots/DlugaKoscielna.jpg) | ![Raszczyce](screenshots/Raszczyce.jpg) |
| **Strassburg** | **Friesach** — three manuals, jambs on their own pages |
| ![Strassburg](screenshots/Strassburg.jpg) | ![Friesach](screenshots/Friesach.jpg) |

Sets that ship several console sizes offer them all; the chooser only appears
when there is a choice to make. Drawn keys and drawstops are clickable, and a
set whose manuals are part of a photographed backdrop falls back to an
on-screen keyboard instead.

---

## What it does

**The console.** Artwork, drawstops, pistons, expression shoes, text labels and
transparency masks, drawn manuals and pedalboards, multiple display pages, and
alternate layouts. Clicking a key or a drawstop takes exactly the same path as
the equivalent MIDI message.

**The sound.** Each pipe plays its own recording — attack, sustain loop, and a
matched release tail crossfaded in rather than cut to, so releasing a key
leaves the room's own decay behind. Transposed ranks are resampled with
four-point interpolation. When polyphony runs out, the voice that gets stolen
is a decaying release first and the oldest quietest note next; a note you have
just pressed is never taken.

**Tuning.** Equal temperament, or one of eight historical temperaments, each
generated from its own fifth-chain definition rather than transcribed. A set
naming a temperament nobody recognises is reported, never quietly played in
equal.

**Expression and tremulants.** Enclosed divisions are filtered per box as the
shoe moves. Tremulants modulate amplitude and pitch per pipe, so one chest can
wobble while another does not.

**Wind.** For sets that describe their own pneumatics — compartments, bellows,
valves, and the air each pipe draws — the wind system is solved as a physical
one, and a large registration sags the wind the way the real instrument does.

**Registration.** Couplers, because a key reaching a pipe is a walk through the
instrument's own switch graph rather than a checkbox. Thumb pistons and
combinations with capture, a general cancel, a crescendo, and a sequencer that
steps through the generals. Captured registrations are saved beside your own
settings, never written back into the sample library.

**Memory.** A large set can be held entirely in RAM, or its release tails
streamed from disk while the rest stays resident — the same audio either way.
Samples can also be kept at reduced precision, which roughly halves the
footprint for a set that would not otherwise fit.

**MIDI.** Every input device at once, each knowing which manual it is.
Right-click a drawstop to learn a control. MIDI out lights the drawstops on a
physical console.

**Alongside the organ.** A metronome, impulse-response reverb, and a recorder
that captures stop changes and shoe movements as well as notes.

---

## Building

Requires a C++20 compiler (MSVC 2022, GCC 12+, or Clang 14+), CMake 3.22+ and
Ninja. JUCE and pugixml are fetched automatically.

```bash
cmake --preset dev
cmake --build --preset dev
```

Presets are also provided for each CI target: `ci-linux`, `ci-macos`,
`ci-windows`, and `ci-linux-arm`, which cross-builds for 32-bit Raspberry Pi.

---

## Running

Launch the app, then **Load organ…** and choose the set's XML definition.

Two flags are useful when you do not want to wait on a multi-gigabyte load:

```
Masterpiece --odf "<path to the definition>" --gui-only
```

`--gui-only` builds the entire console and reads no audio at all: the organ is
silent and appears in a second or two. `--log <file>` writes the load timings,
if you want to know where the time went.

---

## How it is built

| | |
|---|---|
| Language | C++20 |
| Audio and GUI | JUCE 9 |
| XML | pugixml |
| Build | CMake + Ninja, command line only |
| Platforms | Windows, macOS, Linux; Raspberry Pi via cross-build |
| Formats | Standalone, VST3, AU, LV2 |

The engine is split so the parts with no user interface can be tested without
one:

```
mp_core      the XML loader, the validator, the temperament solver
mp_sampler   the voice engine and the streaming backend
mp_control   key flow, couplers, the switch network, pistons, crescendo, wind
mp_dsp       enclosure filters and tremulant modulation
mp_audio     the audio processor, sample storage, routing
mp_ui        the console, the panels, MIDI learn
```

`mp_core`, `mp_sampler` and `mp_control` carry no JUCE at all, which is what
lets the whole musical path be exercised against a synthesised tone instead of
a 40 GB library.

---

## Credits

Masterpiece is its own implementation, but it was written with three
open-source projects open alongside it, and is much the better for them:

- **[GrandOrgue](https://github.com/GrandOrgue/GrandOrgue)** — the shape of the
  voice engine, and the release-crossfade behaviour that stops a key release
  from clicking
- **[rusty-pipes](https://github.com/dividebysandwich/rusty-pipes)** — sample
  and loop handling, and a great deal of hard-won file-format knowledge
- **[HISE](https://github.com/christophhart/HISE)** — the streaming design:
  per-voice ring buffers refilled off a background thread

None of their code is compiled in.

---

## Licence

GPL-3.0-only. See [`LICENCE`](LICENCE) and [`COPYING`](COPYING).
