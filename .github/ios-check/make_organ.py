"""A small organ and a piece to play on it, for checking a build that no one
can sit at: one manual, one 8' stop of sine-wave pipes, and a MIDI file of a
held middle C and then a chord. Written as a GrandOrgue .organ, which the
app reads directly.

    python3 make_organ.py <folder>

Writes <folder>/check.organ, <folder>/pipes/*.wav and <folder>/check.mid,
and the same organ as one package, <folder>/check.orgue: a ZIP with the
organ, its pipes and the organindex.ini a GrandOrgue package carries. Phones
and tablets open organs as packages, so the package is what gets played.
"""
import math
import os
import struct
import sys
import wave
import zipfile

RATE = 48000
SECONDS = 1.5
FIRST, KEYS = 36, 61


def pipe(path, note):
    freq = 440.0 * 2 ** ((note - 69) / 12)
    frames = int(RATE * SECONDS)
    fade = int(RATE * 0.01)
    data = bytearray()
    for i in range(frames):
        env = min(1.0, i / fade, (frames - i) / fade)
        data += struct.pack("<h", int(6000 * env * math.sin(2 * math.pi * freq * i / RATE)))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(data))
    # A sustain loop, as an organ's pipes have, so a held key sounds until it
    # is let go and letting go ends it: a whole number of cycles from half a
    # second in, in a standard smpl chunk.
    cycles = max(1, round(0.5 * freq))
    start = int(0.5 * RATE)
    end = start + round(cycles * RATE / freq) - 1
    smpl = struct.pack("<9I", 0, 0, int(1e9 / RATE), note, 0, 0, 0, 1, 0)
    smpl += struct.pack("<6I", 0, 0, start, end, 0, 0)
    with open(path, "r+b") as f:
        f.seek(0, 2)
        f.write(b"smpl" + struct.pack("<I", len(smpl)) + smpl)
        size = f.tell()
        f.seek(4)
        f.write(struct.pack("<I", size - 8))


def organ(folder):
    lines = [
        "[Organ]", "ChurchName=Check", "OrganBuilder=CI", "HasPedals=N",
        "NumberOfManuals=1", "NumberOfRanks=1", "NumberOfWindchestGroups=1",
        "[WindchestGroup001]", "Name=Main",
        "[Rank001]", "Name=Sine", f"FirstMidiNoteNumber={FIRST}",
        f"NumberOfLogicalPipes={KEYS}", "WindchestGroup=1",
    ]
    for k in range(KEYS):
        lines.append(f"Pipe{k + 1:03d}=pipes/{FIRST + k:03d}.wav")
    lines += [
        "[Manual001]", "Name=Great", f"NumberOfLogicalKeys={KEYS}",
        f"NumberOfAccessibleKeys={KEYS}", f"FirstAccessibleKeyMIDINoteNumber={FIRST}",
        "NumberOfStops=1", "Stop001=1",
        "[Stop001]", "Name=Sine 8'", "NumberOfRanks=1", "Rank001=1",
        f"NumberOfAccessiblePipes={KEYS}",
    ]
    with open(os.path.join(folder, "check.organ"), "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


def midi(path):
    """Format 0, 480 ticks a beat at 120 bpm: a tick is about 1 ms."""
    def var(n):
        out = [n & 0x7F]
        n >>= 7
        while n:
            out.insert(0, (n & 0x7F) | 0x80)
            n >>= 7
        return bytes(out)

    ev = bytearray()
    def note(delta, on, key):
        # Channel 2: the converted manual's default, the first manual after
        # the pedal's place, whether or not the organ has a pedal.
        ev.extend(var(delta) + bytes([0x91 if on else 0x81, key, 100 if on else 0]))
    note(0, True, 60)          # middle C alone, for its pitch
    note(1200, False, 60)
    for k in (60, 64, 67):     # then a C major chord
        note(240 if k == 60 else 0, True, k)
    note(1200, False, 60)
    note(0, False, 64)
    note(0, False, 67)
    ev.extend(var(480) + b"\xFF\x2F\x00")
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480))
        f.write(b"MTrk" + struct.pack(">I", len(ev)) + ev)


def main():
    folder = sys.argv[1]
    os.makedirs(os.path.join(folder, "pipes"), exist_ok=True)
    for k in range(KEYS):
        pipe(os.path.join(folder, "pipes", f"{FIRST + k:03d}.wav"), FIRST + k)
    organ(folder)
    midi(os.path.join(folder, "check.mid"))
    package(folder)


def package(folder):
    index = ("[General]\nTitle=Check\nOrganCount=1\n\n"
             "[Organ001]\nFilename=check.organ\nChurchName=Check\nOrganBuilder=CI\n")
    # Stored, not compressed, as GrandOrgue's own packages are.
    with zipfile.ZipFile(os.path.join(folder, "check.orgue"), "w", zipfile.ZIP_STORED) as z:
        z.writestr("organindex.ini", index)
        z.write(os.path.join(folder, "check.organ"), "check.organ")
        for name in sorted(os.listdir(os.path.join(folder, "pipes"))):
            z.write(os.path.join(folder, "pipes", name), "pipes/" + name)


if __name__ == "__main__":
    main()
