#!/usr/bin/env bash
# Black Mesa (native Linux) - probe #2: renderer selection and ToGL hook surface.
# Read-only. Writes bmvr-probe2.txt (no personal data besides the path you pass).
#
# Usage: ./bmvr-probe2-linux.sh "/path/to/steamapps/common/Black Mesa"

set -uo pipefail
GAME="${1:-}"
[[ -n "$GAME" && -d "$GAME" ]] || { echo "Usage: $0 \"/path/to/Black Mesa\"" >&2; exit 1; }
OUT="${2:-bmvr-probe2.txt}"
for t in nm strings grep readelf; do command -v "$t" >/dev/null || { echo "Missing tool: $t" >&2; exit 1; }; done

{
echo "== probe2 $(date -u +%FT%TZ)  $(uname -srm)"

echo; echo "== 1. Which files mention libdxvk_d3d9 / dxvk at all? (decides if the game can load it)"
grep -rIl -i "dxvk" "$GAME/bms.sh" "$GAME/bms/cfg" 2>/dev/null
for f in "$GAME"/bin/*.so "$GAME"/bms/bin/*.so "$GAME"/bms_linux; do
  [[ -f "$f" ]] || continue
  case "$f" in *.so.r*) continue;; esac
  if strings -n 5 "$f" 2>/dev/null | grep -qi 'dxvk'; then
    echo "-- $(basename "$f"):"; strings -n 5 "$f" | grep -i 'dxvk' | sort -u | head -8
  fi
done

echo; echo "== 2. Full libtogl.so exports (named D3D9 surface we could interpose)"
nm -D --defined-only "$GAME/bin/libtogl.so" 2>/dev/null | awk '{print $3}' | sort

echo; echo "== 3. D3D / ToGL symbols shaderapidx9.so imports from libtogl"
nm -D --undefined-only "$GAME/bin/shaderapidx9.so" 2>/dev/null | awk '{print $2}' | grep -E 'Direct3D|D3D|GLM|togl|ToGL|IDirect3D' | sort

echo; echo "== 4. Other modules that import ToGL / D3D symbols"
for f in "$GAME"/bin/*.so "$GAME"/bms/bin/client.so; do
  case "$f" in *.so.r*) continue;; esac
  c=$(nm -D --undefined-only "$f" 2>/dev/null | grep -cE 'IDirect3D|Direct3DCreate9|D3DX')
  [[ "$c" -gt 0 ]] && echo "$(basename "$f"): $c D3D imports"
done

echo; echo "== 5. client.so anchor strings (used to locate hooks by xref)"
strings -n 8 "$GAME/bms/bin/client.so" 2>/dev/null | grep -E 'UpdateFlashlightState|CalcViewModelView|DrawModelExecute|CViewRender|FlashlightState|C_BaseViewModel|CHudCrosshair|view\.cpp' | sort -u | head -40

echo; echo "== 6. toolchain on this machine (for building a 32-bit preload library)"
for t in gcc g++ clang cmake make; do printf '%-6s' "$t"; command -v "$t" || echo "-"; done
echo '#include <stdio.h>
int main(){return 0;}' > /tmp/bmvr_m32.c
if gcc -m32 /tmp/bmvr_m32.c -o /tmp/bmvr_m32 2>/dev/null; then echo "gcc -m32: OK"; else echo "gcc -m32: FAILED (install lib32 glibc / gcc-multilib)"; fi
rm -f /tmp/bmvr_m32.c /tmp/bmvr_m32
echo "32-bit libs present:"
for p in libGL.so.1 libvulkan.so.1 libSDL2-2.0.so.0 libstdc++.so.6 libX11.so.6; do
  printf '  %-20s' "$p"; (ldconfig -p 2>/dev/null || /usr/bin/ldconfig -p 2>/dev/null) | grep -F "$p" | grep -v x86-64 | head -1 | awk '{print $NF}' | grep . || echo "no 32-bit copy"
done

echo; echo "== 7. VR runtime (host, 64-bit)"
echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-unset}"
cat ~/.config/openxr/1/active_runtime.json 2>/dev/null | head -10
ls ~/.local/share/Steam/steamapps/common/SteamVR/bin/linux32 2>/dev/null | head -10 || true
ls ~/.local/share/Steam/steamapps/common/SteamVR/bin/linux64 2>/dev/null | head -5 || true
} > "$OUT" 2>&1

echo "Wrote $OUT ($(wc -l < "$OUT") lines). Please send it."
