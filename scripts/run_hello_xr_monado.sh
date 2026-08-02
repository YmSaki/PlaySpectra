#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# End-to-end verification: a real OpenXR app (hello_xr) driven on the Windows-built Monado runtime
# (PlaySpectra virtual-device driver, control channel :52702) while the playspectra capture layer
# (:52700) observes the frames it renders -- the full PlaySpectra stack against a real engine app, on
# Windows real GPU, headless. Stack bring-up is shared via lib_monado_stack.sh.
#
# What this proves (machine value = integration_hello_xr.mjs rc, 0 iff every assertion passes):
#   - Monado's client<->service IPC works on Windows (openxr_monado.dll IPC client -> monado-service.exe)
#   - the D3D11/D3D12/Vulkan client-compositor interop path renders real frames captured non-degenerately
#   - the :52702 operate channel drives the Monado-side virtual HMD, reaching the app's xrLocateViews
#
# Headless: XRT_COMPOSITOR_NULL=1. The app still renders into real swapchains and the layer captures
# them at xrEndFrame BEFORE the compositor would present -- "no display" does not mean "no frames". The
# main (display) compositor is a separate path and is NOT exercised here.
#
# Prereqs (see CLAUDE.local.md "Windows で Monado(PlaySpectra driver) をビルドする方法"):
#   - monado built:  cmake --build <build-win> --target monado-service cli openxr_monado
#   - hello_xr: MSVC D3D build for D3D11/D3D12, layer-bundled MinGW build for Vulkan (auto-picked).
# Usage: scripts/run_hello_xr_monado.sh [gfx=D3D11] [secs=60]
#   gfx: D3D11 | D3D12 | Vulkan | all  ('all' runs every graphics API in turn and gates on the combined
#        result -- the full-coverage regression the north star requires: all three graphics bindings)
#   secs: hard-kill watchdog for the app. MUST exceed the ~15-26s test (assertions + coupling), or the
#         watchdog races the test and kills the app mid-assertion (this flaked 'all' at secs=26 under
#         back-to-back load). Normal teardown kills the app the instant the test returns, so a generous
#         watchdog costs nothing -- the run returns when the test does, not at `secs`.
set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib_monado_stack.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib_playspectra.sh"
GFX="${1:-D3D11}"; SECS="${2:-60}"

# 'all': re-invoke self once per graphics API and combine the verdicts (fail if any API fails).
if [ "$GFX" = "all" ]; then
  overall=0
  for api in D3D11 D3D12 Vulkan; do
    echo "########## $api ##########"
    "$0" "$api" "$SECS"; rc=$?
    echo "########## $api rc=$rc ##########"
    [ "$rc" != "0" ] && overall=1
  done
  echo "ALL_APIS_RC=$overall"
  exit $overall
fi

mstack_env "$GFX"
export VR_GFX_API="$GFX"   # integration_hello_xr.mjs asserts capture.api == this

RC=99
if mstack_up "$GFX" "$SECS"; then
  echo "=== integration_hello_xr.mjs (layer observation of a real app on Monado) ==="
  node scripts/integration_hello_xr.mjs 52700
  IRC=$?
  echo "=== runtime coupling: :52702 operate drives Monado virtual HMD -> app xrLocateViews (:52700) ==="
  ps_run verify coupling --capture-port 52700 --port 52702
  CRC=$?
  # verdict gates on BOTH: layer observation of a real app AND runtime-level operate reaching the app.
  RC=$(( IRC != 0 ? IRC : CRC ))
else
  echo "layer channel never came up"
fi

mstack_down
echo "=== app log (loader/runtime/xr lines) ==="
grep -iE "runtime|loader|instance|session|swapchain|d3d|error|fail|compositor|semaphore" "$MSTACK_APPLOG" 2>/dev/null | tail -30
echo "=== service log tail ==="
grep -iE "playspectra|client_connected|compositor|error|fail" "$MSTACK_SVCLOG" 2>/dev/null | tail -15
echo "INTEGRATION_RC=$RC"
exit $RC
