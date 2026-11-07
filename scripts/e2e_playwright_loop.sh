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
#     (a manifest pointing at LAYER_SO is generated; set LAYER_MANIFEST_DIR to load an existing
#     manifest directory instead, e.g. the release package's layer/)
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

MANDIR="$WORK/manifest"; CAPS="$WORK/caps"
mkdir -p "$MANDIR" "$CAPS"; rm -rf "$CAPS"/recording_* 2>/dev/null

# Only processes this run started are ever signalled: no pkill/pgrep by name, which would also hit a
# caller whose command line contains "hello_xr" (issue #25) or an unrelated monado-service.
SVC=""; SVC_STDIN=""; APP=""; APP_STDIN=""
cleanup() {
  for p in "$APP" "$APP_STDIN" "$SVC" "$SVC_STDIN"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
  for p in "$APP" "$APP_STDIN" "$SVC" "$SVC_STDIN"; do [ -n "$p" ] && wait "$p" 2>/dev/null; done
  rm -f "$WORK/svc.stdin" "$WORK/app.stdin"
}
trap cleanup EXIT

# The control channel must belong to the runtime/app started below. If something already listens on
# :52702 (a leftover hello_xr, another runtime), wait-tcp would succeed against it and this run would
# test the wrong process -- refuse instead of killing it.
if (exec 3<>/dev/tcp/127.0.0.1/52702) 2>/dev/null; then
  echo "PORT_BUSY: 127.0.0.1:52702 is already in use; stop the process holding it and re-run"
  exit 7
fi

# stdin for a background process that must not see /dev/null: a FIFO fed by a sleep whose PID we keep.
hold_stdin() { rm -f "$1"; mkfifo "$1"; sleep "$2" > "$1" & }

if [ -z "${LAYER_MANIFEST_DIR:-}" ]; then
  cat > "$MANDIR/XrApiLayer_playspectra.json" <<JSON
{ "file_format_version": "1.0.0",
  "api_layer": { "name": "XR_APILAYER_playspectra", "library_path": "$LAYER_SO",
    "api_version": "1.0", "implementation_version": "1", "description": "PlaySpectra layer" } }
JSON
fi

export PLAYSPECTRA_ENABLE=1 XRT_COMPOSITOR_NULL=1 XR_RUNTIME_JSON="$RUNTIME_JSON"
export VK_ICD_FILENAMES="$VK_ICD"

if [ -n "$MONADO_SERVICE" ]; then
  # Without XDG_RUNTIME_DIR, monado-service uses a literal "~/.cache/monado.pid" and fails with a
  # misleading "already running"; with stdin on /dev/null, its epoll on stdin fails.
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$WORK/xdg}"
  mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
  SVCLOG="$WORK/monado-service.log"
  hold_stdin "$WORK/svc.stdin" 120; SVC_STDIN=$!
  touch "$WORK/svc.started"
  "$MONADO_SERVICE" < "$WORK/svc.stdin" > "$SVCLOG" 2>&1 &
  SVC=$!
  # Ready = OUR service is alive and has (re)created its IPC socket since we started it. Liveness is
  # checked on our PID only, and the -nt test rejects a stale socket left by an earlier run, so
  # another monado-service or a leftover file cannot satisfy this.
  SOCK="$XDG_RUNTIME_DIR/monado_comp_ipc"
  svc_ready() { [ -S "$SOCK" ] && [ "$SOCK" -nt "$WORK/svc.started" ]; }
  for _ in $(seq 1 50); do
    kill -0 "$SVC" 2>/dev/null || break
    svc_ready && break
    sleep 0.2
  done
  if ! kill -0 "$SVC" 2>/dev/null || ! svc_ready; then
    echo "SERVICE_DIED"; tail -15 "$SVCLOG"; exit 6
  fi
fi
if [ -n "${LAYER_MANIFEST_DIR:-}" ]; then
  export XR_API_LAYER_PATH="$LAYER_MANIFEST_DIR"   # e.g. the release package's own layer/ manifest
else
  export XR_API_LAYER_PATH="$MANDIR"
fi
export XR_ENABLE_API_LAYERS=XR_APILAYER_playspectra
export PLAYSPECTRA_CAPTURE_TEST=8 PLAYSPECTRA_CAPTURE_DIR="$CAPS" XRT_LOG=warn

LOG="$WORK/helloxr.log"; rm -f "$LOG"
hold_stdin "$WORK/app.stdin" 36; APP_STDIN=$!
timeout 40 "$HELLOXR" -g Vulkan2 < "$WORK/app.stdin" > "$LOG" 2>&1 &
APP=$!
ps_run internal wait-tcp --address 127.0.0.1:52702 --timeout 18s --interval 300ms || {
  echo "CTRL_TIMEOUT"
  exit 2
}
kill -0 "$APP" 2>/dev/null || { echo APP_DIED; tail -15 "$LOG"; exit 3; }

echo "=== baseline (default state, 6s) ==="
sleep 6
RECDIR=$(find "$CAPS" -maxdepth 1 -type d -name 'recording_*' | head -1)
[ -z "$RECDIR" ] && { echo "NO_RECDIR"; tail -20 "$LOG"; exit 4; }
BASE_LAST=$(ls "$RECDIR"/rec_*.png 2>/dev/null | sort | tail -1)
echo "baseline last: $(basename "$BASE_LAST" 2>/dev/null)"

echo "=== inject big_view_change (held, no reset) ==="
ps_run run "$TOOLS/scenarios/big_view_change.json" --rate 60

echo "=== post (5s) ==="
sleep 5
POST_LAST=$(ls "$RECDIR"/rec_*.png 2>/dev/null | sort | tail -1)
echo "post last: $(basename "$POST_LAST" 2>/dev/null)"
cleanup; APP=""; APP_STDIN=""; SVC=""; SVC_STDIN=""

ps_run internal compare-captures --recording "$RECDIR" --baseline "$BASE_LAST" --post "$POST_LAST"
RC=$?
echo "e2e rc=$RC"
exit "$RC"
