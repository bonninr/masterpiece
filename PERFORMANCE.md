# Performance

Measurements of what a sampled organ costs to hold in memory and to open, and
what each of the engine's memory settings actually buys. All figures are
measured, not derived; the method for each is given so they can be repeated or
disputed.

## Test bed

| | |
|---|---|
| Instrument | Friesach — 44 stops, three manuals and pedal |
| Library | 17.0 GB, 12,148 WAV files, 24-bit / 48 kHz / stereo throughout |
| Machine | 15.9 GB RAM, Windows |
| Build | Masterpiece 0.3.3, Release, MSVC |

The set is larger than the machine's RAM. That is the condition the memory
settings exist for, and it is why "does not fit" is one of the outcomes below
rather than a hypothetical.

All nine sample libraries tested during this work are 24-bit / 48 kHz /
stereo. No 96 kHz library was available, which matters for one of the settings
(see [Sample rate](#sample-rate)).

## Method

Each configuration loads the organ, then walks a registration from a single
Principal 8′ to full organ in eight steps, playing the same 15-second
polyphony ramp at each — notes accumulating to sixteen voices, held, then
released together so every voice enters its release tail at once.

Two independent measurements are taken:

- **Held / not held** — the sample library's own accounting, logged at the end
  of each load. `residentBytes()` sums the actual resident containers;
  `streamedBytesSaved()` sums what streaming left on disk.
- **Working set** — sampled from outside the process every 250 ms. What the
  library believes it holds and what the operating system has actually given
  it are different numbers, and the difference is itself a result.

Harness: `build/memstudy.ps1` (one configuration), `build/memstudy-all.ps1`
(the matrix), `build/memstudy-collect.py` (reconciles samples with the log).

## Memory

Five configurations, full organ:

| Storage | Channels | Release streaming | Held | Not held | Load |
|---|---|---|---:|---:|---:|
| 32-bit float | stereo | off | 21,606.5 MB | — | 77.1 s |
| 24-bit | stereo | off | 16,204.9 MB | — | 55.8 s |
| 24-bit | stereo | on | 6,976.0 MB | 8,891.9 MB | 28.9 s |
| 16-bit | stereo | on | 4,650.7 MB | 5,927.9 MB | 28.0 s |
| 16-bit | mono | on | 2,325.3 MB | 2,964.0 MB | 25.9 s |

### The ratios are exact

Every lever is a linear multiplier on the same frames, and they compose:

- 32-bit float → 24-bit: 21,606.5 → 16,204.9 MB, ratio **0.750**
- 24-bit → 16-bit (streamed): 6,976.0 → 4,650.7 MB, ratio **0.667**
- stereo → mono: 4,650.7 → 2,325.3 MB, ratio **0.500**

The streamed portion follows the same arithmetic (8,891.9 → 5,927.9 → 2,964.0
MB) because the tail is converted on its way out of the file rather than
afterwards.

Exactness is the check that the measurement measures something real. A ratio
of 0.748 would indicate accounting error somewhere.

### 32-bit float is not a quality setting

These files are 24-bit. Decoding them to float to hold them is exact and
**5,401 MB larger** — a fourth byte carrying nothing the file contained.
24-bit is the default: it stores the source integers unmodified, so the
resident audio is bit-for-bit the file's own.

### Streaming is the largest single lever

At 16-bit it leaves 5,927.9 MB on disk, **55% of the sample data**. A release
tail is the only part of an organ that streams well: several seconds long,
played once from the start, never looped. An attack cannot be treated the same
way — its sustain loop must be resident or the note does not sustain.

## Load time

Nothing here trades load time against memory; the configuration holding the
least also opens the fastest.

| Configuration | Load |
|---|---:|
| 32-bit float, resident | 77.1 s |
| 24-bit, resident | 55.8 s |
| 24-bit, streamed | 28.9 s |
| 16-bit, streamed | 28.0 s |
| 16-bit mono, streamed | 25.9 s |

Everything streamed lands between 25.9 s and 28.9 s regardless of format: the
work skipped — reading every release tail from disk — is the same in each
case.

The 77.1 s figure is not decode cost. Both resident configurations read the
same files; the difference is the operating system paging out an organ larger
than itself while still loading it.

## Configurations that do not fit

32-bit float fully resident allocates 21,606 MB on a machine with 15,900 MB.
It does not fail. Windows pages it out, and after loading its working set
settles at **1,329 MB** — most of the organ is not in memory, and playing
fetches pipes back from disk.

This is invisible in the allocation figure and is the reason working set is
sampled separately.

## Registration does not cost memory

Ranks are loaded whether or not a stop is drawn. Walking from one stop to all
44:

| Configuration | Working set change |
|---|---:|
| 16-bit mono, streamed | −4 MB |
| 16-bit, streamed | −5 MB |
| 24-bit, streamed | +178 MB |
| 24-bit, resident | +572 MB |
| 32-bit float, resident | +764 MB |

On configurations that fit, the change is noise. Where it rises, that is page
faults, not allocation: each new stop touches pages that had been evicted.
Tutti is not more expensive than a single Principal.

## Resident format fidelity

Measured on the sample data directly, applying exactly what the loader
applies, to 60 files drawn at random from the 12,148. Script:
`build/quantnoise.py`.

| Format | Against the source | Signal to quantisation noise |
|---|---|---|
| 24-bit | bit-for-bit identical | exact |
| 16-bit | requantised against each file's peak | 78.1 dB median, 75.2 dB worst |

An 8-bit width was implemented and measured at 32.4 dB median (29.1 worst) —
audible hiss under a quiet stop and a long release tail — and withdrawn.

16-bit quantises against each file's own peak rather than full scale, so a
stop recorded 20 dB down still uses every bit available. 24-bit does not
normalise: normalisation would trade exactness for nothing.

### A measurement that does not work

Rendering the same music at two formats and subtracting gives ≈17 dB for any
pair of formats. That is the noise floor of the method, not a property of the
formats: this engine's threaded output is not bit-identical between runs, so
the difference between two renders is dominated by scheduling. The quantiser
is deterministic and is measured directly instead.

## Sample cache

The decoded, converted samples written to one file, so a subsequent load of
the same organ at the same settings is a sequential read rather than 12,148
decodes.

Keyed to a fingerprint of everything that changes the resident bytes: the
definition's size and modification time, resident width, channel fold, load
rate, preload head, loop selection, whether releases stream, and which ranks
were requested. Anything else is a miss. A partial (`--preload-drawn`) load
caches under a different key from a full one.

| Configuration | Decoded | From cache | Cache size |
|---|---:|---:|---:|
| 24-bit, stereo, streamed | 11.9 s | 14.1 s | 7,315 MB |
| 16-bit, mono, streamed | 13.0 s | 2.6 s | 2,439 MB |

**A cache is worth what the settings take away.** At 24-bit it is the same
size as the data it replaces: it removes the decoding work and buys nothing,
because the disk is the limit either way and one sequential read loses to a
decode spread across every core. At 16-bit mono a 2,439 MB read stands in for
roughly seven gigabytes of source, and total load falls from 25.9 s to 18.9 s.

Verified for correctness rather than assumed: a 16-bit cache is reused at
16-bit and refused at 24-bit and at 16-bit mono, each miss rebuilding to
exactly the expected size (291.8 → 437.7 → 146.0 MB on a five-rank load).

By default there is one cache file, replaced as organs change; these run to
gigabytes and one per organ per settings combination fills a disk without
being asked. One-per-organ and off are both available.

## Where the remaining time goes

With samples cached, a full load of Friesach at 16-bit streamed is 26.1 s:

| Phase | Time | |
|---|---:|---|
| ODF parse | 1.0 s | reading 34 MB of XML |
| Model | 9.7 s | see below |
| Samples | 9.5 s | 4,877 MB cache read, ≈513 MB/s — disk bound |
| Prepare | 5.8 s | audio graph, MIDI map, combinations |

The model phase is **not** parsing. Instrumented:

| Sub-phase | Time |
|---|---:|
| stop map | 0.3 ms |
| switches | 0.3 ms |
| switch solve | 0.2 ms |
| controls | 0.3 ms |
| **wind model** | **5,114 ms** |
| stages | 0.4 ms |
| **settle, linkages, player-switch walk** | **4,611 ms** |

Two operations account for all of it. Caching the built model would cut a
cached load to ≈16.4 s, but five seconds to build a wind model for a 44-stop
instrument is a large amount of time for a small amount of data; if either
operation is accidentally superlinear, fixing it returns the same time on cold
loads as well, with nothing to invalidate. That is the open question, not a
settled result.

## Sample rate

Sample data can be converted to another rate as it loads, which halves the
memory of a 96 kHz library played through a 48 kHz device and removes a
per-voice resample at playback.

Verified on Friesach by converting away from its native 48 kHz: resident falls
exactly in proportion (1,599.7 MB as recorded, 1,469.8 at 44.1 kHz, 799.9 at
24 kHz on a five-rank load), pitch and level are unchanged at every rate, and
a converted head spliced onto a converted streamed tail matches a fully
resident render to 0.0 dB across the entire release decay.

It is absent from the tables above because every library tested is already
48 kHz. There is nothing in this collection for it to save, and converting
downward would produce a smaller number and a worse organ.

## Settings that did not earn their place

**Preload head.** 225 MB, 4.6%, between a 2,048-frame head and whole files.
The head is always extended to cover the sustain loop, so there is little left
to truncate. Measured separately and not carried in the matrix.

**8-bit storage.** See [fidelity](#resident-format-fidelity). Withdrawn.

## Reproducing

```
python build\memstudy-midi.py                 # the polyphony ramp
powershell -File build\memstudy-all.ps1       # the matrix
python build\memstudy-collect.py              # reconcile samples with logs
python build\quantnoise.py                    # format fidelity
powershell -File build\checkcache.ps1         # cold vs cached load
```

Friesach is a freely published sample library by Piotr Grabowski and is not
part of this repository. See [ATTRIBUTION.md](ATTRIBUTION.md).
