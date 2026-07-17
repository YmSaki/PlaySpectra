#!/usr/bin/env bash
# OpenVR-path integration test: hellovr_dx12 (OpenVR app) -> OpenComposite -> vr_agent layer -> Monado.
# Prereqs (one-time): scripts/setup_monado.sh, scripts/setup_opencomposite.sh, scripts/setup_hellovr.sh.
# Runtime is Monado by design (OpenComposite needs an OpenXR runtime; matrix stays minimal).
#
# Usage: scripts/integration_openvr_test.sh
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${ROOT}/third_party/hellovr/bin/win64"
CLIENT="${ROOT}/scripts/integration_openvr.mjs"
CAP_DIR="${TEMP:-/tmp}/vr_agent_openvr_integration"
mkdir -p "$CAP_DIR"

winpath() { command -v cygpath >/dev/null 2>&1 && cygpath -m "$1" || echo "$1"; }
export XR_RUNTIME_JSON="$(winpath "${ROOT}/third_party/monado/openxr_monado.json")"
export XR_API_LAYER_PATH="$(winpath "${ROOT}/layer/manifest")"
export XR_ENABLE_API_LAYERS="XR_APILAYER_vr_agent"
export VR_AGENT_LOG="$(winpath "${CAP_DIR}/vr_agent_layer.log")"
export VR_AGENT_CAPTURE_DIR="$(winpath "${CAP_DIR}")"
: > "$VR_AGENT_LOG" 2>/dev/null || true

echo "[openvr-integration] runtime=$XR_RUNTIME_JSON"
[ -x "${APP_DIR}/hellovr_dx12.exe" ] || { echo "hellovr not deployed -- run scripts/setup_hellovr.sh"; exit 1; }

# Process hygiene, then a fresh monado-service (named pipe derives from %TEMP%).
taskkill //F //IM hellovr_dx12.exe //IM monado-service.exe >/dev/null 2>&1
sleep 1
"${ROOT}/third_party/monado/bin/monado-service.exe" > "${CAP_DIR}/monado_service.log" 2>&1 &
sleep 3

# hellovr is an SDL app (no stdin feeder needed; graceful shutdown is WM_CLOSE, not stdin EOF).
( cd "$APP_DIR" && ./hellovr_dx12.exe > "${CAP_DIR}/hellovr.log" 2>&1 ) &

ok=0
for i in $(seq 1 30); do
  if netstat -ano 2>/dev/null | grep -q "127.0.0.1:52700 .*LISTENING"; then ok=1; break; fi
  sleep 1
done
echo "[openvr-integration] control channel LISTEN ok=$ok"

rc=1
if [ "$ok" = "1" ]; then
  sleep 5              # let OpenComposite submit real frames before asserting framesObserved
  node "$CLIENT" 52700; rc=$?
else
  echo "[openvr-integration] control channel never came up"
  tail -15 "${CAP_DIR}/hellovr.log" 2>/dev/null
fi

# Graceful-teardown gate: WM_CLOSE (taskkill without /F) lets hellovr run its own shutdown ->
# VR_Shutdown -> OpenComposite xrDestroyInstance -> layer cleanup markers (same grep as the
# hello_xr suite). If the markers never appear, report FAIL (was SKIP when unproven; now
# demonstrated to PASS reliably, so failure is a regression).
graceful="skipped (asserts did not pass)"
if [ "$ok" = "1" ] && [ "$rc" = "0" ]; then
  LOG_G="$(echo "${CAP_DIR}/vr_agent_layer.log" | tr '\\' '/')"
  taskkill //IM hellovr_dx12.exe >/dev/null 2>&1
  graceful="FAIL (WM_CLOSE did not reach clean xrDestroyInstance within 20s; force-kill fallback)"
  for i in $(seq 1 20); do
    if grep -q "xrDestroyInstance -- stopping control channel" "$LOG_G" 2>/dev/null \
       && grep -q "control_channel: accept loop exited" "$LOG_G" 2>/dev/null; then
      graceful="PASS"; break
    fi
    sleep 1
  done
  if [[ "$graceful" == FAIL* ]]; then rc=1; fi
fi
echo "[openvr-integration] graceful teardown: $graceful"

taskkill //F //IM hellovr_dx12.exe //IM monado-service.exe >/dev/null 2>&1
echo "[openvr-integration] exit rc=$rc (captures + logs in ${CAP_DIR})"
exit $rc
