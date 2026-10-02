"""Checks the recording the app made of check.mid: that it sounds at all, and
that the held middle C at the start is at its pitch (261.63 Hz).

    python3 check_audio.py <recording.wav>

Prints what it found; exits 1 if the organ was silent or out of tune.
"""
import sys
import wave

import numpy as np


def main():
    with wave.open(sys.argv[1], "rb") as w:
        rate, chans, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
    if width == 2:
        x = np.frombuffer(raw, "<i2").astype(float) / 32768
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        x = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        x = np.where(x & 0x800000, x - 0x1000000, x).astype(float) / 8388608
    else:
        x = np.frombuffer(raw, "<f4").astype(float)
    x = x.reshape(-1, chans).mean(axis=1)
    peak = float(np.abs(x).max()) if len(x) else 0.0
    print(f"recording: {len(x) / rate:.2f} s at {rate} Hz, peak {peak:.4f}")
    if peak < 1e-3:
        print("FAIL: silent")
        return 1
    # The first stretch where it sounds, 0.2 s in: the held middle C.
    start = int(np.argmax(np.abs(x) > peak * 0.1)) + int(0.2 * rate)
    seg = x[start:start + int(0.6 * rate)]
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), 8 * len(seg)))
    freq = float(np.argmax(spec)) * rate / (8 * len(seg))
    cents = 1200 * np.log2(freq / 261.6256)
    print(f"middle C: {freq:.2f} Hz ({cents:+.1f} cents)")
    if abs(cents) > 20:
        print("FAIL: out of tune")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
