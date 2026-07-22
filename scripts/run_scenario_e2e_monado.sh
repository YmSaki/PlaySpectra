#!/usr/bin/env bash
# Flagship "VR Playwright loop" on live Windows Monado: OPERATE a scenario -> app re-renders -> OBSERVE
# the screen -> ASSERT the observation changed. The Windows counterpart of scripts/e2e_playwright_loop.sh
# (which is WSL2/Linux-only). Brings up monado-service (:52702) + hello_xr with the capture layer
# (:52700), then has server.py run tools/scenarios/capture_assert_demo.json: capture a reference via the
# layer, assert a no-op leaves it stable, drive look+move_head via the Monado adapter, assert the
# captured image changed. All assert logic lives in server.py (run_scenario/assert_capture) -- this
# script only stands up the live stack. Machine verdict = server.py rc (0 iff every assert passes).
#
# NOTE: the bring-up here mirrors run_hello_xr_monado.sh / run_mcp_verify_monado.sh. If you change the
# stack bring-up (ports, env, binary selection), update all three (candidate for a shared helper if a
# 4th consumer appears or the bring-up starts churning).
# Usage: scripts/run_scenario_e2e_monado.sh [gfx=D3D11] [secs=60]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT" || exit 9
GFX="${1:-D3D11}"; SECS="${2:-60}"
BW="runtime/monado-playspectra/build-win"
SVC="$BW/src/xrt/targets/service/Release/monado-service.exe"
if [ "$GFX" = "Vulkan" ]; then HELLO="layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
else HELLO="third_party/hello_xr_msvc/hello_xr.exe"; fi
MANIFEST="$BW/Release/openxr_monado-dev.json"
SCENARIO="tools/scenarios/capture_assert_demo.json"

if [ -z "${VULKAN_SDK:-}" ]; then
  VULKAN_SDK="$(ls -d /c/VulkanSDK/*/ 2>/dev/null | sort -V | tail -1)"; VULKAN_SDK="${VULKAN_SDK%/}"
fi
VK_BIN=""; [ -n "$VULKAN_SDK" ] && VK_BIN="$(cygpath -u "$VULKAN_SDK" 2>/dev/null || echo "$VULKAN_SDK")/bin"
export VULKAN_SDK
export PATH="${VK_BIN}:$ROOT/$BW/vcpkg_installed/x64-windows/bin:$ROOT/$BW/src/xrt/targets/service/Release:$ROOT/$BW/src/xrt/targets/openxr/Release:$PATH"
export XR_RUNTIME_JSON="$ROOT/$MANIFEST" XR_API_LAYER_PATH="$ROOT/layer/manifest" XR_ENABLE_API_LAYERS="XR_APILAYER_playspectra"
export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1 IPC_IGNORE_VERSION=1
listen() { netstat -ano 2>/dev/null | grep -E ":$1[^0-9].*LISTEN" >/dev/null; }

for f in "$SVC" "$HELLO" "$MANIFEST" "$SCENARIO"; do [ -f "$f" ] || { echo "MISSING $f"; exit 3; }; done
SVCLOG="$(mktemp -t msvc.XXXXXX.log)"; APPLOG="$(mktemp -t hx.XXXXXX.log)"
echo "service log: $SVCLOG   app log: $APPLOG"
taskkill //F //IM monado-service.exe 2>/dev/null; taskkill //F //IM hello_xr.exe 2>/dev/null
for i in $(seq 1 20); do if ! listen 52702 && ! listen 52700; then break; fi; sleep 0.5; done; sleep 1

"$SVC" > "$SVCLOG" 2>&1 & SVC_PID=$!
for i in $(seq 1 40); do listen 52702 && break; kill -0 "$SVC_PID" 2>/dev/null || break; sleep 0.5; done
listen 52702 || { echo "service down"; tail -15 "$SVCLOG"; exit 4; }

sleep "$((SECS + 10))" | "$HELLO" -g "$GFX" > "$APPLOG" 2>&1 & APP_PID=$!
( sleep "$SECS"; taskkill //F //IM hello_xr.exe 2>/dev/null ) &
LUP=0; for i in $(seq 1 50); do listen 52700 && { LUP=1; break; }; kill -0 "$APP_PID" 2>/dev/null || break; sleep 0.4; done
echo "layer :52700 up=$LUP"

RC=99
if [ "$LUP" = "1" ]; then
  sleep 1
  echo "=== flagship loop: server.py runs $(basename "$SCENARIO") (operate :52702 -> observe :52700 -> assert) ==="
  python tools/playspectra_server.py "$SCENARIO" --port 52702 --capture-port 52700; RC=$?
else
  echo "layer never came up"; grep -iE "error|unsupported" "$APPLOG" | tail
fi
taskkill //F //IM hello_xr.exe 2>/dev/null; taskkill //F //IM monado-service.exe 2>/dev/null
kill "$APP_PID" "$SVC_PID" 2>/dev/null
echo "SCENARIO_E2E_RC=$RC"
exit $RC
