#!/usr/bin/env bash
# Build the native-Linux 32-bit shim (linux/) and optionally copy it into the game.
#
#   scripts/build-linux.sh                                   # build only
#   scripts/build-linux.sh --auto                            # build + find the native game in your Steam libraries
#   scripts/build-linux.sh "/path/to/steamapps/common/Black Mesa"   # build + copy to that folder
#   (BMVR_GAME_DIR=... is used when set, instead of searching.)
#   scripts/build-linux.sh --uninstall [/path/to/Black Mesa]  # remove everything this script installed
#
# Always does a clean build (the build folder is deleted first).
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

# ------------------------------------------------------------------ uninstall
# Everything this script adds lives in NEW files (the game's own bms.sh is never modified),
# so uninstalling is just deleting them. Also remove the Steam launch option by hand.
if [[ "${1:-}" == "--uninstall" ]]; then
  U="${2:-${BMVR_GAME_DIR:-}}"
  [[ -n "$U" ]] || U="$(find_native_game)" || { echo "Could not find the native Black Mesa; pass the folder." >&2; exit 2; }
  U="${U%/}"
  [[ -f "$U/bms_linux" ]] || { echo "Not a native Black Mesa folder (no bms_linux): $U" >&2; exit 1; }
  for f in bin/bmvr_preload32.so bmvr_preload32.so bmvr-run.sh bmvr-bms.sh \
           bmvr_run_marker.txt bmvr_inner_marker.txt bmvr_linux_log.txt \
           bmvr_xfer_helper bmvr_xfer.sock bmvr_xfer.lock bmvr_target.txt; do
    [[ -e "$U/$f" ]] && { rm -f -- "$U/$f"; echo "Removed $U/$f"; }
  done
  rm -f -- "$U"/bmvr_snapshot_*.ppm
  echo "Done. Now clear the Black Mesa launch options in Steam (they point at bmvr-run.sh)."
  exit 0
fi

# Always build from scratch: a stale build dir once made the installer ship an old shim ("no work to do").
# $BUILD is a fixed folder inside the project, so removing it is safe.
[[ "$BUILD" == "$ROOT/build_linux_native" ]] && rm -rf -- "$BUILD"
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
         _ZN16IDirect3DDevice913CreateTextureEjjjj10_D3DFORMAT8_D3DPOOLPP17IDirect3DTexture9PPvPc \
         _ZN17IDirect3DTexture915GetSurfaceLevelEjPP17IDirect3DSurface9; do
  nm -D --defined-only "$SO" | grep -q " $s\$" || { echo "MISSING export: $s" >&2; missing=1; }
done
[[ $missing -eq 0 ]] || exit 1
echo "Exports OK."

# Stage 2a/2b: the 64-bit Vulkan helper for the GPU hand-off test. Built with the HOST compiler (64-bit),
# not the 32-bit toolchain. Needs the Vulkan headers only (libvulkan is loaded with dlopen).
# Failure here is not fatal: the shim itself does not need it.
HELPER=""
VKINC=""
if ! echo '#include <vulkan/vulkan.h>' | cc -E -x c - >/dev/null 2>&1; then
  for d in "$ROOT/research/dxvk-gplasync/drop-in-dxvk_new/include/vulkan/include"; do
    [[ -f "$d/vulkan/vulkan.h" ]] && VKINC="-I$d"
  done
fi
if echo '#include <vulkan/vulkan.h>' | cc $VKINC -E -x c - >/dev/null 2>&1; then
  if cc -O2 -Wall -Wextra -std=gnu11 $VKINC -I"$ROOT/linux" -o "$BUILD/bmvr_xfer_helper" "$ROOT/linux/bmvr_xfer_helper.c" -ldl; then
    HELPER="$BUILD/bmvr_xfer_helper"; echo "Built $HELPER (64-bit)"
  else
    echo "WARNING: could not build the 64-bit helper (stage 2a test unavailable)." >&2
  fi
else
  echo "WARNING: Vulkan headers not found, so the stage 2a helper was not built." >&2
  echo "         Install them (Arch/CachyOS: sudo pacman -S vulkan-headers; Debian/Ubuntu: libvulkan-dev) and re-run." >&2
fi

