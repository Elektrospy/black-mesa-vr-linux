#!/usr/bin/env bash
# Stand-in tests for the native Linux shim (stages 2b / 3a). No GPU, no game, no Steam needed.
#   tests/linux/run-tests.sh
# Needs: gcc/g++ with 32-bit support (gcc -m32 must work), python3. Optional: the Vulkan headers, to also
# drive the REAL 64-bit helper (on whatever Vulkan device exists, e.g. Mesa's software driver).
#   BMVR_TEST_NATIVE=1  build everything for the host (64-bit) instead of -m32: no multilib needed. It exercises all the
#                       shim logic (T1-T19) but not the 32-bit build itself; the normal run stays the authoritative one.
#   BMVR_TEST_BUILD=dir build directory (default tests/linux/build; a short temp dir is used when that path is too long)
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$T/../.." && pwd)"
export B="${BMVR_TEST_BUILD:-$T/build}"
# A unix socket path holds at most 107 characters and every case puts its socket in $B/cases/<name>/: a deep checkout
# would make every case fail with "socket path too long", so fall back to a short temporary directory.
if (( ${#B} > 55 )); then B="$(mktemp -d /tmp/bmvr-tests.XXXXXX)"; export B; echo "(build directory too deep for unix sockets; using $B)"; fi
mkdir -p "$B"
M32="-m32"; ARCH="32-bit"
if [[ "${BMVR_TEST_NATIVE:-0}" == 1 ]]; then M32=""; ARCH="NATIVE (host, not 32-bit)"; fi

echo "== building the $ARCH shim and the stand-ins"
export SHIM="$B/bmvr_preload32.so"
gcc $M32 -shared -fPIC -O2 -Wall -Wextra -fvisibility=hidden -fno-stack-protector -U_FORTIFY_SOURCE \
    -o "$SHIM" "$ROOT/linux/bmvr_preload.c" -ldl -lm
gcc $M32 -shared -fPIC -O1 -Wall -o "$B/libSDL2-2.0.so.0" "$T/mock_gl.c"
g++ $M32 -shared -fPIC -I"$T" -o "$B/libtogl.so" "$T/fake_togl.cpp" -L"$B" -l:libSDL2-2.0.so.0 -Wl,-rpath,'$ORIGIN'
g++ $M32 -I"$T" -o "$B/fake_game" "$T/fake_game.cpp" -L"$B" -ltogl -Wl,--no-as-needed -l:libSDL2-2.0.so.0 -Wl,-rpath,'$ORIGIN'
g++ $M32 -I"$T" -DVR_CLASS_NAME=CBMViewRender -o "$B/fake_game_bm" "$T/fake_game.cpp" -L"$B" -ltogl -Wl,--no-as-needed -l:libSDL2-2.0.so.0 -Wl,-rpath,'$ORIGIN'
gcc -Wall -I"$ROOT/linux" -o "$B/mock_helper" "$T/mock_helper.c"

echo "== the shim exports exactly what libtogl must see replaced (names the fake libtogl also exports)"
missing=0
for s in $(grep -oE '"_ZN[0-9A-Za-z_]+"' "$ROOT/linux/bmvr_preload.c" | tr -d '"' | sort -u); do
  nm -D --defined-only "$B/libtogl.so" | grep -q " $s\$" || { echo "  fake libtogl lacks $s"; missing=1; }
done
[[ $missing -eq 0 ]] && echo "  ok"
echo "== glibc symbol versions the shim needs (the Steam runtime's glibc may be older than the host's)"
objdump -T "$SHIM" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1 | sed 's/^/  highest: /'

echo "== one helper per game folder (flock guard, stage 3c; no Vulkan needed)"
cc -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/lock_test" "$T/lock_test.c"
"$B/lock_test" "$B" | sed 's/^/  /'

echo "== pose table (stage 4a X4; no GPU)"
cc -O2 -Wall -Wextra -std=gnu11 -pthread -I"$ROOT/linux" -o "$B/pose_test" "$T/pose_test.c"
"$B/pose_test" | sed 's/^/  /'

echo "== stereo math (stage 4b; no GPU, no game)"
cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/stereo_test" "$T/stereo_test.c" -lm
"$B/stereo_test" | sed 's/^/  /'
cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/viewmodel_test" "$T/viewmodel_test.c" -lm
"$B/viewmodel_test" | sed 's/^/  /'
echo "== transparent call probe (a real 32-bit freestanding program; no 32-bit libc needed)"
if gcc -m32 -O2 -fomit-frame-pointer -ffreestanding -nostdlib -static -fno-pic -no-pie -fno-stack-protector -I"$ROOT/linux" -o "$B/probe_test" "$T/probe_test.c" 2>/dev/null && "$B/probe_test" | sed 's/^/  /'; then :; else echo "  FAIL or skipped: probe_test (needs gcc -m32 and a kernel that runs 32-bit programs)"; fi
cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/inline_test" "$T/inline_test.c"
"$B/inline_test" | sed 's/^/  /'

echo "== controller -> game action mapping (slice 3a; no GPU, no game, no headset)"
cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/input_map_test" "$T/input_map_test.c" -lm
"$B/input_map_test" | sed 's/^/  /'

echo "== hand models: inflate, PNG, GLB, finger rig, controller placement (slice 5; real VR/hands files, no GPU)"
cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/hands_test" "$T/hands_test.c" -lm
"$B/hands_test" "$ROOT/VR/hands" | sed 's/^/  /'

echo "== RTTI vtable finder and slot patcher (stage 4b; real g++ RTTI as ground truth)"
g++ -O0 -Wall -Wextra -I"$ROOT/linux" -o "$B/rtti_test" "$T/rtti_test.cpp"
"$B/rtti_test" | sed 's/^/  /'

echo "== stand-in suite"
python3 "$T/suite.py"

# Optional: the real helper against a real (possibly software) Vulkan device.
if echo '#include <vulkan/vulkan.h>' | cc -E -x c - >/dev/null 2>&1; then
  echo "== real helper (protocol v5 client; skipped if there is no Vulkan device)"
  cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/third_party/openxr/include" -I"$ROOT/linux" -o "$B/bmvr_xfer_helper" "$ROOT/linux/bmvr_xfer_helper.c" "$ROOT/linux/bmvr_xr_sink.c" -pthread -ldl -lm
  cc -Wall -I"$ROOT/linux" -o "$B/client" "$T/client.c"
  if timeout 40 "$B/bmvr_xfer_helper" --selftest-sync > "$B/selftest.txt" 2>&1; then
    grep -E "opaque-fd semaphore|selftest:" "$B/selftest.txt" | sed 's/^\[helper\] /  /'
    G="$B/hgame"; rm -rf "$G"; mkdir -p "$G"
    "$B/bmvr_xfer_helper" "$G" --no-semaphores > "$G/h.out" 2>&1 & HP=$!
    for _ in $(seq 1 60); do grep -q listening "$G/h.out" 2>/dev/null && break; sleep 0.1; done
    "$B/client" "$G" | sed 's/^/  /'
    kill $HP 2>/dev/null || true; wait $HP 2>/dev/null || true
    ls "$G"/bmvr_snapshot_*.ppm 2>/dev/null | xargs -n1 basename | sed 's/^/  wrote /'
    # stage 3d: the same helper with two images; the client sends one single-eye frame and two PAIRS
    G="$B/hgame2"; rm -rf "$G"; mkdir -p "$G"
    "$B/bmvr_xfer_helper" "$G" --no-semaphores --eyes 2 > "$G/h.out" 2>&1 & HP=$!
    for _ in $(seq 1 60); do grep -q listening "$G/h.out" 2>/dev/null && break; sleep 0.1; done
    "$B/client" "$G" finish pair | sed 's/^/  /'
    kill $HP 2>/dev/null || true; wait $HP 2>/dev/null || true
    ls "$G"/bmvr_snapshot_*.ppm 2>/dev/null | xargs -n1 basename | sed 's/^/  wrote /'
    n=$(ls "$G"/bmvr_snapshot_*_eye0_*.ppm "$G"/bmvr_snapshot_*_eye1_*.ppm 2>/dev/null | wc -l)
    [[ "$n" -ge 2 ]] && echo "  pair snapshots written for both eyes" || { echo "  FAIL: no pair snapshots"; exit 1; }
  else
    echo "  skipped: no usable Vulkan device ($(tail -1 "$B/selftest.txt"))"
  fi
else
  echo "== real helper: skipped (no Vulkan headers)"
fi