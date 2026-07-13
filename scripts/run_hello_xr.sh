#!/usr/bin/env bash
# Run the hello_xr bring-up app against the current XR_RUNTIME_JSON (Meta simulator by default).
# Usage: scripts/run_hello_xr.sh [seconds] [graphics-api]
#   seconds: how long to let it run before killing (default 12)
#   graphics-api: Vulkan (default) | D3D11 | D3D12 | OpenGL  (Meta supports Vulkan/D3D11/D3D12, NOT OpenGL)
set -u
VRMCP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${VRMCP_ROOT}/scripts/env.sh"

SECS="${1:-12}"
GFX="${2:-Vulkan}"
HELLO="${VRMCP_ROOT}/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
LOG="$(mktemp -t hello_xr.XXXXXX.log)"

echo "Running hello_xr -g ${GFX} for ${SECS}s (log: ${LOG})"
# hello_xr quits when getchar() on stdin returns; a backgrounded process with redirected stdout
# would hit EOF immediately and exit before the session runs. Hold stdin open with a sleep so
# the render loop actually runs. (See scripts/walking_skeleton_test.sh.)
sleep "$((SECS + 10))" | "${HELLO}" -g "${GFX}" > "${LOG}" 2>&1 &
sleep "${SECS}"
taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null

echo "=== app log (non-runtime lines) ==="
grep -vE "\[Meta XR Simulator\]" "${LOG}" | grep -iE "\[Info|\[Error|\[Warning"
