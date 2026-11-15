#!/usr/bin/env bash
# Black Mesa VR - Linux (Proton) installer / uninstaller.
#
# Port of the Windows install flow (README steps + scripts/install.ps1 and
# UNINSTALL-BMVR.ps1) for the release zip layout ("drop-into-Black-Mesa-folder").
#
# Usage:
#   ./bmvr-linux.sh [install]  [--game-dir DIR] [--src DIR] [--force]
#                              [--separate [--name NAME] [--steam-shortcut [--steam-user ID]]]
#                              [--campaign bms|bshift|asheep]...
#   ./bmvr-linux.sh uninstall  [--game-dir DIR] [--yes] [--force]
#   ./bmvr-linux.sh help
#
# Options:
#   --separate         Install into a SEPARATE copy of the game (copy-on-write when the
#                      filesystem allows it) next to the original, named "Black Mesa VR"
#                      by default (--name to change). The original game is not modified,
#                      so the normal and the VR version can be started independently.
#   --steam-shortcut   With --separate: add a Steam library entry (non-Steam shortcut) for
#                      the copy, one per --campaign. Steam must be fully closed. Needs python3.
#   --campaign C       bms (default), bshift (Blue Shift) or asheep (Azure Sheep); repeatable.
#                      Selects the launch options printed / written to the shortcut.
#   --steam-user ID    Steam account folder (userdata/ID) when several exist.
#
# To remove a separate copy: uninstall --game-dir "<copy>", delete the folder, and
# remove the shortcut in Steam.
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
SEPARATE=0
COPY_NAME="Black Mesa VR"
WANT_SHORTCUT=0
STEAM_USER=""
SHORTCUT_VDF=""
CAMPAIGNS=()

die()  { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }

usage() { sed -n '2,/^set -euo pipefail/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

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
    --separate) SEPARATE=1; shift ;;
    --name)     [[ $# -ge 2 ]] || die "--name needs a value"; COPY_NAME="$2"; shift 2 ;;
    --steam-shortcut) WANT_SHORTCUT=1; shift ;;
    --steam-user) [[ $# -ge 2 ]] || die "--steam-user needs an id"; STEAM_USER="$2"; shift 2 ;;
    --campaign) [[ $# -ge 2 ]] || die "--campaign needs bms, bshift or asheep"; CAMPAIGNS+=("$2"); shift 2 ;;
    --force)    FORCE=1; shift ;;
    --yes|-y)   ASSUME_YES=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    *)          die "Unknown argument: $1 (try --help)" ;;
  esac
done

# campaign list: validate, dedupe, default to bms
_c=(); for _x in ${CAMPAIGNS[@]+"${CAMPAIGNS[@]}"}; do
  case "$_x" in bms|bshift|asheep) ;; *) die "Unknown campaign '$_x' (use bms, bshift or asheep)" ;; esac
  [[ " ${_c[*]-} " == *" $_x "* ]] || _c+=("$_x")
