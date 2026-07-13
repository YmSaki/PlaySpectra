#!/usr/bin/env bash
# GAP-10 integration test: drive the vr_agent layer against a real OpenXR app (hello_xr) under the
# Meta XR Simulator, and assert the engine-independent surfaces (profile/binding interception, view
# override, sync-semantics round-trip, non-degenerate capture) via integration_hello_xr.mjs.
#
# hello_xr is used as the conformant OpenXR app under test; the assertions live on paths every engine
# shares, so they validate engine-independence without any engine-specific project setup.
#
# Usage: scripts/integration_test.sh [graphics-api]   (default Vulkan; Meta supports Vulkan/D3D11/D3D12)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GFX="${1:-Vulkan}"
HELLO="${ROOT}/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
CLIENT="${ROOT}/scripts/integration_hello_xr.mjs"
CAP_DIR="${TEMP:-/tmp}/vr_agent_integration"
LOG="${CAP_DIR}/hello_xr.log"
mkdir -p "$CAP_DIR"

# Windows-style paths: the native hello_xr.exe can't parse MSYS /c/... and would fall back to SteamVR.
winpath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || echo "$1"; }
export XR_RUNTIME_JSON="$(winpath "${ROOT}/third_party/meta_xr_sim/PFiles/MetaXRSimulator/v201.0/meta_openxr_simulator.json")"
export XR_API_LAYER_PATH="$(winpath "${ROOT}/layer/manifest")"
export XR_ENABLE_API_LAYERS="XR_APILAYER_vr_agent"
export VR_AGENT_LOG="$(winpath "${CAP_DIR}/vr_agent_layer.log")"
export VR_AGENT_CAPTURE_DIR="$(winpath "${CAP_DIR}")"
: > "$VR_AGENT_LOG" 2>/dev/null || true

echo "[integration] runtime=$XR_RUNTIME_JSON"
echo "[integration] layer=$XR_API_LAYER_PATH  gfx=$GFX"

# Process hygiene: a live sibling hello_xr contending on the single SES makes runs flaky (see memory).
taskkill //F //IM hello_xr.exe //IM MetaXRSimulator.exe //IM synth_env_server.exe >/dev/null 2>&1
sleep 1

# hello_xr quits when getchar() on stdin returns EOF; hold stdin open with a sleep so the session runs.
sleep 70 | "$HELLO" -g "$GFX" > "$LOG" 2>&1 &

# Wait for the layer control channel to LISTEN on 52700 (session reached FOCUSED).
ok=0; secs=0
for i in $(seq 1 30); do
  if netstat -ano 2>/dev/null | grep -q "127.0.0.1:52700 .*LISTENING"; then ok=1; secs=$i; break; fi
  sleep 1
done
echo "[integration] control channel LISTEN ok=$ok after ${secs}s"

rc=1
if [ "$ok" = "1" ]; then
  sleep 6              # let hello_xr submit projection frames before we screenshot
  node "$CLIENT" 52700; rc=$?
else
  echo "[integration] control channel never came up -- hello_xr did not reach FOCUSED"
  echo "--- hello_xr log tail ---"; tail -25 "$LOG"
fi

taskkill //F //IM hello_xr.exe //IM MetaXRSimulator.exe //IM synth_env_server.exe >/dev/null 2>&1
echo "[integration] exit rc=$rc (captures + logs in ${CAP_DIR})"
exit $rc
