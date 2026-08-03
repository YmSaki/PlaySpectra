#!/usr/bin/env bash
# Copyright (c) 2026 PlaySpectra contributors
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# SPDX-License-Identifier: MPL-2.0

# Build the MSVC hello_xr (D3D11/D3D12 plugins compiled in) and deploy to third_party/hello_xr_msvc/.
# The MinGW hello_xr (layer/build/_deps) has no D3D plugins; this one has no Vulkan (unless a Vulkan
# SDK is installed) -- the two are complementary. Point integration_test.sh at this exe via:
#   HELLO_XR_EXE=third_party/hello_xr_msvc/hello_xr.exe bash scripts/integration_test.sh D3D11
#
# Requires: VS2022 (any edition) + CMake. Uses the OpenXR SDK source already fetched by the layer
# build (layer/build/_deps/openxr_sdk-src); run the layer CMake configure first if it is missing.
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/lib_playspectra.sh"
SRC="${ROOT}/layer/build/_deps/openxr_sdk-src"
# Build tree must live at a SHORT path: MSBuild FileTracker fails (FTK1011) past MAX_PATH.
BUILD="${TEMP:-/tmp}/playspectra_helloxr_msvc"
DEST="${ROOT}/third_party/hello_xr_msvc"

[ -d "$SRC" ] || { echo "[setup_helloxr_msvc] $SRC missing -- configure layer/ first (FetchContent)"; exit 1; }

# OpenXR SDK 1.1.42's hello_xr/logger.cpp lacks <chrono>, which newer MSVC STLs require explicitly.
# Idempotent local patch of the fetched source (re-fetch clobbers it; this script restores it).
LOGGER="$SRC/src/tests/hello_xr/logger.cpp"

# hello_xr's D3D11 plugin hardcodes swapchain sampleCount=1, so the layer's MSAA-resolve path
# is unreachable in E2E without this: make it env-driven via HELLO_XR_SAMPLE_COUNT (unset/<=1 keeps
# the stock behavior). Its RTV/DSV creation is also single-sample-only (TEXTURE2D dimension, depth
# SampleDesc.Count=1 -- E_INVALIDARG on an MSAA swapchain, probed 2026-07-16), so those go
# MSAA-conditional in the same patch. Each sub-patch is independently idempotent (guarded by a
# marker it introduces), same pattern as the <chrono> one above.
GFX11="$SRC/src/tests/hello_xr/graphicsplugin_d3d11.cpp"

# Same treatment for the D3D12 plugin. Its RTV/DSV creation is already MSAA-aware in stock
# hello_xr; the remaining single-sample hardcodes are (a) no GetSupportedSwapchainSampleCount
# override (base class returns recommended=1), (b) the depth buffer's SampleDesc, and (c) the PSO's
# SampleDesc, which D3D12 requires to match the render target's sample count.
GFX12="$SRC/src/tests/hello_xr/graphicsplugin_d3d12.cpp"
ps_run internal patch-helloxr --source "$SRC"

cmake -S "$SRC" -B "$BUILD" -G "Visual Studio 17 2022" -A x64 \
  -DBUILD_TESTS=ON -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build "$BUILD" --config Release --target hello_xr

mkdir -p "$DEST"
cp "$BUILD/src/tests/hello_xr/Release/hello_xr.exe" "$DEST/"
echo "[setup_helloxr_msvc] deployed: $DEST/hello_xr.exe"
