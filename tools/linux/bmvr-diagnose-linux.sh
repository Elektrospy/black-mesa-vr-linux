#!/usr/bin/env bash
# Black Mesa VR (native Linux shim): "why is there no bmvr_linux_log.txt?"
# Read-only except for a tiny self-test in a temp folder. Writes bmvr-diagnose.txt.
#
# Usage:  tools/linux/bmvr-diagnose-linux.sh ["/path/to/Black Mesa"]
# Best run TWICE: once with the game closed, once while the game is running
# (then it can check the live process directly).

set -uo pipefail
OUT="${BMVR_DIAG_OUT:-bmvr-diagnose.txt}"
GAME="${1:-}"

steam_roots() {
  printf '%s\n' "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" "$HOME/snap/steam/common/.local/share/Steam"
}

find_game() {
  local r vdf p libs=() lib real seen=""
  while IFS= read -r r; do
    [[ -d "$r/steamapps" ]] || continue
    libs+=("$r")
    vdf="$r/steamapps/libraryfolders.vdf"
    [[ -f "$vdf" ]] && while IFS= read -r p; do [[ -d "$p/steamapps" ]] && libs+=("$p"); done \
      < <(sed -nE 's/^[[:space:]]*"path"[[:space:]]+"([^"]*)".*/\1/p' "$vdf")
  done < <(steam_roots)
  for lib in "${libs[@]}"; do
    real="$(readlink -f "$lib" 2>/dev/null || echo "$lib")"
    case "|$seen|" in *"|$real|"*) continue ;; esac; seen="$seen|$real"
    [[ -d "$real/steamapps/common/Black Mesa" ]] && { printf '%s\n' "$real/steamapps/common/Black Mesa"; return 0; }
  done
  return 1
}

# Print the text of a { ... } block that follows the first occurrence of "KEY" (python3, brace matching).
vdf_block() {  # file key
  python3 - "$1" "$2" <<'PYEOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r'"%s"\s*\{' % re.escape(sys.argv[2]), text)
if not m:
    sys.exit(3)
i, depth = m.end(), 1
while i < len(text) and depth:
    depth += (text[i] == "{") - (text[i] == "}")
    i += 1
print(text[m.end():i - 1])
PYEOF
}

{
echo "== bmvr diagnose $(date -u +%FT%TZ)   $(uname -srm)"
[[ -n "$GAME" ]] || GAME="$(find_game || true)"
GAME="${GAME%/}"
echo "game dir: ${GAME:-NOT FOUND}"
[[ -d "$GAME" ]] || { echo "Pass the Black Mesa folder as argument."; exit 0; }
SO="$GAME/bin/bmvr_preload32.so"
[[ -f "$SO" ]] || { [[ -f "$GAME/bmvr_preload32.so" ]] && SO="$GAME/bmvr_preload32.so"; }

echo; echo "== 1. files"
for f in bms_linux bms.sh bms.exe bin/bmvr_preload32.so bmvr-run.sh bmvr-bms.sh bmvr_run_marker.txt bmvr_inner_marker.txt bmvr_preload32.so bmvr_linux_log.txt bmvr_xfer_helper bmvr_xfer.sock; do
  if [[ -e "$GAME/$f" ]]; then printf '  %-22s present  (%s)\n' "$f" "$(stat -c '%y' "$GAME/$f" | cut -d. -f1)"; else printf '  %-22s MISSING\n' "$f"; fi
done

echo; echo "== 2. the shim file"
if [[ -f "$SO" ]]; then
  file -b "$SO"
  echo "  required glibc symbol versions:"
  objdump -T "$SO" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+ +[A-Za-z_0-9]+' | sort -u | sed 's/^/    /'
  echo "  highest: $(objdump -T "$SO" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)"
  echo "    (anything above ~2.4 may be too new for the older glibc inside the Steam Linux Runtime;"
  echo "     such a library is silently skipped by the loader: no log, no error)"
  echo "  undefined symbols it needs from the game:"; nm -D --undefined-only "$SO" 2>/dev/null | awk '{print "    "$2}' | head -20
else
  echo "  shim not found. Run scripts/build-linux.sh \"$GAME\""
fi

echo; echo "== 3. Steam launch options for Black Mesa (app 362890)"
found_lo=0
while IFS= read -r r; do
  for lc in "$r"/userdata/*/config/localconfig.vdf; do
    [[ -f "$lc" ]] || continue
    if command -v python3 >/dev/null; then
      blk="$(vdf_block "$lc" 362890 2>/dev/null)" || continue
      lo="$(printf '%s\n' "$blk" | sed -nE 's/^[[:space:]]*"LaunchOptions"[[:space:]]+"(.*)"[[:space:]]*$/\1/p' | head -1)"
      echo "  $lc"; echo "    LaunchOptions: ${lo:-(empty / not set)}"; found_lo=1
      if [[ "$lo" != *bmvr-run.sh* && "$lo" == *LD_PRELOAD* ]]; then
        echo "    -> LD_PRELOAD in the launch options does not survive: native games run in the Steam Linux"
        echo "       Runtime container (pressure-vessel), which rebuilds LD_PRELOAD. Use the wrapper instead:"
        echo "       \"$GAME/bmvr-run.sh\" %command%"
      elif [[ "$lo" != *bmvr-run.sh* ]]; then
        echo "    -> does not use bmvr-run.sh"
      fi
      if [[ "$lo" == */*bmvr_preload32.so* && "$GAME" == *" "* ]]; then
        echo "    -> PROBLEM: absolute path in LD_PRELOAD and the game folder contains a space."
        echo "       LD_PRELOAD splits on spaces, so the loader ignores it (silently). Use instead:"
        echo "       LD_PRELOAD=\"bmvr_preload32.so:\$LD_PRELOAD\" %command%"
      fi
      [[ "$lo" == *"%command%"* ]] || echo "    -> has no %command% (Steam would append the game after your text, not run it)"
    else
      echo "  (python3 missing; check the launch options by hand)"; break 2
    fi
  done
