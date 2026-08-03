#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Launch VRDevApp (Godot 4.7.1 OpenXR app, bin/VRDevApp.exe) under the playspectra layer on the current
# runtime (metasim by default), wait for the control channel, run vrdevapp_client.mjs, then tear
# down. VRDevApp is the real-engine verification target (not hello_xr): its floating
# input/coordinate window and North/East/West/South wall labels are the observation oracle.
#
# Godot is a GUI app with its own loop (unlike hello_xr it does NOT quit on stdin EOF), so we launch
# it backgrounded and taskkill on timeout -- no stdin-holding sleep needed.
#
# Env passthrough to the client: MOVE_INPUT (binding suffix, e.g. "thumbstick"), MOVE_HAND
# (left|right, default left), MOVE_X, MOVE_Y, MOVE_MS. With MOVE_INPUT unset the client only does
# discovery (status + actions dump + baseline screenshot).
# Usage: scripts/run_vrdevapp.sh [seconds]
set -u
PLAYSPECTRA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${PLAYSPECTRA_ROOT}/scripts/env.sh"

export PLAYSPECTRA_LOG="$(mktemp -t playspectra.XXXXXX.log)"
: > "$PLAYSPECTRA_LOG"
APP="${PLAYSPECTRA_ROOT}/bin/VRDevApp.exe"
APPLOG="$(mktemp -t vrdevapp.XXXXXX.log)"
SECS="${1:-30}"

taskkill //F //IM VRDevApp.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
sleep 1

echo "=== layer log: $PLAYSPECTRA_LOG ==="
echo "=== app log:   $APPLOG ==="
echo "Launching VRDevApp (up to ${SECS}s watchdog)"
"${APP}" > "${APPLOG}" 2>&1 &
APP_PID=$!

# Watchdog: hard-kill if the client hangs so we never leave the app running.
( sleep "$SECS"; taskkill //F //IM VRDevApp.exe 2>/dev/null ) &
WATCH_PID=$!

echo -n "waiting for control channel"
UP=0
for i in $(seq 1 120); do
  if grep -q "listening on 127.0.0.1" "$PLAYSPECTRA_LOG" 2>/dev/null; then echo " -> up"; UP=1; break; fi
  if ! kill -0 "$APP_PID" 2>/dev/null; then echo " -> app exited before channel"; break; fi
  echo -n "."; sleep 0.5
done

CLIENT_RC=0
if [ "$UP" = "1" ]; then
  node "${PLAYSPECTRA_ROOT}/scripts/vrdevapp_client.mjs"
  CLIENT_RC=$?
else
  echo "control channel never came up; skipping client"
  CLIENT_RC=1
fi

sleep 1
taskkill //F //IM VRDevApp.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
kill "$APP_PID" 2>/dev/null
kill "$WATCH_PID" 2>/dev/null

echo ""
echo "=== app log (non-runtime lines, tail) ==="
grep -vE "\[Meta XR Simulator\]" "${APPLOG}" | grep -iE "error|openxr|xr_|vulkan|d3d|initiali|fail" | tail -25
exit $CLIENT_RC
