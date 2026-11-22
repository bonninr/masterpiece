#!/usr/bin/env bash
# Render one or more takes of an organ to WAV, repeatably. Linux and macOS.
#
#   render_takes.sh ORGAN TAKE [TAKE ...]
#
# Each TAKE is  STOPS:MIDI:WAV
#   STOPS  organ-file stop ids such as 12,15,31, or a registration word
#          (plenum, tutti, all, chorale, trio, solo)
#   MIDI   the .mid file to play
#   WAV    the recording, captured inside the program
#
# Env: MP_BIN (see mp_env.sh), MP_PRELOAD=1 (load only the drawn ranks; needs
#      id lists), MP_NO_XVFB=1 (no xvfb-run even without a display).
# Close the player's own Masterpiece first: both would share the settings.
set -euo pipefail
. "$(dirname "$0")/mp_env.sh"
BIN="$(mp_bin)"
[ $# -ge 2 ] || { sed -n '2,15p' "$0"; exit 1; }
ORGAN="$1"; shift
[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "no Masterpiece program found; build it or set MP_BIN" >&2; exit 1; }

cmd=("$BIN" --odf "$ORGAN")
[ "${MP_PRELOAD:-0}" = 1 ] && cmd+=(--preload-drawn)
for take in "$@"; do
  IFS=: read -r stops midi wav <<<"$take"
  [ -f "$midi" ] || { echo "missing MIDI file: $midi" >&2; exit 1; }
  if [[ "$stops" =~ ^[0-9,]+$ ]]; then cmd+=(--draw-stops "$stops"); else cmd+=(--registration "$stops"); fi
  cmd+=(--play-midi "$midi" --record-audio "$wav")
done

runner=()
if [ "$(uname -s)" != Darwin ] && [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ "${MP_NO_XVFB:-0}" != 1 ] &&
   command -v xvfb-run >/dev/null; then
  runner=(xvfb-run -a)
fi
printf '>> '; printf '%q ' "${runner[@]}" "${cmd[@]}"; echo
exec "${runner[@]}" "${cmd[@]}"