done < <(steam_roots)
[[ $found_lo -eq 1 ]] || echo "  no LaunchOptions entry found for app 362890 (Steam closed after setting it? Steam writes the file on exit/change)"

echo; echo "== 4. compatibility tool (Proton) for Black Mesa"
cfg_found=0
while IFS= read -r r; do
  cf="$r/config/config.vdf"; [[ -f "$cf" ]] || continue
  command -v python3 >/dev/null || break
  blk="$(vdf_block "$cf" CompatToolMapping 2>/dev/null)" || continue
  cfg_found=1
  m="$(printf '%s\n' "$blk" | python3 -c '
import re,sys
t=sys.stdin.read()
m=re.search(r"\"362890\"\s*\{([^}]*)\}",t)
print(m.group(1).strip().replace("\n"," ") if m else "")')"
  if [[ -n "$m" ]]; then
    echo "  $cf: 362890 -> $m"
    echo "  -> a compatibility tool is forced: Black Mesa starts through Proton (bms.exe) and the NATIVE shim is never loaded."
  else
    echo "  $cf: no compatibility tool set for 362890 (native build runs)"
  fi
done < <(steam_roots)
[[ $cfg_found -eq 1 ]] || echo "  (no CompatToolMapping found: no forced tool, or config.vdf unreadable)"

echo; echo "== 5. any log files already written?"
for d in "$GAME" "$HOME" /tmp; do
  [[ -f "$d/bmvr_linux_log.txt" ]] && { echo "  FOUND $d/bmvr_linux_log.txt"; head -5 "$d/bmvr_linux_log.txt" | sed 's/^/    /'; }
done
find "$HOME" -maxdepth 4 -name bmvr_linux_log.txt -not -path "$GAME/*" 2>/dev/null | head -3 | sed 's/^/  also: /'

echo; echo "== 5a. stage 2a (GPU hand-off test)"
if [[ -f "$GAME/bmvr_linux_log.txt" ]]; then
  grep -E "GL device UUID|xfer:" "$GAME/bmvr_linux_log.txt" | cut -c1-200 | sed 's/^/  /'
  grep -q "xfer:" "$GAME/bmvr_linux_log.txt" || echo "  no xfer lines (helper not started before the game, or an old shim)"
fi
ls "$GAME"/bmvr_snapshot_*.ppm 2>/dev/null | sed 's/^/  snapshot: /'
[[ -S "$GAME/bmvr_xfer.sock" ]] && echo "  helper socket present (helper is running)" || echo "  no helper socket (helper not running)"

