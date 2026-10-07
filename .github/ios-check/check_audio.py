"""Checks the app's recording of check.mid against what the organ must sound
like. make_organ.py writes the organ from numbers, and the same numbers, in
check.json, say what every part of the recording should contain.

    python3 check_audio.py <recording.wav> <check.json>

Prints one line per check, PASS or FAIL, and exits 1 if any failed:

    held       middle C at its pitch; the 4' and 2' at -6 and -12 dB of the 8'
    steady     the held note's level flat across its loop (#189: a dropped loop)
    release    after the key is let go, a decay at the release's own rate
    silence    nothing left a second later (a note that sticks)
    chord      the nine partials of C-E-G at their pitches and levels, and
               nothing else above -40 dB
    clicks     no discontinuity anywhere the organ sounds
    load       --heavy: the ten-note chord on fifteen ranks without dropouts

The organ's overall level is not checked: the master fader, the organ's trim
and the device all set it. Levels are compared with each other.
"""
import json
import sys
import wave

import numpy as np

results = []


def check(name, ok, detail):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")


def load(path):
    with wave.open(path, "rb") as w:
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
    return x.reshape(-1, chans).mean(axis=1), rate


def rms(seg):
    return float(np.sqrt(np.mean(seg * seg))) if len(seg) else 0.0


def db(v, ref):
    return 20 * np.log10(max(v, 1e-12) / max(ref, 1e-12))


def spectrum(seg, rate):
    pad = 8 * len(seg)
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), pad))
    return mag, rate / pad


def peak_near(mag, step, hz, width=0.02):
    lo, hi = int(hz * (1 - width) / step), int(hz * (1 + width) / step) + 1
    i = lo + int(np.argmax(mag[lo:hi]))
    return i * step, float(mag[i])


def partial_checks(name, seg, rate, partials):
    mag, step = spectrum(seg, rate)
    found = {}
    worst_cents, worst_db = 0.0, 0.0
    for p in partials:
        hz, amp = peak_near(mag, step, p["hz"])
        found[(p["key"], p["from"])] = amp
        worst_cents = max(worst_cents, abs(1200 * np.log2(hz / p["hz"])))
    for p in partials:
        ref = found[(p["key"], "Sine 8'")]
        ref_amp = next(q["amp"] for q in partials if q["key"] == p["key"] and q["from"] == "Sine 8'")
        error = db(found[(p["key"], p["from"])], ref) - db(p["amp"], ref_amp)
        worst_db = max(worst_db, abs(error))
    eights = [found[(p["key"], "Sine 8'")] for p in partials if p["from"] == "Sine 8'"]
    spread = db(max(eights), min(eights))
    check(name + " pitch", worst_cents <= 5, f"every partial within {worst_cents:.1f} cents (limit 5)")
    check(name + " levels", worst_db <= 1.5 and spread <= 1.5,
          f"4' and 2' within {worst_db:.2f} dB of -6/-12 dB, keys within {spread:.2f} dB (limit 1.5)")
    # Anything else: the strongest peak away from every expected partial.
    keep = np.ones(len(mag), bool)
    for p in partials:
        keep[int(p["hz"] * 0.97 / step):int(p["hz"] * 1.03 / step) + 1] = False
    keep[: int(30 / step)] = False
    stray = float(mag[keep].max()) if keep.any() else 0.0
    rel = db(stray, float(max(found.values())))
    check(name + " clean", rel <= -40, f"strongest other component {rel:.1f} dB (limit -40)")


