#!/usr/bin/env bash
# End-to-end verification against a REAL ENGINE app: VRAppDummyGame (Godot 4.7, OpenXR, D3D12) driven on
# the Windows-built Monado (PlaySpectra virtual-device driver, operate channel :52702) while the
# playspectra capture layer (:52700) observes the frames it renders.
#
# Why this exists alongside run_hello_xr_monado.sh: hello_xr is an SDK sample that renders and nothing
# else, so "the injection reached the app" can only be read back through our own capture layer.
# VRAppDummyGame self-reports what it received over its [VRTEST] stdout contract, and it contains real
# interactables (button / grabbable cube / lever), so this harness can assert that the ENGINE's own game
# logic ran. It is the first real-engine (non-sample) app in the regression set.
#
# Machine value: playspectra verify vrapp's rc -- 0 iff every assertion passed. SKIPs (absent app
# or absent capture layer) are printed and counted, and never make the rc non-zero.
#
# Unlike mstack_up, the app is launched by the Go harness, not here: the [VRTEST] contract needs the
# app's stdin (requests) and stdout (events), so the process must be owned by whoever speaks it.
#
# Prereqs (see CLAUDE.local.md "Windows で Monado(PlaySpectra driver) をビルドする方法"):
#   - monado built:  cmake --build <build-win> --target monado-service cli openxr_monado
#   - layer built:   layer/manifest/playspectra_layer.dll (cmake --build layer/build)
#   - the app exported from Godot 4.7:
#       Godot_v4.7.1-stable_mono_win64_console.exe --headless --path <VRAppDummyGame> \
#         --export-release "Windows Desktop" build/vrapp.exe
#     The app lives in its OWN repository (sibling directory by default; PLAYSPECTRA_VRAPP_EXE overrides).
# Usage: scripts/run_vrapp_monado.sh
set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib_monado_stack.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_playspectra.sh"

# D3D12 because that is what the app itself selects (project.godot: rendering_device/driver.windows).
mstack_env D3D12

APP="${PLAYSPECTRA_VRAPP_EXE:-$MSTACK_ROOT/../VRAppDummyGame/build/vrapp.console.exe}"
if [ ! -f "$APP" ]; then
  echo "SKIP: VRAppDummyGame not built at $APP"
  echo "      export it from Godot 4.7 (see the prereqs in this script's header), or set PLAYSPECTRA_VRAPP_EXE"
  exit 0
fi
export PLAYSPECTRA_VRAPP_EXE="$APP"

mstack_service_up || exit $?

ps_run verify vrapp --exe "$APP" --port "$PLAYSPECTRA_MONADO_PORT" --capture-port "$PLAYSPECTRA_PORT"
RC=$?

mstack_service_down
echo "=== service log (playspectra / client / error) ==="
grep -iE "playspectra|client_connected|ipc|error|fail" "$MSTACK_SVCLOG" 2>/dev/null | tail -15
echo "VRAPP_E2E_RC=$RC"
exit $RC
