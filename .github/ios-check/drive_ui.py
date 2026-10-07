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


def tap(name, contains=False):
    for e in elements():
        text = label(e)
        if text == name or (contains and name.lower() in text.lower()):
            fr = e["frame"]
            x, y = fr["x"] + fr["width"] / 2, fr["y"] + fr["height"] / 2
            run("idb", "ui", "tap", "--udid", UDID, str(int(x)), str(int(y)))
            time.sleep(2)
            return True
    failures.append(f"no '{name}' on screen")
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
    tap("Skip setup")
    shot("console")
    if tap("Settings"):
        shot("settings-menu")
        if tap("General settings", contains=True):
            shot("general-settings")
            for tab in ("MIDI", "Mixer", "Log"):
                if tap(tab):
                    shot("general-" + tab.lower())
            tap("Close") or tap("close", contains=True)
            shot("after-close")
    if tap("Stop list"):
        shot("stop-list")
        tap("Close") or tap("close", contains=True)
    if tap("Audio"):
        shot("audio")
        tap("Close") or tap("close", contains=True)
    # An organ package opened as a player opens one: Open, then the system's
    # document picker, then the package in "On My iPad" / "On My iPhone",
    # where the run put it beforehand (#197: a package outside the app's own
    # folder, lent by the Files app).
    package = os.environ.get("PICK_PACKAGE")
    if package and tap("Open"):
        shot("picker")
        tap("Browse")
        tap("On My", contains=True)
        shot("picker-on-my-device")
        tap(os.environ.get("PICK_FOLDER", "check"))
        if tap(os.path.splitext(package)[0], contains=True):
            time.sleep(20)
            shot("package-opened")
        else:
            shot("picker-no-package")
            tap("Cancel")
    shot("end")
    with open(os.path.join(OUT, "ui-result.txt"), "w") as f:
        f.write("\n".join(failures) if failures else "every step found its control\n")
    print("\n".join(failures) or "UI: every step found its control")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
