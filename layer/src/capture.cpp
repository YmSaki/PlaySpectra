// VR-MCP frame capture.
//
// Increment A: OpenXR-side tracking + xrEndFrame projection parsing + capture-request handoff.
// Increment B (this pass): the Vulkan GPU pixel readback + PNG encode. At xrEndFrame, the resolved
// projection subimage's last-released VkImage is copied into a host-visible staging buffer and
// encoded to a PNG via lodepng; vr_screenshot returns the file path. D3D11/D3D12 backends are
// core-required follow-ons (CLAUDE.md) and currently return an explicit "not implemented" error
// rather than a silently-broken image.

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12

#include <windows.h>  // LoadLibraryA / GetProcAddress -- the layer loads Vulkan entry points at
                      // runtime and never links libvulkan (established design).

#include <vulkan/vulkan.h>
#include <d3d11.h>
#include <d3d12.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "capture.h"
#include "capture_backends.h"
#include "lodepng.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace vr_agent {

// Logger from openxr_agent_layer.cpp.
void LayerLog(const char* msg, const char* detail);

namespace {

using json = nlohmann::json;

void Log(const std::string& m) { LayerLog("capture", m.c_str()); }

struct SwapchainInfo {
  int64_t format = 0;
  uint32_t width = 0, height = 0, arraySize = 0, sampleCount = 0;
  XrSwapchainUsageFlags usage = 0;
  std::vector<uint64_t> images;  // opaque: VkImage / ID3D11Texture2D* / ID3D12Resource*
  uint32_t lastAcquiredIndex = UINT32_MAX;
  uint32_t lastReleasedIndex = UINT32_MAX;
};

std::mutex g_mutex;
GfxApi g_api = GfxApi::Unknown;

// Raw graphics handles captured from the session's XrGraphicsBinding*.
VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = 0, g_vk_queue_index = 0;

std::map<XrSwapchain, SwapchainInfo> g_swapchains;

// --------------------------------------------------------------------------------------------
// Vulkan entry points loaded at runtime from vulkan-1.dll (no link-time dependency on libvulkan).
// --------------------------------------------------------------------------------------------
struct VkFns {
  PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
  PFN_vkCreateCommandPool createCommandPool = nullptr;
  PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
  PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
  PFN_vkCreateFence createFence = nullptr;
  PFN_vkDestroyFence destroyFence = nullptr;
  PFN_vkResetFences resetFences = nullptr;
  PFN_vkWaitForFences waitForFences = nullptr;
  PFN_vkQueueSubmit queueSubmit = nullptr;
  PFN_vkCreateBuffer createBuffer = nullptr;
  PFN_vkDestroyBuffer destroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
  PFN_vkAllocateMemory allocateMemory = nullptr;
  PFN_vkFreeMemory freeMemory = nullptr;
  PFN_vkBindBufferMemory bindBufferMemory = nullptr;
  PFN_vkMapMemory mapMemory = nullptr;
  PFN_vkUnmapMemory unmapMemory = nullptr;
  bool ok = false;
};
VkFns g_vk;

// Reusable capture resources (created lazily on the app thread, freed at session destroy).
VkQueue g_vk_queue = VK_NULL_HANDLE;
VkCommandPool g_vk_pool = VK_NULL_HANDLE;
VkCommandBuffer g_vk_cmd = VK_NULL_HANDLE;
VkFence g_vk_fence = VK_NULL_HANDLE;
VkBuffer g_vk_staging = VK_NULL_HANDLE;
VkDeviceMemory g_vk_staging_mem = VK_NULL_HANDLE;
VkDeviceSize g_vk_staging_size = 0;

std::atomic<uint64_t> g_capture_counter{0};

template <typename T>
bool LoadInstanceFn(T& fn, const char* name) {
  fn = reinterpret_cast<T>(g_vk.getInstanceProcAddr(g_vk_instance, name));
  return fn != nullptr;
}
template <typename T>
bool LoadDeviceFn(T& fn, const char* name) {
  fn = reinterpret_cast<T>(g_vk.getDeviceProcAddr(g_vk_device, name));
  return fn != nullptr;
}

// Load vulkan-1.dll + resolve every entry point the readback needs. Idempotent.
void LoadVulkan() {
  if (g_vk.ok) return;
  HMODULE dll = LoadLibraryA("vulkan-1.dll");
  if (!dll) {
    Log("LoadVulkan: vulkan-1.dll not found");
    return;
  }
  g_vk.getInstanceProcAddr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dll, "vkGetInstanceProcAddr"));
  if (!g_vk.getInstanceProcAddr) {
    Log("LoadVulkan: vkGetInstanceProcAddr missing");
    return;
  }
  bool ok = true;
  // Instance-level.
  ok &= LoadInstanceFn(g_vk.getDeviceProcAddr, "vkGetDeviceProcAddr");
  ok &= LoadInstanceFn(g_vk.getPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
  // Device-level.
  ok &= LoadDeviceFn(g_vk.getDeviceQueue, "vkGetDeviceQueue");
  ok &= LoadDeviceFn(g_vk.createCommandPool, "vkCreateCommandPool");
  ok &= LoadDeviceFn(g_vk.destroyCommandPool, "vkDestroyCommandPool");
  ok &= LoadDeviceFn(g_vk.allocateCommandBuffers, "vkAllocateCommandBuffers");
  ok &= LoadDeviceFn(g_vk.freeCommandBuffers, "vkFreeCommandBuffers");
  ok &= LoadDeviceFn(g_vk.beginCommandBuffer, "vkBeginCommandBuffer");
  ok &= LoadDeviceFn(g_vk.endCommandBuffer, "vkEndCommandBuffer");
  ok &= LoadDeviceFn(g_vk.resetCommandBuffer, "vkResetCommandBuffer");
  ok &= LoadDeviceFn(g_vk.cmdPipelineBarrier, "vkCmdPipelineBarrier");
  ok &= LoadDeviceFn(g_vk.cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
  ok &= LoadDeviceFn(g_vk.createFence, "vkCreateFence");
  ok &= LoadDeviceFn(g_vk.destroyFence, "vkDestroyFence");
  ok &= LoadDeviceFn(g_vk.resetFences, "vkResetFences");
  ok &= LoadDeviceFn(g_vk.waitForFences, "vkWaitForFences");
  ok &= LoadDeviceFn(g_vk.queueSubmit, "vkQueueSubmit");
  ok &= LoadDeviceFn(g_vk.createBuffer, "vkCreateBuffer");
  ok &= LoadDeviceFn(g_vk.destroyBuffer, "vkDestroyBuffer");
  ok &= LoadDeviceFn(g_vk.getBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
  ok &= LoadDeviceFn(g_vk.allocateMemory, "vkAllocateMemory");
  ok &= LoadDeviceFn(g_vk.freeMemory, "vkFreeMemory");
  ok &= LoadDeviceFn(g_vk.bindBufferMemory, "vkBindBufferMemory");
  ok &= LoadDeviceFn(g_vk.mapMemory, "vkMapMemory");
  ok &= LoadDeviceFn(g_vk.unmapMemory, "vkUnmapMemory");
  g_vk.ok = ok;
  Log(std::string("LoadVulkan: entry points ") + (ok ? "resolved" : "INCOMPLETE"));
}

// Choose a host-visible + host-coherent memory type satisfying `typeBits`.
uint32_t FindHostVisibleMemType(uint32_t typeBits) {
  VkPhysicalDeviceMemoryProperties mp{};
  g_vk.getPhysicalDeviceMemoryProperties(g_vk_phys, &mp);
  const VkMemoryPropertyFlags want =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  }
  return UINT32_MAX;
}

// Create the one-shot command pool / command buffer / fence. Returns false on failure.
bool EnsureVulkanResources() {
  LoadVulkan();
  if (!g_vk.ok || g_vk_device == VK_NULL_HANDLE) return false;
  if (g_vk_pool != VK_NULL_HANDLE) return true;

  g_vk.getDeviceQueue(g_vk_device, g_vk_queue_family, g_vk_queue_index, &g_vk_queue);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = g_vk_queue_family;
  if (g_vk.createCommandPool(g_vk_device, &pci, nullptr, &g_vk_pool) != VK_SUCCESS) {
    Log("EnsureVulkanResources: createCommandPool failed");
    g_vk_pool = VK_NULL_HANDLE;
    return false;
  }
  VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbi.commandPool = g_vk_pool;
  cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbi.commandBufferCount = 1;
  if (g_vk.allocateCommandBuffers(g_vk_device, &cbi, &g_vk_cmd) != VK_SUCCESS) {
    Log("EnsureVulkanResources: allocateCommandBuffers failed");
    return false;
  }
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (g_vk.createFence(g_vk_device, &fci, nullptr, &g_vk_fence) != VK_SUCCESS) {
    Log("EnsureVulkanResources: createFence failed");
    return false;
  }
  return true;
}

// Ensure the staging buffer is at least `size` bytes, (re)creating it if needed.
bool EnsureStaging(VkDeviceSize size) {
  if (g_vk_staging != VK_NULL_HANDLE && g_vk_staging_size >= size) return true;
  if (g_vk_staging != VK_NULL_HANDLE) {
    g_vk.destroyBuffer(g_vk_device, g_vk_staging, nullptr);
    g_vk.freeMemory(g_vk_device, g_vk_staging_mem, nullptr);
    g_vk_staging = VK_NULL_HANDLE;
    g_vk_staging_mem = VK_NULL_HANDLE;
    g_vk_staging_size = 0;
  }
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (g_vk.createBuffer(g_vk_device, &bci, nullptr, &g_vk_staging) != VK_SUCCESS) {
    Log("EnsureStaging: createBuffer failed");
    g_vk_staging = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements req{};
  g_vk.getBufferMemoryRequirements(g_vk_device, g_vk_staging, &req);
  uint32_t memType = FindHostVisibleMemType(req.memoryTypeBits);
  if (memType == UINT32_MAX) {
    Log("EnsureStaging: no host-visible coherent memory type");
    return false;
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memType;
  if (g_vk.allocateMemory(g_vk_device, &mai, nullptr, &g_vk_staging_mem) != VK_SUCCESS) {
    Log("EnsureStaging: allocateMemory failed");
    return false;
  }
  if (g_vk.bindBufferMemory(g_vk_device, g_vk_staging, g_vk_staging_mem, 0) != VK_SUCCESS) {
    Log("EnsureStaging: bindBufferMemory failed");
    return false;
  }
  g_vk_staging_size = size;
  Log("EnsureStaging: staging buffer " + std::to_string(size) + " bytes, memType " +
      std::to_string(memType));
  return true;
}

void FreeVulkanResources() {
  if (!g_vk.ok || g_vk_device == VK_NULL_HANDLE) return;
  if (g_vk_staging != VK_NULL_HANDLE) g_vk.destroyBuffer(g_vk_device, g_vk_staging, nullptr);
  if (g_vk_staging_mem != VK_NULL_HANDLE) g_vk.freeMemory(g_vk_device, g_vk_staging_mem, nullptr);
  if (g_vk_fence != VK_NULL_HANDLE) g_vk.destroyFence(g_vk_device, g_vk_fence, nullptr);
  if (g_vk_cmd != VK_NULL_HANDLE && g_vk_pool != VK_NULL_HANDLE)
    g_vk.freeCommandBuffers(g_vk_device, g_vk_pool, 1, &g_vk_cmd);
  if (g_vk_pool != VK_NULL_HANDLE) g_vk.destroyCommandPool(g_vk_device, g_vk_pool, nullptr);
  g_vk_staging = VK_NULL_HANDLE;
  g_vk_staging_mem = VK_NULL_HANDLE;
  g_vk_staging_size = 0;
  g_vk_fence = VK_NULL_HANDLE;
  g_vk_cmd = VK_NULL_HANDLE;
  g_vk_pool = VK_NULL_HANDLE;
  g_vk_queue = VK_NULL_HANDLE;
}

// Snapshot of the most recent xrEndFrame's projection layer (for diagnostics / capture).
struct EndFrameSnapshot {
  bool hadProjection = false;
  uint32_t viewCount = 0;
  struct View {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t arrayIndex = 0;
    int32_t x = 0, y = 0;
    int32_t w = 0, h = 0;
    // Depth submission (XrCompositionLayerDepthInfoKHR chained off the projection view), if any.
    bool hasDepth = false;
    XrSwapchain depthSwapchain = XR_NULL_HANDLE;
    uint32_t depthArrayIndex = 0;
    int32_t depthX = 0, depthY = 0, depthW = 0, depthH = 0;
    float minDepth = 0.0f, maxDepth = 1.0f;  // depth buffer value range (viewport minDepth/maxDepth)
    float nearZ = 0.0f, farZ = 0.0f;         // near/far planes; nearZ>farZ or farZ==+inf => reversed-Z
  };
  std::vector<View> views;
  uint64_t frameCount = 0;
};
EndFrameSnapshot g_last_frame;

// --- capture-request handoff (control-channel thread <-> xrEndFrame thread) ---
std::mutex g_req_mutex;
std::condition_variable g_req_cv;
bool g_req_pending = false;
std::string g_req_eye;
bool g_req_with_depth = false;
bool g_req_done = false;
std::string g_req_result;

int DominantEyeIndex() {
  if (const char* e = std::getenv("VR_AGENT_DOMINANT_EYE")) {
    if (std::string(e) == "left") return 0;
  }
  return 1;  // default: right eye
}

int EyeToIndex(const std::string& eye, uint32_t viewCount) {
  int idx = 1;
  if (eye == "left") idx = 0;
  else if (eye == "right") idx = 1;
  else idx = DominantEyeIndex();  // "dominant" or unknown
  if (viewCount == 0) return 0;
  if (idx >= static_cast<int>(viewCount)) idx = static_cast<int>(viewCount) - 1;
  return idx;
}

GfxApi DetectGraphicsApi(const void* next) {
  for (const auto* base = static_cast<const XrBaseInStructure*>(next); base != nullptr;
       base = base->next) {
    switch (base->type) {
      case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR: return GfxApi::Vulkan;
      case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR: return GfxApi::D3D11;
      case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR: return GfxApi::D3D12;
      default: break;
    }
  }
  return GfxApi::Unknown;
}

// --------------------------------------------------------------------------------------------
// Vulkan color-format handling. We store bytes straight to an 8-bit RGBA PNG: RGBA formats copy
// directly, BGRA formats get a B<->R swizzle. Anything else (HDR, packed, etc.) is an explicit
// error -- never a silently-broken image (CLAUDE.md).
// --------------------------------------------------------------------------------------------
bool VkFormatIsRGBA8(int64_t f) {
  return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB;
}
bool VkFormatIsBGRA8(int64_t f) {
  return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

std::string CaptureOutputDir() {
  if (const char* d = std::getenv("VR_AGENT_CAPTURE_DIR")) {
    if (d[0]) return std::string(d);
  }
  if (const char* t = std::getenv("TEMP")) {
    if (t[0]) return std::string(t);
  }
  if (const char* t = std::getenv("TMP")) {
    if (t[0]) return std::string(t);
  }
  return std::string(".");
}

// Runs on the app (xrEndFrame) thread. Copies `image` (already COLOR_ATTACHMENT_OPTIMAL, since we
// run before forwarding to the runtime) into the staging buffer and writes a PNG. Returns the
// result JSON (path on success).
json VulkanReadbackToPng(VkImage image, int64_t format, uint32_t sampleCount,
                         const EndFrameSnapshot::View& view, const std::string& eye, int idx) {
  if (sampleCount > 1) {
    return {{"ok", false},
            {"error", "Vulkan MSAA (sampleCount>1) resolve not implemented yet; core-required follow-on"}};
  }
  const bool rgba = VkFormatIsRGBA8(format);
  const bool bgra = VkFormatIsBGRA8(format);
  if (!rgba && !bgra) {
    return {{"ok", false},
            {"error", "unsupported Vulkan color format " + std::to_string(format) +
                          " (only RGBA8/BGRA8 implemented; HDR is a core-required follow-on)"}};
  }
  if (image == VK_NULL_HANDLE) {
    return {{"ok", false}, {"error", "no released swapchain image to read (index out of range?)"}};
  }
  if (view.w <= 0 || view.h <= 0) {
    return {{"ok", false}, {"error", "invalid subimage rect"}};
  }
  if (!EnsureVulkanResources()) {
    return {{"ok", false}, {"error", "failed to create Vulkan capture resources"}};
  }
  const uint32_t w = static_cast<uint32_t>(view.w);
  const uint32_t h = static_cast<uint32_t>(view.h);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
  if (!EnsureStaging(bytes)) {
    return {{"ok", false}, {"error", "failed to allocate staging buffer"}};
  }

  // Record: barrier COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC, copy, barrier back.
  g_vk.resetCommandBuffer(g_vk_cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  g_vk.beginCommandBuffer(g_vk_cmd, &bi);

  VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toSrc.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toSrc.image = image;
  toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, view.arrayIndex, 1};
  g_vk.cmdPipelineBarrier(g_vk_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;    // tightly packed to imageExtent.width
  region.bufferImageHeight = 0;  // tightly packed to imageExtent.height
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, view.arrayIndex, 1};
  region.imageOffset = {view.x, view.y, 0};
  region.imageExtent = {w, h, 1};
  g_vk.cmdCopyImageToBuffer(g_vk_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk_staging, 1,
                            &region);

  VkImageMemoryBarrier toColor = toSrc;
  toColor.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  g_vk.cmdPipelineBarrier(g_vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                          1, &toColor);
  g_vk.endCommandBuffer(g_vk_cmd);

  g_vk.resetFences(g_vk_device, 1, &g_vk_fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &g_vk_cmd;
  if (g_vk.queueSubmit(g_vk_queue, 1, &si, g_vk_fence) != VK_SUCCESS) {
    return {{"ok", false}, {"error", "vkQueueSubmit failed"}};
  }
  const uint64_t kTimeoutNs = 5000000000ULL;  // 5s
  if (g_vk.waitForFences(g_vk_device, 1, &g_vk_fence, VK_TRUE, kTimeoutNs) != VK_SUCCESS) {
    return {{"ok", false}, {"error", "timed out waiting for GPU copy fence"}};
  }

  // Map + copy out, swizzling BGRA->RGBA in place if needed.
  void* mapped = nullptr;
  if (g_vk.mapMemory(g_vk_device, g_vk_staging_mem, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
    return {{"ok", false}, {"error", "vkMapMemory failed"}};
  }
  std::vector<unsigned char> pixels(static_cast<size_t>(bytes));
  std::memcpy(pixels.data(), mapped, static_cast<size_t>(bytes));
  g_vk.unmapMemory(g_vk_device, g_vk_staging_mem);
  if (bgra) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
  }

  const std::string path =
      CaptureOutputDir() + "/vr_capture_" + std::to_string(g_capture_counter.fetch_add(1)) + ".png";
  unsigned err = lodepng::encode(path, pixels, w, h, LCT_RGBA, 8);
  if (err) {
    return {{"ok", false},
            {"error", std::string("lodepng encode failed: ") + lodepng_error_text(err)}};
  }

  Log("readback " + std::to_string(bytes) + " bytes -> " + path);
  return {{"ok", true},
          {"path", path},
          {"eye", eye},
          {"viewIndex", idx},
          {"api", "Vulkan"},
          {"width", w},
          {"height", h},
          {"arrayIndex", view.arrayIndex},
          {"format", format}};
}

// --------------------------------------------------------------------------------------------
// Depth capture. When the app chains an XrCompositionLayerDepthInfoKHR onto a projection view, its
// depth swapchain image holds NDC depth in [0,1] (scaled into the viewport's [minDepth,maxDepth]).
// We copy that image out (VK_IMAGE_ASPECT_DEPTH_BIT), linearize NDC depth to positive view-space
// metres, and encode a 16-bit grayscale PNG normalized over the frame's finite depth range.
// Reversed-Z (nearZ>farZ) and infinite-far reversed-Z (farZ==+inf) are handled. Depth is a
// nice-to-have (CLAUDE.md): when the app submits none we say so honestly, never a fabricated image.
// --------------------------------------------------------------------------------------------
enum class DepthKind { None, U16, D24, F32 };

// Classify a Vulkan depth format and report bytes-per-texel a DEPTH-aspect buffer copy produces.
// Per the Vulkan spec a depth-aspect copy of D24_UNORM_S8_UINT / X8_D24_UNORM_PACK32 packs the
// 24-bit depth into the low bits of a 32-bit word; D32_SFLOAT(_S8_UINT) copies one float; D16 copies
// one uint16. Stencil bytes are never included in a depth-aspect copy.
DepthKind ClassifyDepthFormat(int64_t f, uint32_t& texelBytes) {
  switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
      texelBytes = 2; return DepthKind::U16;
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
      texelBytes = 4; return DepthKind::D24;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      texelBytes = 4; return DepthKind::F32;
    default:
      texelBytes = 0; return DepthKind::None;
  }
}

// NDC depth (z in [0,1]; Vulkan convention: 0=near plane, 1=far plane in the non-reversed case) ->
// positive view-space distance in metres. Endpoints: z=0 -> nearZ, z=1 -> farZ, so reversed-Z
// (nearZ>farZ) falls out of the same expression. Infinite far plane (farZ==+inf, the classic
// reversed-Z infinite projection) uses zView = nearZ / z, with z==0 meaning infinitely far.
float LinearizeViewDepth(float z, float nearZ, float farZ) {
  if (std::isinf(farZ)) {
    if (z <= 0.0f) return std::numeric_limits<float>::infinity();
    return nearZ / z;
  }
  const float denom = farZ - z * (farZ - nearZ);
  if (std::fabs(denom) < 1e-20f) return std::numeric_limits<float>::infinity();
  return farZ * nearZ / denom;
}

// Runs on the app (xrEndFrame) thread, after the color readback. `image` is the depth swapchain's
// last-released image, assumed left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL by the app (mirrors the
// color path's COLOR_ATTACHMENT_OPTIMAL assumption). Returns {available:true, depthPath, depthMeta}
// or {available:false, note:...} -- never a silently-wrong depth image.
json VulkanReadbackDepthToPng(VkImage image, int64_t format, uint32_t sampleCount,
                              const EndFrameSnapshot::View& view) {
  if (sampleCount > 1) {
    return {{"available", false},
            {"note", "depth swapchain is multisampled (sampleCount>1); depth MSAA resolve not implemented"}};
  }
  uint32_t texelBytes = 0;
  const DepthKind kind = ClassifyDepthFormat(format, texelBytes);
  if (kind == DepthKind::None) {
    return {{"available", false},
            {"note", "unsupported Vulkan depth format " + std::to_string(format) +
                         " (D16_UNORM / D24_UNORM_S8 / X8_D24 / D32_SFLOAT[_S8] supported)"}};
  }
  if (image == VK_NULL_HANDLE) {
    return {{"available", false}, {"note", "no released depth swapchain image to read"}};
  }
  if (view.depthW <= 0 || view.depthH <= 0) {
    return {{"available", false}, {"note", "invalid depth subimage rect"}};
  }
  if (!EnsureVulkanResources()) {
    return {{"available", false}, {"note", "failed to create Vulkan capture resources for depth"}};
  }
  const uint32_t w = static_cast<uint32_t>(view.depthW);
  const uint32_t h = static_cast<uint32_t>(view.depthH);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * texelBytes;
  if (!EnsureStaging(bytes)) {
    return {{"available", false}, {"note", "failed to allocate depth staging buffer"}};
  }

  // Record: barrier DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> TRANSFER_SRC, copy DEPTH aspect, barrier back.
  g_vk.resetCommandBuffer(g_vk_cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  g_vk.beginCommandBuffer(g_vk_cmd, &bi);

  const VkPipelineStageFlags depthStages =
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toSrc.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toSrc.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toSrc.image = image;
  toSrc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, view.depthArrayIndex, 1};
  g_vk.cmdPipelineBarrier(g_vk_cmd, depthStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &toSrc);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;    // tightly packed to imageExtent.width
  region.bufferImageHeight = 0;  // tightly packed to imageExtent.height
  region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, view.depthArrayIndex, 1};
  region.imageOffset = {view.depthX, view.depthY, 0};
  region.imageExtent = {w, h, 1};
  g_vk.cmdCopyImageToBuffer(g_vk_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk_staging, 1,
                            &region);

  VkImageMemoryBarrier toDepth = toSrc;
  toDepth.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toDepth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  toDepth.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toDepth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  g_vk.cmdPipelineBarrier(g_vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, depthStages, 0, 0, nullptr, 0,
                          nullptr, 1, &toDepth);
  g_vk.endCommandBuffer(g_vk_cmd);

  g_vk.resetFences(g_vk_device, 1, &g_vk_fence);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &g_vk_cmd;
  if (g_vk.queueSubmit(g_vk_queue, 1, &si, g_vk_fence) != VK_SUCCESS) {
    return {{"available", false}, {"note", "vkQueueSubmit failed for depth copy"}};
  }
  const uint64_t kTimeoutNs = 5000000000ULL;  // 5s
  if (g_vk.waitForFences(g_vk_device, 1, &g_vk_fence, VK_TRUE, kTimeoutNs) != VK_SUCCESS) {
    return {{"available", false}, {"note", "timed out waiting for GPU depth copy fence"}};
  }

  void* mapped = nullptr;
  if (g_vk.mapMemory(g_vk_device, g_vk_staging_mem, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
    return {{"available", false}, {"note", "vkMapMemory failed for depth"}};
  }
  std::vector<unsigned char> raw(static_cast<size_t>(bytes));
  std::memcpy(raw.data(), mapped, static_cast<size_t>(bytes));
  g_vk.unmapMemory(g_vk_device, g_vk_staging_mem);

  // Decode each texel -> [0,1] stored value, undo the viewport [minDepth,maxDepth] scale to recover
  // NDC z, then linearize to view-space metres. Track the finite range for output normalization.
  const size_t count = static_cast<size_t>(w) * h;
  std::vector<float> viewDepth(count);
  const float dspan = view.maxDepth - view.minDepth;
  const float invSpan = (std::fabs(dspan) > 1e-8f) ? (1.0f / dspan) : 0.0f;
  float minView = std::numeric_limits<float>::infinity();
  float maxView = -std::numeric_limits<float>::infinity();
  for (size_t i = 0; i < count; ++i) {
    float stored = 0.0f;  // depth buffer value in [0,1]
    switch (kind) {
      case DepthKind::U16: {
        uint16_t v = 0;
        std::memcpy(&v, raw.data() + i * 2, 2);
        stored = static_cast<float>(v) / 65535.0f;
        break;
      }
      case DepthKind::D24: {
        uint32_t v = 0;
        std::memcpy(&v, raw.data() + i * 4, 4);
        stored = static_cast<float>(v & 0x00FFFFFFu) / 16777215.0f;
        break;
      }
      case DepthKind::F32: {
        std::memcpy(&stored, raw.data() + i * 4, 4);
        break;
      }
      default:
        break;
    }
    // Undo viewport depth range -> NDC z in [0,1].
    float z = (invSpan != 0.0f) ? (stored - view.minDepth) * invSpan : stored;
    if (z < 0.0f) z = 0.0f;
    if (z > 1.0f) z = 1.0f;
    const float zv = LinearizeViewDepth(z, view.nearZ, view.farZ);
    viewDepth[i] = zv;
    if (std::isfinite(zv)) {
      if (zv < minView) minView = zv;
      if (zv > maxView) maxView = zv;
    }
  }

  if (!(std::isfinite(minView) && std::isfinite(maxView))) {
    // Every sample is at the (possibly infinite) far plane -> nothing meaningful to visualize.
    return {{"available", false},
            {"note", "depth submitted but all samples are at the far plane; nothing to visualize"}};
  }
  float range = maxView - minView;
  if (!(range > 0.0f)) range = 1.0f;  // flat depth -> avoid divide-by-zero (all pixels map to 0)

  // 16-bit grayscale, big-endian as lodepng requires. Nearest depth -> 0, farthest -> 65535;
  // infinite-far samples clamp to 65535 (white).
  std::vector<unsigned char> png(count * 2);
  for (size_t i = 0; i < count; ++i) {
    const float zv = viewDepth[i];
    uint16_t g;
    if (!std::isfinite(zv)) {
      g = 65535;
    } else {
      float norm = (zv - minView) / range;
      if (norm < 0.0f) norm = 0.0f;
      if (norm > 1.0f) norm = 1.0f;
      long q = std::lroundf(norm * 65535.0f);
      if (q < 0) q = 0;
      if (q > 65535) q = 65535;
      g = static_cast<uint16_t>(q);
    }
    png[i * 2] = static_cast<unsigned char>((g >> 8) & 0xFF);  // MSB first (lodepng 16-bit is big-endian)
    png[i * 2 + 1] = static_cast<unsigned char>(g & 0xFF);
  }

  const std::string path =
      CaptureOutputDir() + "/vr_depth_" + std::to_string(g_capture_counter.fetch_add(1)) + ".png";
  unsigned err = lodepng::encode(path, png, w, h, LCT_GREY, 16);
  if (err) {
    return {{"available", false},
            {"note", std::string("lodepng depth encode failed: ") + lodepng_error_text(err)}};
  }

  const bool reversedZ = std::isinf(view.farZ) ? true : (view.nearZ > view.farZ);
  Log("depth readback " + std::to_string(bytes) + " bytes -> " + path);
  json meta = {
      {"nearZ", view.nearZ},
      {"reversedZ", reversedZ},
      {"minView", minView},
      {"maxView", maxView},
      {"width", w},
      {"height", h},
      {"format", format},
      {"encoding",
       "linear-view-depth normalized to [minView,maxView] metres over 16-bit: "
       "zView = minView + (pixel/65535)*(maxView-minView); nearest=0, far/infinite=65535"}};
  // JSON has no infinity literal; emit an infinite far plane as the string "inf".
  if (std::isinf(view.farZ)) meta["farZ"] = "inf";
  else meta["farZ"] = view.farZ;
  return {{"available", true}, {"depthPath", path}, {"depthMeta", meta}};
}

// Resolve the depth swapchain's last-released image for `view` and read it back. Returns the
// {available:...} depth JSON. When the app chained no depth info, this is the honest "no depth
// submitted" answer (CLAUDE.md: never fabricate depth). Vulkan only (the only implemented backend).
json ResolveDepth(const EndFrameSnapshot::View& view) {
  if (!view.hasDepth) {
    return {{"available", false},
            {"note", "app submitted no XrCompositionLayerDepthInfoKHR; enable depth submission"}};
  }
  VkImage depthImage = VK_NULL_HANDLE;
  int64_t depthFormat = 0;
  uint32_t depthSamples = 1;
  bool haveDepthImage = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(view.depthSwapchain);
    if (it != g_swapchains.end()) {
      depthFormat = it->second.format;
      depthSamples = it->second.sampleCount;
      const uint32_t ri = it->second.lastReleasedIndex;
      if (ri < it->second.images.size()) {
        depthImage = reinterpret_cast<VkImage>(it->second.images[ri]);
        haveDepthImage = true;
      }
    }
  }
  if (!haveDepthImage) {
    return {{"available", false}, {"note", "no tracked released image for the depth swapchain"}};
  }
  return VulkanReadbackDepthToPng(depthImage, depthFormat, depthSamples, view);
}

}  // namespace

const char* GfxApiName(GfxApi api) {
  switch (api) {
    case GfxApi::Vulkan: return "Vulkan";
    case GfxApi::D3D11: return "D3D11";
    case GfxApi::D3D12: return "D3D12";
    default: return "Unknown";
  }
}

// Shared color-capture output path (same numbering/dir the Vulkan path uses at line ~460). Exposed to
// the D3D11/D3D12 backend TUs via capture_backends.h so they don't re-derive it. CaptureOutputDir()
// and g_capture_counter live in this TU's anonymous namespace but are visible here.
std::string NextColorCapturePath() {
  return CaptureOutputDir() + "/vr_capture_" + std::to_string(g_capture_counter.fetch_add(1)) + ".png";
}

void CaptureOnCreateSession(const XrSessionCreateInfo* createInfo, XrSession /*session*/) {
  if (!createInfo) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_api = DetectGraphicsApi(createInfo->next);
  for (const auto* base = static_cast<const XrBaseInStructure*>(createInfo->next); base != nullptr;
       base = base->next) {
    if (base->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
      const auto* b = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(base);
      g_vk_instance = b->instance;
      g_vk_phys = b->physicalDevice;
      g_vk_device = b->device;
      g_vk_queue_family = b->queueFamilyIndex;
      g_vk_queue_index = b->queueIndex;
    } else if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
      // Hand the app-owned device to the D3D11 backend (capture_d3d11.cpp). Not ref-held here.
      const auto* b = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(base);
      D3D11SetDevice(b->device);
    } else if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
      // Hand the app-owned device + DIRECT queue to the D3D12 backend (capture_d3d12.cpp).
      const auto* b = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base);
      D3D12SetDevice(b->device, b->queue);
    }
  }
  Log(std::string("session graphics API = ") + GfxApiName(g_api));
}

