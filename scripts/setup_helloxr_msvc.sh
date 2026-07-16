#!/usr/bin/env bash
# Build the MSVC hello_xr (D3D11/D3D12 plugins compiled in) and deploy to third_party/hello_xr_msvc/.
# The MinGW hello_xr (layer/build/_deps) has no D3D plugins; this one has no Vulkan (unless a Vulkan
# SDK is installed) -- the two are complementary. Point integration_test.sh at this exe via:
#   HELLO_XR_EXE=third_party/hello_xr_msvc/hello_xr.exe bash scripts/integration_test.sh D3D11
#
# Requires: VS2022 (any edition) + CMake. Uses the OpenXR SDK source already fetched by the layer
# build (layer/build/_deps/openxr_sdk-src); run the layer CMake configure first if it is missing.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/layer/build/_deps/openxr_sdk-src"
# Build tree must live at a SHORT path: MSBuild FileTracker fails (FTK1011) past MAX_PATH.
BUILD="${TEMP:-/tmp}/vr_agent_helloxr_msvc"
DEST="${ROOT}/third_party/hello_xr_msvc"

[ -d "$SRC" ] || { echo "[setup_helloxr_msvc] $SRC missing -- configure layer/ first (FetchContent)"; exit 1; }

# OpenXR SDK 1.1.42's hello_xr/logger.cpp lacks <chrono>, which newer MSVC STLs require explicitly.
# Idempotent local patch of the fetched source (re-fetch clobbers it; this script restores it).
LOGGER="$SRC/src/tests/hello_xr/logger.cpp"
if ! grep -q '#include <chrono>' "$LOGGER"; then
  python - "$LOGGER" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
s = s.replace('#include <sstream>', '#include <chrono>\n#include <sstream>', 1)
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('patched <chrono> into', p)
EOF
fi

cmake -S "$SRC" -B "$BUILD" -G "Visual Studio 17 2022" -A x64 \
  -DBUILD_TESTS=ON -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build "$BUILD" --config Release --target hello_xr

mkdir -p "$DEST"
cp "$BUILD/src/tests/hello_xr/Release/hello_xr.exe" "$DEST/"
echo "[setup_helloxr_msvc] deployed: $DEST/hello_xr.exe"
