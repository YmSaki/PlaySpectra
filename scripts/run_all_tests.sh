#!/usr/bin/env bash
# Run every environment-independent unit test with one command and exit non-zero if any fail:
#   - mcp/     node:test   (pure quaternion/vector math)                            -> 42 cases
#   - layer/   gtest/ctest (capture/pixel/pose helpers + xr_math/dxgi/pose_override) -> 92 Win / 83 non-Win (DXGI is WIN32-only)
#   - tools/   unittest    (Scenario Runner interpolation math + wait_for/assert_capture retry logic,
#                           all mocked over localhost sockets -- no Monado/GPU/host app needed)      -> 33 cases
#   - submodule playspectra_proto (gcc standalone: frame verdict + content_sig)       -> 43 cases
# Dep-complete total: 210 (Windows) / 201 (non-Windows, DXGI excluded).
#
# WHY: GitHub Actions (.github/workflows/ci.yml) is billing-blocked on this private repo, so nothing
# auto-runs these. This script is the local stand-in -- it recovers CI's "one command runs everything"
# plus a correct pass/fail gate (a pre-push hook can call it later). It does NOT give a clean,
# independent environment, so it cannot catch "works on my machine" issues (locale/dep/platform) --
# that property only comes back with hosted CI once billing is fixed, or by making the repo public.
#
# SKIP discipline (review-checklist.md lens 6 / rules/setup-scripts.md #4): a missing python/gcc/
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
echo "== tools: PlaySpectra framework math + wait_for/assert_capture retry logic (unittest) =="
PY=""
if command -v python >/dev/null 2>&1; then
  PY=python
elif command -v python3 >/dev/null 2>&1; then
  PY=python3
fi
if [ -n "$PY" ]; then
  "$PY" tools/playspectra_math_test.py || { echo ">> framework math tests FAILED"; fail=1; }
  "$PY" tools/playspectra_waitfor_test.py || { echo ">> wait_for retry tests FAILED"; fail=1; }
  "$PY" tools/playspectra_capture_assert_test.py || { echo ">> assert_capture retry tests FAILED"; fail=1; }
else
  note_skip "tools" "python not found"
fi

echo ""
echo "== submodule: playspectra_proto (gcc standalone) =="
PROTO="runtime/monado-playspectra/src/xrt/drivers/playspectra"
if command -v gcc >/dev/null 2>&1 && [ -f "$PROTO/playspectra_proto_test.c" ]; then
  mkdir -p build
  # No Monado deps: proto parse/verdict/content_sig test compiles from proto.c + bundled cJSON only.
  if gcc -I runtime/monado-playspectra/src/external/cjson -I "$PROTO" \
         "$PROTO/playspectra_proto_test.c" "$PROTO/playspectra_proto.c" \
         runtime/monado-playspectra/src/external/cjson/cjson/cJSON.c -o build/proto_test.exe; then
    ./build/proto_test.exe || { echo ">> proto tests FAILED"; fail=1; }
  else
    echo ">> proto build FAILED"; fail=1
  fi
else
  note_skip "proto" "gcc or submodule source not found"
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
