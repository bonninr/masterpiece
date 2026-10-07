"""An organ whose every sound is known in advance, and a piece to play on it,
for checking a build that no one can sit at.

    python3 make_organ.py <folder> [--heavy]

One manual and three stops of pure sine-wave pipes, each pipe looped and
each with a release that dies away exponentially:

    Sine 8'   the key's own pitch       amplitude 0.20
    Sine 4'   an octave above           amplitude 0.10
    Sine 2'   two octaves above         amplitude 0.05

The attack files end on the last frame of their loop, as many real sets do.
The piece holds middle C, lets it go, then plays a C major chord. What the
recording must contain follows from the numbers above, and is written to
<folder>/check.json for check_audio.py to compare against.

--heavy adds twelve more 8' ranks (each a sine a few cents off the key, as
celestes are) with six-second samples, and a ten-note chord on all of them:
about 0.6 GB of samples and 160 voices, a load closer to a real organ's.

Writes <folder>/check.organ, <folder>/pipes/, <folder>/check.mid,
<folder>/check.json and the same organ as one package, <folder>/check.orgue:
a ZIP with the organ, its pipes and the organindex.ini a GrandOrgue package
carries. Phones and tablets open organs as packages, so the package is what
gets played.
"""
import json
import os
import struct
import sys
import wave
import zipfile

import numpy as np

RATE = 48000
FIRST, KEYS = 36, 61
STOPS = [("Sine 8'", 1, 0.20), ("Sine 4'", 2, 0.10), ("Sine 2'", 4, 0.05)]
RELEASE_TAU = 0.15     # seconds for the release to fall by 1/e
RELEASE_LEN = 1.2      # seconds; ends below -60 dB of its start
LOAD_RANKS = 12        # --heavy
HEAVY_SCALE = 0.1      # --heavy: every amplitude, so ten keys on fifteen ranks do not clip
LOAD_CENTS = [-9, -7, -5, -4, -3, -2, 2, 3, 4, 5, 7, 9]

HEAVY = "--heavy" in sys.argv


def freq_of(note, factor=1.0, cents=0.0):
    return 440.0 * 2 ** ((note - 69) / 12) * factor * 2 ** (cents / 1200)


def write_wav(path, samples, loop=None):
    data = np.clip(np.round(samples * 32767), -32768, 32767).astype("<i2").tobytes()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data)
    if loop is None:
        return
    start, end = loop  # end is the loop's last frame, as the smpl chunk counts
    smpl = struct.pack("<9I", 0, 0, int(1e9 / RATE), 60, 0, 0, 0, 1, 0)
    smpl += struct.pack("<6I", 0, 0, start, end, 0, 0)
    with open(path, "r+b") as f:
        f.seek(0, 2)
        f.write(b"smpl" + struct.pack("<I", len(smpl)) + smpl)
        size = f.tell()
        f.seek(4)
        f.write(struct.pack("<I", size - 8))


def attack(freq, amp, seconds):
    """A 10 ms rise into a steady sine, looped over whole cycles from half a
    second in to the file's last frame."""
    start = int(0.5 * RATE)
    cycles = max(1, int((seconds - 0.5) * freq))
    length = round(cycles * RATE / freq)
    end = start + length  # one past the loop's last frame
    # The loop must hold a whole number of cycles in a whole number of
    # samples, or every wrap jumps: the pitch is moved by a few hundredths of
    # a cent to fit.
    freq = cycles * RATE / length
    t = np.arange(end) / RATE
    env = np.minimum(1.0, t / 0.01)
    return amp * env * np.sin(2 * np.pi * freq * t), (start, end - 1)


def release(freq, amp):
    t = np.arange(int(RELEASE_LEN * RATE)) / RATE
    return amp * np.exp(-t / RELEASE_TAU) * np.sin(2 * np.pi * freq * t)


def ranks():
    """(name, folder, factor, cents, amplitude, seconds) for every rank."""
    # The heavy organ plays fifteen ranks under ten keys at once: everything is
    # scaled down so that sum stays clear of full scale.
    scale = HEAVY_SCALE if HEAVY else 1.0
    out = [(name, f"r{i + 1}", factor, 0.0, amp * scale, 1.5) for i, (name, factor, amp) in enumerate(STOPS)]
    if HEAVY:
        for i, cents in enumerate(LOAD_CENTS):
            out.append((f"Load {i + 1:02d}", f"l{i + 1}", 1, cents, 0.02 * scale, 6.0))
    return out


def pipes(folder):
    for name, sub, factor, cents, amp, seconds in ranks():
        os.makedirs(os.path.join(folder, "pipes", sub), exist_ok=True)
        for k in range(KEYS):
            note = FIRST + k
            f = freq_of(note, factor, cents)
            a, loop = attack(f, amp, seconds)
            write_wav(os.path.join(folder, "pipes", sub, f"{note:03d}.wav"), a, loop)
            write_wav(os.path.join(folder, "pipes", sub, f"{note:03d}-rel.wav"), release(f, amp))