void CaptureOnDestroySession(XrSession /*session*/) {
  std::lock_guard<std::mutex> lock(g_mutex);
  FreeVulkanResources();
  D3D11Free();
  D3D12Free();
  g_swapchains.clear();
  g_api = GfxApi::Unknown;
  g_vk_instance = VK_NULL_HANDLE;
  g_vk_phys = VK_NULL_HANDLE;
  g_vk_device = VK_NULL_HANDLE;
}

void CaptureOnCreateSwapchain(const XrSwapchainCreateInfo* createInfo, XrSwapchain swapchain) {
  if (!createInfo) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  SwapchainInfo info;
  info.format = createInfo->format;
  info.width = createInfo->width;
  info.height = createInfo->height;
  info.arraySize = createInfo->arraySize;
  info.sampleCount = createInfo->sampleCount;
  info.usage = createInfo->usageFlags;
  g_swapchains[swapchain] = std::move(info);
  Log("swapchain created " + std::to_string(createInfo->width) + "x" +
      std::to_string(createInfo->height) + " arr=" + std::to_string(createInfo->arraySize) +
      " samples=" + std::to_string(createInfo->sampleCount) +
      " fmt=" + std::to_string(createInfo->format));
}

void CaptureOnDestroySwapchain(XrSwapchain swapchain) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_swapchains.erase(swapchain);
}

