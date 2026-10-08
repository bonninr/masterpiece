"""Taps through the app on a running Android emulator by the names on its
buttons, keeping a screenshot and the list of what was on screen at every
step: .github/ios-check/drive_ui.py for Android.

    python3 drive.py <output folder>

The names come from the accessibility listing uiautomator writes, which JUCE
fills from each component's name; positions there are in pixels, which is
what `input tap` takes. A step whose control is not on screen is recorded and
skipped. Exits 1 if any step failed.
"""
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

OUT = sys.argv[1]
failures = []
step = 0


def adb(*args, binary=False):
    r = subprocess.run(["adb", *args], capture_output=True, timeout=120)
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


def elements():
    adb("shell", "uiautomator", "dump", "/sdcard/ui.xml")
    xml = adb("shell", "cat", "/sdcard/ui.xml")
    try:
        root = ET.fromstring(xml[xml.find("<"):])
    except ET.ParseError:
        return []
    found = []
    for n in root.iter("node"):
        name = (n.get("text") or n.get("content-desc") or "").strip()
        m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", n.get("bounds", ""))
        if m:
            x1, y1, x2, y2 = map(int, m.groups())
            found.append((name, n.get("class", ""), (x1, y1, x2, y2)))
    return found


def shot(name):
    global step
    step += 1
    base = os.path.join(OUT, f"{step:02d}-{name}")
    with open(base + ".png", "wb") as f:
        f.write(adb("exec-out", "screencap", "-p", binary=True))
    with open(base + ".txt", "w") as f:
        for e in elements():
            f.write(f"{e[0]!r} {e[1]} {e[2]}\n")


def tap(name, contains=False, required=True):
    for text, _, (x1, y1, x2, y2) in elements():
        if text == name or (contains and name.lower() in text.lower()):
            adb("shell", "input", "tap", str((x1 + x2) // 2), str((y1 + y2) // 2))
            time.sleep(2)
            return True
    if required:
        failures.append(f"no '{name}' on screen")
    return False


def close_window():
    # A panel's close button, or the system's back key when it has none.
    if not (tap("Close", required=False) or tap("close", contains=True, required=False)):
        failures.append("no close button: used the back key")
        adb("shell", "input", "keyevent", "KEYCODE_BACK")
        time.sleep(2)


def main():
    os.makedirs(OUT, exist_ok=True)
    shot("start")
    if any("closed unexpectedly" in e[0] for e in elements()):
        tap("OK")
        shot("notice-dismissed")
    tap("Skip setup", required=False)  # only on a first run
    shot("console")
    if tap("Settings"):
        shot("settings-menu")
        if tap("General settings", contains=True):
            shot("general-settings")
            for tab in ("MIDI", "Mixer", "Log"):
                if tap(tab):
                    shot("general-" + tab.lower())
            close_window()
            shot("after-close")
        else:
            adb("shell", "input", "keyevent", "KEYCODE_BACK")
    if tap("Stop list"):
        shot("stop-list")
        close_window()
    if tap("Audio"):
        shot("audio")
        close_window()
    if tap("Open"):
        shot("picker")
        adb("shell", "input", "keyevent", "KEYCODE_BACK")
        time.sleep(2)
    shot("end")
    with open(os.path.join(OUT, "ui-result.txt"), "w") as f:
        f.write("\n".join(failures) + "\n" if failures else "every step found its control\n")
    print(OUT + ": " + ("; ".join(failures) or "every step found its control"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