def organ(folder):
    rs = ranks()
    lines = [
        "[Organ]", "ChurchName=Check", "OrganBuilder=CI", "HasPedals=N",
        "NumberOfManuals=1", f"NumberOfRanks={len(rs)}", "NumberOfWindchestGroups=1",
        "[WindchestGroup001]", "Name=Main",
    ]
    for i, (name, sub, *_rest) in enumerate(rs):
        lines += [f"[Rank{i + 1:03d}]", f"Name={name}", f"FirstMidiNoteNumber={FIRST}",
                  f"NumberOfLogicalPipes={KEYS}", "WindchestGroup=1"]
        for k in range(KEYS):
            p = f"Pipe{k + 1:03d}"
            lines += [f"{p}=pipes/{sub}/{FIRST + k:03d}.wav",
                      f"{p}ReleaseCount=1", f"{p}Release001=pipes/{sub}/{FIRST + k:03d}-rel.wav"]
    lines += [
        "[Manual001]", "Name=Great", f"NumberOfLogicalKeys={KEYS}",
        f"NumberOfAccessibleKeys={KEYS}", f"FirstAccessibleKeyMIDINoteNumber={FIRST}",
        f"NumberOfStops={len(rs)}",
    ] + [f"Stop{i + 1:03d}={i + 1}" for i in range(len(rs))]
    for i, (name, *_rest) in enumerate(rs):
        lines += [f"[Stop{i + 1:03d}]", f"Name={name}", "NumberOfRanks=1", f"Rank001={i + 1}",
                  f"NumberOfAccessiblePipes={KEYS}"]
    with open(os.path.join(folder, "check.organ"), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


# The piece, in seconds from the first note. Channel 2: the converted
# manual's default, the first manual after the pedal's place.
HELD = (0.0, 2.0, [60])
CHORD = (3.0, 4.5, [60, 64, 67])
BIG = (5.5, 8.0, [48, 52, 55, 60, 64, 67, 72, 76, 79, 84])
END = 9.5


def piece():
    return [HELD, CHORD] + ([BIG] if HEAVY else [])


def midi(path):
    """Format 0, 480 ticks a beat at 120 bpm: a tick is 1/960 s."""
    def var(n):
        out = [n & 0x7F]
        n >>= 7
        while n:
            out.insert(0, (n & 0x7F) | 0x80)
            n >>= 7
        return bytes(out)

    events = []
    for on, off, keys in piece():
        for k in keys:
            events.append((round(on * 960), 0x91, k, 100))
            events.append((round(off * 960), 0x81, k, 0))
    events.sort(key=lambda e: (e[0], e[1] == 0x91))
    ev = bytearray()
    now = 0
    for tick, status, key, vel in events:
        ev.extend(var(tick - now) + bytes([status, key, vel]))
        now = tick
    ev.extend(var(round(END * 960) - now) + b"\xFF\x2F\x00")
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480))
        f.write(b"MTrk" + struct.pack(">I", len(ev)) + ev)


def expectations(folder):
    """What check_audio.py compares the recording against."""
    def partials(keys):
        out = []
        for k in keys:
            for name, factor, amp in STOPS:
                out.append({"hz": freq_of(k, factor), "amp": amp, "from": name, "key": k})
        return out

    spec = {
        "rate": RATE,
        "release_tau": RELEASE_TAU,
        "held": {"on": HELD[0], "off": HELD[1], "partials": partials(HELD[2])},
        "chord": {"on": CHORD[0], "off": CHORD[1], "partials": partials(CHORD[2])},
        "end": END,
        "heavy": HEAVY,
    }
    if HEAVY:
        spec["big"] = {"on": BIG[0], "off": BIG[1], "keys": BIG[2]}
    with open(os.path.join(folder, "check.json"), "w") as f:
        json.dump(spec, f, indent=1)


def package(folder):
    index = ("[General]\nTitle=Check\nOrganCount=1\n\n"
             "[Organ001]\nFilename=check.organ\nChurchName=Check\nOrganBuilder=CI\n")
    # Stored, not compressed, as GrandOrgue's own packages are.
    with zipfile.ZipFile(os.path.join(folder, "check.orgue"), "w", zipfile.ZIP_STORED) as z:
        z.writestr("organindex.ini", index)
        z.write(os.path.join(folder, "check.organ"), "check.organ")
        for root, _dirs, files in os.walk(os.path.join(folder, "pipes")):
            for name in sorted(files):
                full = os.path.join(root, name)
                z.write(full, os.path.relpath(full, folder).replace(os.sep, "/"))


def main():
    folder = sys.argv[1]
    os.makedirs(folder, exist_ok=True)
    pipes(folder)
    organ(folder)
    midi(os.path.join(folder, "check.mid"))
    expectations(folder)
    package(folder)


if __name__ == "__main__":
    main()
