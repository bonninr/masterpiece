#!/bin/bash
# Runs the app on several simulators, one after another, and keeps what each
# showed and played in ios-shots/<device>/.
#
#   bash run_devices.sh <Masterpiece.app> <organs folder>
#
# On every device: the test organ played from a MIDI file and recorded inside
# the app, the recording checked against the numbers it was made from
# (check_audio.py), then the UI tapped through by the names on its buttons
# (drive_ui.py), ending with the organ package opened through the system's
# document picker from "On My iPad", outside the app's own folder (#197). On
# the large iPad the heavy organ is played as well.
#
# Exits 1 if any recording failed its checks. The tap-through reports only:
# the picker is the system's, and its labels change between iOS versions.
set -u
APP=$1
ORGANS=$2
BUNDLE=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$APP/Info.plist")
mkdir -p ios-shots
failed=0

# A recital whose audio stops partway is played once more. On the runners
# the simulator's audio is now and then interrupted for good (the host's
# audio route changes as Simulator.app opens and quits), which leaves the
# recording empty with the app idle; the first attempt's files are kept,
# named "-first", and a second stall fails the run as before.
play_twice() {  # device, organ folder name, output folder
  local before=$failed
  play "$@"
  local organ=$2 out=$3
  [ -f "$out/$organ-stalled.txt" ] || return 0
  for f in "$out/$organ"-*; do mv "$f" "${f/$organ-/$organ-first-}"; done
  [ -f "$out/$organ.wav" ] && mv "$out/$organ.wav" "$out/$organ-first.wav"
  echo "the $organ recital stalled; playing it once more"
  failed=$before
  play "$@"
}

udid_for() {  # the newest runtime's device whose name contains $1
  xcrun simctl list devices available -j | python3 -c "
import json, sys
d = json.load(sys.stdin)['devices']
for k in sorted(d, reverse=True):
    if 'iOS' not in k: continue
    for x in d[k]:
        if sys.argv[1] in x['name']:
            print(x['udid'], x['name'].replace(' ', '_')); sys.exit()
" "$1"
}

play() {  # device, organ folder name, output folder
  local dev=$1 organ=$2 out=$3
  local data
  data=$(xcrun simctl get_app_container "$dev" "$BUNDLE" data)
  rm -rf "$data/Documents/$organ"
  cp -R "$ORGANS/$organ" "$data/Documents/$organ"
  # Straight after an install the system may not know the app yet ("unknown
  # to FrontBoard"): a few tries, a few seconds apart.
  for _ in 1 2 3 4 5; do
    xcrun simctl launch --terminate-running-process "$dev" "$BUNDLE" \
      --odf "$data/Documents/$organ/check.orgue" --draw-stops all \
      --play-midi "$data/Documents/$organ/check.mid" \
      --record-audio "$data/Documents/$organ/out.wav" \
      --log "$data/Documents/$organ/run.log" --stay-open > "$out/$organ-launch.txt" 2>&1 && break
    sleep 5
  done
  local finished=""
  for _ in $(seq 1 60); do
    grep -q "recital finished" "$data/Documents/$organ/run.log" 2>/dev/null && { finished=1; break; }
    sleep 2
  done
  # A recital that never ends: where every thread of the app is, for the
  # artifact. A simulator app is a process of this Mac, so sample reads it.
  if [ -z "$finished" ]; then
    local pid
    pid=$(grep -oE "[0-9]+$" "$out/$organ-launch.txt" | tail -1)
    echo "recital did not finish; sampling pid $pid" | tee "$out/$organ-stalled.txt"
    [ -n "$pid" ] && sample "$pid" 3 -file "$out/$organ-threads.txt" >/dev/null 2>&1
    ps -o pid,stat,%cpu,rss,command -p "$pid" >> "$out/$organ-stalled.txt" 2>&1
  fi
  sleep 3
  cp "$data/Documents/$organ/run.log" "$out/$organ-run.log" 2>/dev/null || echo "no log" > "$out/$organ-run.log"
  xcrun simctl io "$dev" screenshot "$out/$organ-playing.png" >/dev/null 2>&1
  grep -ciE "late block|late-block|underrun" "$out/$organ-run.log" > "$out/$organ-late-blocks.txt" || true
  if [ -f "$data/Documents/$organ/out.wav" ]; then
    cp "$data/Documents/$organ/out.wav" "$out/$organ.wav"
    if ! python3 .github/ios-check/check_audio.py "$out/$organ.wav" "$ORGANS/$organ/check.json" \
         > "$out/$organ-audio.txt" 2>&1; then
      failed=1
    fi
  else
    echo "FAIL: no recording" > "$out/$organ-audio.txt"
    failed=1
  fi
  cat "$out/$organ-audio.txt"
}