void CaptureOnEnumerateImages(XrSwapchain swapchain, uint32_t count,
                              const XrSwapchainImageBaseHeader* images) {
  if (!images || count == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_swapchains.find(swapchain);
  if (it == g_swapchains.end()) return;
  it->second.images.clear();
  it->second.images.reserve(count);
  // All XrSwapchainImage* structs begin with {type, next}; the image handle follows. Read it by
  // the concrete type per graphics API.
  for (uint32_t i = 0; i < count; ++i) {
    switch (images->type) {
      case XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR: {
        const auto* arr = reinterpret_cast<const XrSwapchainImageVulkanKHR*>(images);
        it->second.images.push_back(reinterpret_cast<uint64_t>(arr[i].image));
        break;
      }
      case XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR: {
        const auto* arr = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
        it->second.images.push_back(reinterpret_cast<uint64_t>(arr[i].texture));
        break;
      }
      case XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR: {
        const auto* arr = reinterpret_cast<const XrSwapchainImageD3D12KHR*>(images);
        it->second.images.push_back(reinterpret_cast<uint64_t>(arr[i].texture));
        break;
      }
      default:
        return;  // unknown image type; leave empty
    }
  }
  Log("swapchain images enumerated: " + std::to_string(it->second.images.size()));
}

void CaptureOnAcquireImage(XrSwapchain swapchain, uint32_t index) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_swapchains.find(swapchain);
  if (it != g_swapchains.end()) it->second.lastAcquiredIndex = index;
}