done
[[ ${#_c[@]} -gt 0 ]] || _c=(bms)
CAMPAIGNS=("${_c[@]}")
if [[ $WANT_SHORTCUT -eq 1 && $SEPARATE -ne 1 ]]; then
  die "--steam-shortcut needs --separate. A shortcut to the ORIGINAL folder would not be independent:
the mod replaces files inside the game folder."
fi
if [[ -z "$COPY_NAME" || "$COPY_NAME" == */* || "$COPY_NAME" == "." || "$COPY_NAME" == ".." ]]; then
  die "--name must be a plain folder name (no slashes)."
fi

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

# ============================================================ campaigns / launch
BASE_ARGS='-heapsize 524288 -processheap -high -novid -windowed -oldgameui -enabledxvk'

camp_args()  { case "$1" in bms) printf '' ;; bshift) printf -- '-game bshift' ;; asheep) printf -- '-game asheep' ;; esac; }
camp_title() { case "$1" in bms) printf 'Black Mesa' ;; bshift) printf 'Black Mesa: Blue Shift' ;; asheep) printf 'Azure Sheep' ;; esac; }
camp_shortcut_name() {
  case "$1" in
    bms)    printf '%s' "$COPY_NAME" ;;
    bshift) printf '%s - Blue Shift' "$COPY_NAME" ;;
    asheep) printf '%s - Azure Sheep' "$COPY_NAME" ;;
  esac
}
launch_string() {
  local extra; extra="$(camp_args "$1")"
  printf 'WINEDLLOVERRIDES="d3d9=n,b" %%command%% %s%s' "$BASE_ARGS" "${extra:+ $extra}"
}

launch_options() {
  local c
  echo ""
  if [[ $SEPARATE -eq 1 ]]; then
    echo "Launch options for the separate copy (already set if you used --steam-shortcut):"
  else
    echo "Steam -> Black Mesa -> Properties -> Launch Options:"
    echo "(That entry has ONE launch-options field. A '-game ...' string makes it start that"
    echo " campaign every time; use --separate to get dedicated entries instead.)"
  fi
  for c in "${CAMPAIGNS[@]}"; do
    echo ""
    echo "  $(camp_title "$c"):"
    echo "  $(launch_string "$c")"
  done
  cat <<'EOF'

Notes:
  * -oldgameui is required (the new game UI is upside-down in the headset).
  * -enabledxvk makes the game load the DXVK folder copy of d3d9.dll.
  * Blue Shift needs to be installed. The mod reads bms/cfg/bmvr.cfg for every
    campaign, so the one config file this installer writes covers Blue Shift too.
  * Make sure your OpenXR runtime is active on the host (e.g. SteamVR set as
    the OpenXR runtime) before launching the game from Steam.
  * Success check: bmvr_log.txt appears next to bms.exe and starts with
    "BMVR d3d9.dll loaded". Send that log (and the helper log) if it fails.
EOF
}

# ================================================= separate copy + Steam shortcut
steam_roots() {
  printf '%s\n' "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"
}

steam_is_running() { pgrep -x steam >/dev/null 2>&1 || pgrep -x steamwebhelper >/dev/null 2>&1; }

# Sets SHORTCUT_VDF to <steam>/userdata/<id>/config/shortcuts.vdf
resolve_shortcuts_vdf() {
  local r d id real seen="" found=() entry
  while IFS= read -r r; do
    [[ -d "$r/userdata" ]] || continue
    for d in "$r"/userdata/*/; do
      [[ -d "$d" ]] || continue
      id="$(basename "$d")"
      [[ "$id" =~ ^[0-9]+$ && "$id" != "0" ]] || continue
      real="$(readlink -f "$d")"
      case "|$seen|" in *"|$real|"*) continue ;; esac
      seen="$seen|$real"
      found+=("$id|${d%/}")
    done
  done < <(steam_roots)
  [[ ${#found[@]} -gt 0 ]] || die "No Steam user data found (userdata/<id>). Start Steam once and log in, then retry."
  local pick=""
  if [[ -n "$STEAM_USER" ]]; then
    for entry in "${found[@]}"; do [[ "${entry%%|*}" == "$STEAM_USER" ]] && pick="${entry#*|}"; done
    [[ -n "$pick" ]] || die "No Steam user folder with id $STEAM_USER."
  elif [[ ${#found[@]} -eq 1 ]]; then
    pick="${found[0]#*|}"
  else
    echo "Several Steam accounts found:" >&2
    for entry in "${found[@]}"; do echo "  --steam-user ${entry%%|*}    (${entry#*|})" >&2; done
    die "Pick one with --steam-user ID."
  fi
  SHORTCUT_VDF="$pick/config/shortcuts.vdf"
}

preflight_shortcut() {
  command -v python3 >/dev/null 2>&1 || die "--steam-shortcut needs python3."
  if [[ $FORCE -ne 1 ]] && steam_is_running; then
    die "Steam is running. Fully quit Steam first (it rewrites shortcuts.vdf when it exits),
then run this again. --force skips this check (risky)."
  fi
  resolve_shortcuts_vdf
  info "Steam shortcuts file: $SHORTCUT_VDF"
}

# Copy-on-write when the filesystem allows it; never hardlinks (a hardlinked file
# would be changed in the original when the installer overwrites it).
make_separate_copy() {
  local orig="$GAME_DIR" parent dest
  parent="$(dirname "$orig")"
  dest="$parent/$COPY_NAME"
  [[ "$dest" != "$orig" ]] || die "--name must differ from the original folder name ($(basename "$orig"))."
  if [[ -f "$dest/bms.exe" ]]; then
    info "Reusing existing copy: $dest"
  else
    [[ ! -e "$dest" ]] || die "$dest exists but is not a Black Mesa copy (no bms.exe). Choose another --name."
    local need_kb avail_kb reflink=0 probe="$parent/.bmvr_reflink_probe.$$"
    need_kb="$(du -sk "$orig" | awk '{print $1}')"
    avail_kb="$(df -Pk "$parent" | awk 'NR==2 {print $4}')"
    if cp --reflink=always -- "$orig/bms.exe" "$probe" 2>/dev/null; then reflink=1; fi
    rm -f -- "$probe"
    if [[ $reflink -eq 1 ]]; then
      info "Filesystem supports copy-on-write: the copy shares disk space with the original."
    else
      info "No copy-on-write here: the copy needs about $((need_kb / 1024)) MiB."
      if (( avail_kb < need_kb + need_kb / 20 )); then
        die "Not enough free space in $parent ($((avail_kb / 1024)) MiB free, ~$((need_kb / 1024)) MiB needed)."
      fi
    fi
    info "Copying game files to: $dest   (this can take a while)"
    mkdir -p -- "$dest"
    if ! cp -a --reflink=auto -- "$orig/." "$dest/"; then
      rm -rf -- "$dest"
      die "Copy failed; removed the partial copy. The original was not changed."
    fi
  fi
  GAME_DIR="$dest"
  BIN_DIR="$GAME_DIR/bin"
  DXVK_DIR="$GAME_DIR/bin/thirdparty/dxvk-windows-x86"
  [[ -f "$GAME_DIR/steam_appid.txt" ]] || printf '362890\n' > "$GAME_DIR/steam_appid.txt"
  info "Separate copy: $GAME_DIR   (original untouched: $orig)"
}

# Writes non-Steam shortcuts (one per campaign) into shortcuts.vdf. Never modifies a
# file it cannot rewrite byte-for-byte; keeps a timestamped backup.
add_steam_shortcuts() {
  local args=() c
  for c in "${CAMPAIGNS[@]}"; do
    args+=("$(camp_shortcut_name "$c")" "$(launch_string "$c")")
  done
  local rc=0
  python3 - "$SHORTCUT_VDF" "$GAME_DIR/bms.exe" "$GAME_DIR/" "${args[@]}" <<'PYEOF' || rc=$?
import os, shutil, struct, sys, time, zlib

vdf_path, exe, startdir = sys.argv[1:4]
rest = sys.argv[4:]
if len(rest) % 2:
    sys.exit("bad arguments")
wanted = [(rest[i], rest[i + 1]) for i in range(0, len(rest), 2)]
qexe, qstart = '"%s"' % exe, '"%s"' % startdir


def parse(data):
    pos = 0

    def cstr():
        nonlocal pos
        end = data.index(b"\x00", pos)
        s = data[pos:end].decode("utf-8", "surrogateescape")
        pos = end + 1
        return s

    def obj():
        nonlocal pos
        out = []
        while True:
            t = data[pos]
            pos += 1
            if t == 8:
                return out
            key = cstr()
            if t == 0:
                out.append([key, 0, obj()])
            elif t == 1:
                out.append([key, 1, cstr()])
            elif t == 2:
                raw = bytes(data[pos:pos + 4])
                if len(raw) != 4:
                    raise ValueError("truncated integer")
                out.append([key, 2, raw])
                pos += 4
            else:
                raise ValueError("unsupported VDF value type %d" % t)

    root = obj()
    if pos != len(data):
        raise ValueError("trailing data")
    return root


def dump(items):
    out = bytearray()
    for key, t, val in items:
        out.append(t)
        out += key.encode("utf-8", "surrogateescape") + b"\x00"
        if t == 0:
            out += dump(val)
        elif t == 1:
            out += val.encode("utf-8", "surrogateescape") + b"\x00"
        else:
            out += val
    out.append(8)
    return bytes(out)


def i32(v):
    return struct.pack("<I", v & 0xFFFFFFFF)


def field(entry, name):
    for f in entry:
        if f[0].lower() == name.lower():
            return f
    return None


def setf(entry, name, t, val):
    f = field(entry, name)
    if f:
        f[1], f[2] = t, val
    else:
        entry.append([name, t, val])


data = open(vdf_path, "rb").read() if os.path.exists(vdf_path) else b"\x00shortcuts\x00\x08\x08"
try:
    root = parse(data)
    if dump(root) != data:
        raise ValueError("file does not round-trip exactly")
    if len(root) != 1 or root[0][0].lower() != "shortcuts" or root[0][1] != 0:
        raise ValueError("unexpected top-level structure")
except Exception as e:
    sys.exit("shortcuts.vdf NOT modified (%s). Add the entry in Steam instead." % e)

shortcuts = root[0][2]
changed = False
for name, launch in wanted:
    existing = None
    for key, t, entry in shortcuts:
        if t != 0:
            continue
        n, x = field(entry, "appname"), field(entry, "exe")
        if n and x and n[2] == name and x[2] == qexe:
            existing = entry
            break
    app = (zlib.crc32((qexe + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000
    if existing is not None:
        before = dump(existing)
        setf(existing, "StartDir", 1, qstart)
        setf(existing, "LaunchOptions", 1, launch)
        if dump(existing) != before:
            changed = True
            print("UPDATED  %s" % name)
        else:
            print("UNCHANGED %s" % name)
        continue
    idx = max([int(k) for k, t, _ in shortcuts if k.isdigit()] + [-1]) + 1
    shortcuts.append([str(idx), 0, [
        ["appid", 2, i32(app)], ["AppName", 1, name], ["Exe", 1, qexe], ["StartDir", 1, qstart],
        ["icon", 1, ""], ["ShortcutPath", 1, ""], ["LaunchOptions", 1, launch],
        ["IsHidden", 2, i32(0)], ["AllowDesktopConfig", 2, i32(1)], ["AllowOverlay", 2, i32(1)],
        ["OpenVR", 2, i32(0)], ["Devkit", 2, i32(0)], ["DevkitGameID", 1, ""],
        ["DevkitOverrideAppID", 2, i32(0)], ["LastPlayTime", 2, i32(0)], ["FlatpakAppID", 1, ""],
        ["tags", 0, []],
    ]])
    changed = True
    print("ADDED    %s" % name)

if changed:
    out = dump(root)
    if dump(parse(out)) != out:
        sys.exit("internal check failed; shortcuts.vdf NOT modified")
    os.makedirs(os.path.dirname(vdf_path), exist_ok=True)
    if os.path.exists(vdf_path):
        bak = vdf_path + ".bmvr-backup-" + time.strftime("%Y%m%d-%H%M%S")
        shutil.copy2(vdf_path, bak)
        print("Backup:  %s" % bak)
    tmp = vdf_path + ".bmvr-tmp"
    with open(tmp, "wb") as fh:
        fh.write(out)
    os.replace(tmp, vdf_path)
PYEOF
  if (( rc != 0 )); then
    warn "The Steam shortcut was not written (the game copy and the mod install are fine)."
    cat <<EOF

Add it by hand instead:
  Steam -> Library -> Add a Game -> Add a Non-Steam Game -> Browse -> $GAME_DIR/bms.exe
  Then Properties: set the launch options printed below and, under Compatibility, force Proton.
EOF
    return 0
  fi
  cat <<EOF

Steam shortcut(s) written. Next:
  1. Start Steam. The new entries are under your Library (non-Steam games).
  2. For each one: Properties -> Compatibility -> "Force the use of a specific Steam Play
     compatibility tool" -> pick Proton. (Non-Steam shortcuts do not get Proton by default.)
  3. The copy has its own saves and settings (bms/SAVE, bms/cfg). Steam updates only the
     ORIGINAL; to refresh the copy after a game update, delete it and run this again
     (cheap on a copy-on-write filesystem).
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

  [[ $WANT_SHORTCUT -eq 1 ]] && preflight_shortcut
  check_not_running
  [[ $SEPARATE -eq 1 ]] && make_separate_copy

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
  [[ $SEPARATE -eq 1 ]] && info "Installed into the separate copy only: $GAME_DIR"
  [[ $WANT_SHORTCUT -eq 1 ]] && add_steam_shortcuts
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
