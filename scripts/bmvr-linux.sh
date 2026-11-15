#!/usr/bin/env bash
# Black Mesa VR - Linux (Proton) installer / uninstaller.
#
# Port of the Windows install flow (README steps + scripts/install.ps1 and
# UNINSTALL-BMVR.ps1) for the release zip layout ("drop-into-Black-Mesa-folder").
#
# Usage:
#   ./bmvr-linux.sh [install]  [--game-dir DIR] [--src DIR] [--force]
#   ./bmvr-linux.sh uninstall  [--game-dir DIR] [--yes] [--force]
#   ./bmvr-linux.sh help
#
# Run it from the unzipped release (next to d3d9.dll and the VR/ folder), or
# from the folder that contains drop-into-Black-Mesa-folder/.
#
# Black Mesa must be the Windows build running through Proton:
#   Steam -> Black Mesa -> Properties -> Compatibility ->
#   "Force the use of a specific Steam Play compatibility tool"
# (the folder must contain bms.exe). Quit the game before installing.

set -euo pipefail

MODE="install"
GAME_DIR="${BMVR_GAME_DIR:-}"
SRC_DIR="${BMVR_SRC_DIR:-}"
FORCE=0
ASSUME_YES=0

die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- arguments
if [[ $# -gt 0 ]]; then
  case "$1" in
    install|uninstall) MODE="$1"; shift ;;
    help|-h|--help)    usage; exit 0 ;;
  esac
fi
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir) [[ $# -ge 2 ]] || die "--game-dir needs a path"; GAME_DIR="$2"; shift 2 ;;
    --src)      [[ $# -ge 2 ]] || die "--src needs a path";      SRC_DIR="$2";  shift 2 ;;
    --force)    FORCE=1; shift ;;
    --yes|-y)   ASSUME_YES=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    *)          die "Unknown argument: $1 (try --help)" ;;
  esac
done

# ------------------------------------------------------- locate Black Mesa
find_game_dir() {
  local roots=(
    "$HOME/.local/share/Steam"
    "$HOME/.steam/steam"
    "$HOME/.steam/root"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    "$HOME/snap/steam/common/.local/share/Steam"
  )
  local libs=() r vdf p
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
  local seen="" lib real cand
  for lib in "${libs[@]}"; do
    real="$(readlink -f "$lib" 2>/dev/null || printf '%s' "$lib")"
    case "|$seen|" in *"|$real|"*) continue ;; esac
    seen="$seen|$real"
    cand="$real/steamapps/common/Black Mesa"
    if [[ -d "$cand" ]]; then printf '%s\n' "$cand"; return 0; fi
  done
  return 1
}

if [[ -z "$GAME_DIR" ]]; then
  GAME_DIR="$(find_game_dir)" || die "Could not find Black Mesa in your Steam libraries.
Pass the folder that contains bms.exe:  $0 $MODE --game-dir \"/path/to/Black Mesa\""
fi
GAME_DIR="${GAME_DIR%/}"
[[ -d "$GAME_DIR" ]] || die "Not a directory: $GAME_DIR"
if [[ ! -f "$GAME_DIR/bms.exe" ]]; then
  die "$GAME_DIR has no bms.exe.
This mod needs the Windows build of Black Mesa. In Steam, open Black Mesa ->
Properties -> Compatibility -> force a Steam Play (Proton) tool, let it
download the Windows files, then run this again."
fi

BIN_DIR="$GAME_DIR/bin"
DXVK_DIR="$GAME_DIR/bin/thirdparty/dxvk-windows-x86"
[[ -d "$DXVK_DIR" ]] || die "DXVK folder missing: $DXVK_DIR (verify the game files in Steam)"

info "Black Mesa folder: $GAME_DIR"

# ---------------------------------------------------------------- running?
check_not_running() {
  [[ $FORCE -eq 1 ]] && return 0
  local hits
  # Match the process name (Wine/Proton names processes after the .exe, truncated
  # to 15 chars), not the full command line, so this script never matches itself.
  hits="$(pgrep -i '^(bms\.exe|OpenXRHelper64)' 2>/dev/null || true)"
  if [[ -n "$hits" ]]; then
    die "Black Mesa or the OpenXR helper is still running (PID: $(echo "$hits" | tr '\n' ' ')).
Quit the game from Steam first (do not kill it), then retry. --force skips this check."
  fi
}

