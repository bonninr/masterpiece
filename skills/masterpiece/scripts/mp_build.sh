#!/usr/bin/env bash
# Configure and build a preset, through ccache when it is installed, and print
# the program's path. Linux and macOS; on Windows run the two cmake commands
# from the x64 Native Tools Command Prompt.
#
#   scripts/mp_build.sh            # preset dev-fast
#   PRESET=dev scripts/mp_build.sh
#
# Env: MP_REPO (default: the repository holding this script), PRESET
# (default dev-fast), JOBS (default: all cores).
set -euo pipefail
. "$(dirname "$0")/mp_env.sh"
REPO="$(mp_repo)"
PRESET="${PRESET:-dev-fast}"
cd "$REPO"
[ -f CMakePresets.json ] || { echo "no CMakePresets.json in $REPO; set MP_REPO to the repository" >&2; exit 1; }
extra=()
if command -v ccache >/dev/null; then
  extra=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
echo ">> configure $PRESET" >&2
cmake --preset "$PRESET" "${extra[@]}" >&2
echo ">> build" >&2
if [ -n "${JOBS:-}" ]; then cmake --build --preset "$PRESET" -j"$JOBS" >&2
else cmake --build --preset "$PRESET" >&2; fi
dir="$REPO/build/$PRESET/apps/MasterpieceApp/MasterpieceApp_artefacts"
if [ "$(uname -s)" = Darwin ]; then
  bin="$(ls -d "$dir"/*/Masterpiece.app 2>/dev/null | head -1)"
else
  bin="$(ls "$dir"/*/Masterpiece 2>/dev/null | head -1)"
fi
[ -n "$bin" ] && echo "$bin" || { echo "the build produced no program under $dir" >&2; exit 1; }