# Stage 3a: can this GPU/driver share semaphores with OpenGL at all? The helper checks, then runs the exact
# frame cycle of the game (signal, wait, copy, signal) with itself playing the GL side. Bounded by a timeout;
# a failure only means sync=sem will not work here (glFinish + ack, the default, is unaffected).
if [[ -n "$HELPER" ]]; then
  echo "Helper self-test (stage 3a, GPU semaphore sync):"
  if out="$(timeout 40 "$HELPER" --selftest-sync 2>&1)"; then
    printf '%s\n' "$out" | grep -E "opaque-fd semaphore|selftest:" | sed 's/^\[helper\] /  /'
  else
    printf '%s\n' "$out" | tail -6 | sed 's/^/    /'
    echo "  The self-test did not pass. This only affects the optional sync=sem mode." >&2
  fi
fi

# Portability: the game may run inside an OLDER glibc (Steam Linux Runtime). A preload that needs
# newer symbol versions is silently skipped by the dynamic loader. List what this build needs.
maxver="$(objdump -T "$SO" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
echo "Highest glibc symbol version required: ${maxver:-none}"
case "${maxver:-GLIBC_2.0}" in
  GLIBC_2.0|GLIBC_2.1*|GLIBC_2.2*|GLIBC_2.3*|GLIBC_2.4) ;;
  *) echo "WARNING: requires ${maxver}; the game's runtime may be older, and then this library would NOT load." >&2 ;;
esac

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
  mkdir -p "$GAME/bin"
  cp -f "$SO" "$GAME/bin/bmvr_preload32.so"
  rm -f "$GAME/bmvr_preload32.so"   # older versions of this script copied it to the game root

  # Two small launcher files, both NEW; the game's own bms.sh is never touched.
  #
  # Why two: Steam expands the launch option "<wrapper> %command%" to
  #     <wrapper> steam-launch-wrapper -- reaper ... -- scout-on-soldier-entry-point-v2 -- <game>/bms.sh -game bms ...
  # so <wrapper> runs on the HOST, in front of the Steam Linux Runtime container (pressure-vessel),
  # and pressure-vessel REBUILDS LD_PRELOAD inside the container (a bare library name set outside is
  # dropped). Only code that runs INSIDE the container can set it. The only thing inside the container
  # we control is the path of the start script, so the host wrapper swaps the "bms.sh" argument for
  # bmvr-bms.sh, which sets LD_PRELOAD and then runs the original, unmodified bms.sh.
  cat > "$GAME/bmvr-run.sh" <<'WRAP'
#!/bin/sh
# Black Mesa VR (native Linux), HOST side. Steam launch options:   "<game folder>/bmvr-run.sh" %command%
# Replaces the ".../bms.sh" argument by ".../bmvr-bms.sh" (see build-linux.sh), then runs the command.
D="$(cd "$(dirname "$0")" && pwd)"
swapped=0
n=$#
while [ "$n" -gt 0 ]; do
  a="$1"; shift
  if [ "$swapped" = 0 ]; then
    case "$a" in
      */bms.sh) a="${a%bms.sh}bmvr-bms.sh"; swapped=1 ;;
    esac
  fi
  set -- "$@" "$a"
  n=$((n - 1))
done
{ date; echo "stage: host wrapper (outside the container)"; echo "swapped bms.sh -> bmvr-bms.sh: $swapped"; echo "args: $*"; } > "$D/bmvr_run_marker.txt" 2>/dev/null
exec "$@"
WRAP
  cat > "$GAME/bmvr-bms.sh" <<'INNER'
#!/bin/sh
# Black Mesa VR (native Linux), CONTAINER side. Started instead of bms.sh by bmvr-run.sh; sets
# LD_PRELOAD inside the Steam Linux Runtime and then runs the original, unmodified bms.sh.
D="$(cd "$(dirname "$0")" && pwd)"
# Bare file name on purpose: LD_PRELOAD splits on spaces ("Black Mesa"), so an absolute path breaks.
# The loader finds it through LD_LIBRARY_PATH.
export LD_LIBRARY_PATH="$D/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_PRELOAD="bmvr_preload32.so${LD_PRELOAD:+:$LD_PRELOAD}"
{ date; echo "stage: inside the container"; echo "LD_PRELOAD=$LD_PRELOAD"; echo "LD_LIBRARY_PATH=$LD_LIBRARY_PATH"; } > "$D/bmvr_inner_marker.txt" 2>/dev/null
exec "${BMVR_REAL_BMS:-$D/bms.sh}" "$@"
INNER
  chmod +x "$GAME/bmvr-run.sh" "$GAME/bmvr-bms.sh"
  echo "Copied to $GAME/bin/bmvr_preload32.so and wrote bmvr-run.sh + bmvr-bms.sh (bms.sh untouched)"
  if [[ -n "$HELPER" ]]; then cp -f "$HELPER" "$GAME/bmvr_xfer_helper" && chmod +x "$GAME/bmvr_xfer_helper" && echo "Installed $GAME/bmvr_xfer_helper (64-bit, stages 2a-3a)"; fi

  # End-to-end self-test in a folder WITH A SPACE: fake game dir, fake bms.sh, same argument shape Steam uses.
  T="$(mktemp -d)"
  printf 'int main(void){return 0;}\n' > "$T/t.c"
  if gcc -m32 "$T/t.c" -o "$T/t32" 2>/dev/null; then
    G="$T/Black Mesa"; mkdir -p "$G/bin"
    cp "$GAME/bin/bmvr_preload32.so" "$G/bin/"; cp "$GAME/bmvr-run.sh" "$GAME/bmvr-bms.sh" "$G/"
    cp "$T/t32" "$G/t32"
    printf '#!/bin/sh\nexec "$(dirname "$0")/t32"\n' > "$G/bms.sh"; chmod +x "$G/bms.sh"
    ( cd "$G" && "$G/bmvr-run.sh" env -i /bin/sh -c 'exec "$@"' sh "$G/bms.sh" -game bms >/dev/null 2>&1 )
    if [[ -s "$G/bmvr_linux_log.txt" ]]; then echo "Self-test: PASS (host wrapper -> bmvr-bms.sh -> bms.sh -> shim loads, path with a space)"
    else echo "Self-test: FAILED - run tools/linux/bmvr-diagnose-linux.sh" >&2; fi
  fi
  rm -rf "$T"

  cat <<EOT

