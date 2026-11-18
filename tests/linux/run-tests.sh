#!/usr/bin/env bash
# Stand-in tests for the native Linux shim (stages 2b / 3a). No GPU, no game, no Steam needed.
#   tests/linux/run-tests.sh
# Needs: gcc/g++ with 32-bit support (gcc -m32 must work), python3. Optional: the Vulkan headers, to also
# drive the REAL 64-bit helper (on whatever Vulkan device exists, e.g. Mesa's software driver).
set -euo pipefail
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$T/../.." && pwd)"
export B="$T/build"
mkdir -p "$B"

echo "== building the 32-bit shim and the stand-ins"
export SHIM="$B/bmvr_preload32.so"
gcc -m32 -shared -fPIC -O2 -Wall -Wextra -fvisibility=hidden -fno-stack-protector -U_FORTIFY_SOURCE \
    -o "$SHIM" "$ROOT/linux/bmvr_preload.c" -ldl
gcc -m32 -shared -fPIC -O1 -Wall -o "$B/libSDL2-2.0.so.0" "$T/mock_gl.c"
g++ -m32 -shared -fPIC -I"$T" -o "$B/libtogl.so" "$T/fake_togl.cpp" -L"$B" -l:libSDL2-2.0.so.0 -Wl,-rpath,'$ORIGIN'
g++ -m32 -I"$T" -o "$B/fake_game" "$T/fake_game.cpp" -L"$B" -ltogl -Wl,--no-as-needed -l:libSDL2-2.0.so.0 -Wl,-rpath,'$ORIGIN'
gcc -Wall -I"$ROOT/linux" -o "$B/mock_helper" "$T/mock_helper.c"

echo "== the shim exports exactly what libtogl must see replaced (names the fake libtogl also exports)"
missing=0
for s in $(grep -oE '"_ZN[0-9A-Za-z_]+"' "$ROOT/linux/bmvr_preload.c" | tr -d '"' | sort -u); do
  nm -D --defined-only "$B/libtogl.so" | grep -q " $s\$" || { echo "  fake libtogl lacks $s"; missing=1; }
done
[[ $missing -eq 0 ]] && echo "  ok"
echo "== glibc symbol versions the shim needs (the Steam runtime's glibc may be older than the host's)"
objdump -T "$SHIM" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1 | sed 's/^/  highest: /'

echo "== stand-in suite"
python3 "$T/suite.py"

# Optional: the real helper against a real (possibly software) Vulkan device.
if echo '#include <vulkan/vulkan.h>' | cc -E -x c - >/dev/null 2>&1; then
  echo "== real helper (protocol v3 client; skipped if there is no Vulkan device)"
  cc -O2 -Wall -Wextra -std=gnu11 -I"$ROOT/linux" -o "$B/bmvr_xfer_helper" "$ROOT/linux/bmvr_xfer_helper.c" -ldl
  cc -Wall -I"$ROOT/linux" -o "$B/client" "$T/client.c"
  if timeout 40 "$B/bmvr_xfer_helper" --selftest-sync > "$B/selftest.txt" 2>&1; then
    grep -E "opaque-fd semaphore|selftest:" "$B/selftest.txt" | sed 's/^\[helper\] /  /'
    G="$B/hgame"; rm -rf "$G"; mkdir -p "$G"
    "$B/bmvr_xfer_helper" "$G" --no-semaphores > "$G/h.out" 2>&1 & HP=$!
    for _ in $(seq 1 60); do grep -q listening "$G/h.out" 2>/dev/null && break; sleep 0.1; done
    "$B/client" "$G" | sed 's/^/  /'
    kill $HP 2>/dev/null || true; wait $HP 2>/dev/null || true
    ls "$G"/bmvr_snapshot_*.ppm 2>/dev/null | xargs -n1 basename | sed 's/^/  wrote /'
  else
    echo "  skipped: no usable Vulkan device ($(tail -1 "$B/selftest.txt"))"
  fi
else
  echo "== real helper: skipped (no Vulkan headers)"
fi
