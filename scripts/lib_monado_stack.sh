#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Shared bring-up for the run_*_monado.sh Windows E2E harnesses. Source it, then:
#   mstack_env  "$GFX"            # sets HELLO selection, VULKAN_SDK, PATH, XR_*/PLAYSPECTRA_* env; cd repo root
#   mstack_up   "$GFX" "$SECS"    # starts monado-service (:52702) + hello_xr w/ layer (:52700);
#                                 # sets SVC_PID/APP_PID and MSTACK_SVCLOG/MSTACK_APPLOG; returns 0 iff :52700 up
#   ... run your client against :52702 (operate) / :52700 (observe) ...
#   mstack_down                   # taskkill + kill the stack
# When the harness must launch the app itself (e.g. it speaks the app's stdin/stdout contract), use the
# service half on its own instead:
#   mstack_service_up / mstack_service_down    # monado-service only; mstack_up is built on these
# GFX: D3D11 | D3D12 | Vulkan. The hello_xr binary is auto-picked: MSVC build for D3D (Vulkan-less),
# the layer-bundled MinGW build for Vulkan. XR_RUNTIME_JSON points THIS process at Monado per-process
# (system ActiveRuntime untouched); XRT_COMPOSITOR_NULL=1 (headless); IPC_IGNORE_VERSION=1 tolerates a
# client/service built at different commits (/utf-8 is build-only, no IPC ABI change).

MSTACK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MSTACK_BW="runtime/monado-playspectra/build-win"
MSTACK_SVC="$MSTACK_BW/src/xrt/targets/service/Release/monado-service.exe"
MSTACK_MANIFEST="$MSTACK_BW/Release/openxr_monado-dev.json"

mstack_hello_bin() {  # $1=GFX -> the hello_xr binary that supports that API
  if [ "$1" = "Vulkan" ] || [ "$1" = "Vulkan2" ]; then
    echo "layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
  else
    echo "third_party/hello_xr_msvc/hello_xr.exe"
  fi
}

mstack_listen() { netstat -ano 2>/dev/null | grep -E ":$1[^0-9].*LISTEN" >/dev/null; }

mstack_env() {  # $1=GFX
  cd "$MSTACK_ROOT" || return 9
  if [ -z "${VULKAN_SDK:-}" ]; then
    VULKAN_SDK="$(ls -d /c/VulkanSDK/*/ 2>/dev/null | sort -V | tail -1)"; VULKAN_SDK="${VULKAN_SDK%/}"
  fi
  local vkbin=""
  [ -n "$VULKAN_SDK" ] && vkbin="$(cygpath -u "$VULKAN_SDK" 2>/dev/null || echo "$VULKAN_SDK")/bin"
  export VULKAN_SDK
  export PATH="${vkbin}:$MSTACK_ROOT/$MSTACK_BW/vcpkg_installed/x64-windows/bin:$MSTACK_ROOT/$MSTACK_BW/src/xrt/targets/service/Release:$MSTACK_ROOT/$MSTACK_BW/src/xrt/targets/openxr/Release:$PATH"
  export XR_RUNTIME_JSON="$MSTACK_ROOT/$MSTACK_MANIFEST"
  export XR_API_LAYER_PATH="$MSTACK_ROOT/layer/manifest" XR_ENABLE_API_LAYERS="XR_APILAYER_playspectra"
  export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1 IPC_IGNORE_VERSION=1
  export PLAYSPECTRA_MONADO_PORT=52702 PLAYSPECTRA_PORT=52700
}

mstack_service_up() {  # -> starts monado-service alone; sets SVC_PID/MSTACK_SVCLOG; 0 iff :52702 up
  # Split out of mstack_up because not every harness wants us to launch the app: a harness that speaks
  # the app's stdin/stdout contract (run_vrapp_monado.sh) must own the app process itself.
  local f i
  for f in "$MSTACK_SVC" "$MSTACK_MANIFEST"; do
    [ -f "$f" ] || { echo "MISSING $f"; return 3; }
  done
  MSTACK_SVCLOG="${MSTACK_SVCLOG:-$(mktemp -t msvc.XXXXXX.log)}"
  echo "service log: $MSTACK_SVCLOG"
  taskkill //F //IM monado-service.exe 2>/dev/null
  # taskkill //F is async: wait for a prior run's ports to actually free before we bind, else the
  # readiness check latches onto the DYING process (this flaked back-to-back 'all' runs).
  for i in $(seq 1 20); do
    if ! mstack_listen 52702 && ! mstack_listen 52700; then break; fi
    sleep 0.5
  done
  sleep 1
  "$MSTACK_SVC" > "$MSTACK_SVCLOG" 2>&1 & SVC_PID=$!
  for i in $(seq 1 40); do mstack_listen 52702 && break; kill -0 "$SVC_PID" 2>/dev/null || break; sleep 0.5; done
  mstack_listen 52702 || { echo "service down"; tail -15 "$MSTACK_SVCLOG"; taskkill //F //IM monado-service.exe 2>/dev/null; return 4; }
  return 0
}

mstack_service_down() {
  taskkill //F //IM monado-service.exe 2>/dev/null
  kill "${SVC_PID:-}" 2>/dev/null
}

mstack_up() {  # $1=GFX $2=SECS -> service + app; sets SVC_PID/APP_PID; returns 0 iff layer :52700 up
  local GFX="$1" SECS="$2" HELLO i LUP=0
  HELLO="$(mstack_hello_bin "$GFX")"
  [ -f "$HELLO" ] || { echo "MISSING $HELLO"; return 3; }
  MSTACK_APPLOG="$(mktemp -t hx.XXXXXX.log)"
  taskkill //F //IM hello_xr.exe 2>/dev/null
  mstack_service_up || return $?
  echo "app log: $MSTACK_APPLOG"
  # hello_xr quits on stdin EOF; hold it open with a sleep pipe. The watchdog is a backstop for a hung
  # test only (must outlast the assertions); normal teardown kills the app the instant the test returns.
  sleep "$((SECS + 10))" | "$HELLO" -g "$GFX" > "$MSTACK_APPLOG" 2>&1 & APP_PID=$!
  ( sleep "$SECS"; taskkill //F //IM hello_xr.exe 2>/dev/null ) &
  for i in $(seq 1 50); do
    mstack_listen 52700 && { LUP=1; break; }
    kill -0 "$APP_PID" 2>/dev/null || break
    sleep 0.4
  done
  echo "layer :52700 up=$LUP"
  [ "$LUP" = "1" ]
}

mstack_down() {
  taskkill //F //IM hello_xr.exe 2>/dev/null
  kill "${APP_PID:-}" 2>/dev/null
  mstack_service_down
}
