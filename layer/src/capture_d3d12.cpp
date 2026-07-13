// D3D12 color-readback backend for VR-MCP frame capture. See capture_backends.h for the interface
// contract and capture.cpp's VulkanReadbackToPng for the reference implementation pattern.
//
// SCAFFOLD (this pass): device/queue capture + explicit "not implemented" result so the layer builds
// green and every graphics path is wired. The GPU readback lands in the follow-on that fills
// D3D12ReadbackToPng: record on a DIRECT command allocator/list (COPY cannot transition RTV state),
// transition the released texture RENDER_TARGET -> COPY_SOURCE, CopyTextureRegion into a readback
// ID3D12Resource whose footprint has 256-byte-aligned row pitch (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT),
// execute on the app-owned queue, fence-wait, map + de-pad rows, BGRA swizzle, lodepng encode. Kept in
// its own TU so the D3D12-specific code stays isolated; one backend behind capture.cpp's single
// dispatch, not a parallel capture system.

#define XR_USE_GRAPHICS_API_D3D12

#include <windows.h>
#include <d3d12.h>

#include "capture_backends.h"

namespace vr_agent {
namespace {

// App-owned handles from XrGraphicsBindingD3D12KHR. Not ref-held (the app owns their lifetime); we
// only borrow them between xrCreateSession and xrDestroySession. queue is the app's DIRECT queue.
ID3D12Device* g_d3d12_device = nullptr;
ID3D12CommandQueue* g_d3d12_queue = nullptr;

}  // namespace

void D3D12SetDevice(void* id3d12Device, void* id3d12Queue) {
  g_d3d12_device = reinterpret_cast<ID3D12Device*>(id3d12Device);
  g_d3d12_queue = reinterpret_cast<ID3D12CommandQueue*>(id3d12Queue);
}

void D3D12Free() {
  g_d3d12_device = nullptr;
  g_d3d12_queue = nullptr;
}

nlohmann::json D3D12ReadbackToPng(uint64_t /*imageHandle*/, int64_t /*dxgiFormat*/,
                                  uint32_t /*sampleCount*/, int32_t /*x*/, int32_t /*y*/,
                                  int32_t /*w*/, int32_t /*h*/, uint32_t /*arrayIndex*/,
                                  const std::string& eye, int viewIndex) {
  return {{"ok", false},
          {"error", "D3D12 capture backend not implemented yet (core-required follow-on)"},
          {"api", "D3D12"},
          {"eye", eye},
          {"viewIndex", viewIndex}};
}

}  // namespace vr_agent
