// D3D11 color-readback backend for VR-MCP frame capture. See capture_backends.h for the interface
// contract and capture.cpp's VulkanReadbackToPng for the reference implementation pattern.
//
// SCAFFOLD (this pass): device capture + explicit "not implemented" result so the layer builds green
// and every graphics path is wired. The GPU readback (staging ID3D11Texture2D, CopySubresourceRegion
// with D3D11CalcSubresource(arrayIndex), Map/RowPitch de-pad, BGRA swizzle, lodepng encode) lands in
// the follow-on that fills D3D11ReadbackToPng. Kept in its own TU so the D3D11-specific code stays
// isolated; it is one backend behind capture.cpp's single dispatch, not a parallel capture system.

#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>

#include "capture_backends.h"

namespace vr_agent {
namespace {

// App-owned device from XrGraphicsBindingD3D11KHR. Not ref-held (the app owns its lifetime); we only
// borrow it between xrCreateSession and xrDestroySession.
ID3D11Device* g_d3d11_device = nullptr;

}  // namespace

void D3D11SetDevice(void* id3d11Device) {
  g_d3d11_device = reinterpret_cast<ID3D11Device*>(id3d11Device);
}

void D3D11Free() { g_d3d11_device = nullptr; }

nlohmann::json D3D11ReadbackToPng(uint64_t /*imageHandle*/, int64_t /*dxgiFormat*/,
                                  uint32_t /*sampleCount*/, int32_t /*x*/, int32_t /*y*/,
                                  int32_t /*w*/, int32_t /*h*/, uint32_t /*arrayIndex*/,
                                  const std::string& eye, int viewIndex) {
  return {{"ok", false},
          {"error", "D3D11 capture backend not implemented yet (core-required follow-on)"},
          {"api", "D3D11"},
          {"eye", eye},
          {"viewIndex", viewIndex}};
}

}  // namespace vr_agent
