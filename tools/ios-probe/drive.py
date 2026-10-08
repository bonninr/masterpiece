"""Runs the probe app in a booted simulator once per variant and taps it.

    python3 drive.py <udid> <bundle id> <screen scale> <output folder> <variant>...

For each variant: launch, a screenshot and the accessibility listing, then
taps on Alpha, Menu, Item One, Window and Close by the names on screen, each
followed by a screenshot. The taps sent are written to taps.txt; the app's
own log (probe.log) says where each touch arrived.
"""
import json
import os
import struct
import subprocess
import sys
import time

UDID, BUNDLE, SCALE, OUT = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
VARIANTS = sys.argv[5:]


def run(*args):
    return subprocess.run(list(args), capture_output=True, text=True, timeout=120)


def elements():
    r = run("idb", "ui", "describe-all", "--udid", UDID, "--json")
    try:
        data = json.loads(r.stdout)
    except ValueError:
        data = [json.loads(l) for l in r.stdout.splitlines() if l.strip().startswith("{")]
    return data if isinstance(data, list) else [data]


def label(e):
    return (e.get("AXLabel") or e.get("title") or "").strip()


def upright_width():
    path = os.path.join(OUT, ".w.png")
    run("xcrun", "simctl", "io", UDID, "screenshot", path)
    with open(path, "rb") as f:
        px = struct.unpack(">I", f.read(24)[16:20])[0]
    os.remove(path)
    return px / SCALE


def shot(folder, name, notes):
    base = os.path.join(folder, name)
    run("xcrun", "simctl", "io", UDID, "screenshot", base + ".png")
    es = elements()
    with open(base + ".txt", "w") as f:
        for e in es:
            f.write(f"{label(e)!r} {e.get('type', '')} {e.get('frame', {})}\n")
    notes.append(f"{name}: {len(es)} elements: " + ", ".join(label(e) for e in es if label(e))[:300])
    return es


def settled():
    """The listing once the app's frame reads the same twice in a row."""
    frame = lambda l: next((e.get("frame") for e in l if e.get("type") == "Application"), None)
    es = elements()
    for _ in range(8):
        time.sleep(0.7)
        again = elements()
        if frame(again) == frame(es):
            return again
        es = again
    return es


def tap(name, notes):
    es = settled()
    app = next((e.get("frame") for e in es if e.get("type") == "Application"), None)
    for e in es:
        if label(e) == name:
            fr = e["frame"]
            x, y = fr["x"] + fr["width"] / 2, fr["y"] + fr["height"] / 2
            tx, ty = x, y
            if app and app["width"] > app["height"]:
                tx, ty = upright_width() - y, x
            run("idb", "ui", "tap", "--udid", UDID, str(int(tx)), str(int(ty)))
            notes.append(f"tap {name}: element at {x:.0f},{y:.0f} (app {app}) sent {tx:.0f},{ty:.0f}")
            time.sleep(1.5)
            return True
    notes.append(f"tap {name}: not on screen")
    return False


def main():
    for v in VARIANTS:
        folder = os.path.join(OUT, v)
        os.makedirs(folder, exist_ok=True)
        notes = []
        r = run("xcrun", "simctl", "launch", "--terminate-running-process", UDID, BUNDLE, v)
        notes.append("launch: " + (r.stdout + r.stderr).strip())
        time.sleep(5)
        shot(folder, "1-start", notes)
        if v.startswith("alert"):
            tap("OK", notes)
            shot(folder, "1b-after-ok", notes)
        tap("Alpha", notes)
        if tap("Menu", notes):
            shot(folder, "2-menu", notes)
            tap("Item One", notes)
        if tap("Window", notes):
            shot(folder, "3-second", notes)
            tap("Close", notes)
        shot(folder, "4-end", notes)
        run("xcrun", "simctl", "terminate", UDID, BUNDLE)
        data = run("xcrun", "simctl", "get_app_container", UDID, BUNDLE, "data").stdout.strip()
        log = os.path.join(data, "Documents", "probe.log")
        text = open(log).read() if os.path.exists(log) else "no probe.log\n"
        with open(os.path.join(folder, "probe.log"), "w") as f:
            f.write(text)
        with open(os.path.join(folder, "taps.txt"), "w") as f:
            f.write("\n".join(notes) + "\n")
        print(f"== {v}")
        print("\n".join(notes))
        print(text)


if __name__ == "__main__":
    main()
