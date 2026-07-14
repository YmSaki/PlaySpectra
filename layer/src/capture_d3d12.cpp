// D3D12 color-readback backend for VR-MCP frame capture. See capture_backends.h for the interface
// contract and capture.cpp's VulkanReadbackToPng for the reference implementation pattern.
//
// This backend does the same job the Vulkan inline path does, in D3D12 terms: the released color
// swapchain image (an ID3D12Resource) is transitioned RENDER_TARGET -> COPY_SOURCE, the requested
// subrect is copied into a HEAP_TYPE_READBACK ID3D12Resource whose placed footprint has a 256-byte
// aligned row pitch (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT), the queue is fence-waited, and the mapped
// bytes are de-padded (RowPitch -> w*4), BGRA-swizzled if needed, and lodepng-encoded. It is one
// backend behind capture.cpp's single dispatch, not a parallel capture system.
//
// Recording MUST happen on a DIRECT command allocator/list: a COPY-type list cannot execute the
// RENDER_TARGET->COPY_SOURCE resource-state transition, and the app queue we submit on is DIRECT
// (XR_KHR_D3D12_enable contract), so the list type must match.

#define XR_USE_GRAPHICS_API_D3D12

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstring>
#include <string>
#include <vector>

#include "capture_backends.h"
#include "capture_common.h"
#include "lodepng.h"

