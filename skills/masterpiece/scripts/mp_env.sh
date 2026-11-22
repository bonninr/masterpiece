# Sourced by the other scripts: where the program and its settings are.
#
#   MP_BIN     the program; default: the newest build in this repository, then
#              Masterpiece on the PATH, then the usual install places
#   MP_REPO    the repository; default: the one holding these scripts
#   MP_CONFIG  the settings folder; default: the system's

mp_repo() {
  if [ -n "${MP_REPO:-}" ]; then echo "$MP_REPO"; return; fi
  local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  local root; root="$(cd "$here/../../.." 2>/dev/null && pwd)"
  if [ -f "$root/CMakePresets.json" ]; then echo "$root"; else pwd; fi
}

mp_config() {
  if [ -n "${MP_CONFIG:-}" ]; then echo "$MP_CONFIG"; return; fi
  case "$(uname -s)" in
    Darwin) echo "$HOME/Library/Masterpiece" ;;
    *)      echo "${XDG_CONFIG_HOME:-$HOME/.config}/Masterpiece" ;;
  esac
}

mp_bin() {
  if [ -n "${MP_BIN:-}" ]; then echo "$MP_BIN"; return; fi
  local repo; repo="$(mp_repo)"
  local found=""
  if [ "$(uname -s)" = Darwin ]; then
    found="$(ls -td "$repo"/build/*/apps/MasterpieceApp/MasterpieceApp_artefacts/*/Masterpiece.app 2>/dev/null | head -1)"
    [ -n "$found" ] && found="$found/Contents/MacOS/Masterpiece"
    [ -z "$found" ] && [ -x /Applications/Masterpiece.app/Contents/MacOS/Masterpiece ] &&
      found=/Applications/Masterpiece.app/Contents/MacOS/Masterpiece
  else
    found="$(ls -t "$repo"/build/*/apps/MasterpieceApp/MasterpieceApp_artefacts/*/Masterpiece 2>/dev/null | head -1)"
  fi
  [ -z "$found" ] && found="$(command -v Masterpiece 2>/dev/null || true)"
  echo "$found"
}
