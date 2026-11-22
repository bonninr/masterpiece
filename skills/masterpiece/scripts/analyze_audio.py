#!/usr/bin/env python3
"""Analyze a Masterpiece WAV render for voicing QA.

Reports overall RMS (dBFS), true-ish peak, an estimated noise floor, and the
bass/mid/treble energy split. With --segments N, splits the file into N equal
parts and reports each part's level (e.g. one note per key of a chromatic render,
to spot a dead or loud pipe). Compare two renders with --compare OTHER.wav.

Needs numpy. Reads 16/32-bit PCM and float WAV via the stdlib `wave` module;
for 24-bit or exotic files install `soundfile` (used automatically if present).

Usage:
  analyze_audio.py take.wav
  analyze_audio.py scale.wav --segments 61
  analyze_audio.py after.wav --compare before.wav
"""
import sys, wave, math
import numpy as np

def load(path):
    try:
        import soundfile as sf
        x, sr = sf.read(path, always_2d=True)
        return x.astype(np.float64), sr
    except Exception:
        pass
    w = wave.open(path, "rb")
    sr, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
    raw = w.readframes(n); w.close()
    if sw == 2:
        a = np.frombuffer(raw, "<i2").astype(np.float64) / 32768.0
    elif sw == 4:
        a = np.frombuffer(raw, "<i4").astype(np.float64) / 2147483648.0
    elif sw == 1:
        a = (np.frombuffer(raw, "u1").astype(np.float64) - 128) / 128.0
    else:
        sys.exit(f"{path}: {sw*8}-bit WAV not supported by the stdlib reader; pip install soundfile")
    return a.reshape(-1, ch), sr

def db(x):
    return -float("inf") if x <= 0 else 20 * math.log10(x)

def metrics(x, sr):
    mono = x.mean(axis=1)
    rms = math.sqrt(float(np.mean(mono**2)))
    peak = float(np.max(np.abs(mono))) if mono.size else 0.0
    # noise floor: 5th percentile of short-window RMS
    win = max(1, sr // 50)
    frames = mono[: (len(mono)//win)*win].reshape(-1, win)
    fr_rms = np.sqrt(np.mean(frames**2, axis=1)) if frames.size else np.array([0.0])
    floor = float(np.percentile(fr_rms, 5)) if fr_rms.size else 0.0
    # spectral split
    spec = np.abs(np.fft.rfft(mono * np.hanning(len(mono)))) ** 2
    freq = np.fft.rfftfreq(len(mono), 1 / sr)
    def band(lo, hi):
        m = (freq >= lo) & (freq < hi)
        return float(spec[m].sum())
    tot = float(spec.sum()) or 1.0
    return dict(rms_db=db(rms), peak_db=db(peak), floor_db=db(floor),
                bass=100*band(20, 200)/tot, mid=100*band(200, 3000)/tot,
                treble=100*band(3000, sr/2)/tot, dur=len(mono)/sr, sr=sr)

def report(name, m):
    print(f"== {name} ==")
    print(f"  duration {m['dur']:.2f}s @ {m['sr']} Hz")
    print(f"  RMS   {m['rms_db']:+6.1f} dBFS   peak {m['peak_db']:+6.1f} dBFS   "
          f"noise floor {m['floor_db']:+6.1f} dBFS")
    print(f"  spectrum  bass {m['bass']:4.1f}%  mid {m['mid']:4.1f}%  treble {m['treble']:4.1f}%")

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(1)
    path = args[0]
    segs = int(args[args.index("--segments")+1]) if "--segments" in args else 0
    other = args[args.index("--compare")+1] if "--compare" in args else None
    x, sr = load(path)
    report(path, metrics(x, sr))
    if segs > 1:
        mono = x.mean(axis=1)
        step = len(mono)//segs
        print(f"  -- {segs} segments (RMS dBFS) --")
        lvls = []
        for i in range(segs):
            seg = mono[i*step:(i+1)*step]
            r = db(math.sqrt(float(np.mean(seg**2)))) if seg.size else -float("inf")
            lvls.append(r)
            print(f"    seg {i:3d}: {r:+6.1f}")
        fin = [v for v in lvls if math.isfinite(v)]
        if fin:
            med = sorted(fin)[len(fin)//2]
            out = [i for i, v in enumerate(lvls) if math.isfinite(v) and abs(v-med) > 6]
            print(f"  median {med:+.1f} dBFS; outliers >6 dB: {out or 'none'}")
    if other:
        xo, sro = load(other)
        mo = metrics(xo, sro); mn = metrics(x, sr)
        report(other, mo)
        print("== delta (this - other) ==")
        print(f"  RMS {mn['rms_db']-mo['rms_db']:+.1f} dB   "
              f"bass {mn['bass']-mo['bass']:+.1f}pp  mid {mn['mid']-mo['mid']:+.1f}pp  "
              f"treble {mn['treble']-mo['treble']:+.1f}pp")

if __name__ == "__main__":
    main()