namespace vr_agent {
namespace {

using Microsoft::WRL::ComPtr;

// App-owned handles from XrGraphicsBindingD3D12KHR. Not ref-held (the app owns their lifetime); we
// only borrow them between xrCreateSession and xrDestroySession. queue is the app's DIRECT queue.
ID3D12Device* g_d3d12_device = nullptr;
ID3D12CommandQueue* g_d3d12_queue = nullptr;

// 8-bit-per-channel RGBA-order formats (channel bytes already in R,G,B,A order in memory).
bool DxgiIsRGBA8(int64_t f) {
  return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
// 8-bit-per-channel BGRA-order formats (need B<->R swizzle to get RGBA for PNG).
bool DxgiIsBGRA8(int64_t f) {
  return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

// A fully-typed DXGI format for the copyable footprint / copy destination. TYPELESS resources have an
// undefined copyable layout, so resolve them to the matching UNORM member of the same (copy-compatible)
// format family. SRGB carries the same byte layout and copies fine, so it is left as-is.
DXGI_FORMAT ResolveFootprintFormat(int64_t f) {
  if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) return DXGI_FORMAT_R8G8B8A8_UNORM;
  if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) return DXGI_FORMAT_B8G8R8A8_UNORM;
  return static_cast<DXGI_FORMAT>(f);
}

}  // namespace

void D3D12SetDevice(void* id3d12Device, void* id3d12Queue) {
  g_d3d12_device = reinterpret_cast<ID3D12Device*>(id3d12Device);
  g_d3d12_queue = reinterpret_cast<ID3D12CommandQueue*>(id3d12Queue);
}

void D3D12Free() {
  g_d3d12_device = nullptr;
  g_d3d12_queue = nullptr;
}

// Runs on the app (xrEndFrame) thread. Copies the released swapchain image `imageHandle` subrect into
// a readback buffer and writes a PNG. Returns the result JSON (path on success), mirroring
// VulkanReadbackToPng's shape. tex/device/queue are app-owned and are NOT released here.
nlohmann::json D3D12ReadbackToPng(uint64_t imageHandle, int64_t dxgiFormat, uint32_t sampleCount,
                                  int32_t x, int32_t y, int32_t w, int32_t h, uint32_t arrayIndex,
                                  const std::string& eye, int viewIndex) {
  const nlohmann::json base = {{"api", "D3D12"}, {"eye", eye}, {"viewIndex", viewIndex}};
  auto fail = [&](const std::string& msg) {
    nlohmann::json r = base;
    r["ok"] = false;
    r["error"] = msg;
    return r;
  };

  if (sampleCount > 1) {
    return fail("D3D12 MSAA (sampleCount>1) resolve not implemented yet; core-required follow-on");
  }
  const bool rgba = DxgiIsRGBA8(dxgiFormat);
  const bool bgra = DxgiIsBGRA8(dxgiFormat);
  if (!rgba && !bgra) {
    return fail("unsupported D3D12 color format " + std::to_string(dxgiFormat) +
                " (only RGBA8/BGRA8 implemented; HDR is a core-required follow-on)");
  }
  if (g_d3d12_device == nullptr || g_d3d12_queue == nullptr) {
    return fail("no D3D12 device/queue captured (xrCreateSession binding missing?)");
  }
  auto* tex = reinterpret_cast<ID3D12Resource*>(imageHandle);
  if (tex == nullptr) {
    return fail("no released swapchain image to read (index out of range?)");
  }
  if (w <= 0 || h <= 0 || x < 0 || y < 0) {
    return fail("invalid subimage rect");
  }
  const uint32_t uw = static_cast<uint32_t>(w);
  const uint32_t uh = static_cast<uint32_t>(h);

  // Source geometry for subresource indexing + bounds check.
  const D3D12_RESOURCE_DESC srcDesc = tex->GetDesc();
  const UINT mipLevels = srcDesc.MipLevels ? srcDesc.MipLevels : 1;
  const UINT arraySize = srcDesc.DepthOrArraySize ? srcDesc.DepthOrArraySize : 1;
  if (arrayIndex >= arraySize) {
    return fail("arrayIndex " + std::to_string(arrayIndex) + " out of range (arraySize " +
                std::to_string(arraySize) + ")");
  }
  if (static_cast<UINT64>(x) + uw > srcDesc.Width ||
      static_cast<UINT64>(y) + uh > srcDesc.Height) {
    return fail("subimage rect exceeds source texture bounds");
  }
  // D3D12CalcSubresource(MipSlice=0, ArraySlice=arrayIndex, PlaneSlice=0, MipLevels, ArraySize).
  const UINT subresource = 0 + arrayIndex * mipLevels + 0 * mipLevels * arraySize;

  // Copyable footprint for the w*h subrect. GetCopyableFootprints aligns RowPitch to
  // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256) and reports the total padded byte size for the buffer.
  D3D12_RESOURCE_DESC rectDesc = {};
  rectDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rectDesc.Alignment = 0;
  rectDesc.Width = uw;
  rectDesc.Height = uh;
  rectDesc.DepthOrArraySize = 1;
  rectDesc.MipLevels = 1;
  rectDesc.Format = ResolveFootprintFormat(dxgiFormat);
  rectDesc.SampleDesc.Count = 1;
  rectDesc.SampleDesc.Quality = 0;
  rectDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rectDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT numRows = 0;
  UINT64 rowSizeInBytes = 0;
  UINT64 totalBytes = 0;
  g_d3d12_device->GetCopyableFootprints(&rectDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes,
                                        &totalBytes);
  if (totalBytes == 0 || footprint.Footprint.RowPitch == 0) {
    return fail("GetCopyableFootprints returned empty layout");
  }

  // Readback buffer (HEAP_TYPE_READBACK, ROW_MAJOR, initial state COPY_DEST).
  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_READBACK;
  heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask = 1;
  heapProps.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC bufDesc = {};
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Alignment = 0;
  bufDesc.Width = totalBytes;
  bufDesc.Height = 1;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.MipLevels = 1;
  bufDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.SampleDesc.Quality = 0;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

  ComPtr<ID3D12Resource> readback;
  HRESULT hr = g_d3d12_device->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_ID3D12Resource, reinterpret_cast<void**>(readback.GetAddressOf()));
  if (FAILED(hr)) {
    return fail("CreateCommittedResource(readback) failed hr=0x" + std::to_string(hr));
  }

