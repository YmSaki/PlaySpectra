#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# End-to-end "VR Playwright" loop verification (WSL2 / Linux): OPERATE -> app re-renders -> OBSERVE,
# and assert the observation changed. This ties together every PlaySpectra piece:
#
#   The playspectra Go control plane injects a head pose via the Monado adapter control channel
#   (:52702) -> an ordinary OpenXR app (hello_xr -g Vulkan2) reads the new pose and re-renders a
#   different view -> the PlaySpectra OpenXR layer (loaded into the app) captures the rendered left
#   eye to PNG -> we assert the post-injection PNG DIFFERS from the baseline PNG, proving the capture
#   tracks the injected state (not a fixed/blank buffer) and that operate+observe compose.
#
# Prerequisites (build first; see docs/getting-started-linux.md):
#   - Monado fork built with the playspectra driver:  MONADO_BUILD/openxr_monado-dev.json
#     (or a release package: set XR_RUNTIME_JSON=<pkg>/runtime/share/openxr/1/openxr_monado.json and
#     MONADO_SERVICE=<pkg>/runtime/bin/monado-service; the package runtime is service-based, so this
#     script then starts monado-service itself, with XDG_RUNTIME_DIR set and a non-/dev/null stdin)
#   - hello_xr (OpenXR-SDK-Source) built with Vulkan:  HELLOXR
#   - PlaySpectra layer built for this platform:        LAYER_SO  (see layer/, cmake -B build)
#   - A software or hardware Vulkan ICD (WSL2: lavapipe).  VK_ICD override via VK_ICD_FILENAMES.
#
# All paths are overridable via env; defaults match the WSL2 bring-up used during development.
set -u
MONADO_BUILD="${MONADO_BUILD:-$HOME/monado-playspectra/build}"
HELLOXR="${HELLOXR:-$HOME/OpenXR-SDK-Source/build/src/tests/hello_xr/hello_xr}"
LAYER_SO="${LAYER_SO:-$HOME/layer_build/playspectra_layer.so}"
TOOLS="${TOOLS:-$(cd "$(dirname "$0")/../tools" && pwd)}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/lib_playspectra.sh"
# lavapipe's ICD file is lvp_icd.x86_64.json on Ubuntu 22.04 and lvp_icd.json on 24.04: use whichever exists.
if [ -n "${VK_ICD_FILENAMES:-}" ]; then
  VK_ICD="$VK_ICD_FILENAMES"
else
  VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
  [ -e "$VK_ICD" ] || VK_ICD=/usr/share/vulkan/icd.d/lvp_icd.json
fi
RUNTIME_JSON="${XR_RUNTIME_JSON:-$MONADO_BUILD/openxr_monado-dev.json}"
MONADO_SERVICE="${MONADO_SERVICE:-}"
WORK="${WORK:-$HOME/ps_e2e}"

for f in "$RUNTIME_JSON" ${MONADO_SERVICE:+"$MONADO_SERVICE"} "$HELLOXR" "$LAYER_SO" \
         "$TOOLS/scenarios/big_view_change.json"; do
  [ -e "$f" ] || { echo "MISSING PREREQ: $f"; exit 5; }
done

# -x matches the process name exactly; -f would also match (and kill) a caller whose command line
# merely contains "hello_xr", e.g. `HELLOXR=.../hello_xr bash scripts/e2e_playwright_loop.sh`.
pkill -x hello_xr 2>/dev/null; sleep 1
MANDIR="$WORK/manifest"; CAPS="$WORK/caps"
mkdir -p "$MANDIR" "$CAPS"; rm -rf "$CAPS"/recording_* 2>/dev/null
cat > "$MANDIR/XrApiLayer_playspectra.json" <<JSON
{ "file_format_version": "1.0.0",
  "api_layer": { "name": "XR_APILAYER_playspectra", "library_path": "$LAYER_SO",
    "api_version": "1.0", "implementation_version": "1", "description": "PlaySpectra layer" } }
JSON

export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1 XR_RUNTIME_JSON="$RUNTIME_JSON"
export VK_ICD_FILENAMES="$VK_ICD"

SVC=""
if [ -n "$MONADO_SERVICE" ]; then
  # Without XDG_RUNTIME_DIR, monado-service uses a literal "~/.cache/monado.pid" and fails with a
  # misleading "already running"; with stdin on /dev/null, its epoll on stdin fails. Pipe a sleep in.
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$WORK/xdg}"
  mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
  SVCLOG="$WORK/monado-service.log"
  sleep 60 | "$MONADO_SERVICE" > "$SVCLOG" 2>&1 &
  SVC=$!
  trap 'kill "$SVC" 2>/dev/null; pkill -x monado-service 2>/dev/null' EXIT
  sleep 2
  pgrep -x monado-service >/dev/null || { echo "SERVICE_DIED"; tail -15 "$SVCLOG"; exit 6; }
fi
export XR_API_LAYER_PATH="$MANDIR" XR_ENABLE_API_LAYERS=XR_APILAYER_playspectra
export PLAYSPECTRA_CAPTURE_TEST=8 PLAYSPECTRA_CAPTURE_DIR="$CAPS" XRT_LOG=warn

LOG="$WORK/helloxr.log"; rm -f "$LOG"
timeout 40 bash -c "sleep 36 | '$HELLOXR' -g Vulkan2" > "$LOG" 2>&1 &
APP=$!
ps_run internal wait-tcp --address 127.0.0.1:52702 --timeout 18s --interval 300ms || {
  echo "CTRL_TIMEOUT"
  kill "$APP" 2>/dev/null
  wait "$APP" 2>/dev/null
  exit 2
}
kill -0 "$APP" 2>/dev/null || { echo APP_DIED; tail -15 "$LOG"; exit 3; }

echo "=== baseline (default state, 6s) ==="
sleep 6
RECDIR=$(find "$CAPS" -maxdepth 1 -type d -name 'recording_*' | head -1)
[ -z "$RECDIR" ] && { echo "NO_RECDIR"; tail -20 "$LOG"; kill "$APP" 2>/dev/null; exit 4; }
BASE_LAST=$(ls "$RECDIR"/rec_*.png 2>/dev/null | sort | tail -1)
echo "baseline last: $(basename "$BASE_LAST" 2>/dev/null)"

echo "=== inject big_view_change (held, no reset) ==="
ps_run run "$TOOLS/scenarios/big_view_change.json" --rate 60

echo "=== post (5s) ==="
sleep 5
POST_LAST=$(ls "$RECDIR"/rec_*.png 2>/dev/null | sort | tail -1)
echo "post last: $(basename "$POST_LAST" 2>/dev/null)"
kill "$APP" 2>/dev/null; wait "$APP" 2>/dev/null

ps_run internal compare-captures --recording "$RECDIR" --baseline "$BASE_LAST" --post "$POST_LAST"
RC=$?
echo "e2e rc=$RC"
exit "$RC"