# Files the game/mod leaves behind that must not survive a reinstall.
STALE_FLAGS=(
  bmvr_in_hmd_world.flag bmvr_in_hmd_offscreen.flag bmvr_in_hl2vr_ham.flag
  bmvr_in_hl2vr_pixelvis.flag bmvr_in_hl2vr_predraw.flag
  bmvr_in_hl2vr_playercull.flag bmvr_in_hl2vr_proj.flag bmvr_in_hl2vr_cproj.flag
  bmvr_in_hl2vr_neye.flag bmvr_in_stereo_vis.flag bmvr_in_ctrl_vm.flag
  bmvr_in_fl_gbmatch.flag bmvr_in_drawhud.flag bmvr_in_menu_vr.flag
)
FALSE_BANS='ctrl_pose|ctrl_vm|vr_gloves|hand_overlay|hl2vr_playercull|hl2vr_pixelvis|hl2vr_ham|hl2vr_proj|hl2vr_cproj|hl2vr_neye|hmd_world|fl_gbmatch|drawhud'

launch_options() {
  cat <<'EOF'

Steam -> Black Mesa -> Properties -> Launch Options:

  WINEDLLOVERRIDES="d3d9=n,b" %command% -heapsize 524288 -processheap -high -novid -windowed -oldgameui -enabledxvk

Notes:
  * -oldgameui is required (the new game UI is upside-down in the headset).
  * -enabledxvk makes the game load the DXVK folder copy of d3d9.dll.
  * Blue Shift / Azure Sheep: add  -game bshift  /  -game asheep.
  * Make sure your OpenXR runtime is active on the host (e.g. SteamVR set as
    the OpenXR runtime) before launching the game from Steam.
  * Success check: bmvr_log.txt appears next to bms.exe and starts with
    "BMVR d3d9.dll loaded". Send that log (and the helper log) if it fails.
EOF
}

