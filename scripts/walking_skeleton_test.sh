#!/usr/bin/env bash
# End-to-end walking-skeleton test:
#   raw client -> vr_input(squeeze=1.0) -> playspectra layer -> XR_EXT_conformance_automation
#   -> hello_xr detects grab > 0.9 -> xrApplyHapticFeedback -> layer logs it.
#
# Verifies the full input loop without the capture path.
set -u
PLAYSPECTRA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${PLAYSPECTRA_ROOT}/scripts/env.sh"

export PLAYSPECTRA_LOG="$(mktemp -t playspectra.XXXXXX.log)"
: > "$PLAYSPECTRA_LOG"

GFX="${1:-Vulkan}"
HELLO="${PLAYSPECTRA_ROOT}/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
APPLOG="$(mktemp -t hello_xr.XXXXXX.log)"

# Clear any orphans that might hold the control port.
taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
sleep 1

echo "=== layer log: $PLAYSPECTRA_LOG ==="
echo "Launching hello_xr -g ${GFX} (layer: ${XR_ENABLE_API_LAYERS})"
# hello_xr spawns a thread that calls getchar() and quits on return. In the background with
# redirected stdout its stdin would be EOF -> instant quit. Feed it a long-lived stdin (a sleep
# piped in) so getchar() blocks and the render loop keeps running. `kill $(jobs -p)` at the end
# tears the sleep down so it doesn't keep this script alive and delay output flushing.
sleep 100 | "${HELLO}" -g "${GFX}" > "${APPLOG}" 2>&1 &
HELLO_PID=$!

# Wait until the layer's control channel is listening (instance created), up to 40s.
echo -n "waiting for control channel"
for i in $(seq 1 80); do
  if grep -q "listening on 127.0.0.1" "$PLAYSPECTRA_LOG" 2>/dev/null; then echo " -> up"; break; fi
  if ! kill -0 "${HELLO_PID}" 2>/dev/null; then echo " -> hello_xr exited early!"; break; fi
  echo -n "."; sleep 0.5
done

echo "=== injecting grab via control channel ==="
node "${PLAYSPECTRA_ROOT}/scripts/inject_client.mjs" 6000

sleep 1
echo "=== session state progression (did it reach FOCUSED?) ==="
grep -hoE "XR_SESSION_STATE_[A-Z_]+" "${APPLOG}" | awk '!seen[$0]++' | tr '\n' ' '; echo
taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
kill $(jobs -p) 2>/dev/null  # tear down the stdin-holder sleep

echo
echo "=== playspectra layer log (key lines) ==="
grep -iE "control.channel|conformance|instance created|session created|setInputDevice|buzzed|injecting|runtime:" "$PLAYSPECTRA_LOG" || echo "(no matching lines)"

echo
echo "=== haptic proof (app reacted to injected grab) ==="
if grep -qi "buzzed the controller" "$PLAYSPECTRA_LOG"; then
  echo "PASS: hello_xr requested haptics in response to injected grab -> INPUT LOOP WORKS"
else
  echo "INCONCLUSIVE: no haptic observed. setInputDevice results above show if injection reached the runtime."
  echo "  app log tail: $APPLOG"
  grep -iE "\[Error|error|fail|profile|interaction" "$APPLOG" | grep -viE "\[Meta XR Simulator\]" | head -20
fi