#!/usr/bin/env bash
# Flagship "VR Playwright loop" on live Windows Monado: OPERATE a scenario -> app re-renders -> OBSERVE
# the screen -> ASSERT the observation changed. The Windows counterpart of scripts/e2e_playwright_loop.sh
# (which is WSL2/Linux-only). Stands up monado-service (:52702) + hello_xr with the capture layer
# (:52700) via lib_monado_stack.sh, then has server.py run tools/scenarios/capture_assert_demo.json:
# capture a reference via the layer, assert a no-op leaves it stable, drive look+move_head via the
# Monado adapter, assert the captured image changed. All assert logic lives in server.py -- this script
# only stands up the live stack. Machine verdict = server.py rc (0 iff every assert passes).
# Usage: scripts/run_scenario_e2e_monado.sh [gfx=D3D11] [secs=60]  (bring-up shared: lib_monado_stack.sh)
set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib_monado_stack.sh"
GFX="${1:-D3D11}"; SECS="${2:-60}"
SCENARIO="tools/scenarios/capture_assert_demo.json"

mstack_env "$GFX"
[ -f "$SCENARIO" ] || { echo "MISSING $SCENARIO"; exit 3; }

RC=99
if mstack_up "$GFX" "$SECS"; then
  sleep 1
  echo "=== flagship loop: server.py runs $(basename "$SCENARIO") (operate :52702 -> observe :52700 -> assert) ==="
  python tools/playspectra_server.py "$SCENARIO" --port 52702 --capture-port 52700; RC=$?
else
  echo "layer never came up"; grep -iE "error|unsupported" "$MSTACK_APPLOG" 2>/dev/null | tail
fi
mstack_down
echo "SCENARIO_E2E_RC=$RC"
exit $RC
