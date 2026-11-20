#!/usr/bin/env bash
# Stage 4a, milestone X0: the OpenXR probe (`bmvr_xfer_helper --xr-probe`) against a real OpenXR runtime.
#
#   tests/linux/test_xr_probe.sh [path/to/bmvr_xfer_helper] [--software]
#
# NOT part of run-tests.sh: it needs an OpenXR loader and Monado (monado-service) installed. It starts its OWN headless
# Monado (simulated HMD, null compositor) and stops it again. It refuses to run when a Monado is already running (it would
# clobber that session). --software forces Mesa's lavapipe (a CPU Vulkan device) for machines without a GPU, e.g. CI.
#
# Cases:
#   X0-a  runtime path that does not exist           -> exit 2 (UNAVAILABLE)
#   X0-b  Monado installed, service NOT running       -> exit 2 (UNAVAILABLE), the message names monado-service
#   X0-c  Monado running, simulated HMD               -> exit 0 (PASS), every required item present
#   X0-d  same, swapchain bigger than the view's max  -> reports what the runtime does (informational)
#   X0-e  10 probes in a row                          -> all exit 0 (teardown is stable)
#
# What this does NOT show: anything about SteamVR, a real GPU, a real headset, or frames (milestone X1).
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
ok()   { echo "  PASS  $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
T="$(mktemp -d)"; MPID=""
cleanup() {
  [[ -n "$MPID" ]] && kill "$MPID" 2>/dev/null && wait "$MPID" 2>/dev/null
  exec 9>&- 2>/dev/null
  rm -rf "$T"
}
trap cleanup EXIT

probe() { # $1 = log name, rest = helper args; env is inherited. Never inherits our stdin (a runtime must not read it).
  local log="$T/$1.log"; shift
  timeout 90 "$HELPER" --xr-probe "$@" </dev/null >"$log" 2>&1
  echo $?
}

echo "X0-a: a runtime path that does not exist"
rc="$(XR_RUNTIME_JSON=/nonexistent-runtime.json probe a --xr-wait 0)"
[[ "$rc" == 2 ]] && ok "exit 2 (UNAVAILABLE)" || bad "expected exit 2, got $rc"
grep -q 'xr-probe: UNAVAILABLE' "$T/a.log" && ok "verdict line says UNAVAILABLE" || bad "no UNAVAILABLE verdict"

echo "X0-b: Monado installed but its service is not running"
rc="$(probe b --xr-wait 0)"
[[ "$rc" == 2 ]] && ok "exit 2 (UNAVAILABLE)" || bad "expected exit 2, got $rc"
grep -q 'start monado-service' "$T/b.log" && ok "the message tells the user to start monado-service" || bad "hint missing"

echo "starting a headless Monado (simulated HMD, null compositor)"
# Monado polls stdin for a \"quit\" command and exits when epoll on it fails: give it a FIFO that stays open.
mkfifo "$T/stdin.fifo"; exec 9<>"$T/stdin.fifo"
SIMULATED_ENABLE=1 XRT_COMPOSITOR_NULL=1 XRT_COMPOSITOR_FORCE_GPU_INDEX=0 monado-service <"$T/stdin.fifo" >"$T/monado.log" 2>&1 &
MPID=$!
for _ in $(seq 1 100); do monado_socket >/dev/null && break; kill -0 "$MPID" 2>/dev/null || break; sleep 0.1; done
if ! monado_socket >/dev/null; then echo "  Monado did not come up; its log:"; tail -15 "$T/monado.log" | sed 's/^/    /'; exit 2; fi

echo "X0-c: Monado running, simulated HMD"
rc="$(probe c --xr-wait 5000)"
[[ "$rc" == 0 ]] && ok "exit 0 (PASS)" || bad "expected exit 0, got $rc"
for item in 'XR_KHR_vulkan_enable2' 'xrCreateVulkanInstanceKHR' 'xrCreateVulkanDeviceKHR' 'xrCreateSession' \
            'swapchain format R8G8B8A8_SRGB' 'xrCreateSwapchain' 'session reaches READY'; do
  grep -qE "^\[xr\] ok +$item" "$T/c.log" && ok "$item" || bad "$item (not ok in the summary)"
done
grep -E 'runtime-chosen GPU|device UUID|view sizes|swapchain formats' "$T/c.log" | sed 's/^/        /'

echo "X0-d: swapchain bigger than the view's recommended/max size (informational: what does this runtime do?)"
rc="$(probe d --xr-wait 0 --xr-size 3840x2160)"
grep -E 'view sizes|xrCreateSwapchain:' "$T/d.log" | head -2 | sed 's/^/        /'
[[ "$rc" == 0 || "$rc" == 1 ]] && ok "probe ended with a verdict (exit $rc), no crash" || bad "unexpected exit $rc"

echo "X0-e: 10 probes in a row (teardown stability)"
all=1
for i in $(seq 1 10); do rc="$(probe "e$i" --xr-wait 3000)"; [[ "$rc" == 0 ]] || { all=0; echo "        run $i exit $rc"; }; done
[[ $all == 1 ]] && ok "10/10 exit 0" || bad "a repeated probe failed"

echo
echo "PASS=$PASS FAIL=$FAIL"
[[ $FAIL -eq 0 ]] && { echo "ALL CHECKS PASSED (Monado only: nothing here says anything about SteamVR or real hardware)"; exit 0; }
exit 1
