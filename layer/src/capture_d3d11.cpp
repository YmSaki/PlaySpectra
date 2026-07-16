// D3D11 color-readback backend for VR-MCP frame capture. See capture_backends.h for the interface
// contract and capture.cpp's VulkanReadbackToPng for the reference implementation pattern.
//
// This backend is one leaf behind capture.cpp's single dispatch (not a parallel capture system): at
// xrEndFrame capture.cpp resolves the released swapchain image + subimage geometry under its mutex,
// then hands us the raw ID3D11Texture2D* (as uint64_t) plus the DXGI format and rect. We acquire the
// image's keyed mutex (runtime-shared textures gate reads on it), copy the requested rect into a
// STAGING texture (multisampled sources get a ResolveSubresource into a reusable single-sample
// intermediate first -- the D3D11 sibling of the Vulkan GAP-03 resolve), map it, de-pad rows,
// swizzle BGRA->RGBA if needed, and encode an 8-bit RGBA PNG via lodepng -- mirroring
// VulkanReadbackToPng's format handling / json result shape.
// Unsupported formats return an explicit error json, never a silently-broken image (CLAUDE.md).

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

// Reusable single-sample intermediate for the MSAA resolve (the D3D11 sibling of capture_vulkan's
// EnsureResolveImage / GAP-03). ResolveSubresource always resolves a WHOLE subresource -- no rect
// form exists -- so this is sized to the full swapchain texture and the requested rect is copied
// out of it afterwards ("resolve, then rect-copy"). Cached across frames, recreated on
// width/height/format change, released in D3D11Free.
ID3D11Texture2D* g_resolve_tex = nullptr;
UINT g_resolve_w = 0;
UINT g_resolve_h = 0;
DXGI_FORMAT g_resolve_format = DXGI_FORMAT_UNKNOWN;

// Ensure the resolve intermediate matches w x h x fmt, (re)creating it on change. DEFAULT usage,
// no bind flags (resolve destination + copy source need none). Returns false on failure.
bool EnsureResolveTexture(UINT w, UINT h, DXGI_FORMAT fmt) {
  if (g_resolve_tex != nullptr && g_resolve_w == w && g_resolve_h == h && g_resolve_format == fmt) {
    return true;
  }
  if (g_resolve_tex != nullptr) {
    g_resolve_tex->Release();
    g_resolve_tex = nullptr;
  }
  g_resolve_w = 0;
  g_resolve_h = 0;
  g_resolve_format = DXGI_FORMAT_UNKNOWN;

  D3D11_TEXTURE2D_DESC rdesc{};
  rdesc.Width = w;
  rdesc.Height = h;
  rdesc.MipLevels = 1;
  rdesc.ArraySize = 1;
  rdesc.Format = fmt;
  rdesc.SampleDesc.Count = 1;  // resolve target is single-sample by definition
  rdesc.SampleDesc.Quality = 0;
  rdesc.Usage = D3D11_USAGE_DEFAULT;
  rdesc.BindFlags = 0;
  rdesc.CPUAccessFlags = 0;
  rdesc.MiscFlags = 0;
  HRESULT hr = g_d3d11_device->CreateTexture2D(&rdesc, nullptr, &g_resolve_tex);
  if (FAILED(hr) || g_resolve_tex == nullptr) {
    g_resolve_tex = nullptr;
    return false;
  }
  g_resolve_w = w;
  g_resolve_h = h;
  g_resolve_format = fmt;
  char buf[64];
  std::snprintf(buf, sizeof buf, "resolve intermediate %ux%u fmt=%d", w, h, static_cast<int>(fmt));
  LayerLog("d3d11 capture:", buf);
  return true;
}

}  // namespace

void D3D11SetDevice(void* id3d11Device) {
  g_d3d11_device = reinterpret_cast<ID3D11Device*>(id3d11Device);
}

void D3D11Free() {
  if (g_resolve_tex != nullptr) {
    g_resolve_tex->Release();
    g_resolve_tex = nullptr;
  }
  g_resolve_w = 0;
  g_resolve_h = 0;
  g_resolve_format = DXGI_FORMAT_UNKNOWN;
  g_d3d11_device = nullptr;
}

