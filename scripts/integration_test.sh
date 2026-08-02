#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# GAP-10 integration test: drive the playspectra layer against a real OpenXR app (hello_xr) under the
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
# Default hello_xr is the MinGW build (Vulkan/GL only). For D3D11/D3D12 point HELLO_XR_EXE at the
# MSVC build (third_party/hello_xr_msvc/hello_xr.exe), which has all graphics plugins compiled in.
HELLO="${HELLO_XR_EXE:-${ROOT}/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe}"
CLIENT="${ROOT}/scripts/integration_hello_xr.mjs"
CAP_DIR="${TEMP:-/tmp}/playspectra_integration"
LOG="${CAP_DIR}/hello_xr.log"
mkdir -p "$CAP_DIR"

# Windows-style paths: the native hello_xr.exe can't parse MSYS /c/... and would fall back to SteamVR.
winpath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || echo "$1"; }
# Runtime selection: VR_RUNTIME=metasim (default) | monado (Monado CI build, out-of-process service).
RUNTIME="${VR_RUNTIME:-metasim}"
if [ "$RUNTIME" = "monado" ]; then
  export XR_RUNTIME_JSON="$(winpath "${ROOT}/third_party/monado/openxr_monado.json")"
else
  export XR_RUNTIME_JSON="$(winpath "${ROOT}/third_party/meta_xr_sim/PFiles/MetaXRSimulator/v201.0/meta_openxr_simulator.json")"
fi
export XR_API_LAYER_PATH="$(winpath "${ROOT}/layer/manifest")"
export XR_ENABLE_API_LAYERS="XR_APILAYER_playspectra"
export VR_GFX_API="$GFX"    # tells the client which capture.api to expect
export PLAYSPECTRA_LOG="$(winpath "${CAP_DIR}/playspectra_layer.log")"
export PLAYSPECTRA_CAPTURE_DIR="$(winpath "${CAP_DIR}")"
: > "$PLAYSPECTRA_LOG" 2>/dev/null || true

echo "[integration] runtime=$XR_RUNTIME_JSON"
echo "[integration] layer=$XR_API_LAYER_PATH  gfx=$GFX  vr_runtime=$RUNTIME"

# Process hygiene: a live sibling hello_xr contending on the single SES makes runs flaky (see memory).
taskkill //F //IM hello_xr.exe //IM MetaXRSimulator.exe //IM synth_env_server.exe >/dev/null 2>&1
if [ "$RUNTIME" = "monado" ]; then
  # Monado's Windows build is service-mode: openxr_monado.dll connects to monado-service over a
  # named pipe derived from %TEMP%. Start a fresh service for this run (plain user process, no install).
  taskkill //F //IM monado-service.exe >/dev/null 2>&1
  sleep 1
  "${ROOT}/third_party/monado/bin/monado-service.exe" > "${CAP_DIR}/monado_service.log" 2>&1 &
  MONADO_SVC_PID=$!
  sleep 3
fi
sleep 1

# hello_xr quits when getchar() on stdin returns EOF; hold stdin open with a sleep so the session
# runs. We wrap the sleep in a subshell that records its PID, so later we can kill JUST the feeder to
# send EOF and trigger hello_xr's own graceful teardown (see the graceful block below). A real pipe
# (not a FIFO) is required so the native hello_xr.exe gets a valid Windows stdin handle.
( sleep 85 & echo $! > "$CAP_DIR/feed.pid"; wait ) | "$HELLO" -g "$GFX" > "$LOG" 2>&1 &
HELLO_PID=$!

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

# Explicit MSAA-capability SKIP: with HELLO_XR_SAMPLE_COUNT>1 some runtimes refuse the multisampled
# swapchain outright (Monado v25.1.0-646: xrCreateSwapchain -> XR_ERROR_VALIDATION_FAILURE; its
# Vulkan compositor does not import multisampled D3D11 shares). That is a runtime capability limit,
# not a layer defect, so it must not FAIL the run -- but only the ACTUAL observed rejection converts
# to SKIP (a future runtime that accepts MSAA runs the full assertions). rc stays FAIL-only otherwise.
msaa_skip=0
if [ "$rc" != "0" ] && [ "${HELLO_XR_SAMPLE_COUNT:-1}" -gt 1 ] \
   && grep -q "XR_ERROR_VALIDATION_FAILURE" "$LOG" 2>/dev/null \
   && grep -q "xrCreateSwapchain" "$LOG" 2>/dev/null; then
  echo "[integration] SKIP: runtime rejected the multisampled swapchain (xrCreateSwapchain -> XR_ERROR_VALIDATION_FAILURE with HELLO_XR_SAMPLE_COUNT=${HELLO_XR_SAMPLE_COUNT}) -- MSAA E2E not runnable on this runtime"
  msaa_skip=1
  rc=0
fi
[ "$msaa_skip" = "1" ] && graceful_default="skipped (MSAA capability SKIP -- app exited at xrCreateSwapchain)" || graceful_default="skipped (session/asserts did not pass)"

# Graceful-teardown gate: force-kill (below) never calls xrDestroySession/xrDestroyInstance, so the
# layer's ClearLayerDispatch / VulkanFree / registry+pose+inject cleanup go UNVERIFIED. Here we close
# hello_xr's stdin (kill the feeder) so getchar() hits EOF and hello_xr runs its OWN teardown chain
# through our layer: xrEndSession -> xrDestroySession (VulkanFree + session-scoped clears) -> ... ->
# xrDestroyInstance (ClearLayerDispatch + control-channel stop). We assert the chain COMPLETED by
# waiting for the instance-destroy markers in the layer log -- a crash in VulkanFree/a session clear
# would abort before them. We deliberately do NOT wait on process exit: the Meta sim's own process
# teardown lingers well past our cleanup and is not what this gate covers. This is the runtime gate
# for every "Destroy* -> Clear function" conversion in the refactor (phases 4-6).
graceful="$graceful_default"
if [ "$ok" = "1" ] && [ "$rc" = "0" ] && [ "$msaa_skip" = "0" ]; then
  LOG_G="$(echo "${CAP_DIR}/playspectra_layer.log" | tr '\\' '/')"   # forward slashes for MSYS grep
  FEED_PID="$(cat "$CAP_DIR/feed.pid" 2>/dev/null)"
  [ -n "$FEED_PID" ] && kill "$FEED_PID" >/dev/null 2>&1
  graceful="FAIL (layer did not reach clean xrDestroyInstance within 20s -- cleanup hung or crashed)"
  for i in $(seq 1 20); do
    if grep -q "xrDestroyInstance -- stopping control channel" "$LOG_G" 2>/dev/null \
       && grep -q "control_channel: accept loop exited" "$LOG_G" 2>/dev/null; then
      graceful="PASS"; break
    fi
    sleep 1
  done
  [ "$graceful" = "PASS" ] || rc=3
fi
echo "[integration] graceful teardown (xrDestroy* -> layer cleanup ran clean): $graceful"

taskkill //F //IM hello_xr.exe //IM MetaXRSimulator.exe //IM synth_env_server.exe >/dev/null 2>&1
[ "$RUNTIME" = "monado" ] && taskkill //F //IM monado-service.exe >/dev/null 2>&1
echo "[integration] exit rc=$rc (captures + logs in ${CAP_DIR})"
exit $rc
