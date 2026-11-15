#!/usr/bin/env bash
# Black Mesa (native Linux) feasibility probe for a VR port.
# Read-only: it only inspects files and writes bmvr-probe.txt.
#
# Usage:  ./bmvr-probe-linux.sh "/path/to/steamapps/common/Black Mesa"
# Then send me bmvr-probe.txt (it contains no personal data except the path you pass).

set -uo pipefail
GAME="${1:-}"
[[ -n "$GAME" && -d "$GAME" ]] || { echo "Usage: $0 \"/path/to/Black Mesa\"" >&2; exit 1; }
OUT="${2:-bmvr-probe.txt}"
for t in file readelf nm strings; do
  command -v "$t" >/dev/null || { echo "Missing tool: $t (install binutils / file)" >&2; exit 1; }
done

# Game code libs (bms/bin) and engine libs (bin).
LIBS=(
  bin/launcher.so bin/engine.so bin/materialsystem.so bin/shaderapidx9.so
  bin/libtogl.so bin/vguimatsurface.so bin/studiorender.so bin/vphysics.so
  bms/bin/client.so bms/bin/server.so
)

{
echo "== probe $(date -u +%FT%TZ)  host: $(uname -srm)"
echo "== game dir: $GAME"
echo "== steam.inf:"; cat "$GAME/bms/steam.inf" 2>/dev/null | head -12
echo
echo "== launch script / binaries"
ls -la "$GAME"/bms.sh "$GAME"/bms_linux 2>/dev/null
echo
echo "== bin/ listing (names + sizes)"
ls -la "$GAME/bin" 2>/dev/null | head -80
echo
for rel in "${LIBS[@]}"; do
  f="$GAME/$rel"
  echo "################ $rel"
  if [[ ! -f "$f" ]]; then echo "(missing)"; echo; continue; fi
  ls -l "$f" | awk '{print "size:", $5}'
  file -b "$f"
  echo "-- needed libs:"; readelf -d "$f" 2>/dev/null | grep -E 'NEEDED|SONAME|RUNPATH|RPATH'
  dyn=$(nm -D --defined-only "$f" 2>/dev/null | wc -l)
  all=$(nm -C --defined-only "$f" 2>/dev/null | wc -l)
  echo "-- exported dynamic symbols: $dyn   full symbol table entries: $all"
  echo "-- key exports / interface names:"
  nm -D --defined-only "$f" 2>/dev/null | grep -E ' (CreateInterface|LauncherMain|Direct3DCreate9|Direct3DCreate9Ex|CreateD3DDevice|GetTogl|GLMgr)' | head -10
  strings -n 6 "$f" | grep -E '^(VClient|VEngineClient|VMaterialSystem|VShaderApi|ShaderDevice|ShaderApi|MaterialSystem_Config|VEngineRenderView|VEngineServer|ServerGameDLL|ServerGameEnts)[0-9A-Za-z_]*$' | sort -u | head -20
  echo "-- Source symbols present (demangled; empty = stripped):"
  nm -C "$f" 2>/dev/null | grep -E 'CViewRender::(RenderView|Render|SetUpView)|CBaseViewModel|::CalcViewModelView|CMatRenderContext|CShaderDeviceMgrDx8|CMaterialSystem::|CDirect3DDevice9|GLMContext::|C_BasePlayer::CreateMove|CHLClient::CreateMove|ClientModeShared' | head -25
  echo
done

echo "== Which renderer does the Linux build load? (dxvk / togl strings)"
for rel in bin/launcher.so bin/shaderapidx9.so bin/materialsystem.so bin/engine.so bin/libtogl.so; do
  f="$GAME/$rel"; [[ -f "$f" ]] || continue
  echo "-- $rel"; strings -n 5 "$f" | grep -iE 'dxvk|libdxvk|d3d9\.so|enabledxvk|-vulkan|-gl\b|togl|SDL_Vulkan|ToGL' | sort -u | head -25
done
echo
echo "== libdxvk_d3d9.so / libopenvr_api.so"
for rel in bin/libdxvk_d3d9.so bin/libopenvr_api.so; do
  f="$GAME/$rel"; [[ -f "$f" ]] || { echo "$rel missing"; continue; }
  file -b "$f"; readelf -d "$f" | grep -E 'NEEDED|SONAME'
  echo "-- exports (first 25):"; nm -D --defined-only "$f" | awk '{print $3}' | head -25
done
echo
echo "== libtogl.so exports (Direct3D*/GL related, first 40)"
nm -D --defined-only "$GAME/bin/libtogl.so" 2>/dev/null | awk '{print $3}' | grep -E 'Direct3D|D3D|GLM|Togl|ToGL|CreateD3D' | head -40
echo
echo "== engine.so dynamic export sample (what are the 4089 symbols?)"
nm -DC --defined-only "$GAME/bin/engine.so" 2>/dev/null | awk '{ $1=""; $2=""; print }' | head -25
echo
echo "== client.so dynamic exports (first 40, demangled)"
nm -DC --defined-only "$GAME/bms/bin/client.so" 2>/dev/null | awk '{ $1=""; $2=""; print }' | head -40
echo

echo "== GL / Vulkan / OpenXR libs the game will see at runtime"
for p in libGL.so.1 libGLX.so.0 libvulkan.so.1 libopenvr_api.so libopenxr_loader.so.1; do
  printf '%-22s' "$p"; (ldconfig -p 2>/dev/null || /sbin/ldconfig -p 2>/dev/null || /usr/bin/ldconfig -p 2>/dev/null) | grep -F "$p" | head -4 | tr '\n' ';' ; echo
done
echo
echo "== SteamVR / OpenXR active runtime"
echo "XR_RUNTIME_JSON=${XR_RUNTIME_JSON:-unset}"
ls ~/.config/openxr/1/active_runtime.json 2>/dev/null && cat ~/.config/openxr/1/active_runtime.json | head -12
} > "$OUT" 2>&1

echo "Wrote $OUT ($(wc -l < "$OUT") lines)."
