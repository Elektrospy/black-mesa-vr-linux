#!/usr/bin/env bash
# Stage 4a, milestone X2: `bmvr_xfer_helper --sink xr` against a headless Monado, with a Vulkan stand-in for the game.
#
#   tests/linux/test_xr_sink.sh [path/to/build_dir] [--software] [--kill-rounds N]
#
# NOT part of run-tests.sh (needs Monado + an OpenXR loader). Same setup as test_xr_selftest.sh. build_dir holds bmvr_xfer_helper and
# xr_client (cc -O2 -o xr_client tests/linux/xr_client.c -lvulkan).
#
#   X2-a  runtime unavailable                  -> exit 2, says so, NO snapshot files, no socket left behind
#   X2-b  1 eye, 120 frames                    -> every frame acked; --xr-check: pixels of every stage carry the message's tick
#   X2-c  2 eyes                               -> both eyes of every checked pair carry the SAME tick (no split pair)
#   X2-d  game leaves, helper keeps running    -> rate lines continue with displayed > 0 and pairs = 0 (last image re-shown)
#   X2-e  second helper in the same folder     -> refused (one helper per game folder)
#   X2-f  SIGINT                               -> helper exits by itself, "session ends cleanly"-style shutdown, no crash
#   X2-g  kill -9 loop (N rounds)              -> the game process never hangs, reconnects after each kill, ticks keep counting
#
# What this does NOT show: the semaphore path (lavapipe has no semaphore export), the picture in a headset (Monado's null compositor
# shows nothing), SteamVR, GL import in the real shim, or the game. See docs/linux/openxr-results.md "X2 results".
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build_linux_native"; SOFT=0; ROUNDS=10
while [[ $# -gt 0 ]]; do case "$1" in --software) SOFT=1 ;; --kill-rounds) shift; ROUNDS="$1" ;; *) BUILD="$1" ;; esac; shift; done
BUILD="$(cd "$BUILD" && pwd)"; HELPER="$BUILD/bmvr_xfer_helper"; CLIENT="$BUILD/xr_client"
[[ -x "$HELPER" && -x "$CLIENT" ]] || { echo "need $HELPER and $CLIENT"; exit 2; }
command -v monado-service >/dev/null || { echo "monado-service not installed"; exit 2; }
if [[ $SOFT -eq 1 ]]; then
  ICD="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)"; [[ -n "$ICD" ]] || { echo "lavapipe ICD not found"; exit 2; }
  export VK_ICD_FILENAMES="$ICD"
fi
SOCK_DIRS=("${XDG_RUNTIME_DIR:-/nonexistent}" /tmp)
monado_socket() { for d in "${SOCK_DIRS[@]}"; do [[ -S "$d/monado_comp_ipc" ]] && { echo "$d/monado_comp_ipc"; return 0; }; done; return 1; }
if s="$(monado_socket)"; then echo "A Monado service is already running ($s). Stop it first; this test starts its own."; exit 2; fi
PASS=0; FAIL=0
ok()  { echo "  PASS  $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
T="$(mktemp -d /tmp/bmvr_x2.XXXXXX)"; MPID=""; HPID=""
cleanup() { [[ -n "$HPID" ]] && kill -9 "$HPID" 2>/dev/null; [[ -n "$MPID" ]] && kill "$MPID" 2>/dev/null && wait "$MPID" 2>/dev/null; exec 9>&- 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
G="$T/game"; mkdir -p "$G"; SOCK="$G/bmvr_xfer.sock"
XRSZ="256x256"

start_helper() { # $1 = log, rest = extra args. Sets HPID. Waits for the socket.
  local log="$T/$1.log"; shift
  ( cd "$G" && exec "$HELPER" "$G" --sink xr --xr-size "$XRSZ" --xr-wait 8000 "$@" </dev/null >"$log" 2>&1 ) &
  HPID=$!
  for _ in $(seq 1 150); do [[ -S "$SOCK" ]] && return 0; kill -0 "$HPID" 2>/dev/null || return 1; sleep 0.1; done; return 1
}
stop_helper() { kill -INT "$HPID" 2>/dev/null; for _ in $(seq 1 150); do kill -0 "$HPID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$HPID" 2>/dev/null; then kill -9 "$HPID"; wait "$HPID" 2>/dev/null; return 99; fi; wait "$HPID"; return $?; }

echo "X2-a: runtime unavailable"
( cd "$G" && XR_RUNTIME_JSON=/nonexistent.json "$HELPER" "$G" --sink xr --xr-wait 0 </dev/null >"$T/a.log" 2>&1 ); rc=$?
[[ $rc == 2 ]] && ok "exit 2 (UNAVAILABLE)" || bad "expected exit 2, got $rc"
grep -q 'refusing to fall back to snapshots' "$T/a.log" && ok "says it will not fall back to snapshots" || bad "no refusal message"
[[ -z "$(ls -A "$G")" || "$(ls "$G")" == "bmvr_xfer.lock" ]] && ok "no snapshot files, no socket left behind" || bad "files in game folder: $(ls "$G")"

echo "starting a headless Monado (simulated HMD, null compositor)"
mkfifo "$T/stdin.fifo"; exec 9<>"$T/stdin.fifo"
SIMULATED_ENABLE=1 XRT_COMPOSITOR_NULL=1 XRT_COMPOSITOR_FORCE_GPU_INDEX=0 monado-service <"$T/stdin.fifo" >"$T/monado.log" 2>&1 &
MPID=$!
for _ in $(seq 1 100); do monado_socket >/dev/null && break; kill -0 "$MPID" 2>/dev/null || break; sleep 0.1; done
monado_socket >/dev/null || { echo "  Monado did not come up:"; tail -15 "$T/monado.log" | sed 's/^/    /'; exit 2; }

# decode the check lines: "check: pair tick T ...: swapchain px0=AABBCCDD ... ring eye0 px0=... ring eye1 px0=..."
check_ticks() { # $1 log -> prints "n_lines n_bad": bad = a stage whose pixel tick differs from the message tick, or eyes disagree
  python3 - "$1" <<'PY'
import re,sys
n=bad=0
for l in open(sys.argv[1]):
    m=re.search(r'check: pair tick (\d+) ',l)
    if not m: continue
    t=int(m.group(1)); n+=1
    stages=re.findall(r'(swapchain|ring eye\d) px0=([0-9a-f]{8})',l)
    ok=len(stages)>0
    for name,hx in stages:
        v=int(hx[0:2],16)|(int(hx[2:4],16)<<8)
        if v!=(t&0xffff): ok=False
    if not ok: bad+=1
print(n,bad)
PY
}

echo "X2-b: one eye, 120+ frames"
start_helper b --eyes 1 --xr-check 6 || bad "helper did not start: $(tail -3 "$T/b.log")"
out="$("$CLIENT" "$SOCK" --seconds 4 --fps 40 --max-eyes 1)"; crc=$?
echo "        $out"
[[ $crc == 0 ]] && ok "client finished without a hang" || bad "client exit $crc"
sent="$(sed -n 's/.*sent=\([0-9]*\).*/\1/p' <<<"$out")"; acked="$(sed -n 's/.*acked=\([0-9]*\).*/\1/p' <<<"$out")"
[[ "${sent:-0}" -ge 120 && "$sent" == "$acked" ]] && ok "every frame acked ($acked)" || bad "sent=$sent acked=$acked"
sleep 1
read -r n nbad < <(check_ticks "$T/b.log")
[[ "$n" -ge 3 && "$nbad" == 0 ]] && ok "pixels at every stage carry the message's tick ($n checked pairs)" || bad "check lines: $n, inconsistent: $nbad"
grep -q 'swapchain px0=' "$T/b.log" && ok "the swapchain image itself was read back" || echo "        note: swapchain readback n/a on this runtime"
grep -E 'check:' "$T/b.log" | head -2 | cut -c1-230 | sed 's/^/        /'
echo "X2-d: the game left; the helper keeps showing the last pair"
sleep 6
grep -E 'xr-sink: pairs=0\.0/s displayed=[1-9]' "$T/b.log" >/dev/null && ok "rate line with pairs=0 and displayed>0 after the game left" || bad "no 'pairs=0 displayed>0' rate line"
grep -E 'xr-sink:' "$T/b.log" | tail -2 | sed 's/^/        /'
echo "X2-e: a second helper in the same folder"
( cd "$G" && timeout 20 "$HELPER" "$G" --sink xr --xr-size "$XRSZ" </dev/null >"$T/e.log" 2>&1 ); rc=$?
[[ $rc != 0 ]] && grep -qi 'already\|lock' "$T/e.log" && ok "refused (exit $rc)" || bad "second helper not refused (exit $rc): $(tail -2 "$T/e.log")"
echo "X2-f: SIGINT"
stop_helper; rc=$?
[[ $rc == 0 ]] && ok "helper exited 0 on SIGINT" || bad "exit $rc after SIGINT"
grep -q 'session state -> STOPPING' "$T/b.log" && ok "session went through STOPPING" || bad "no STOPPING in the log"
[[ ! -e "$SOCK" ]] && ok "socket removed" || bad "socket left behind"

echo "X2-c: two eyes"
start_helper c --eyes 2 --xr-check 6 || bad "helper did not start: $(tail -3 "$T/c.log")"
out="$("$CLIENT" "$SOCK" --seconds 4 --fps 40)"; crc=$?
echo "        $out"
[[ $crc == 0 && "$out" == *"eyes=2"* ]] && ok "two eyes negotiated, no hang" || bad "client: $out (exit $crc)"
sleep 1
read -r n nbad < <(check_ticks "$T/c.log")
[[ "$n" -ge 3 && "$nbad" == 0 ]] && ok "eye 0 and eye 1 carry the same tick in every checked pair ($n pairs)" || bad "check lines: $n, inconsistent: $nbad"
grep -E 'check:' "$T/c.log" | head -1 | cut -c1-260 | sed 's/^/        /'
stop_helper >/dev/null

echo "X2-g: kill -9 the helper $ROUNDS times while the game runs"
start_helper g --eyes 1 || bad "helper did not start"
"$CLIENT" "$SOCK" --seconds $((ROUNDS * 2 + 6)) --fps 30 --max-eyes 1 --reconnect >"$T/g.client" 2>"$T/g.err" &
CPID=$!
sleep 2
conn_ok=0
for i in $(seq 1 "$ROUNDS"); do
  sleep "0.$((RANDOM % 9 + 1))"
  kill -9 "$HPID" 2>/dev/null; wait "$HPID" 2>/dev/null
  kill -0 "$CPID" 2>/dev/null || { bad "round $i: the game process died with the helper"; break; }
  start_helper "g$i" --eyes 1 --xr-check 0 || { bad "round $i: helper did not restart: $(tail -2 "$T/g$i.log")"; break; }
  sleep 1.2
  grep -q 'game connected' "$T/g$i.log" && conn_ok=$((conn_ok + 1))
done
kill -INT "$HPID" 2>/dev/null
wait "$CPID"; crc=$?
cat "$T/g.client" | sed 's/^/        /'
[[ $crc == 0 ]] && ok "the game never hung (exit 0)" || bad "game exit $crc: $(cat "$T/g.err")"
[[ $conn_ok == $ROUNDS ]] && ok "reconnected after each of $ROUNDS kills" || bad "reconnected only $conn_ok of $ROUNDS times"
wait "$HPID" 2>/dev/null

echo; echo "PASS=$PASS FAIL=$FAIL"; [[ $FAIL -eq 0 ]]