# Turns a booted simulator to landscape through the Simulator window's own
# menu, Device > Rotate Left; the window has to be open for that. The app runs
# in landscape only: with the simulator upright, iOS draws it turned or scaled
# into a band, and a tap at a position idb reads lands somewhere else.
landscape() {
  open -a Simulator --args -CurrentDeviceUDID "$1"
  sleep 5
  osascript <<'OSA'
tell application "Simulator" to activate
delay 1
tell application "System Events" to tell process "Simulator"
  set frontmost to true
  click menu item "Rotate Left" of menu "Device" of menu bar 1
end tell
OSA
  sleep 3
}

for want in "iPad Pro 13" "iPad mini" "iPhone 1"; do
  read -r DEV NAME < <(udid_for "$want")
  if [ -z "${DEV:-}" ]; then echo "no simulator like '$want'"; continue; fi
  # Each device starts upright and on its own window.
  osascript -e 'tell application "Simulator" to quit' >/dev/null 2>&1 || true
  OUT="ios-shots/$NAME"
  mkdir -p "$OUT"
  echo "== $NAME ($DEV)"
  xcrun simctl boot "$DEV" 2>/dev/null
  xcrun simctl bootstatus "$DEV" -b >/dev/null
  xcrun simctl install "$DEV" "$APP"

  play_twice "$DEV" check "$OUT"
  case "$want" in "iPad Pro"*) play_twice "$DEV" heavy "$OUT" ;; esac

  # The package where the Files app keeps "On My iPad", for the picker.
  GROUP=$(xcrun simctl get_app_container "$DEV" com.apple.DocumentsApp groups 2>/dev/null |
          awk '/LocalStorage/ {print $2}')
  if [ -n "$GROUP" ]; then
    mkdir -p "$GROUP/File Provider Storage/check"
    cp "$ORGANS/check/check.orgue" "$GROUP/File Provider Storage/check/"
  else
    echo "no Files storage on this simulator" > "$OUT/picker-note.txt"
  fi
  landscape "$DEV" > "$OUT/rotate.txt" 2>&1 || echo "could not rotate" >> "$OUT/rotate.txt"
  data=$(xcrun simctl get_app_container "$DEV" "$BUNDLE" data)
  xcrun simctl launch --terminate-running-process "$DEV" "$BUNDLE" --log "$data/Documents/ui.log" --log-touches \
    > "$OUT/ui-launch.txt" 2>&1
  sleep 8
  if command -v idb >/dev/null; then
    idb connect "$DEV" >/dev/null 2>&1
    case "$NAME" in iPhone*) SCALE=3 ;; *) SCALE=2 ;; esac
    SCREEN_SCALE=$SCALE PICK_PACKAGE=check.orgue python3 .github/ios-check/drive_ui.py "$DEV" "$OUT/ui" || true
  else
    echo "idb is not installed: no tap-through" > "$OUT/ui-note.txt"
    xcrun simctl io "$DEV" screenshot "$OUT/console.png" >/dev/null 2>&1
  fi
  cp "$data/Documents/ui.log" "$OUT/ui.log" 2>/dev/null || true

  # Still running, or did it crash?
  xcrun simctl spawn "$DEV" launchctl list | grep -i "$BUNDLE" > "$OUT/running.txt" || echo "NOT RUNNING" > "$OUT/running.txt"
  xcrun simctl spawn "$DEV" log show --last 10m --predicate "process == 'Masterpiece'" --style compact \
    > "$OUT/system-log.txt" 2>&1 || true
  xcrun simctl shutdown "$DEV"
done
find ~/Library/Logs/DiagnosticReports -name "Masterpiece*" -newer "$APP" -exec cp {} ios-shots/ \; 2>/dev/null || true
exit $failed