echo; echo "== 5b. did the wrapper run?"
if [[ -f "$GAME/bmvr_run_marker.txt" ]]; then
  echo "  yes, the host wrapper ran (launch option applied; this is OUTSIDE the container):"; sed 's/^/    /' "$GAME/bmvr_run_marker.txt"
  grep -q 'swapped bms.sh -> bmvr-bms.sh: 0' "$GAME/bmvr_run_marker.txt" && echo "  -> PROBLEM: no '*/bms.sh' argument was found to swap, so LD_PRELOAD is never set. Send the args line."
else
  echo "  no bmvr_run_marker.txt: the wrapper has not run since it was installed (launch option not set, set while"
  echo "  the game was already running, or Steam was not restarted/closed after changing it)."
fi

echo; echo "== 5c. did the container-side launcher run?"
if [[ -f "$GAME/bmvr_inner_marker.txt" ]]; then
  echo "  yes (bmvr-bms.sh ran inside the container):"; sed 's/^/    /' "$GAME/bmvr_inner_marker.txt"
else
  echo "  no bmvr_inner_marker.txt: bms.sh was not replaced by bmvr-bms.sh (see 5b), or the old wrapper is still installed: re-run scripts/build-linux.sh"
fi

echo; echo "== 6. self-test: does the shim load into a plain 32-bit process on THIS system?"
if [[ -f "$SO" ]] && command -v gcc >/dev/null; then
  T="$(mktemp -d)"
  printf 'int main(void){return 0;}\n' > "$T/t.c"
  if gcc -m32 "$T/t.c" -o "$T/t32" 2>/dev/null; then
    echo "  (a) bare name + LD_LIBRARY_PATH=<game>/bin   (how bms.sh makes it work):"
    ( cd "$T" && LD_LIBRARY_PATH="$(dirname "$SO")" LD_PRELOAD="$(basename "$SO")" ./t32 ) 2> "$T/stderr.txt"
    if [[ -f "$T/bmvr_linux_log.txt" ]]; then echo "      PASS: loaded and wrote $(wc -l < "$T/bmvr_linux_log.txt") log lines"
    else echo "      FAIL. loader output:"; sed 's/^/        /' "$T/stderr.txt" | head -6; fi
    rm -f "$T/bmvr_linux_log.txt"
    echo "  (b) absolute path   LD_PRELOAD=$SO"
    ( cd "$T" && LD_PRELOAD="$SO" ./t32 ) 2> "$T/stderr2.txt"
    if [[ -f "$T/bmvr_linux_log.txt" ]]; then echo "      PASS"
    else echo "      FAIL (expected when the path contains a space). loader output:"; sed 's/^/        /' "$T/stderr2.txt" | head -3; fi
  else echo "  skipped: gcc -m32 does not work here"; fi
  rm -rf "$T"
else echo "  skipped (no shim file or no gcc)"; fi

echo; echo "== 7. is the game running right now? (definitive check)"
pids="$(pgrep -x bms_linux 2>/dev/null || true)"
if [[ -z "$pids" ]]; then
  echo "  no bms_linux process. Start the game, reach the main menu, then run this script again."
  echo "  (If you start the game and it shows up as 'bms.exe' instead, it is running through Proton.)"
  pgrep -a -x 'bms.exe' 2>/dev/null | head -2 | sed 's/^/  /'
else
  for pid in $pids; do
    echo "  bms_linux pid $pid"
    if grep -q bmvr_preload32 "/proc/$pid/maps" 2>/dev/null; then echo "  -> shim IS mapped into the process"
    else echo "  -> shim is NOT mapped into the process"; fi
    echo "  LD_PRELOAD in its environment: $(tr '\0' '\n' < /proc/$pid/environ 2>/dev/null | sed -n 's/^LD_PRELOAD=//p' | head -1)"
    echo "  process ancestry:"
    cur=$pid; for _ in 1 2 3 4 5 6 7 8; do
      [[ -r /proc/$cur/stat ]] || break
      printf '    %s: %s\n' "$cur" "$(tr '\0' ' ' < /proc/$cur/cmdline 2>/dev/null | cut -c1-150)"
      cur="$(awk '{print $4}' /proc/$cur/stat 2>/dev/null)"; [[ -n "$cur" && "$cur" -gt 1 ]] || break
    done
  done
fi
} > "$OUT" 2>&1

echo "Wrote $OUT. Please send it."
