#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Regression test for e2e_playwright_loop.sh process handling (PR #26 review): the script must judge
# success only by the processes it started, and must never signal anything else. No GPU, runtime, or
# OpenXR app is needed: every case fails before hello_xr would start.
#
#   A  an unrelated process named monado-service is running and MONADO_SERVICE exits 42
#      -> SERVICE_DIED (rc 6); the unrelated process is still alive. The caller's command line
#      contains "hello_xr" and must survive (issue #25).
#   B  something already listens on 127.0.0.1:52702 -> PORT_BUSY (rc 7); the listener is still alive.
#   C  a stale monado_comp_ipc socket exists and MONADO_SERVICE stays alive without creating a new
#      one -> SERVICE_DIED (rc 6), not a false "ready".
# B and C need OpenBSD-style nc (-l / -lU); without it they are reported as SKIP.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
FAIL=0; SKIP=0; PASS=0
pids=()
finish() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$T"; }
trap finish EXIT

ok()   { echo "PASS $1"; PASS=$((PASS+1)); }
bad()  { echo "FAIL $1"; FAIL=$((FAIL+1)); }
skip() { echo "SKIP $1"; SKIP=$((SKIP+1)); }

# Prerequisite files only need to exist; nothing gets far enough to use them.
touch "$T/runtime.json" "$T/layer.so"
mkdir -p "$T/fake_hello_xr"; HELLO="$T/fake_hello_xr/hello_xr"; touch "$HELLO"
run_e2e() { # $1 = MONADO_SERVICE, $2 = work dir; prints the script output, returns its rc
  bash -c 'XR_RUNTIME_JSON="$1" MONADO_SERVICE="$2" LAYER_SO="$3" HELLOXR="$4" WORK="$5" \
    XDG_RUNTIME_DIR="$5/xdg" bash "$6/scripts/e2e_playwright_loop.sh"  # caller mentions hello_xr' \
    _ "$T/runtime.json" "$1" "$T/layer.so" "$HELLO" "$2" "$ROOT"
}

# --- A ---------------------------------------------------------------------------------------
cp "$(command -v sleep)" "$T/monado-service"
"$T/monado-service" 300 & EXISTING=$!; pids+=("$EXISTING")
printf '#!/bin/sh\nexit 42\n' > "$T/failing-service"; chmod +x "$T/failing-service"
out="$(run_e2e "$T/failing-service" "$T/wa" 2>&1)"; rc=$?
if [ "$rc" -eq 6 ] && grep -q SERVICE_DIED <<<"$out"; then ok "A: failed service detected (rc=6)"
else bad "A: expected SERVICE_DIED rc=6, got rc=$rc: $(tail -3 <<<"$out")"; fi
if kill -0 "$EXISTING" 2>/dev/null; then ok "A: unrelated monado-service left running"
else bad "A: unrelated monado-service was killed"; fi

if ! nc -h 2>&1 | grep -q -- '-U'; then
  skip "B: OpenBSD nc not available"; skip "C: OpenBSD nc not available"
else
  # --- B -------------------------------------------------------------------------------------
  nc -lk 127.0.0.1 52702 </dev/null >/dev/null 2>&1 & LISTENER=$!; pids+=("$LISTENER")
  sleep 0.5
  out="$(run_e2e "$T/failing-service" "$T/wb" 2>&1)"; rc=$?
  if [ "$rc" -eq 7 ] && grep -q PORT_BUSY <<<"$out"; then ok "B: busy control port refused (rc=7)"
  else bad "B: expected PORT_BUSY rc=7, got rc=$rc: $(tail -3 <<<"$out")"; fi
  if kill -0 "$LISTENER" 2>/dev/null; then ok "B: existing listener left running"
  else bad "B: existing listener was killed"; fi
  kill "$LISTENER" 2>/dev/null; wait "$LISTENER" 2>/dev/null; sleep 0.5

  # --- C -------------------------------------------------------------------------------------
  mkdir -p "$T/wc/xdg"; chmod 700 "$T/wc/xdg"
  nc -lU "$T/wc/xdg/monado_comp_ipc" </dev/null >/dev/null 2>&1 & S=$!; sleep 0.5; kill "$S" 2>/dev/null
  wait "$S" 2>/dev/null
  if [ ! -S "$T/wc/xdg/monado_comp_ipc" ]; then
    skip "C: could not leave a stale socket behind with this nc"
  else
    printf '#!/bin/sh\necho $$ > "%s"\nexec sleep 30\n' "$T/silent.pid" > "$T/silent-service"
    chmod +x "$T/silent-service"
    out="$(run_e2e "$T/silent-service" "$T/wc" 2>&1)"; rc=$?
    if [ "$rc" -eq 6 ] && grep -q SERVICE_DIED <<<"$out"; then ok "C: stale socket not taken as ready (rc=6)"
    else bad "C: expected SERVICE_DIED rc=6, got rc=$rc: $(tail -3 <<<"$out")"; fi
    # The script must reap what it started: the silent service may not outlive it.
    SPID="$(cat "$T/silent.pid" 2>/dev/null)"
    if [ -z "$SPID" ]; then bad "C: silent service never started"
    elif kill -0 "$SPID" 2>/dev/null; then bad "C: started service left running"; kill "$SPID"
    else ok "C: started service reaped"; fi
  fi
fi

echo "summary: pass=$PASS fail=$FAIL skip=$SKIP"
[ "$FAIL" -eq 0 ]
