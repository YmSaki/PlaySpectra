// D3D11 color-readback backend for VR-MCP frame capture. See capture_backends.h for the interface
// contract and capture.cpp's VulkanReadbackToPng for the reference implementation pattern.
//
// This backend is one leaf behind capture.cpp's single dispatch (not a parallel capture system): at
// xrEndFrame capture.cpp resolves the released swapchain image + subimage geometry under its mutex,
// then hands us the raw ID3D11Texture2D* (as uint64_t) plus the DXGI format and rect. We copy the
// requested rect into a STAGING texture, map it, de-pad rows, swizzle BGRA->RGBA if needed, and encode
// an 8-bit RGBA PNG via lodepng -- mirroring VulkanReadbackToPng's format handling / json result shape.
// Unsupported formats and MSAA sources return an explicit error json, never a silently-broken image
// (CLAUDE.md).

#define XR_USE_GRAPHICS_API_D3D11

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "capture_backends.h"
#include "capture_common.h"
#include "layer_log.h"
#include "lodepng.h"

namespace vr_agent {
namespace {

// App-owned device from XrGraphicsBindingD3D11KHR. Not ref-held (the app owns its lifetime); we only
// borrow it between xrCreateSession and xrDestroySession.
ID3D11Device* g_d3d11_device = nullptr;

// D3D11 color-format handling mirrors the Vulkan path: we store bytes straight to an 8-bit RGBA PNG.
// RGBA8 formats copy directly; BGRA8 formats get a B<->R swizzle. Anything else (HDR, typeless,
// packed) is an explicit error -- never a silently-broken image (CLAUDE.md).
bool DxgiIsRGBA8(int64_t f) {
  return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
bool DxgiIsBGRA8(int64_t f) {
  return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

}  // namespace

void D3D11SetDevice(void* id3d11Device) {
  g_d3d11_device = reinterpret_cast<ID3D11Device*>(id3d11Device);
}

void D3D11Free() { g_d3d11_device = nullptr; }

// Runs on the app (xrEndFrame) thread. Copies the subimage rect of `imageHandle` (an
// ID3D11Texture2D*) into a STAGING texture and writes a PNG. Returns the result JSON (path on
// success), mirroring VulkanReadbackToPng's contract.
nlohmann::json D3D11ReadbackToPng(uint64_t imageHandle, int64_t dxgiFormat, uint32_t sampleCount,
                                  int32_t x, int32_t y, int32_t w, int32_t h, uint32_t arrayIndex,
                                  const std::string& eye, int viewIndex) {
  // Step 1: MSAA guard. CopySubresourceRegion cannot read a multisampled source (it needs a
  // ResolveSubresource first), so refuse rather than emit a broken image. Guard BEFORE any copy.
  if (sampleCount > 1) {
    return {{"ok", false},
            {"error", "D3D11 MSAA (sampleCount>1) resolve not implemented yet; core-required follow-on"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // Step 2: format guard. Only 8-bit RGBA/BGRA are encoded; BGRA needs a B<->R swizzle on row copy.
  const bool rgba = DxgiIsRGBA8(dxgiFormat);
  const bool bgra = DxgiIsBGRA8(dxgiFormat);
  if (!rgba && !bgra) {
    return {{"ok", false},
            {"error", "unsupported D3D11 color format " + std::to_string(dxgiFormat) +
                          " (only RGBA8/BGRA8 implemented; HDR/typeless/packed are a core-required follow-on)"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  if (g_d3d11_device == nullptr) {
    return {{"ok", false},
            {"error", "no D3D11 device captured (session not created with XrGraphicsBindingD3D11KHR?)"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }
  if (imageHandle == 0) {
    return {{"ok", false},
            {"error", "no released swapchain image to read (index out of range?)"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }
  if (w <= 0 || h <= 0) {
    return {{"ok", false},
            {"error", "invalid subimage rect"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // Step 3: source texture + its desc (need MipLevels for D3D11CalcSubresource).
  ID3D11Texture2D* tex = reinterpret_cast<ID3D11Texture2D*>(imageHandle);
  D3D11_TEXTURE2D_DESC desc{};
  tex->GetDesc(&desc);

  // TEMP DIAG (M0 step 1): one-time evidence for the flat-capture investigation -- shared-texture
  // flags and keyed-mutex presence decide the fix (inv.md hypothesis 1). Capture runs serially on
  // the app's xrEndFrame thread, so a plain static is race-free here.
  static bool s_diag_done = false;
  const bool diag_this_call = !s_diag_done;
  if (diag_this_call) {
    s_diag_done = true;
    IDXGIKeyedMutex* km = nullptr;
    const HRESULT qhr = tex->QueryInterface(IID_IDXGIKeyedMutex, reinterpret_cast<void**>(&km));
    char buf[160];
    std::snprintf(buf, sizeof buf, "MiscFlags=0x%X BindFlags=0x%X Usage=%d keyedMutexQI=0x%08lX",
                  desc.MiscFlags, desc.BindFlags, static_cast<int>(desc.Usage),
                  static_cast<unsigned long>(qhr));
    LayerLog("d3d11 capture diag:", buf);
    if (km != nullptr) km->Release();
  }

  // Defensive bounds check: an out-of-range D3D11_BOX is invalid and would copy garbage / be a no-op.
  // Better an honest error than a silently-broken image (CLAUDE.md).
  if (x < 0 || y < 0 || static_cast<UINT>(x + w) > desc.Width ||
      static_cast<UINT>(y + h) > desc.Height) {
    return {{"ok", false},
            {"error", "subimage rect out of texture bounds"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // Step 4: STAGING texture matching the source format, w x h, single mip/slice/sample, CPU-readable.
  D3D11_TEXTURE2D_DESC sdesc{};
  sdesc.Width = static_cast<UINT>(w);
  sdesc.Height = static_cast<UINT>(h);
  sdesc.MipLevels = 1;
  sdesc.ArraySize = 1;
  sdesc.Format = desc.Format;  // identical format keeps CopySubresourceRegion legal
  sdesc.SampleDesc.Count = 1;
  sdesc.SampleDesc.Quality = 0;
  sdesc.Usage = D3D11_USAGE_STAGING;
  sdesc.BindFlags = 0;
  sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sdesc.MiscFlags = 0;

  ID3D11Texture2D* staging = nullptr;
  HRESULT hr = g_d3d11_device->CreateTexture2D(&sdesc, nullptr, &staging);
  if (FAILED(hr) || staging == nullptr) {
    return {{"ok", false},
            {"error", "CreateTexture2D(staging) failed (hr=" + std::to_string(static_cast<long>(hr)) + ")"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // Step 5: immediate context (GetImmediateContext AddRef's -> must Release below).
  ID3D11DeviceContext* ctx = nullptr;
  g_d3d11_device->GetImmediateContext(&ctx);
  if (ctx == nullptr) {
    staging->Release();
    return {{"ok", false},
            {"error", "GetImmediateContext returned null"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // Step 6: copy the requested rect from (mip 0, array slice arrayIndex) into staging (0,0).
  UINT srcSub = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
  D3D11_BOX box{};
  box.left = static_cast<UINT>(x);
  box.top = static_cast<UINT>(y);
  box.front = 0;
  box.right = static_cast<UINT>(x + w);
  box.bottom = static_cast<UINT>(y + h);
  box.back = 1;
  ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, srcSub, &box);

  // Step 7: Map for read. D3D11 has no explicit layout barrier/fence; Map implicitly waits for the
  // copy to complete (this is the D3D11 idiom -- do NOT port the Vulkan barrier/fence dance here).
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr) || mapped.pData == nullptr) {
    ctx->Release();
    staging->Release();
    return {{"ok", false},
            {"error", "Map(staging) failed (hr=" + std::to_string(static_cast<long>(hr)) + ")"},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // TEMP DIAG (M0 step 1): first 16 mapped bytes -- distinguishes all-zero (never-synced read) from
  // clear-color content (app wrote, we read the wrong thing).
  if (diag_this_call) {
    const unsigned char* p = static_cast<const unsigned char*>(mapped.pData);
    char buf[160];
    std::snprintf(buf, sizeof buf,
                  "first16=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X rowPitch=%u",
                  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12],
                  p[13], p[14], p[15], static_cast<unsigned>(mapped.RowPitch));
    LayerLog("d3d11 capture diag:", buf);
  }

  // Step 8: repack into tight w*4 RGBA rows (honoring RowPitch, which != w*4 in general), swizzling
  // BGRA->RGBA if needed. Shared with the other backends (capture_common.cpp).
  const uint32_t uw = static_cast<uint32_t>(w);
  const uint32_t uh = static_cast<uint32_t>(h);
  std::vector<unsigned char> pixels =
      RepackRows(static_cast<const unsigned char*>(mapped.pData), mapped.RowPitch, uw, uh, bgra);

  // Step 9: unmap, then encode.
  ctx->Unmap(staging, 0);

  const std::string path = NextColorCapturePath();
  unsigned err = EncodeRgbaPng(path, pixels, uw, uh);

  // Step 10: release the resources WE own (staging + the AddRef'd context). tex and g_d3d11_device
  // are app-owned -- never Release them.
  ctx->Release();
  staging->Release();

  if (err) {
    return {{"ok", false},
            {"error", std::string("lodepng encode failed: ") + lodepng_error_text(err)},
            {"api", "D3D11"},
            {"eye", eye},
            {"viewIndex", viewIndex}};
  }

  // Step 11: success.
  return BuildCaptureSuccessJson(path, eye, viewIndex, "D3D11", uw, uh, arrayIndex, dxgiFormat);
}

}  // namespace vr_agent