// Runs on the app (xrEndFrame) thread. Copies the subimage rect of `imageHandle` (an
// ID3D11Texture2D*) into a STAGING texture and writes a PNG. Returns the result JSON (path on
// success), mirroring VulkanReadbackToPng's contract.
nlohmann::json D3D11ReadbackToPng(uint64_t imageHandle, int64_t dxgiFormat, uint32_t sampleCount,
                                  int32_t x, int32_t y, int32_t w, int32_t h, uint32_t arrayIndex,
                                  const std::string& eye, int viewIndex) {
  // Step 1: MSAA handling. CopySubresourceRegion cannot read a multisampled source; sampleCount>1
  // takes a ResolveSubresource into a single-sample intermediate first (see Step 6).
  const bool msaa = sampleCount > 1;

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

  // One-time capture-path log. The shared-texture flags decide the keyed-mutex handling below, and
  // having them in the layer log keeps runtime-share regressions (the M0 flat-capture class)
  // diagnosable without a rebuild. Capture runs serially on the app's xrEndFrame thread, so a plain
  // static is race-free here.
  static bool s_flags_logged = false;
  if (!s_flags_logged) {
    s_flags_logged = true;
    char buf[96];
    std::snprintf(buf, sizeof buf, "MiscFlags=0x%X BindFlags=0x%X (keyed mutex %s)", desc.MiscFlags,
                  desc.BindFlags,
                  (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) ? "present" : "absent");
    LayerLog("d3d11 capture:", buf);
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

  // Step 6: the runtime shares D3D11 swapchain images with its compositor via a keyed mutex
  // (MiscFlags D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX -- observed 0x100 on both Meta XR Simulator
  // and Monado). Reading without holding the mutex is undefined and yields all zeros = the flat
  // grey capture bug. Acquire key 0 (the share's convention) around the copy; the staging texture
  // is process-local so the mutex can be released as soon as the copy is issued (Map orders after
  // the copy on the immediate context regardless).
  // NOTE: AcquireSync returns WAIT_TIMEOUT (0x102) on timeout, which SUCCEEDED() treats as success
  // -- compare against S_OK exactly, and fail loudly (never a silently-broken image, CLAUDE.md).
  IDXGIKeyedMutex* keyedMutex = nullptr;
  if (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
    HRESULT khr = tex->QueryInterface(IID_IDXGIKeyedMutex, reinterpret_cast<void**>(&keyedMutex));
    if (FAILED(khr) || keyedMutex == nullptr) {
      ctx->Release();
      staging->Release();
      return {{"ok", false},
              {"error", "swapchain image has KEYED_MUTEX but QueryInterface(IDXGIKeyedMutex) failed (hr=" +
                            std::to_string(static_cast<long>(khr)) + ")"},
              {"api", "D3D11"},
              {"eye", eye},
              {"viewIndex", viewIndex}};
    }
    khr = keyedMutex->AcquireSync(0, 1000);
    if (khr != S_OK) {
      keyedMutex->Release();
      ctx->Release();
      staging->Release();
      return {{"ok", false},
              {"error", "AcquireSync(0) on the swapchain keyed mutex failed or timed out (hr=" +
                            std::to_string(static_cast<long>(khr)) + ")"},
              {"api", "D3D11"},
              {"eye", eye},
              {"viewIndex", viewIndex}};
    }
  }

  // Copy the requested rect from (mip 0, array slice arrayIndex) into staging (0,0).
  UINT srcSub = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
  D3D11_BOX box{};
  box.left = static_cast<UINT>(x);
  box.top = static_cast<UINT>(y);
  box.front = 0;
  box.right = static_cast<UINT>(x + w);
  box.bottom = static_cast<UINT>(y + h);
  box.back = 1;
  if (msaa) {
    // ResolveSubresource has no rect form (whole-subresource only), so resolve the full source
    // subresource into the reusable intermediate, then rect-copy from THAT. Both commands that read
    // the shared texture are issued while the keyed mutex is held; the intermediate is
    // process-local, so the mutex is released before the rect copy. The resolve format is the
    // typed OpenXR swapchain format (it already passed the RGBA8/BGRA8 guard, so it is never
    // typeless -- legal even if desc.Format were a typeless family).
    if (!EnsureResolveTexture(desc.Width, desc.Height, desc.Format)) {
      if (keyedMutex != nullptr) {
        keyedMutex->ReleaseSync(0);
        keyedMutex->Release();
      }
      ctx->Release();
      staging->Release();
      return {{"ok", false},
              {"error", "CreateTexture2D(MSAA resolve intermediate) failed"},
              {"api", "D3D11"},
              {"eye", eye},
              {"viewIndex", viewIndex}};
    }
    ctx->ResolveSubresource(g_resolve_tex, 0, tex, srcSub, static_cast<DXGI_FORMAT>(dxgiFormat));
    if (keyedMutex != nullptr) {
      keyedMutex->ReleaseSync(0);
      keyedMutex->Release();
      keyedMutex = nullptr;
    }
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, g_resolve_tex, 0, &box);
  } else {
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, srcSub, &box);
  }

  if (keyedMutex != nullptr) {
    keyedMutex->ReleaseSync(0);
    keyedMutex->Release();
  }

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

  // Step 11: success. sampleCount/msaaResolved mirror the Vulkan result shape (capture_vulkan.cpp).
  nlohmann::json result =
      BuildCaptureSuccessJson(path, eye, viewIndex, "D3D11", uw, uh, arrayIndex, dxgiFormat);
  result["sampleCount"] = sampleCount;
  result["msaaResolved"] = msaa;
  return result;
}

}  // namespace vr_agent
