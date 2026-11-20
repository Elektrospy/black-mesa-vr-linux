#!/usr/bin/env bash
# Stage 4a, milestone X1: the frame-loop self-test (`bmvr_xfer_helper --selftest-xr`) against a headless Monado.
#
#   tests/linux/test_xr_selftest.sh [path/to/bmvr_xfer_helper] [--software]
#
# NOT part of run-tests.sh. Same setup and refusals as test_xr_probe.sh (own Monado: simulated HMD, null compositor; --software
# forces Mesa's lavapipe). STATUS: written, NOT yet executed anywhere (the build sandbox had no Vulkan device and no Monado).
#
# Cases:
#   X1-a  runtime path that does not exist          -> exit 2 (UNAVAILABLE)
#   X1-b  quad, 6 s                                  -> exit 0, frames rendered, no per-frame log lines, no files created
#   X1-c  projection, 6 s                            -> exit 0
#   X1-d  both layers, Ctrl-C (SIGINT) after 3 s     -> the helper exits by itself within 15 s, no crash signal
#   X1-e  5 short runs in a row (teardown stability) -> all exit 0
#
# What this does NOT show: that the picture is right (eye order, colours: Monado's null compositor shows nothing), anything
# about SteamVR or a real headset, the game, the pair ring or poses. Run `--selftest-xr` by hand on a headset for that.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HELPER=""; SOFT=0
for a in "$@"; do case "$a" in --software) SOFT=1 ;; *) HELPER="$a" ;; esac; done
HELPER="${HELPER:-$ROOT/build_linux_native/bmvr_xfer_helper}"
[[ -x "$HELPER" ]] || { echo "helper not found: $HELPER (build it: scripts/build-linux.sh)"; exit 2; }
command -v monado-service >/dev/null || { echo "monado-service not installed (Debian/Ubuntu: monado-service libopenxr1-monado)"; exit 2; }

if [[ $SOFT -eq 1 ]]; then
  ICD="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1)"
  [[ -n "$ICD" ]] || { echo "--software: lavapipe ICD (lvp_icd.json) not found (mesa-vulkan-drivers)"; exit 2; }
  export VK_ICD_FILENAMES="$ICD"
fi

SOCK_DIRS=("${XDG_RUNTIME_DIR:-/nonexistent}" /tmp)
monado_socket() { for d in "${SOCK_DIRS[@]}"; do [[ -S "$d/monado_comp_ipc" ]] && { echo "$d/monado_comp_ipc"; return 0; }; done; return 1; }
if s="$(monado_socket)"; then echo "A Monado service is already running ($s). Stop it first (or remove the socket if it is stale); this test starts its own."; exit 2; fi

PASS=0; FAIL=0
ok()  { echo "  PASS  $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
T="$(mktemp -d)"; MPID=""
cleanup() {
  [[ -n "$MPID" ]] && kill "$MPID" 2>/dev/null && wait "$MPID" 2>/dev/null
  exec 9>&- 2>/dev/null
  rm -rf "$T"
}
trap cleanup EXIT
mkdir -p "$T/work"

run() { # $1 = log name, rest = helper args. Runs in an empty directory; never inherits our stdin.
  local log="$T/$1.log"; shift
  ( cd "$T/work" && timeout 120 "$HELPER" --selftest-xr "$@" </dev/null >"$log" 2>&1 )
  echo $?
}

echo "X1-a: a runtime path that does not exist"
rc="$(XR_RUNTIME_JSON=/nonexistent-runtime.json run a --xr-wait 0)"
[[ "$rc" == 2 ]] && ok "exit 2 (UNAVAILABLE)" || bad "expected exit 2, got $rc"
grep -q 'xr-selftest: UNAVAILABLE' "$T/a.log" && ok "verdict line says UNAVAILABLE" || bad "no UNAVAILABLE verdict"

echo "starting a headless Monado (simulated HMD, null compositor)"
mkfifo "$T/stdin.fifo"; exec 9<>"$T/stdin.fifo"
SIMULATED_ENABLE=1 XRT_COMPOSITOR_NULL=1 XRT_COMPOSITOR_FORCE_GPU_INDEX=0 monado-service <"$T/stdin.fifo" >"$T/monado.log" 2>&1 &
MPID=$!
for _ in $(seq 1 100); do monado_socket >/dev/null && break; kill -0 "$MPID" 2>/dev/null || break; sleep 0.1; done
if ! monado_socket >/dev/null; then echo "  Monado did not come up; its log:"; tail -15 "$T/monado.log" | sed 's/^/    /'; exit 2; fi

check_mode() { # $1 = log, $2 = mode name
  grep -qE "^\[xr\] ok +frames submitted +$2:" "$1" && ok "frames submitted [$2]" || bad "frames submitted [$2] (not ok in the summary)"
}

echo "X1-b: quad, 6 s"
rc="$(run b --xr-layer quad --xr-seconds 6 --xr-wait 8000)"
[[ "$rc" == 0 ]] && ok "exit 0 (PASS)" || bad "expected exit 0, got $rc"
check_mode "$T/b.log" quad
n="$(wc -l <"$T/b.log")"
[[ "$n" -lt 80 ]] && ok "log is short ($n lines): no per-frame logging" || bad "log has $n lines: per-frame logging?"
[[ -z "$(ls -A "$T/work")" ]] && ok "no files created in the working directory" || bad "files appeared: $(ls "$T/work")"
grep -E 'selftest\[quad\]|swapchain|session state' "$T/b.log" | head -8 | sed 's/^/        /'

echo "X1-c: projection, 6 s"
rc="$(run c --xr-layer projection --xr-seconds 6 --xr-wait 8000)"
[[ "$rc" == 0 ]] && ok "exit 0 (PASS)" || bad "expected exit 0, got $rc"
check_mode "$T/c.log" projection

echo "X1-d: both layers, SIGINT after 3 s"
( cd "$T/work" && timeout 60 "$HELPER" --selftest-xr --xr-seconds 30 --xr-wait 8000 </dev/null >"$T/d.log" 2>&1 ) &
HP=$!
sleep 3; pkill -INT -f "bmvr_xfer_helper --selftest-xr --xr-seconds 30" 2>/dev/null
for _ in $(seq 1 150); do kill -0 "$HP" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$HP" 2>/dev/null; then bad "helper still running 15 s after SIGINT"; kill -9 "$HP" 2>/dev/null; else
  wait "$HP"; rc=$?
  [[ "$rc" -lt 128 ]] && ok "helper exited by itself (exit $rc), no crash signal" || bad "helper died with signal (exit $rc)"
  grep -q 'interrupted: shutting down cleanly' "$T/d.log" && ok "logged the clean shutdown" || bad "no clean-shutdown line"
fi

echo "X1-e: 5 short runs in a row (teardown stability)"
all=1
for i in $(seq 1 5); do rc="$(run "e$i" --xr-layer quad --xr-seconds 2 --xr-wait 5000)"; [[ "$rc" == 0 ]] || { all=0; echo "        run $i exit $rc"; }; done
[[ $all == 1 ]] && ok "5/5 exit 0" || bad "a repeated run failed"

echo
echo "PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && { echo "ALL CHECKS PASSED (Monado only: nothing here says the picture is right, or anything about SteamVR or a real headset)"; exit 0; }
exit 1