Steam -> Black Mesa -> Properties -> Launch Options (NATIVE Linux build, not Proton):

  "$GAME/bmvr-run.sh" %command%

Keep the quotes: the folder name contains a space. Do NOT put LD_PRELOAD=... in the launch
options: Steam's container (pressure-vessel) discards it before the game starts.
The game's own bms.sh is NOT modified. To undo everything:  scripts/build-linux.sh --uninstall

Start the game, load a map for a few seconds, quit, then look for:
  $GAME/bmvr_linux_log.txt      the shim's log (send this)
  $GAME/bmvr_run_marker.txt     written by the host wrapper (proves the launch option is applied)
  $GAME/bmvr_inner_marker.txt   written inside the container (proves bms.sh was swapped and LD_PRELOAD set)

Stages 2a-3a (GPU hand-off test, optional): BEFORE starting the game, in a terminal:
  "$GAME/bmvr_xfer_helper"
Leave it running, start the game, load a MAP (gameplay, not just the menu) and stay in it ~15 s.
The helper then writes, in the game folder, pictures of an engine render target copied through
shared GPU memory:
  bmvr_snapshot_0_<target>_flip0.ppm, bmvr_snapshot_1_<target>_flip1.ppm, ...
(two orientations on purpose: one of them is upright - tell me which, flip0 or flip1).
The shim log gets "xfer:" lines and, twice per session, an "rt-trace:" block that lists the render
targets of one frame in order, with what OpenGL reports for each. That block is the most useful
thing to send even if no snapshot appears (it also works without the helper).
Choose another source (default is the render target whose label contains _rt_FullFrameFB) with a
file "$GAME/bmvr_target.txt":   target=name:<part of label>   or   target=size:WxH   or   target=window
and optionally   flip=0|1|alt.   Two eyes (stage 3d, start the helper with --eyes 2):   target2=name:<label>|size:WxH   flip2=0|1|alt.

Stage 3a: GPU synchronisation. By default the shim stalls the GPU with glFinish and waits for the helper
(verified). To try GPU semaphores instead (no stall), add this line to "$GAME/bmvr_target.txt":
    sync=sem
It is used only if the helper offers semaphores (the self-test above must have said PASS and exported/imported
the fds) and OpenGL has GL_EXT_semaphore_fd; otherwise the log says why it fell back. The log then shows, per
snapshot, "GL side N us" (time spent in the game's render thread) and "helper round trip N us"; run once
with sync=finish and once with sync=sem and send both logs. Caution: if the helper process is killed while
the game is mid-frame, the game's GPU can wait forever on a semaphore (restart the game). That is why it is
opt-in. \"\$GAME/bmvr_xfer_helper\" --no-semaphores forces the old behaviour from the helper side.

Kill test (game running, no helper running): ./tests/linux/test_kill_loop.sh -n 10   (needs sync=sem in bmvr_target.txt to be meaningful;
add --eyes 2 for the two-eye case).
Only one helper can serve a game folder; a second one exits with an error (stage 3c).

The shim only observes. Remove the launch option to go back to normal.
No log file appears? Run tools/linux/bmvr-diagnose-linux.sh (ideally while the game is running).
EOT
fi