void CaptureOnReleaseImage(XrSwapchain swapchain) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_swapchains.find(swapchain);
  if (it != g_swapchains.end()) it->second.lastReleasedIndex = it->second.lastAcquiredIndex;
}

void CaptureOnEndFrame(const XrFrameEndInfo* frameEndInfo) {
  EndFrameSnapshot snap;
  if (frameEndInfo && frameEndInfo->layers) {
    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
      const XrCompositionLayerBaseHeader* layer = frameEndInfo->layers[i];
      if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
      const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
      snap.hadProjection = true;
      snap.viewCount = proj->viewCount;
      for (uint32_t v = 0; v < proj->viewCount; ++v) {
        const XrCompositionLayerProjectionView& pv = proj->views[v];
        EndFrameSnapshot::View view;
        view.swapchain = pv.subImage.swapchain;
        view.arrayIndex = pv.subImage.imageArrayIndex;
        view.x = pv.subImage.imageRect.offset.x;
        view.y = pv.subImage.imageRect.offset.y;
        view.w = pv.subImage.imageRect.extent.width;
        view.h = pv.subImage.imageRect.extent.height;
        // Walk the projection view's next chain for a chained depth submission
        // (XrCompositionLayerDepthInfoKHR). Most apps submit none; that's fine (depth stays absent).
        for (const auto* base = static_cast<const XrBaseInStructure*>(pv.next); base != nullptr;
             base = base->next) {
          if (base->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            const auto* d = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(base);
            view.hasDepth = true;
            view.depthSwapchain = d->subImage.swapchain;
            view.depthArrayIndex = d->subImage.imageArrayIndex;
            view.depthX = d->subImage.imageRect.offset.x;
            view.depthY = d->subImage.imageRect.offset.y;
            view.depthW = d->subImage.imageRect.extent.width;
            view.depthH = d->subImage.imageRect.extent.height;
            view.minDepth = d->minDepth;
            view.maxDepth = d->maxDepth;
            view.nearZ = d->nearZ;
            view.farZ = d->farZ;
            break;
          }
        }
        snap.views.push_back(view);
      }
      break;  // first projection layer wins
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    snap.frameCount = g_last_frame.frameCount + 1;
    g_last_frame = snap;
  }

  // Fulfil a pending screenshot request, if any.
  std::unique_lock<std::mutex> rlock(g_req_mutex);
  if (!g_req_pending) return;
  const std::string eye = g_req_eye;
  const bool withDepth = g_req_with_depth;
  g_req_pending = false;

  // The whole fulfillment is guarded: a capture failure (bad_alloc on the pixel buffer, a json
  // throw, etc.) must NOT propagate out of CaptureOnEndFrame -- that would fail the app's xrEndFrame
  // (Hook_xrEndFrame turns any exception into XR_ERROR_RUNTIME_FAILURE, breaking the render loop).
  // And g_req_done + notify must ALWAYS run, or the requester waits its full timeout for nothing.
  json result;
  try {
    if (!snap.hadProjection) {
      result = {{"ok", false}, {"error", "no projection layer submitted this frame"}};
    } else {
      int idx = EyeToIndex(eye, snap.viewCount);
      if (snap.views.empty() || idx < 0 || idx >= static_cast<int>(snap.views.size())) {
        result = {{"ok", false}, {"error", "projection layer had no usable views"}};
      } else {
        const auto& view = snap.views[idx];

        // Resolve the target image + format under g_mutex, then release it before GPU work. rawHandle
        // is the API-agnostic opaque handle (VkImage / ID3D11Texture2D* / ID3D12Resource*) the backends
        // receive as uint64_t; vkImage is the Vulkan-typed view of the same value for the inline path.
        VkImage vkImage = VK_NULL_HANDLE;
        uint64_t rawHandle = 0;
        int64_t format = 0;
        uint32_t sampleCount = 1;
        bool haveImage = false;
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          auto it = g_swapchains.find(view.swapchain);
          if (it != g_swapchains.end()) {
            format = it->second.format;
            sampleCount = it->second.sampleCount;
            const uint32_t ri = it->second.lastReleasedIndex;
            if (ri < it->second.images.size()) {
              rawHandle = it->second.images[ri];
              vkImage = reinterpret_cast<VkImage>(rawHandle);
              haveImage = true;
            }
          }
        }

        // Depth answer for the non-Vulkan backends (depth readback is Vulkan-only, a nice-to-have).
        const json kDepthVulkanOnly = {{"available", false},
                                       {"note", "depth capture implemented for the Vulkan backend only"}};

        if (g_api == GfxApi::Vulkan) {
          if (!haveImage) {
            result = {{"ok", false}, {"error", "no tracked released image for this swapchain"}};
          } else {
            result = VulkanReadbackToPng(vkImage, format, sampleCount, view, eye, idx);
          }
          if (withDepth) result["depth"] = ResolveDepth(view);
        } else if (g_api == GfxApi::D3D11) {
          if (!haveImage) {
            result = {{"ok", false}, {"error", "no tracked released image for this swapchain"}};
          } else {
            result = D3D11ReadbackToPng(rawHandle, format, sampleCount, view.x, view.y, view.w, view.h,
                                        view.arrayIndex, eye, idx);
          }
          if (withDepth) result["depth"] = kDepthVulkanOnly;
        } else if (g_api == GfxApi::D3D12) {
          if (!haveImage) {
            result = {{"ok", false}, {"error", "no tracked released image for this swapchain"}};
          } else {
            result = D3D12ReadbackToPng(rawHandle, format, sampleCount, view.x, view.y, view.w, view.h,
                                        view.arrayIndex, eye, idx);
          }
          if (withDepth) result["depth"] = kDepthVulkanOnly;
        } else {
          result = {{"ok", false},
                    {"error", std::string("capture backend for ") + GfxApiName(g_api) +
                                  " not implemented yet (core-required follow-on)"},
                    {"api", GfxApiName(g_api)},
                    {"eye", eye},
                    {"viewIndex", idx}};
          if (withDepth) result["depth"] = kDepthVulkanOnly;
        }
      }
    }
    // Uniform depth answer for the early-error paths (no projection / no usable view) that never
    // reached a backend branch: if depth was requested, still say honestly that none is available.
    if (withDepth && result.is_object() && !result.contains("depth")) {
      result["depth"] = {{"available", false},
                         {"note", "no usable projection view this frame to attach depth to"}};
    }
  } catch (const std::exception& e) {
    result = {{"ok", false}, {"error", std::string("capture failed: ") + e.what()}};
  } catch (...) {
    result = {{"ok", false}, {"error", "capture failed (unknown)"}};
  }
  try {
    g_req_result = result.dump();
  } catch (...) {
    g_req_result = "{\"ok\":false,\"error\":\"capture result serialize failed\"}";
  }
  g_req_done = true;
  rlock.unlock();
  g_req_cv.notify_all();
}

