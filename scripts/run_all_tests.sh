#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Run every environment-independent unit test with one command and exit non-zero if any fail:
#   - mcp/     node:test   (pure quaternion/vector math)                            -> 42 cases
#   - layer/   gtest/ctest (capture/pixel/pose helpers + xr_math/dxgi/pose_override) -> 92 Win / 83 non-Win (DXGI is WIN32-only)
#   - Go       go test     (Core, protocol, Scenario, MCP, record/replay, setup helpers, probes)
#   - devicecore playspectra_proto (gcc standalone: frame verdict + content_sig)      -> 43 cases
#
# WHY: GitHub Actions runs the environment-independent MCP and layer suites, while this script adds
# the Go control-plane and Monado-protocol suites as a local one-command gate. It does NOT give a clean,
# independent environment, so it cannot catch every "works on my machine" issue (locale/dep/platform).
#
# SKIP discipline (review-checklist.md lens 6 / rules/setup-scripts.md #4): a missing Go/gcc/
# submodule SKIPs only that suite -- it never fails the run (rc is decided by fail alone) -- but it
# is tallied and named in the final summary so "green" can never look identical to "green, but N
# suites never ran". Do not silently fold a SKIP into a plain "ALL GREEN".
#
# Fast + hermetic: no GPU / HMD / running service / network needed. mcp ~1s; layer ~20s (build reused
# when already present). Assumes the standard single-config layer/build (as built locally). Run from
# anywhere:  bash scripts/run_all_tests.sh
set -uo pipefail
cd "$(dirname "$0")/.." # repo root
fail=0
skipped=0
skipped_names=""
note_skip() { skipped=$((skipped + 1)); skipped_names="$skipped_names $1"; echo "  ($1 -- skipped: $2)"; }

echo "== mcp: node:test =="
if [ -d mcp ]; then
  (
    cd mcp
    [ -d node_modules ] || npm ci
    npm test
  ) || { echo ">> mcp tests FAILED"; fail=1; }
else
  note_skip "mcp" "mcp/ not found"
fi

echo ""
echo "== layer: gtest / ctest =="
BUILD="layer/build"
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "  configuring (host unit tests on)..."
  cmake -S layer -B "$BUILD" -DPLAYSPECTRA_BUILD_TESTS=ON || { echo ">> layer configure FAILED"; fail=1; }
fi
if [ -f "$BUILD/CMakeCache.txt" ]; then
  cmake --build "$BUILD" --target playspectra_test || { echo ">> layer build FAILED"; fail=1; }
  # -E loader_test: the fetched OpenXR SDK's own test is not ours to run (mirrors ci.yml). The
  # BUILD_TESTING=OFF seed in layer/CMakeLists.txt is the primary guard; this is belt-and-suspenders.
  ctest --test-dir "$BUILD" -E loader_test --output-on-failure || { echo ">> layer tests FAILED"; fail=1; }
fi

echo ""
echo "== go: control plane =="
if command -v go >/dev/null 2>&1; then
  go test ./... || { echo ">> go tests FAILED"; fail=1; }
  go vet ./... || { echo ">> go vet FAILED"; fail=1; }
  mkdir -p build
  CGO_ENABLED=0 go build -o build/playspectra-test ./cmd/playspectra || { echo ">> cgo-free build FAILED"; fail=1; }
else
  note_skip "go" "go not found"
fi

echo ""
echo "== devicecore: playspectra_proto (gcc standalone) =="
PROTO="devicecore"
CJSON="runtime/monado-playspectra/src/external/cjson"
if command -v gcc >/dev/null 2>&1 && [ -f "$PROTO/playspectra_proto_test.c" ] && [ -f "$CJSON/cjson/cJSON.c" ]; then
  mkdir -p build
  # No Monado deps: proto parse/verdict/content_sig test compiles from proto.c + bundled cJSON only
  # (cJSON source itself still lives in the Monado submodule -- hence the submodule presence check).
  if gcc -I "$CJSON" -I "$PROTO" \
         "$PROTO/playspectra_proto_test.c" "$PROTO/playspectra_proto.c" \
         "$CJSON/cjson/cJSON.c" -o build/proto_test.exe; then
    ./build/proto_test.exe || { echo ">> proto tests FAILED"; fail=1; }
  else
    echo ">> proto build FAILED"; fail=1
  fi
else
  note_skip "proto" "gcc or submodule cJSON not found"
fi

echo ""
if [ "$fail" -ne 0 ]; then
  echo "SOME TESTS FAILED"
elif [ "$skipped" -ne 0 ]; then
  echo "GREEN WITH SKIPS ($skipped suite(s) skipped:$skipped_names -- not a full run; install the missing toolchain for a complete gate)"
else
  echo "ALL GREEN"
fi
exit "$fail"
