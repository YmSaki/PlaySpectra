// Per-graphics-API color-readback backend interface (internal to the capture TUs).
//
// DESIGN (why this file exists): color capture must cover every OpenXR graphics binding a runtime can
// hand the app -- Vulkan, D3D11, D3D12 (XrGraphicsBinding{Vulkan,D3D11,D3D12}KHR). This is one
// dispatch mechanism with a pluggable backend per API, NOT a parallel capture system: capture.cpp
// resolves the released swapchain image + subimage geometry under its mutex, then calls the matching
// backend below. Each backend lives in its own translation unit (capture_d3d11.cpp / capture_d3d12.cpp)
// so the GPU-specific code stays isolated and reviewable; Vulkan stays inline in capture.cpp because it
// also owns MSAA-resolve / HDR handling on the shared readback path.
//
// The interface is deliberately FLAT (primitive args only): no capture.cpp-internal type
// (EndFrameSnapshot::View, SwapchainInfo, g_swapchains) crosses the TU boundary. The raw image handle
// is passed as an opaque uint64_t (an ID3D11Texture2D* / ID3D12Resource*), the subimage rect as plain
// ints. Each backend returns a JSON result object mirroring VulkanReadbackToPng:
//   success  -> {ok:true, path, eye, viewIndex, api, width, height, arrayIndex, format}
//   failure  -> {ok:false, error, ...} (e.g. unsupported format / MSAA / no device) -- never a
//               silently-broken image (CLAUDE.md).

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace vr_agent {

// Next color-capture output path, e.g. "<VR_AGENT_CAPTURE_DIR>/vr_capture_<n>.png". Defined in
// capture.cpp, which owns the shared output dir + atomic counter. Backends call this instead of
// re-deriving the path so numbering stays consistent across all APIs.
std::string NextColorCapturePath();

// ---- D3D11 ----------------------------------------------------------------------------------------
// device is the app-owned ID3D11Device* from XrGraphicsBindingD3D11KHR (NOT ref-held; the app owns it).
// Set at xrCreateSession, cleared at xrDestroySession. imageHandle is an ID3D11Texture2D* as uint64_t.
void D3D11SetDevice(void* id3d11Device);
void D3D11Free();
nlohmann::json D3D11ReadbackToPng(uint64_t imageHandle, int64_t dxgiFormat, uint32_t sampleCount,
                                  int32_t x, int32_t y, int32_t w, int32_t h, uint32_t arrayIndex,
                                  const std::string& eye, int viewIndex);

// ---- D3D12 ----------------------------------------------------------------------------------------
// device / queue are the app-owned ID3D12Device* + ID3D12CommandQueue* from XrGraphicsBindingD3D12KHR.
// The queue is the app's DIRECT queue (XR_KHR_D3D12_enable contract). imageHandle is an
// ID3D12Resource* as uint64_t.
void D3D12SetDevice(void* id3d12Device, void* id3d12Queue);
void D3D12Free();
nlohmann::json D3D12ReadbackToPng(uint64_t imageHandle, int64_t dxgiFormat, uint32_t sampleCount,
                                  int32_t x, int32_t y, int32_t w, int32_t h, uint32_t arrayIndex,
                                  const std::string& eye, int viewIndex);

}  // namespace vr_agent
