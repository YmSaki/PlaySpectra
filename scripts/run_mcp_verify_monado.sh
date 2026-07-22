#!/usr/bin/env bash
# Full-chain check of the PlaySpectra MCP server (the AI-agent-facing interface) against the LIVE
# Windows-built Monado stack. Brings up monado-service (:52702) + hello_xr with the capture layer
# (:52700), then runs tools/playspectra_mcp_verify.py, which spawns the MCP server over stdio and
# drives operate/observe/screenshot/scenario/reset tools. Proves an AI agent can operate AND observe
# (screenshots included) a real VR app on Windows via MCP. (Bring-up mirrors run_hello_xr_monado.sh.)
#
# Requires the `mcp` Python SDK. Install it in a VENV, never a shared global env -- mcp pulls a newer
# pydantic/starlette and will break other apps in a shared interpreter. Then point PY at the venv:
#   python -m venv .venv-mcp && .venv-mcp/Scripts/python -m pip install mcp
#   PY=.venv-mcp/Scripts/python.exe scripts/run_mcp_verify_monado.sh
# Usage: [PY=<python-with-mcp>] scripts/run_mcp_verify_monado.sh [gfx=D3D11] [secs=60]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 9
GFX="${1:-D3D11}"; SECS="${2:-60}"
BW="runtime/monado-playspectra/build-win"
SVC="$BW/src/xrt/targets/service/Release/monado-service.exe"
if [ "$GFX" = "Vulkan" ]; then HELLO="layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
else HELLO="third_party/hello_xr_msvc/hello_xr.exe"; fi
MANIFEST="$BW/Release/openxr_monado-dev.json"
PY="${PY:-python}"

if [ -z "${VULKAN_SDK:-}" ]; then
  VULKAN_SDK="$(ls -d /c/VulkanSDK/*/ 2>/dev/null | sort -V | tail -1)"; VULKAN_SDK="${VULKAN_SDK%/}"
fi
VK_BIN=""; [ -n "$VULKAN_SDK" ] && VK_BIN="$(cygpath -u "$VULKAN_SDK" 2>/dev/null || echo "$VULKAN_SDK")/bin"
export VULKAN_SDK
export PATH="${VK_BIN}:$ROOT/$BW/vcpkg_installed/x64-windows/bin:$ROOT/$BW/src/xrt/targets/service/Release:$ROOT/$BW/src/xrt/targets/openxr/Release:$PATH"
export XR_RUNTIME_JSON="$ROOT/$MANIFEST" XR_API_LAYER_PATH="$ROOT/layer/manifest" XR_ENABLE_API_LAYERS="XR_APILAYER_playspectra"
export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1 IPC_IGNORE_VERSION=1
export PLAYSPECTRA_MONADO_PORT=52702 PLAYSPECTRA_PORT=52700
listen() { netstat -ano 2>/dev/null | grep -E ":$1[^0-9].*LISTEN" >/dev/null; }

for f in "$SVC" "$HELLO" "$MANIFEST"; do [ -f "$f" ] || { echo "MISSING $f"; exit 3; }; done
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
  echo "=== playspectra_mcp_verify.py (MCP full-chain against live Windows Monado) ==="
  "$PY" tools/playspectra_mcp_verify.py; RC=$?
else
  echo "layer never came up"; grep -iE "error|unsupported|module" "$APPLOG" | tail
fi
taskkill //F //IM hello_xr.exe 2>/dev/null; taskkill //F //IM monado-service.exe 2>/dev/null
kill "$APP_PID" "$SVC_PID" 2>/dev/null
echo "MCP_VERIFY_RC=$RC"
exit $RC