std::string CaptureRequestScreenshot(const std::string& eye, int timeoutMs, bool withDepth) {
  std::unique_lock<std::mutex> lock(g_req_mutex);
  g_req_eye = eye;
  g_req_with_depth = withDepth;
  g_req_done = false;
  g_req_pending = true;
  bool done = g_req_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_req_done; });
  if (!done) {
    g_req_pending = false;
    return json{{"ok", false},
                {"error", "timeout waiting for xrEndFrame; is the app rendering frames?"}}
        .dump();
  }
  return g_req_result;
}

std::string CaptureStatusJson() {
  std::lock_guard<std::mutex> lock(g_mutex);
  json sc = json::array();
  for (const auto& [handle, info] : g_swapchains) {
    sc.push_back({{"format", info.format},
                  {"width", info.width},
                  {"height", info.height},
                  {"arraySize", info.arraySize},
                  {"sampleCount", info.sampleCount},
                  {"imageCount", static_cast<uint64_t>(info.images.size())},
                  {"lastReleasedIndex", info.lastReleasedIndex}});
  }
  return json{{"api", GfxApiName(g_api)},
              {"swapchains", sc},
              {"lastFrameHadProjection", g_last_frame.hadProjection},
              {"lastFrameViewCount", g_last_frame.viewCount},
              {"framesObserved", g_last_frame.frameCount}}
      .dump();
}

}  // namespace vr_agent
