#!/usr/bin/env bash
# Stop the Masterpiece that mp_launch.sh started (its whole group, xvfb-run
# included), then restore settings.mpglobal from the backup, which switches the
# API off again. Safe to run twice.
set -u
. "$(dirname "$0")/mp_env.sh"
CONFIG="$(mp_config)"
SETTINGS="$CONFIG/settings.mpglobal"
BACKUP="$SETTINGS.pre-mp-launch"
STATE="$CONFIG/mp_launch.state"

PID="${MP_PID:-}"
if [ -z "$PID" ] && [ -f "$STATE" ]; then PID="$(. "$STATE"; echo "${MP_PID:-}")"; fi

if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
  kill -TERM -- "-$PID" 2>/dev/null || kill -TERM "$PID" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
  kill -KILL -- "-$PID" 2>/dev/null || kill -KILL "$PID" 2>/dev/null || true
  echo ">> stopped $PID" >&2
else
  echo ">> no running instance recorded" >&2
fi

# Only after the program has exited: it writes its settings when it quits.
if [ -f "$BACKUP" ]; then
  mv -f "$BACKUP" "$SETTINGS"
  echo ">> settings.mpglobal restored" >&2
elif [ -f "$SETTINGS" ]; then
  grep -viE '^(api|apiport|apitoken|apiwrite|apilocal)[[:space:]]' "$SETTINGS" > "$SETTINGS.tmp" || true
  mv -f "$SETTINGS.tmp" "$SETTINGS"
  echo ">> API lines removed from settings.mpglobal" >&2
fi
rm -f "$STATE"
