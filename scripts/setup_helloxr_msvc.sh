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

# hello_xr's D3D11 plugin hardcodes swapchain sampleCount=1, so the layer's MSAA-resolve path (R08)
# is unreachable in E2E without this: make it env-driven via HELLO_XR_SAMPLE_COUNT (unset/<=1 keeps
# the stock behavior). Its RTV/DSV creation is also single-sample-only (TEXTURE2D dimension, depth
# SampleDesc.Count=1 -- E_INVALIDARG on an MSAA swapchain, probed 2026-07-16), so those go
# MSAA-conditional in the same patch. Each sub-patch is independently idempotent (guarded by a
# marker it introduces), same pattern as the <chrono> one above.
GFX11="$SRC/src/tests/hello_xr/graphicsplugin_d3d11.cpp"
python - "$GFX11" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

def sub(marker, old, new):
    global s
    if marker in s:
        return
    assert s.count(old) == 1, 'anchor not found exactly once: ' + old[:60]
    s = s.replace(old, new, 1)
    print('patched:', marker)

# (a) env-driven swapchain sample count (default 1 = stock behavior).
sub('HELLO_XR_SAMPLE_COUNT',
    'uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override { return 1; }',
    'uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override {\n'
    '        const char* e = std::getenv("HELLO_XR_SAMPLE_COUNT");\n'
    '        const int n = e ? std::atoi(e) : 1;\n'
    '        return n > 1 ? static_cast<uint32_t>(n) : 1u;\n'
    '    }')
# pch.h has no <cstdlib>; std::getenv/std::atoi need it explicitly.
sub('#include <cstdlib>',
    '#include "pch.h"',
    '#include "pch.h"\n\n#include <cstdlib>')
# (b) RTV dimension must be TEXTURE2DMS for a multisampled swapchain image.
sub('rtvColorDesc',
    'const CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(D3D11_RTV_DIMENSION_TEXTURE2D, (DXGI_FORMAT)swapchainFormat);',
    'D3D11_TEXTURE2D_DESC rtvColorDesc;\n'
    '        colorTexture->GetDesc(&rtvColorDesc);\n'
    '        const CD3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc(\n'
    '            rtvColorDesc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D,\n'
    '            (DXGI_FORMAT)swapchainFormat);')
# (c) depth buffer must match the color target's sample count, and its DSV dimension follows.
sub('colorDesc.SampleDesc.Count;',
    'depthDesc.SampleDesc.Count = 1;',
    'depthDesc.SampleDesc.Count = colorDesc.SampleDesc.Count;')
sub('D3D11_DSV_DIMENSION_TEXTURE2DMS',
    'CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(D3D11_DSV_DIMENSION_TEXTURE2D, DXGI_FORMAT_D32_FLOAT);',
    'CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(\n'
    '            colorDesc.SampleDesc.Count > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D,\n'
    '            DXGI_FORMAT_D32_FLOAT);')
# (d) HELLO_XR_HDR=1: prefer R16G16B16A16_FLOAT when the runtime enumerates it (R10 HDR E2E).
# NOTE stock find_first_of walks runtimeFormats in RUNTIME order, so merely adding 16F to the
# supported list cannot force it -- an explicit early return is required.
sub('HELLO_XR_HDR',
    'int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {\n'
    '        // List of supported color swapchain formats.',
    'int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {\n'
    '        const char* hdrEnv = std::getenv("HELLO_XR_HDR");\n'
    '        if (hdrEnv && *hdrEnv && *hdrEnv != \'0\') {\n'
    '            for (int64_t f : runtimeFormats) {\n'
    '                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return f;\n'
    '            }\n'
    '        }\n'
    '        // List of supported color swapchain formats.')

open(p, 'w', encoding='utf-8', newline='\n').write(s)
EOF

# Same treatment for the D3D12 plugin (R09). Its RTV/DSV creation is already MSAA-aware in stock
# hello_xr; the remaining single-sample hardcodes are (a) no GetSupportedSwapchainSampleCount
# override (base class returns recommended=1), (b) the depth buffer's SampleDesc, and (c) the PSO's
# SampleDesc, which D3D12 requires to match the render target's sample count.
GFX12="$SRC/src/tests/hello_xr/graphicsplugin_d3d12.cpp"
python - "$GFX12" <<'EOF'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()

def sub(marker, old, new):
    global s
    if marker in s:
        return
    assert s.count(old) == 1, 'anchor not found exactly once: ' + old[:60]
    s = s.replace(old, new, 1)
    print('patched:', marker)

# (a) env-driven swapchain sample count override (base default returns recommended = 1).
sub('HELLO_XR_SAMPLE_COUNT',
    '    void UpdateOptions(const std::shared_ptr<Options>& options) override { m_clearColor = options->GetBackgroundClearColor(); }',
    '    uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView&) override {\n'
    '        const char* e = std::getenv("HELLO_XR_SAMPLE_COUNT");\n'
    '        const int n = e ? std::atoi(e) : 1;\n'
    '        return n > 1 ? static_cast<uint32_t>(n) : 1u;\n'
    '    }\n\n'
    '    void UpdateOptions(const std::shared_ptr<Options>& options) override { m_clearColor = options->GetBackgroundClearColor(); }')
sub('#include <cstdlib>',
    '#include "pch.h"',
    '#include "pch.h"\n\n#include <cstdlib>')
# (b) depth buffer sample count must match the color target's.
sub('colorDesc.SampleDesc.Count;',
    'depthDesc.SampleDesc.Count = 1;',
    'depthDesc.SampleDesc.Count = colorDesc.SampleDesc.Count;')
# (c) PSO SampleDesc must match the render target sample count (D3D12 validation requirement).
sub('psoSampleCountEnv',
    'pipelineStateDesc.SampleDesc = {1, 0};',
    '{\n'
    '            const char* psoSampleCountEnv = std::getenv("HELLO_XR_SAMPLE_COUNT");\n'
    '            const int n = psoSampleCountEnv ? std::atoi(psoSampleCountEnv) : 1;\n'
    '            pipelineStateDesc.SampleDesc = {n > 1 ? static_cast<UINT>(n) : 1u, 0};\n'
    '        }')
# (d) HELLO_XR_HDR=1: prefer R16G16B16A16_FLOAT when the runtime enumerates it (R10 HDR E2E).
# Same rationale as the D3D11 block: find_first_of walks in runtime order, so early-return is needed.
sub('HELLO_XR_HDR',
    'int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {\n'
    '        // List of supported color swapchain formats.',
    'int64_t SelectColorSwapchainFormat(const std::vector<int64_t>& runtimeFormats) const override {\n'
    '        const char* hdrEnv = std::getenv("HELLO_XR_HDR");\n'
    '        if (hdrEnv && *hdrEnv && *hdrEnv != \'0\') {\n'
    '            for (int64_t f : runtimeFormats) {\n'
    '                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) return f;\n'
    '            }\n'
    '        }\n'
    '        // List of supported color swapchain formats.')

open(p, 'w', encoding='utf-8', newline='\n').write(s)
EOF

cmake -S "$SRC" -B "$BUILD" -G "Visual Studio 17 2022" -A x64 \
  -DBUILD_TESTS=ON -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
cmake --build "$BUILD" --config Release --target hello_xr

mkdir -p "$DEST"
cp "$BUILD/src/tests/hello_xr/Release/hello_xr.exe" "$DEST/"
echo "[setup_helloxr_msvc] deployed: $DEST/hello_xr.exe"
