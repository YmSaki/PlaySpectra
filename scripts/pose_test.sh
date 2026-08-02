#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Launch hello_xr under the playspectra layer, drive a "前ならえ" controller pose via the control
# channel (pose_client.mjs), and capture screenshots. POSE_X/POSE_Y/POSE_Z/HOLD_MS pass through.
set -u
PLAYSPECTRA_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${PLAYSPECTRA_ROOT}/scripts/env.sh"

export PLAYSPECTRA_LOG="$(mktemp -t playspectra.XXXXXX.log)"
: > "$PLAYSPECTRA_LOG"
GFX="${1:-Vulkan}"
HELLO="${PLAYSPECTRA_ROOT}/layer/build/_deps/openxr_sdk-build/src/tests/hello_xr/hello_xr.exe"
APPLOG="$(mktemp -t hello_xr.XXXXXX.log)"

taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
sleep 1

echo "=== layer log: $PLAYSPECTRA_LOG ==="
echo "Launching hello_xr -g ${GFX}; pose X=${POSE_X:-0.2} Y=${POSE_Y:--0.2} Z=${POSE_Z:--0.5}"
sleep 100 | "${HELLO}" -g "${GFX}" > "${APPLOG}" 2>&1 &

echo -n "waiting for control channel"
for i in $(seq 1 80); do
  if grep -q "listening on 127.0.0.1" "$PLAYSPECTRA_LOG" 2>/dev/null; then echo " -> up"; break; fi
  echo -n "."; sleep 0.5
done

node "${PLAYSPECTRA_ROOT}/scripts/pose_client.mjs"

sleep 1
taskkill //F //IM hello_xr.exe 2>/dev/null
taskkill //F //IM MetaXRSimulator.exe 2>/dev/null
kill $(jobs -p) 2>/dev/null

echo
echo "=== layer pose/location log ==="
grep -iE "setInputDeviceLocation|LocalSpace|pose dropped|setInputDeviceActive" "$PLAYSPECTRA_LOG" | tail -12
