#!/usr/bin/env bash
# Kill-loop test for the Linux shim (stage 3b): kill -9 the xfer helper over and over while the game runs,
# and check that the shim notices each time and reconnects to the next helper.
#
# Written for bash, run from ANY shell (fish, zsh, bash, Konsole or not). Do not paste it into fish and do
# not `source` it from fish - run it as a program:
#     ./test_kill_loop.sh                 (fish and bash both honour the shebang)
#     bash test_kill_loop.sh -n 10
#
# Options:  -n N        cycles (default 30)
#           -g DIR      game folder (default: ~/.local/share/Steam/steamapps/common/Black Mesa, or $BMVR_GAME)
#           --eyes 2    start the helper with two shared images (stage 3d). The game side needs `target2=...` in
#                       bmvr_target.txt (see docs/linux/eye-pairs.md); a cycle only passes when the shim
#                       actually imported both eyes, so a missing target2 is reported instead of silently testing one eye
#           --no-game-check   do not require a running bms_linux (for the stand-in tests only)
#           -h          help
#
# Start Black Mesa from Steam first (with the bmvr-run.sh launch option) and get past the startup, ideally
# into a map. Do NOT start the helper yourself: this script starts and kills its own.
# The script reads "$GAME/bmvr_linux_log.txt" (written by the shim) to decide PASS/FAIL per cycle.
# If a cycle makes the game's GPU hang (the semaphore risk in docs/linux/gpu-sync.md), the shim stops logging and
# the cycle is reported as FAIL "shim did not notice"; restart the game afterwards.

set -u

GAME="${BMVR_GAME:-$HOME/.local/share/Steam/steamapps/common/Black Mesa}"
CYCLES=30
GAME_CHECK=1
EYES=1

while [ $# -gt 0 ]; do
  case "$1" in
    -n) CYCLES="${2:?-n needs a number}"; shift 2 ;;
    -g) GAME="${2:?-g needs a folder}"; shift 2 ;;
    --eyes) EYES="${2:?--eyes needs 1 or 2}"; shift 2 ;;
    --no-game-check) GAME_CHECK=0; shift ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try -h)" >&2; exit 2 ;;
  esac
done

HELPER="${BMVR_HELPER:-$GAME/bmvr_xfer_helper}"
LOG="$GAME/bmvr_linux_log.txt"
OUT="$(mktemp -d)"
H=""

[ -x "$HELPER" ] || { echo "no executable helper at: $HELPER" >&2; exit 2; }
[ -f "$LOG" ]    || { echo "no shim log at: $LOG (has the game been started with the bmvr-run.sh launch option?)" >&2; exit 2; }
case "$EYES" in 1|2) ;; *) echo "--eyes must be 1 or 2" >&2; exit 2 ;; esac
case "$CYCLES" in ''|*[!0-9]*) echo "-n must be a whole number" >&2; exit 2 ;; esac

game_alive() { [ "$GAME_CHECK" -eq 0 ] || pgrep -x bms_linux >/dev/null 2>&1; }
game_alive || { echo "bms_linux is not running; start the game from Steam first" >&2; exit 2; }

# A second helper would unlink() the first one's socket and bind its own, while the game stays connected to the
# first one: the test would then never see a connection. So there must be no helper running at the start.
# (pgrep -x cannot be used: the process name "bmvr_xfer_helper" is 16 chars, the kernel keeps 15.)
others="$(pgrep -f 'bmvr_xfer_helper' 2>/dev/null | tr '\n' ' ')"
if [ -n "$others" ]; then
  echo "a bmvr_xfer_helper is already running (pid $others)." >&2
  echo "Stop it first (Ctrl-C in the terminal where you started it, or: pkill -f bmvr_xfer_helper)." >&2
  echo "The game may stay connected to it; if the log keeps showing snapshots afterwards, the shim has not noticed" >&2
  echo "yet - wait a few seconds, or restart the game, and run this again." >&2
  exit 2
fi

cleanup() {
  if [ -n "$H" ] && kill -0 "$H" 2>/dev/null; then kill -9 "$H" 2>/dev/null; wait "$H" 2>/dev/null; fi
}
trap cleanup EXIT
trap 'echo; echo "interrupted"; exit 130' INT TERM

# lines of the log written after line number $1
new_lines() { tail -n +"$(( $1 + 1 ))" "$LOG" 2>/dev/null; }
# wait up to $3 seconds for the regex $2 in log lines after line $1
wait_for() {
  local from="$1" re="$2" max="$3" i=0
  while [ "$i" -lt $(( max * 5 )) ]; do
    new_lines "$from" | grep -Eq "$re" && return 0
    sleep 0.2; i=$(( i + 1 ))
  done
  return 1
}

pass=0; fail=0
echo "kill loop: $CYCLES cycles, helper=$HELPER, eyes=$EYES"
echo "log: $LOG"

n=1
while [ "$n" -le "$CYCLES" ]; do
  if ! game_alive; then echo "cycle $n: the game is gone (crash?) - stopping"; fail=$(( fail + 1 )); break; fi

  start="$(wc -l < "$LOG")"
  "$HELPER" "$GAME" --eyes "$EYES" >"$OUT/helper_$n.out" 2>&1 &
  H=$!

  result="PASS"; why=""
  if ! wait_for "$start" 'xfer: helper connected' 45; then
    result="FAIL"; why="shim never connected to the new helper (45 s)"
  elif [ "$EYES" -eq 2 ] && ! wait_for "$start" 'imported the helper.s Vulkan memory as GL textures' 5; then
    result="FAIL"; why="--eyes 2, but the shim imported one image only (target2 missing in bmvr_target.txt, or an old shim?)"
    kill -9 "$H" 2>/dev/null; wait "$H" 2>/dev/null
  else
    sleep $(( RANDOM % 6 ))          # let it deliver a few frames; kill at varying moments
    kill -9 "$H" 2>/dev/null         # no cleanup, like a crash
    wait "$H" 2>/dev/null
    mark="$(wc -l < "$LOG")"
    if ! wait_for "$start" 'lost the helper|helper.s socket is closed|back to searching' 12; then
      result="FAIL"; why="shim did not notice the dead helper (12 s) - game GPU hung? restart the game"
    fi
  fi
  H=""

  if [ "$result" = "PASS" ]; then pass=$(( pass + 1 )); else fail=$(( fail + 1 )); fi
  printf 'cycle %2d/%d: %s %s\n' "$n" "$CYCLES" "$result" "$why"
  if [ "$result" = "FAIL" ]; then
    echo "  --- last shim log lines ---"; tail -n 8 "$LOG" | sed 's/^/  /'
    echo "  --- helper output ---";       tail -n 5 "$OUT/helper_$n.out" | sed 's/^/  /'
    break
  fi
  sleep 2
  n=$(( n + 1 ))
done

echo
echo "result: $pass passed, $fail failed (helper outputs kept in $OUT)"
[ "$fail" -eq 0 ]