def heavy_checks(x, rate, spec, at, peak):
    """The heavy organ: fifteen ranks, some beating against each other as
    celestes do, so pitch and levels are the normal organ's to check. Here:
    no clipping, no dropout or click under load, silence after the release."""
    check("headroom", peak < 0.99, f"peak {peak:.3f} (limit 0.99)")
    big = spec["big"]
    w20 = int(0.02 * rate)
    w = [rms(x[i:i + w20]) for i in range(at(big["on"] + 0.7), at(big["off"] - 0.05), w20)]
    dip = db(min(w), float(np.median(w))) if w else -99
    check("load", dip >= -6, f"the big chord's level dips {dip:.1f} dB at most (limit -6)")
    worst = 0.0
    for i in range(at(big["on"] + 0.7), at(big["off"] - 0.05), w20):
        seg = x[i:i + w20]
        d2 = np.abs(np.diff(seg, 2))
        worst = max(worst, float(d2.max() / (np.median(d2) + 1e-12)))
    check("load clicks", worst <= 15, f"largest spike {worst:.1f}x its surroundings under load (limit 15)")
    level = float(np.median(w)) if w else 1.0
    after = db(rms(x[at(big["off"] + 1.3):at(big["off"] + 1.45)]), level)
    check("load silence", after <= -60, f"{after:.1f} dB after the big chord's release (limit -60)")
    print("OK" if all(results) else "FAILED")
    return 0 if all(results) else 1


def main():
    x, rate = load(sys.argv[1])
    spec = json.load(open(sys.argv[2]))
    peak = float(np.abs(x).max()) if len(x) else 0.0
    print(f"recording: {len(x) / rate:.2f} s at {rate} Hz, peak {peak:.4f}")
    if peak < 1e-3:
        check("sound", False, "the recording is silent")
        return 1
    # The first note's onset: where it first reaches a tenth of its level,
    # less the 10 ms the pipes take to rise.
    t0 = int(np.argmax(np.abs(x) > 0.1 * peak)) / rate - 0.005
    def at(t):
        return int((t0 + t) * rate)

    held, chord = spec["held"], spec["chord"]
    if spec.get("heavy"):
        return heavy_checks(x, rate, spec, at, peak)
    partial_checks("held", x[at(held["on"] + 0.6):at(held["off"] - 0.1)], rate, held["partials"])

    win = int(0.05 * rate)
    levels = [rms(x[i:i + win]) for i in range(at(held["on"] + 0.3), at(held["off"] - 0.06), win)]
    swing = db(max(levels), min(levels))
    check("steady", swing <= 1.0, f"held-note level varies {swing:.2f} dB across its loop (limit 1.0)")

    # The release: 20 ms levels from 50 ms after the key until it is 40 dB
    # down, fitted with a straight line in dB.
    held_level = float(np.median(levels))
    w20 = int(0.02 * rate)
    ts, ls = [], []
    for i in range(at(held["off"] + 0.05), at(held["off"] + 1.0), w20):
        level = db(rms(x[i:i + w20]), held_level)
        if level < -40:
            break
        ts.append(i / rate)
        ls.append(level)
    if len(ts) >= 4:
        slope = np.polyfit(ts, ls, 1)[0]  # dB per second
        tau = -20 / np.log(10) / slope if slope < 0 else float("inf")
        expect = spec["release_tau"]
        check("release", abs(tau - expect) <= 0.4 * expect,
              f"decays with a time constant of {tau * 1000:.0f} ms (release {expect * 1000:.0f} ms, limit 40%)")
    else:
        check("release", False, "the note stopped within 80 ms of the key: no release")

    gap = db(rms(x[at(held["off"] + 1.25):at(chord["on"] - 0.05)]), held_level)
    check("silence", gap <= -60, f"{gap:.1f} dB a second after the release (limit -60)")

    partial_checks("chord", x[at(chord["on"] + 0.6):at(chord["off"] - 0.1)], rate, chord["partials"])
    after = db(rms(x[at(chord["off"] + 1.3):at(chord["off"] + 1.45)]), held_level)
    check("chord silence", after <= -60, f"{after:.1f} dB after the chord's release (limit -60)")

    # Clicks: the second difference of a sum of sines is a smooth sum of
    # sines; a discontinuity is a spike far above its neighbourhood.
    worst, where = 0.0, 0.0
    for i in range(at(0.0), min(len(x), at(spec["end"])) - w20, w20):
        seg = x[i:i + w20]
        if rms(seg) < 1e-4:
            continue
        d2 = np.abs(np.diff(seg, 2))
        ratio = float(d2.max() / (np.median(d2) + 1e-12))
        if ratio > worst:
            worst, where = ratio, i / rate - t0
    check("clicks", worst <= 15, f"largest spike {worst:.1f}x its surroundings at {where:.2f} s (limit 15)")

    print("OK" if all(results) else "FAILED")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