# =================================================================== INSTALL
do_install() {
  # --- find the package
  if [[ -z "$SRC_DIR" ]]; then
    local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if   [[ -f "$here/d3d9.dll" && -d "$here/VR" ]]; then SRC_DIR="$here"
    elif [[ -f "$here/drop-into-Black-Mesa-folder/d3d9.dll" ]]; then SRC_DIR="$here/drop-into-Black-Mesa-folder"
    elif [[ -f "$PWD/d3d9.dll" && -d "$PWD/VR" ]]; then SRC_DIR="$PWD"
    elif [[ -f "$PWD/drop-into-Black-Mesa-folder/d3d9.dll" ]]; then SRC_DIR="$PWD/drop-into-Black-Mesa-folder"
    else die "Could not find the mod files (d3d9.dll + VR/).
Run this next to them, or pass  --src /path/to/drop-into-Black-Mesa-folder"
    fi
  fi
  SRC_DIR="${SRC_DIR%/}"
  [[ -f "$SRC_DIR/d3d9.dll" ]] || die "Missing $SRC_DIR/d3d9.dll"
  [[ -d "$SRC_DIR/VR" ]]      || die "Missing $SRC_DIR/VR"
  info "Mod package:       $SRC_DIR"

  check_not_running

  # --- stale crash-sticky flags / false bans (same cleanup as install.ps1)
  local d f
  for d in "$GAME_DIR" "$BIN_DIR" "$DXVK_DIR"; do
    for f in "${STALE_FLAGS[@]}"; do
      if [[ -e "$d/$f" ]]; then rm -f -- "$d/$f"; info "Removed leftover $d/$f"; fi
    done
    if [[ -f "$d/bmvr_skip.txt" ]]; then
      if grep -qE "^($FALSE_BANS)\r?$" "$d/bmvr_skip.txt"; then
        sed -i -E "/^($FALSE_BANS)\r?$/d" "$d/bmvr_skip.txt"
        info "Removed false-ban entries from $d/bmvr_skip.txt"
      fi
    fi
  done

  # --- back up stock DXVK once (never back up our own DLL)
  local stock="$DXVK_DIR/d3d9.dll" backup="$DXVK_DIR/d3d9.dll.stock-dxvk"
  if [[ -f "$stock" && ! -f "$backup" ]] && ! grep -qa 'BMVR' "$stock"; then
    cp -f -- "$stock" "$backup"
    info "Backed up stock DXVK to $backup"
  fi
  # back up the game's own openvr_api.dll once (the zip replaces it)
  if [[ -f "$BIN_DIR/openvr_api.dll" && ! -f "$BIN_DIR/openvr_api.dll.bmvr-orig" ]] \
     && [[ -f "$SRC_DIR/openvr_api.dll" ]] \
     && ! cmp -s "$BIN_DIR/openvr_api.dll" "$SRC_DIR/openvr_api.dll"; then
    cp -f -- "$BIN_DIR/openvr_api.dll" "$BIN_DIR/openvr_api.dll.bmvr-orig"
    info "Backed up stock bin/openvr_api.dll"
  fi

  # --- d3d9.dll in all three places the game can load it from
  local dest
  for dest in "$DXVK_DIR" "$BIN_DIR" "$GAME_DIR"; do
    cp -f -- "$SRC_DIR/d3d9.dll" "$dest/d3d9.dll"
    cmp -s "$SRC_DIR/d3d9.dll" "$dest/d3d9.dll" || die "Copy verification failed: $dest/d3d9.dll"
    if [[ -f "$SRC_DIR/openvr_api.dll" ]]; then
      cp -f -- "$SRC_DIR/openvr_api.dll" "$dest/openvr_api.dll"
    fi
    info "Installed $dest/d3d9.dll"
  done

  # --- VR/ folder (keep an existing config.txt)
  local vr_src="$SRC_DIR/VR" vr_dst="$GAME_DIR/VR"
  mkdir -p "$vr_dst"
  if [[ -d "$vr_src/SteamVRActionManifest" ]]; then
    rm -rf -- "$vr_dst/SteamVRActionManifest"
    cp -r -- "$vr_src/SteamVRActionManifest" "$vr_dst/SteamVRActionManifest"
  fi
  local sub
  for sub in hands weapon_wheel openxr_helper64; do
    if [[ -d "$vr_src/$sub" ]]; then
      mkdir -p "$vr_dst/$sub"
      cp -rf -- "$vr_src/$sub/." "$vr_dst/$sub/"
    fi
  done
  [[ -f "$vr_src/viewmodel_offsets.txt" ]] && cp -f -- "$vr_src/viewmodel_offsets.txt" "$vr_dst/viewmodel_offsets.txt"
  if [[ -f "$vr_dst/config.txt" ]]; then
    info "Kept existing $vr_dst/config.txt (not rewritten)"
  elif [[ -f "$vr_src/config.txt" ]]; then
    cp -f -- "$vr_src/config.txt" "$vr_dst/config.txt"
    info "Installed $vr_dst/config.txt"
  fi
  info "Installed VR/ files into $vr_dst"

  # --- optional top-level extras and the game cfg
  [[ -f "$SRC_DIR/BMVR_README.txt" ]] && cp -f -- "$SRC_DIR/BMVR_README.txt" "$GAME_DIR/BMVR_README.txt"
  if [[ -f "$SRC_DIR/bms/cfg/bmvr.cfg" ]]; then
    mkdir -p "$GAME_DIR/bms/cfg"
    cp -f -- "$SRC_DIR/bms/cfg/bmvr.cfg" "$GAME_DIR/bms/cfg/bmvr.cfg"
    info "Installed $GAME_DIR/bms/cfg/bmvr.cfg"
  fi

  # --- stock dxvk.conf overrides the mod's bms.exe profile; set it for VR
  printf 'd3d9.deferSurfaceCreation = True\r\nd3d9.deviceLossOnFocusLoss = False\r\n' > "$DXVK_DIR/dxvk.conf"
  info "Set $DXVK_DIR/dxvk.conf (deviceLossOnFocusLoss = False)"

  # --- sourcevr.dll pops "VR_Shutdown could not be located" with our openvr_api
  local sv="$BIN_DIR/sourcevr.dll" svd="$BIN_DIR/sourcevr.dll.disabled"
  if [[ -f "$sv" ]]; then
    if [[ -f "$svd" ]]; then rm -f -- "$sv"; info "Removed bin/sourcevr.dll (backup already present)"
    else mv -- "$sv" "$svd"; info "Disabled bin/sourcevr.dll -> sourcevr.dll.disabled"; fi
  fi

  info ""
  info "Install complete."
  launch_options
}

