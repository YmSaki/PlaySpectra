#!/usr/bin/env bash
# Run every environment-independent unit test with one command and exit non-zero if any fail:
#   - mcp/   node:test suite  (pure quaternion/vector math)      -> 42 cases
#   - layer/ gtest via ctest  (capture_common/pixel_convert/pose_animator + header-only units) -> 78 cases
#
# WHY: GitHub Actions (.github/workflows/ci.yml) is billing-blocked on this private repo, so nothing
# auto-runs these. This script is the local stand-in -- it recovers CI's "one command runs everything"
# plus a correct pass/fail gate (a pre-push hook can call it later). It does NOT give a clean,
# independent environment, so it cannot catch "works on my machine" issues (locale/dep/platform) --
# that property only comes back with hosted CI once billing is fixed, or by making the repo public.
#
# Fast + hermetic: no GPU / HMD / running service / network needed. mcp ~1s; layer ~20s (build reused
# when already present). Assumes the standard single-config layer/build (as built locally). Run from
# anywhere:  bash scripts/run_all_tests.sh
set -uo pipefail
cd "$(dirname "$0")/.." # repo root
fail=0

echo "== mcp: node:test =="
if [ -d mcp ]; then
  (
    cd mcp
    [ -d node_modules ] || npm ci
    npm test
  ) || { echo ">> mcp tests FAILED"; fail=1; }
else
  echo "  (mcp/ not found -- skipped)"
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
if [ "$fail" -eq 0 ]; then
  echo "ALL GREEN"
else
  echo "SOME TESTS FAILED"
fi
exit "$fail"
