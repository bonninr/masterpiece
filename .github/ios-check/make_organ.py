"""A small organ and a piece to play on it, for checking a build that no one
can sit at: one manual, one 8' stop of sine-wave pipes, and a MIDI file of a
held middle C and then a chord. Written as a GrandOrgue .organ, which the
app reads directly.

    python3 make_organ.py <folder>

Writes <folder>/check.organ, <folder>/pipes/*.wav and <folder>/check.mid.
"""
import math
import os
import struct
import sys
import wave

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


if __name__ == "__main__":
    main()
