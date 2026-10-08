#!/bin/bash
# The app on a running Android emulator, as .github/ios-check/run_devices.sh
# runs it on the iOS simulators: the sine-wave test organs played and
# recorded inside the app and checked against their numbers, then the console
# tapped through by the names on its buttons, upright and turned.
#
#   bash run.sh <apk> <organs folder> <output folder>
#
# An Android app has no command line: the options go in as the intent extra
# mp.args, which the app reads at startup (Main.cpp, launchArguments).
set -u
APK=$1
ORGANS=$2
OUT=$3
PKG=org.masterpiece.player
ACT=android.app.Activity
# The app's own folder on shared storage: the app reads and writes it without
# a permission, and adb can put files there.
DATA=/sdcard/Android/data/$PKG/files
mkdir -p "$OUT"
failed=0

adb wait-for-device
adb shell 'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 1; done'
adb install -r -g "$APK" > "$OUT/install.txt" 2>&1 || { cat "$OUT/install.txt"; exit 1; }
adb shell mkdir -p "$DATA"
adb shell wm size > "$OUT/screen.txt"
adb shell wm density >> "$OUT/screen.txt"

launch() {  # the options, as one string
  adb shell am force-stop $PKG
  adb shell "am start -W -n $PKG/$ACT --es mp.args '$1'" >> "$OUT/launches.txt" 2>&1
}

play() {  # organ folder name
  local organ=$1 dir="$DATA/$1"
  adb shell rm -rf "$dir"
  adb push "$ORGANS/$organ" "$DATA/" > /dev/null
  launch "--odf $dir/check.orgue --draw-stops all --play-midi $dir/check.mid --record-audio $dir/out.wav --log $dir/run.log --stay-open"
  local finished=""
  for _ in $(seq 1 90); do
    adb shell cat "$dir/run.log" 2>/dev/null | grep -q "recital finished" && { finished=1; break; }
    sleep 2
  done
  [ -z "$finished" ] && echo "recital did not finish" | tee "$OUT/$organ-stalled.txt"
  sleep 3
  adb shell cat "$dir/run.log" > "$OUT/$organ-run.log" 2>&1
  adb exec-out screencap -p > "$OUT/$organ-playing.png"
  if adb pull "$dir/out.wav" "$OUT/$organ.wav" > /dev/null 2>&1; then
    python3 .github/ios-check/check_audio.py "$OUT/$organ.wav" "$ORGANS/$organ/check.json" \
      > "$OUT/$organ-audio.txt" 2>&1 || failed=1
  else
    echo "FAIL: no recording" > "$OUT/$organ-audio.txt"
    failed=1
  fi
  echo "== $organ"; cat "$OUT/$organ-audio.txt"
}

play check
play heavy

# The console, upright and turned. The recital runs above leave the app
# stopped by force, which it reports at the next start: the driver dismisses
# that notice like a player would.
adb shell settings put system accelerometer_rotation 0
for turn in 0 1; do
  adb shell settings put system user_rotation $turn
  sleep 2
  launch ""
  sleep 6
  python3 .github/android-check/drive.py "$OUT/ui-rotation-$turn" || true
done
adb logcat -d -t 2000 > "$OUT/logcat.txt" 2>&1 || true

exit $failed
