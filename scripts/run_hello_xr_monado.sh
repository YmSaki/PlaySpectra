#!/usr/bin/env bash
# End-to-end verification: a real OpenXR app (hello_xr) driven on the Windows-built Monado runtime
# (PlaySpectra virtual-device driver, control channel :52702) while the playspectra capture layer
# (:52700) observes the frames it renders -- the full PlaySpectra stack against a real engine app, on
# Windows real GPU, headless.
#
# What this proves (machine value = integration_hello_xr.mjs rc, 0 iff every assertion passes):
#   - Monado's client<->service IPC works on Windows (openxr_monado.dll IPC client -> monado-service.exe)
#   - the D3D11 client-compositor interop path renders real frames the layer can capture non-degenerately
#   - the :52702 operate channel drives the Monado-side virtual HMD (coupling probe, informational)
#
# Headless: XRT_COMPOSITOR_NULL=1. The app still renders into real D3D11 swapchains and the layer
# captures them at xrEndFrame BEFORE the compositor would present -- so "no display" does not mean "no
# frames". The main (display) compositor is a separate path and is NOT exercised here.
#
# Safe: XR_RUNTIME_JSON points THIS process at Monado per-process; the system ActiveRuntime registry is
# never touched, so a real Rift/Meta runtime keeps working.
#
# Prereqs (see CLAUDE.local.md "Windows で Monado(PlaySpectra driver) をビルドする方法"):
#   - monado built:  cmake --build <build-win> --target monado-service cli openxr_monado
#   - hello_xr (MSVC, D3D):  third_party/hello_xr_msvc/hello_xr.exe  (scripts/setup_helloxr_msvc.sh)
# Usage: scripts/run_hello_xr_monado.sh [gfx=D3D11] [secs=25]
#   gfx: D3D11 | D3D12  (the MSVC hello_xr is built WITHOUT Vulkan -- '-g Vulkan' errors "Unsupported")
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 9
GFX="${1:-D3D11}"
SECS="${2:-25}"
BW="runtime/monado-playspectra/build-win"
SVC="$BW/src/xrt/targets/service/Release/monado-service.exe"
HELLO="third_party/hello_xr_msvc/hello_xr.exe"
MANIFEST="$BW/Release/openxr_monado-dev.json"

# Vulkan SDK: needed on PATH so openxr_monado.dll's vulkan-1.dll dependency resolves at load time.
if [ -z "${VULKAN_SDK:-}" ]; then
  VULKAN_SDK="$(ls -d /c/VulkanSDK/*/ 2>/dev/null | sort -V | tail -1)"
  VULKAN_SDK="${VULKAN_SDK%/}"
fi
VK_BIN=""
[ -n "$VULKAN_SDK" ] && VK_BIN="$(cygpath -u "$VULKAN_SDK" 2>/dev/null || echo "$VULKAN_SDK")/bin"
export VULKAN_SDK
# runtime DLLs: vulkan + monado's vcpkg deps (cjson/pthreads) that ship next to the built targets.
export PATH="${VK_BIN}:$ROOT/$BW/vcpkg_installed/x64-windows/bin:$ROOT/$BW/src/xrt/targets/service/Release:$ROOT/$BW/src/xrt/targets/openxr/Release:$PATH"

echo "=== preconditions ==="
MISS=0
for f in "$SVC" "$HELLO" "$MANIFEST"; do
  if [ -f "$f" ]; then echo "OK  $f"; else echo "MISSING  $f"; MISS=1; fi
done
[ "$MISS" = "1" ] && { echo "precondition missing -> abort (see header prereqs)"; exit 3; }

# per-process runtime + layer (system registry untouched)
export XR_RUNTIME_JSON="$ROOT/$MANIFEST"
export XR_API_LAYER_PATH="$ROOT/layer/manifest"
export XR_ENABLE_API_LAYERS="XR_APILAYER_playspectra"
export PLAYSPECTRA_ENABLE=1
export XRT_COMPOSITOR_NULL=1
export VR_GFX_API="$GFX"
# If monado-service and openxr_monado.dll were built at different commits, Monado's git-tag guard
# refuses the connection. Bypass it: our only cross-commit delta is the /utf-8 build flag (no IPC ABI
# change). Rebuild both at one commit to drop this. Monado documents this exact dev-mode escape hatch.
export IPC_IGNORE_VERSION=1

listen() { netstat -ano 2>/dev/null | grep -E ":$1[^0-9].*LISTEN" >/dev/null; }

SVCLOG="$(mktemp -t monadosvc.XXXXXX.log)"
APPLOG="$(mktemp -t helloxr.XXXXXX.log)"
echo "=== service log: $SVCLOG ==="
echo "=== app log:     $APPLOG ==="

taskkill //F //IM monado-service.exe 2>/dev/null; taskkill //F //IM hello_xr.exe 2>/dev/null; sleep 1

echo "=== start monado-service (:52702, null compositor) ==="
"$SVC" > "$SVCLOG" 2>&1 &
SVC_PID=$!
UP=0
for i in $(seq 1 40); do
  if listen 52702; then UP=1; break; fi
  if ! kill -0 "$SVC_PID" 2>/dev/null; then echo "service exited early"; break; fi
  sleep 0.5
done
echo "service :52702 up=$UP"
[ "$UP" = "1" ] || { echo "=== service log ==="; tail -30 "$SVCLOG"; taskkill //F //IM monado-service.exe 2>/dev/null; exit 4; }

echo "=== launch hello_xr -g $GFX against Monado (stdin held open) ==="
# hello_xr quits on stdin EOF; hold it open with a sleep pipe so the render loop runs.
sleep "$((SECS + 10))" | "$HELLO" -g "$GFX" > "$APPLOG" 2>&1 &
APP_PID=$!
( sleep "$SECS"; taskkill //F //IM hello_xr.exe 2>/dev/null ) &

LUP=0
for i in $(seq 1 50); do
  if listen 52700; then LUP=1; break; fi
  if ! kill -0 "$APP_PID" 2>/dev/null; then echo "hello_xr exited before layer channel"; break; fi
  sleep 0.4
done
echo "layer :52700 up=$LUP"

RC=99
if [ "$LUP" = "1" ]; then
  echo "=== integration_hello_xr.mjs (layer observation of a real app on Monado) ==="
  node scripts/integration_hello_xr.mjs 52700
  IRC=$?
  echo "=== runtime coupling: :52702 operate drives Monado virtual HMD -> app xrLocateViews (:52700) ==="
  python tools/playspectra_coupling_probe.py 52700 52702
  CRC=$?
  # verdict gates on BOTH: layer observation of a real app AND runtime-level operate reaching the app.
  RC=$(( IRC != 0 ? IRC : CRC ))
else
  echo "layer channel never came up"
fi

echo "=== teardown ==="
taskkill //F //IM hello_xr.exe 2>/dev/null; taskkill //F //IM monado-service.exe 2>/dev/null
kill "$APP_PID" "$SVC_PID" 2>/dev/null

echo "=== app log (loader/runtime/xr lines) ==="
grep -iE "runtime|loader|instance|session|swapchain|d3d|error|fail|compositor|semaphore" "$APPLOG" | tail -30
echo "=== service log tail ==="
grep -iE "playspectra|client_connected|compositor|error|fail" "$SVCLOG" | tail -15
echo "INTEGRATION_RC=$RC"
exit $RC
