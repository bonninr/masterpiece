"""Taps through the app in a booted simulator by the names on the screen, and
keeps a screenshot and the list of what was on screen at every step.

JUCE gives every button its name in the accessibility tree; idb reads that
tree and taps, so a step says "Settings", not a coordinate that moves when
the layout does.

    python3 drive_ui.py <simulator udid> <output folder>

A step whose control is not on screen is recorded and skipped, so one
missing button shows up in the results rather than ending the run. Exits 1
if any step failed.
"""
import json
import struct
import os
import subprocess
import sys
import time

UDID, OUT = sys.argv[1], sys.argv[2]
failures = []
step_no = 0


def run(*args):
    return subprocess.run(list(args), capture_output=True, text=True, timeout=120)


def elements():
    r = run("idb", "ui", "describe-all", "--udid", UDID, "--json")
    try:
        data = json.loads(r.stdout)
    except ValueError:
        # Older idb prints one object per line.
        data = [json.loads(line) for line in r.stdout.splitlines() if line.strip().startswith("{")]
    return data if isinstance(data, list) else [data]


def label(e):
    return (e.get("AXLabel") or e.get("title") or "").strip()


def shot(name):
    global step_no
    step_no += 1
    base = os.path.join(OUT, f"{step_no:02d}-{name}")
    run("xcrun", "simctl", "io", UDID, "screenshot", base + ".png")
    with open(base + ".txt", "w") as f:
        for e in elements():
            fr = e.get("frame", {})
            f.write(f"{label(e)!r} {e.get('type', '')} {fr}\n")


def portrait_width():
    """The screen's upright width in points, from a screenshot's pixels."""
    path = os.path.join(OUT, ".probe.png")
    run("xcrun", "simctl", "io", UDID, "screenshot", path)
    with open(path, "rb") as f:
        pixels = struct.unpack(">I", f.read(24)[16:20])[0]
    os.remove(path)
    return pixels / float(os.environ.get("SCREEN_SCALE", "2"))


def to_touch(es, x, y):
    """idb reads positions in the app's landscape points and taps in the
    screen's upright ones. Turned to the left, the app's top edge lies along
    the screen's right edge and its left edge along the top."""
    app = next((e.get("frame") for e in es if e.get("type") == "Application"), None)
    if not app or app["width"] <= app["height"]:
        return x, y
    return portrait_width() - y, x


def settled():
    """The listing once the app's frame reads the same twice: for a while
    after a launch idb reports it upright and turned by turns, and a tap
    converted with the wrong one lands elsewhere."""
    es = elements()
    for _ in range(8):
        time.sleep(0.7)
        again = elements()
        frame = lambda l: next((e.get("frame") for e in l if e.get("type") == "Application"), None)
        if frame(again) == frame(es):
            return again
        es = again
    return es


def tap(name, contains=False, required=True):
    es = settled()
    for e in es:
        text = label(e)
        if text == name or (contains and name.lower() in text.lower()):
            fr = e["frame"]
            x, y = to_touch(es, fr["x"] + fr["width"] / 2, fr["y"] + fr["height"] / 2)
            run("idb", "ui", "tap", "--udid", UDID, str(int(x)), str(int(y)))
            time.sleep(2)
            return True
    if required:
        failures.append(f"no '{name}' on screen")
    return False


def tap_text(words, seconds=25):
    """Taps text found in a screenshot: for the system's document picker,
    which draws in another process and so is not in the app's accessibility
    listing. The screenshot is the screen upright; with the app turned it is
    read turned too, and a match is taken back to upright points, the ones
    idb taps in."""
    try:
        import pytesseract
        from PIL import Image
    except ImportError:
        failures.append(f"no OCR to find '{words}'")
        return False
    scale = float(os.environ.get("SCREEN_SCALE", "2"))
    want = words.lower().split()
    end = time.time() + seconds
    while time.time() < end:
        path = os.path.join(OUT, ".ocr.png")
        run("xcrun", "simctl", "io", UDID, "screenshot", path)
        upright = Image.open(path)
        app = next((e.get("frame") for e in elements() if e.get("type") == "Application"), None)
        turned = bool(app and app["width"] > app["height"])
        img = upright.rotate(90, expand=True) if turned else upright
        d = pytesseract.image_to_data(img, output_type=pytesseract.Output.DICT)
        text = [t.lower().strip() for t in d["text"]]
        for i in range(len(text) - len(want) + 1):
            if all(text[i + k].startswith(want[k]) for k in range(len(want))):
                last = i + len(want) - 1
                x = (d["left"][i] + d["left"][last] + d["width"][last]) / 2
                y = d["top"][i] + d["height"][i] / 2
                if turned:  # back from the image turned a quarter to the left
                    x, y = upright.width - 1 - y, x
                run("idb", "ui", "tap", "--udid", UDID, str(int(x / scale)), str(int(y / scale)))
                time.sleep(2)
                return True
        time.sleep(2)
    failures.append(f"no '{words}' read on screen")
    return False


def close_panel():
    """A window's close button, by either of the names it is listed under."""
    if not (tap("Close", required=False) or tap("close", contains=True, required=False)):
        failures.append("no close button on screen")


def wait_for(name, seconds=25):
    """The system's document picker draws its contents from another process,
    which can take seconds on a simulator: its labels are waited for."""
    end = time.time() + seconds
    while time.time() < end:
        if any(name.lower() in label(e).lower() for e in elements()):
            return True
        time.sleep(1.5)
    return False


def main():
    os.makedirs(OUT, exist_ok=True)
    shot("start")
    # The run before this one was ended by the simulator, which the app takes
    # for a crash and says so at the next start: the notice covers the
    # console until it is dismissed.
    if any("closed unexpectedly" in label(e) for e in elements()):
        tap("OK")
        shot("notice-dismissed")
    # The first-run wizard, when it is up.
    tap("Skip setup", required=False)
    shot("console")
    if tap("Settings"):
        shot("settings-menu")
        if tap("General settings", contains=True):
            shot("general-settings")
            for tab in ("MIDI", "Mixer", "Log"):
                if tap(tab):
                    shot("general-" + tab.lower())
            close_panel()
            shot("after-close")
    # On a phone or tablet the stop list takes the console's place, and its
    # button becomes "Console", which brings the console back.
    if tap("Stop list"):
        shot("stop-list")
        tap("Console")
    if tap("Audio"):
        shot("audio")
        close_panel()
    # An organ package opened as a player opens one: Open, then the system's
    # document picker, then the package in "On My iPad" / "On My iPhone",
    # where the run put it beforehand (#197: a package outside the app's own
    # folder, lent by the Files app).
    package = os.environ.get("PICK_PACKAGE")
    if package and tap("Open"):
        # The picker is another process's: found by its text on screen.
        shot("picker")
        if tap_text("On My"):
            shot("picker-on-my-device")
            tap_text(os.environ.get("PICK_FOLDER", "check"))
            shot("picker-folder")
            if tap_text(os.path.splitext(package)[0]):
                time.sleep(20)
                shot("package-opened")
    shot("end")
    with open(os.path.join(OUT, "ui-result.txt"), "w") as f:
        f.write("\n".join(failures) if failures else "every step found its control\n")
    print("\n".join(failures) or "UI: every step found its control")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
