#!/usr/bin/env bash
# Launch hello_xr under the vr_agent layer, drive a "前ならえ" controller pose via the control
# channel (pose_client.mjs), and capture screenshots. POSE_X/POSE_Y/POSE_Z/HOLD_MS pass through.
set -u
VRMCP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${VRMCP_ROOT}/scripts/env.sh"

export VR_AGENT_LOG="$(mktemp -t vr_agent.XXXXXX.log)"
: > "$VR_AGENT_LOG"
GFX="${1:-Vulkan}"
HELLO="${VRMCP_ROOT}/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
APPLOG="$(mktemp -t hello_xr.XXXXXX.log)"

taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
sleep 1

echo "=== layer log: $VR_AGENT_LOG ==="
echo "Launching hello_xr -g ${GFX}; pose X=${POSE_X:-0.2} Y=${POSE_Y:--0.2} Z=${POSE_Z:--0.5}"
sleep 100 | "${HELLO}" -g "${GFX}" > "${APPLOG}" 2>&1 &

echo -n "waiting for control channel"
for i in $(seq 1 80); do
  if grep -q "listening on 127.0.0.1" "$VR_AGENT_LOG" 2>/dev/null; then echo " -> up"; break; fi
  echo -n "."; sleep 0.5
done

node "${VRMCP_ROOT}/scripts/pose_client.mjs"

sleep 1
taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
kill $(jobs -p) 2>/dev/null

echo
echo "=== layer pose/location log ==="
grep -iE "setInputDeviceLocation|LocalSpace|pose dropped|setInputDeviceActive" "$VR_AGENT_LOG" | tail -12