# ================================================================= UNINSTALL
do_uninstall() {
  info ""
  info "This removes Black Mesa VR from: $GAME_DIR"
  info "(VR folder, d3d9.dll copies, logs, flags, bmvr.cfg, crowbar assets;"
  info " stock DXVK is restored from backup.)"
  if [[ $ASSUME_YES -ne 1 ]]; then
    local ans; read -r -p "Type YES to continue: " ans || true
    [[ "$ans" == "YES" ]] || { info "Cancelled."; exit 0; }
  fi
  check_not_running

  local removed=0
  rm_target() { if [[ -e "$1" || -L "$1" ]]; then rm -rf -- "$1" && { info "Removed $1"; removed=$((removed+1)); }; fi; }
  rm_glob() {  # rm_glob DIR PATTERN
    local dir="$1" pat="$2" f
    [[ -d "$dir" ]] || return 0
    shopt -s nullglob dotglob
    for f in "$dir"/$pat; do [[ -f "$f" ]] && rm_target "$f"; done
    shopt -u nullglob dotglob
  }

  # restore stock DXVK before deleting the mod copy
  local stock="$DXVK_DIR/d3d9.dll" backup="$DXVK_DIR/d3d9.dll.stock-dxvk" restored=0
  if [[ -f "$backup" ]]; then
    cp -f -- "$backup" "$stock" && restored=1 && info "Restored stock DXVK d3d9.dll from backup"
  fi
  printf 'd3d9.deferSurfaceCreation = True\r\nd3d9.deviceLossOnFocusLoss = True\r\n' > "$DXVK_DIR/dxvk.conf"
  info "Restored stock $DXVK_DIR/dxvk.conf"

  rm_target "$GAME_DIR/d3d9.dll"
  rm_target "$GAME_DIR/openvr_api.dll"
  rm_target "$GAME_DIR/openxr_loader.dll"
  rm_target "$GAME_DIR/BMVR_README.txt"
  rm_target "$GAME_DIR/bms_d3d9.log"
  rm_target "$GAME_DIR/VR"

  local d
  for d in "$GAME_DIR" "$BIN_DIR" "$DXVK_DIR"; do
    rm_glob "$d" 'd3d9.dll.hung*'
    rm_glob "$d" 'd3d9.dll.userbackup-*'
    rm_glob "$d" 'bmvr_*'
    rm_glob "$d" 'bms_d3d9.log'
  done
  rm_target "$DXVK_DIR/d3d9.dll.stock-dxvk"

  rm_target "$BIN_DIR/d3d9.dll"
  rm_target "$BIN_DIR/openxr_loader.dll"
  rm_target "$DXVK_DIR/openvr_api.dll"

  # restore the game's own openvr_api.dll if we backed it up
  if [[ -f "$BIN_DIR/openvr_api.dll.bmvr-orig" ]]; then
    mv -f -- "$BIN_DIR/openvr_api.dll.bmvr-orig" "$BIN_DIR/openvr_api.dll"
    info "Restored stock bin/openvr_api.dll"
  fi
  # restore sourcevr.dll
  if [[ -f "$BIN_DIR/sourcevr.dll.disabled" ]]; then
    if [[ -f "$BIN_DIR/sourcevr.dll" ]]; then
      info "Kept existing bin/sourcevr.dll"
    else
      mv -- "$BIN_DIR/sourcevr.dll.disabled" "$BIN_DIR/sourcevr.dll"
      info "Restored bin/sourcevr.dll"
    fi
  fi

  local g cfg
  for g in bms bshift; do
    cfg="$GAME_DIR/$g/cfg"
    rm_target "$cfg/bmvr.cfg"
    if [[ -f "$cfg/autoexec.cfg" ]] && grep -qE '^[[:space:]]*exec[[:space:]]+bmvr\b' "$cfg/autoexec.cfg"; then
      sed -i -E '/^[[:space:]]*exec[[:space:]]+bmvr\b/d' "$cfg/autoexec.cfg"
      info "Removed 'exec bmvr' from $cfg/autoexec.cfg"
      [[ -s "$cfg/autoexec.cfg" ]] || rm_target "$cfg/autoexec.cfg"
    fi
    rm_glob "$GAME_DIR/$g/models/weapons" 'vr_crowbar.*'
    rm_target "$GAME_DIR/$g/materials/models/weapons/vr_crowbar"
  done

  info ""
  info "Removed $removed item(s)."
  [[ $restored -eq 1 ]] || warn "No stock DXVK backup was found. If Black Mesa misbehaves, use Steam -> Properties -> Installed Files -> Verify integrity."
}

case "$MODE" in
  install)   do_install ;;
  uninstall) do_uninstall ;;
esac