  // DIRECT allocator + list (COPY type cannot do the RTV->COPY_SOURCE transition; queue is DIRECT).
  ComPtr<ID3D12CommandAllocator> allocator;
  hr = g_d3d12_device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
      reinterpret_cast<void**>(allocator.GetAddressOf()));
  if (FAILED(hr)) {
    return fail("CreateCommandAllocator(DIRECT) failed hr=0x" + std::to_string(hr));
  }
  ComPtr<ID3D12GraphicsCommandList> cmdList;
  hr = g_d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_ID3D12GraphicsCommandList,
                                         reinterpret_cast<void**>(cmdList.GetAddressOf()));
  if (FAILED(hr)) {
    return fail("CreateCommandList(DIRECT) failed hr=0x" + std::to_string(hr));
  }

  // Barrier: RENDER_TARGET -> COPY_SOURCE (OpenXR color swapchain images are handed to the app in
  // RENDER_TARGET state under XR_KHR_D3D12_enable). Transition only the subresource we copy.
  D3D12_RESOURCE_BARRIER toCopy = {};
  toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  toCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  toCopy.Transition.pResource = tex;
  toCopy.Transition.Subresource = subresource;
  toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cmdList->ResourceBarrier(1, &toCopy);

  // Copy the subrect into the readback buffer's placed footprint.
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = tex;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = subresource;

  D3D12_BOX srcBox = {};
  srcBox.left = static_cast<UINT>(x);
  srcBox.top = static_cast<UINT>(y);
  srcBox.front = 0;
  srcBox.right = static_cast<UINT>(x) + uw;
  srcBox.bottom = static_cast<UINT>(y) + uh;
  srcBox.back = 1;
  cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);

  // Barrier back: COPY_SOURCE -> RENDER_TARGET so we leave the image as the runtime expects.
  D3D12_RESOURCE_BARRIER toRt = toCopy;
  toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  toRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  cmdList->ResourceBarrier(1, &toRt);

  hr = cmdList->Close();
  if (FAILED(hr)) {
    return fail("command list Close failed hr=0x" + std::to_string(hr));
  }

  ID3D12CommandList* lists[] = {cmdList.Get()};
  g_d3d12_queue->ExecuteCommandLists(1, lists);

  // Fence-wait for GPU completion.
  ComPtr<ID3D12Fence> fence;
  hr = g_d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                                   reinterpret_cast<void**>(fence.GetAddressOf()));
  if (FAILED(hr)) {
    return fail("CreateFence failed hr=0x" + std::to_string(hr));
  }
  HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (fenceEvent == nullptr) {
    return fail("CreateEvent failed");
  }
  hr = g_d3d12_queue->Signal(fence.Get(), 1);
  if (FAILED(hr)) {
    CloseHandle(fenceEvent);
    return fail("queue Signal failed hr=0x" + std::to_string(hr));
  }
  if (fence->GetCompletedValue() < 1) {
    hr = fence->SetEventOnCompletion(1, fenceEvent);
    if (FAILED(hr)) {
      CloseHandle(fenceEvent);
      return fail("SetEventOnCompletion failed hr=0x" + std::to_string(hr));
    }
    const DWORD wait = WaitForSingleObject(fenceEvent, 5000);  // 5s
    if (wait != WAIT_OBJECT_0) {
      CloseHandle(fenceEvent);
      return fail("timed out waiting for GPU copy fence");
    }
  }
  CloseHandle(fenceEvent);

  // Map + de-pad (256-aligned RowPitch -> tight w*4), swizzle BGRA->RGBA if needed.
  void* mapped = nullptr;
  D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
  hr = readback->Map(0, &readRange, &mapped);
  if (FAILED(hr) || mapped == nullptr) {
    return fail("readback Map failed hr=0x" + std::to_string(hr));
  }
  const unsigned char* srcBytes = static_cast<const unsigned char*>(mapped) + footprint.Offset;
  std::vector<unsigned char> pixels =
      RepackRows(srcBytes, static_cast<size_t>(footprint.Footprint.RowPitch), uw, uh, bgra);
  const D3D12_RANGE noWrite = {0, 0};  // CPU wrote nothing back to the buffer.
  readback->Unmap(0, &noWrite);

  const std::string path = NextColorCapturePath();
  unsigned err = EncodeRgbaPng(path, pixels, uw, uh);
  if (err) {
    return fail(std::string("lodepng encode failed: ") + lodepng_error_text(err));
  }

  return BuildCaptureSuccessJson(path, eye, viewIndex, "D3D12", uw, uh, arrayIndex, dxgiFormat);
}

}  // namespace vr_agent
