#!/usr/bin/env bash
# Start Masterpiece with the control API on, for this computer only, with a
# token; wait until the API answers and, when an organ is given, until it has
# loaded; then print export lines for MP_BASE, MP_TOKEN and MP_PID.
#
#   eval "$(scripts/mp_launch.sh --odf '/path/Organ.organ')"
#   ...
#   scripts/mp_stop.sh      # always: stops the program, restores the settings
#
# Linux and macOS. The program runs in its own session, so it outlives the
# shell that started it; mp_stop.sh stops the whole group (and xvfb-run).
# settings.mpglobal is backed up before the API lines are written, and
# mp_stop.sh restores the backup.
#
# The arguments go to the program (--odf, --gui-only, --log ...).
# Env:
#   MP_BIN, MP_REPO, MP_CONFIG  see mp_env.sh
#   MP_PORT          API port (default 8675, apart from the usual 8080)
#   MP_TOKEN         token (default: random)
#   MP_NO_XVFB=1     no xvfb-run even without a display
#   MP_WAIT_ORGAN    1 (default) waits for the organ when one is given
#   MP_LOAD_TIMEOUT  seconds to wait for the organ (default 600)
#   MP_FORCE=1       start even though another Masterpiece runs
# Progress goes to stderr; only the export lines go to stdout.
set -euo pipefail
. "$(dirname "$0")/mp_env.sh"
BIN="$(mp_bin)"
CONFIG="$(mp_config)"
PORT="${MP_PORT:-8675}"
TOKEN="${MP_TOKEN:-$(head -c18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | cut -c1-24)}"
SETTINGS="$CONFIG/settings.mpglobal"
BACKUP="$SETTINGS.pre-mp-launch"
STATE="$CONFIG/mp_launch.state"

[ -n "$BIN" ] && [ -x "$BIN" ] || { echo "no Masterpiece program found; build it or set MP_BIN" >&2; exit 1; }
mkdir -p "$CONFIG"
if [ -f "$STATE" ]; then
  old="$(. "$STATE"; echo "${MP_PID:-}")"
  if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
    echo "a Masterpiece started by mp_launch.sh still runs (pid $old); run mp_stop.sh first" >&2
    exit 1
  fi
fi
# Two programs on one settings folder overwrite each other's settings when
# they quit: the player's own Masterpiece comes first.
if [ "${MP_FORCE:-0}" != 1 ] && pgrep -x Masterpiece >/dev/null 2>&1; then
  echo "Masterpiece already runs on this computer. Use that one through its API" >&2
  echo "(Settings > General settings > Remote), or close it, or set MP_FORCE=1." >&2
  exit 1
fi
touch "$SETTINGS"
[ -f "$BACKUP" ] || cp "$SETTINGS" "$BACKUP"

# The API lines, every other line kept.
tmp="$(mktemp)"
grep -viE '^(api|apiport|apitoken|apiwrite|apilocal)[[:space:]]' "$SETTINGS" > "$tmp" || true
{ echo "api 1"; echo "apiwrite 1"; echo "apilocal 1"; echo "apiport $PORT"; echo "apitoken $TOKEN"; } >> "$tmp"
mv "$tmp" "$SETTINGS"
echo ">> settings.mpglobal: API on, this computer only, port $PORT (backup: $BACKUP)" >&2

want_organ=0
for a in "$@"; do
  case "$a" in
    --odf|*.organ|*.Organ_Hauptwerk_xml|*.CustomOrgan_Hauptwerk_xml|*.rar|*.orgue) want_organ=1 ;;
  esac
done

LOG="$CONFIG/masterpiece.api-launch.log"
runner=()
if [ "$(uname -s)" != Darwin ] && [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && [ "${MP_NO_XVFB:-0}" != 1 ]; then
  command -v xvfb-run >/dev/null || { echo "no display and no xvfb-run; install xvfb or set DISPLAY" >&2; exit 1; }
  runner=(xvfb-run -a)
  echo ">> no display: xvfb-run" >&2
fi
if command -v setsid >/dev/null; then
  setsid "${runner[@]}" "$BIN" "$@" </dev/null >"$LOG" 2>&1 &
else
  "${runner[@]}" "$BIN" "$@" </dev/null >"$LOG" 2>&1 &
fi
PID=$!
BASE="http://127.0.0.1:$PORT/api/v1"
printf 'MP_PID=%q\nMP_BASE=%q\nMP_TOKEN=%q\n' "$PID" "$BASE" "$TOKEN" > "$STATE"
echo ">> started $PID (output: $LOG)" >&2

fail() { echo "!! $1; see $LOG" >&2; tail -20 "$LOG" >&2; "$(dirname "$0")/mp_stop.sh" >&2 || true; exit 1; }

echo ">> waiting for $BASE" >&2
ok=0
for _ in $(seq 1 120); do
  kill -0 "$PID" 2>/dev/null || fail "the program exited"
  if curl -fsS --max-time 2 "$BASE" >/dev/null 2>&1; then ok=1; break; fi
  sleep 1
done
[ "$ok" = 1 ] || fail "the API did not answer"
echo ">> API up" >&2

if [ "$want_organ" = 1 ] && [ "${MP_WAIT_ORGAN:-1}" = 1 ]; then
  limit="${MP_LOAD_TIMEOUT:-600}"; t=0; st=""
  while [ "$t" -lt "$limit" ]; do
    kill -0 "$PID" 2>/dev/null || fail "the program exited while loading"
    read -r st prog < <(curl -fsS --max-time 3 "$BASE/program" 2>/dev/null \
      | python3 -c 'import json,sys;d=json.load(sys.stdin);print(d.get("organState"),d.get("loadProgress"))' 2>/dev/null || echo "? ?")
    [ "$st" = "2" ] && break
    [ $((t % 10)) -eq 0 ] && echo ">> loading: state $st, ${prog}/1000" >&2
    sleep 1; t=$((t+1))
  done
  [ "$st" = "2" ] || fail "the organ had not loaded after ${limit} s"
  echo ">> organ loaded" >&2
fi

printf 'export MP_BASE=%q\n' "$BASE"
printf 'export MP_TOKEN=%q\n' "$TOKEN"
printf 'export MP_PID=%q\n' "$PID"
