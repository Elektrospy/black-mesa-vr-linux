#!/usr/bin/env bash
# Build the native-Linux 32-bit shim (linux/) and optionally copy it into the game.
#
#   scripts/build-linux.sh                                   # build only
#   scripts/build-linux.sh --auto                            # build + find the native game in your Steam libraries
#   scripts/build-linux.sh "/path/to/steamapps/common/Black Mesa"   # build + copy to that folder
#   (BMVR_GAME_DIR=... is used when set, instead of searching.)
#
# Needs: cmake >= 3.20, gcc with 32-bit support (`gcc -m32` must work).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build_linux_native"

# Find the NATIVE Linux Black Mesa (folder containing bms_linux) in Steam libraries.
find_native_game() {
  local roots=(
    "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    "$HOME/snap/steam/common/.local/share/Steam"
  )
  local libs=() r vdf p seen="" lib real cand
  for r in "${roots[@]}"; do
    [[ -d "$r/steamapps" ]] || continue
    libs+=("$r")
    vdf="$r/steamapps/libraryfolders.vdf"
    if [[ -f "$vdf" ]]; then
      while IFS= read -r p; do
        [[ -n "$p" && -d "$p/steamapps" ]] && libs+=("$p")
      done < <(sed -nE 's/^[[:space:]]*"path"[[:space:]]+"([^"]*)".*/\1/p' "$vdf")
    fi
  done
  for lib in "${libs[@]}"; do
    real="$(readlink -f "$lib" 2>/dev/null || printf '%s' "$lib")"
    case "|$seen|" in *"|$real|"*) continue ;; esac
    seen="$seen|$real"
    cand="$real/steamapps/common/Black Mesa"
    if [[ -f "$cand/bms_linux" ]]; then printf '%s\n' "$cand"; return 0; fi
    [[ -f "$cand/bms.exe" ]] && echo "Note: found $cand but it is the Windows (Proton) build, not native." >&2
  done
  return 1
}

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/linux-x86.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --parallel
SO="$(find "$BUILD" -name bmvr_preload32.so -print -quit)"
[[ -n "$SO" ]] || { echo "bmvr_preload32.so not found after build" >&2; exit 1; }
echo "Built $SO"
file "$SO" | sed 's/^/  /'

# The interposers must carry the exact ToGL mangled names.
missing=0
for s in _ZN16IDirect3DDevice97PresentEPK5_RECTS2_PvPK7RGNDATA \
         _ZN16IDirect3DDevice915SetRenderTargetEjP17IDirect3DSurface9 \
         _ZN16IDirect3DDevice913CreateTextureEjjjj10_D3DFORMAT8_D3DPOOLPP17IDirect3DTexture9PPvPc; do
  nm -D --defined-only "$SO" | grep -q " $s\$" || { echo "MISSING export: $s" >&2; missing=1; }
done
[[ $missing -eq 0 ]] || exit 1
echo "Exports OK."

TARGET="${1:-${BMVR_GAME_DIR:-}}"
if [[ "$TARGET" == "--auto" ]]; then
  TARGET="${BMVR_GAME_DIR:-}"
  if [[ -z "$TARGET" ]]; then
    TARGET="$(find_native_game)" || {
      echo "Built OK, but could not find the native Black Mesa (a folder with bms_linux) in your Steam libraries." >&2
      echo "Pass the path:  scripts/build-linux.sh \"/path/to/Black Mesa\"   or set BMVR_GAME_DIR." >&2
      exit 2
    }
    echo "Found native Black Mesa: $TARGET"
  fi
fi

if [[ -n "$TARGET" ]]; then  GAME="${TARGET%/}"
  [[ -f "$GAME/bms_linux" ]] || { echo "Not a native Black Mesa folder (no bms_linux): $GAME" >&2; exit 1; }
  cp -f "$SO" "$GAME/bmvr_preload32.so"
  echo "Copied to $GAME/bmvr_preload32.so"
  cat <<EOT

Steam -> Black Mesa -> Properties -> Launch Options (NATIVE Linux build, not Proton):

  LD_PRELOAD="\$LD_PRELOAD:$GAME/bmvr_preload32.so" %command%

Start the game, load a map for a few seconds, quit, then send:
  $GAME/bmvr_linux_log.txt

Steam's 64-bit helper processes may print "wrong ELF class" for the 32-bit preload; harmless.
The shim only observes. Remove the launch option to return to normal.
EOT
fi
