// Vulkan color/depth-readback backend for VR-MCP frame capture. See capture_backends.h for the
// interface contract; this is the reference implementation the D3D11/D3D12 backends mirror.
//
// This backend is one leaf behind capture.cpp's single dispatch (not a parallel capture system): at
// xrEndFrame capture.cpp resolves the released swapchain image + subimage geometry under its mutex,
// then hands us the raw VkImage (as uint64_t) plus the Vulkan format and rect. We load vulkan-1.dll
// at runtime (no link-time libvulkan dependency), copy the requested rect into a host-visible staging
// buffer (resolving MSAA into a single-sample intermediate first), and encode an 8-bit RGBA PNG via
// lodepng -- decoding R16G16B16A16_SFLOAT (HDR) to sRGB along the way. Depth (Vulkan-only,
// nice-to-have) is linearized to view-space metres and encoded as a 16-bit grayscale PNG. Unsupported
// formats and MSAA return an explicit error json, never a silently-broken image (CLAUDE.md).

#include <windows.h>  // LoadLibraryA / GetProcAddress -- the layer loads Vulkan entry points at
                      // runtime and never links libvulkan (established design).

#include <vulkan/vulkan.h>

#include "capture_backends.h"
#include "capture_common.h"
#include "lodepng.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "layer_log.h"

namespace vr_agent {

using json = nlohmann::json;

namespace {

void Log(const std::string& m) { LayerLog("capture", m.c_str()); }

// Raw graphics handles captured from the session's XrGraphicsBindingVulkanKHR.
VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = 0, g_vk_queue_index = 0;

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
  PFN_vkCmdResolveImage cmdResolveImage = nullptr;
  PFN_vkCreateImage createImage = nullptr;
  PFN_vkDestroyImage destroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
  PFN_vkBindImageMemory bindImageMemory = nullptr;
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
// GAP-07(c): true between a successful vkQueueSubmit and the fence being observed signalled. If a
// readback times out waiting for its fence, this stays true so FreeVulkanResources waits the fence
// before destroying the pool/buffer/fence the GPU may still be reading (readback and destroy are both
// on the app thread and serialized, but a timed-out submit is the one case work can outlive the call).
bool g_vk_capture_inflight = false;
VkBuffer g_vk_staging = VK_NULL_HANDLE;
VkDeviceMemory g_vk_staging_mem = VK_NULL_HANDLE;
VkDeviceSize g_vk_staging_size = 0;

// Reusable single-sample intermediate image for MSAA resolve (GAP-03). Multisample swapchain images
// cannot be copied to a buffer directly; we vkCmdResolveImage into this device-local image (same
// format as the source) and copy from it. Lazily (re)created on size/format change, freed at session
// destroy. Its extent is exactly w x h and it is single-layer, so copies from it use origin {0,0,0}
// and baseArrayLayer 0 -- NOT the source subimage's view.x/y/arrayIndex.
VkImage g_vk_resolve_image = VK_NULL_HANDLE;
VkDeviceMemory g_vk_resolve_mem = VK_NULL_HANDLE;
uint32_t g_vk_resolve_w = 0, g_vk_resolve_h = 0;
int64_t g_vk_resolve_format = 0;

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
  // MSAA resolve + intermediate single-sample image (GAP-03). A missing one flips g_vk.ok=false so
  // the whole capture path dies loudly instead of silently -- paired with the load below on purpose.
  ok &= LoadDeviceFn(g_vk.cmdResolveImage, "vkCmdResolveImage");
  ok &= LoadDeviceFn(g_vk.createImage, "vkCreateImage");
  ok &= LoadDeviceFn(g_vk.destroyImage, "vkDestroyImage");
  ok &= LoadDeviceFn(g_vk.getImageMemoryRequirements, "vkGetImageMemoryRequirements");
  ok &= LoadDeviceFn(g_vk.bindImageMemory, "vkBindImageMemory");
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

// Choose a DEVICE_LOCAL memory type satisfying `typeBits`. Used for the MSAA-resolve / HDR
// intermediate VkImage (never CPU-mapped -- the readback still goes through the host-visible staging
// buffer), so device-local is both correct and fastest for the resolve/copy on the GPU.
uint32_t FindDeviceLocalMemType(uint32_t typeBits) {
  VkPhysicalDeviceMemoryProperties mp{};
  g_vk.getPhysicalDeviceMemoryProperties(g_vk_phys, &mp);
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
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

// Ensure the single-sample MSAA-resolve intermediate image is w x h with `format`, (re)creating it on
// change. Device-local, single mip/layer, usage TRANSFER_DST|TRANSFER_SRC (resolve target, then copy
// source). Returns false on failure.
bool EnsureResolveImage(uint32_t w, uint32_t h, int64_t format) {
  if (g_vk_resolve_image != VK_NULL_HANDLE && g_vk_resolve_w == w && g_vk_resolve_h == h &&
      g_vk_resolve_format == format) {
    return true;
  }
  if (g_vk_resolve_image != VK_NULL_HANDLE) {
    g_vk.destroyImage(g_vk_device, g_vk_resolve_image, nullptr);
    g_vk_resolve_image = VK_NULL_HANDLE;
  }
  if (g_vk_resolve_mem != VK_NULL_HANDLE) {
    g_vk.freeMemory(g_vk_device, g_vk_resolve_mem, nullptr);
    g_vk_resolve_mem = VK_NULL_HANDLE;
  }
  g_vk_resolve_w = 0;
  g_vk_resolve_h = 0;
  g_vk_resolve_format = 0;

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = static_cast<VkFormat>(format);
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;  // resolve target is single-sample by definition
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g_vk.createImage(g_vk_device, &ici, nullptr, &g_vk_resolve_image) != VK_SUCCESS) {
    Log("EnsureResolveImage: createImage failed");
    g_vk_resolve_image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements req{};
  g_vk.getImageMemoryRequirements(g_vk_device, g_vk_resolve_image, &req);
  uint32_t memType = FindDeviceLocalMemType(req.memoryTypeBits);
  if (memType == UINT32_MAX) {
    Log("EnsureResolveImage: no device-local memory type");
    g_vk.destroyImage(g_vk_device, g_vk_resolve_image, nullptr);
    g_vk_resolve_image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = memType;
  if (g_vk.allocateMemory(g_vk_device, &mai, nullptr, &g_vk_resolve_mem) != VK_SUCCESS) {
    Log("EnsureResolveImage: allocateMemory failed");
    g_vk.destroyImage(g_vk_device, g_vk_resolve_image, nullptr);
    g_vk_resolve_image = VK_NULL_HANDLE;
    return false;
  }
  if (g_vk.bindImageMemory(g_vk_device, g_vk_resolve_image, g_vk_resolve_mem, 0) != VK_SUCCESS) {
    Log("EnsureResolveImage: bindImageMemory failed");
    g_vk.destroyImage(g_vk_device, g_vk_resolve_image, nullptr);
    g_vk.freeMemory(g_vk_device, g_vk_resolve_mem, nullptr);
    g_vk_resolve_image = VK_NULL_HANDLE;
    g_vk_resolve_mem = VK_NULL_HANDLE;
    return false;
  }
  g_vk_resolve_w = w;
  g_vk_resolve_h = h;
  g_vk_resolve_format = format;
  Log("EnsureResolveImage: " + std::to_string(w) + "x" + std::to_string(h) + " fmt " +
      std::to_string(format));
  return true;
}

void FreeVulkanResources() {
  if (!g_vk.ok || g_vk_device == VK_NULL_HANDLE) return;
  // GAP-07(c): if a readback submitted work but its fence wait timed out, the GPU may still be reading
  // the very resources we are about to destroy. Wait the fence (bounded) before freeing so we never
  // pull the pool/buffer out from under an in-flight copy.
  if (g_vk_capture_inflight && g_vk_fence != VK_NULL_HANDLE) {
    g_vk.waitForFences(g_vk_device, 1, &g_vk_fence, VK_TRUE, 5000000000ULL);  // 5s, best-effort
    g_vk_capture_inflight = false;
  }
  if (g_vk_resolve_image != VK_NULL_HANDLE) g_vk.destroyImage(g_vk_device, g_vk_resolve_image, nullptr);
  if (g_vk_resolve_mem != VK_NULL_HANDLE) g_vk.freeMemory(g_vk_device, g_vk_resolve_mem, nullptr);
  if (g_vk_staging != VK_NULL_HANDLE) g_vk.destroyBuffer(g_vk_device, g_vk_staging, nullptr);
  if (g_vk_staging_mem != VK_NULL_HANDLE) g_vk.freeMemory(g_vk_device, g_vk_staging_mem, nullptr);
  if (g_vk_fence != VK_NULL_HANDLE) g_vk.destroyFence(g_vk_device, g_vk_fence, nullptr);
  if (g_vk_cmd != VK_NULL_HANDLE && g_vk_pool != VK_NULL_HANDLE)
    g_vk.freeCommandBuffers(g_vk_device, g_vk_pool, 1, &g_vk_cmd);
  if (g_vk_pool != VK_NULL_HANDLE) g_vk.destroyCommandPool(g_vk_device, g_vk_pool, nullptr);
  g_vk_resolve_image = VK_NULL_HANDLE;
  g_vk_resolve_mem = VK_NULL_HANDLE;
  g_vk_resolve_w = 0;
  g_vk_resolve_h = 0;
  g_vk_resolve_format = 0;
  g_vk_staging = VK_NULL_HANDLE;
  g_vk_staging_mem = VK_NULL_HANDLE;
  g_vk_staging_size = 0;
  g_vk_fence = VK_NULL_HANDLE;
  g_vk_cmd = VK_NULL_HANDLE;
  g_vk_pool = VK_NULL_HANDLE;
  g_vk_queue = VK_NULL_HANDLE;
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
// HDR color. VK_FORMAT_R16G16B16A16_SFLOAT is the format Unity/Unreal use for HDR/linear
// swapchains, so it is the one that must work; it is decoded (half-float -> sRGB 8-bit) rather than
// copied straight. Other wide formats (R16G16B16A16_UNORM / R32G32B32A32_SFLOAT / A2B10G10R10) are
// left as an explicit "unsupported" error for now -- honest, never a silently-broken image.
bool VkFormatIsHDR(int64_t f) {
  return f == VK_FORMAT_R16G16B16A16_SFLOAT;
}
// Bytes-per-texel for a supported color format; 0 => unsupported. The MSAA-resolve intermediate is
// the same format as the source, so its texelBytes is identical (staging bytes = w*h*texelBytes).
uint32_t VkColorTexelBytes(int64_t f) {
  if (VkFormatIsRGBA8(f) || VkFormatIsBGRA8(f)) return 4;
  if (VkFormatIsHDR(f)) return 8;  // R16G16B16A16_SFLOAT: 4 channels * 2 bytes
  return 0;
}

// ---- IEEE half-float (binary16) -> float, hand-rolled (no compiler intrinsic dependency). ----
// Handles zero, subnormals (normalized on the way out), inf and NaN, exponent bias 15 -> 127.
float HalfToFloat(uint16_t hbits) {
  const uint32_t sign = static_cast<uint32_t>(hbits & 0x8000u) << 16;  // -> float sign bit (bit 31)
  uint32_t exp = (hbits >> 10) & 0x1Fu;
  uint32_t mant = hbits & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;  // +/- zero
    } else {
      // Subnormal half: renormalize into a normal float. Shift mantissa left until the implicit
      // leading 1 (bit 10) appears, decrementing the exponent for each shift.
      exp = 127u - 15u + 1u;  // = 113, the exponent of 2^-14 in float bias
      while ((mant & 0x400u) == 0) {
        mant <<= 1;
        exp--;
      }
      mant &= 0x3FFu;  // drop the now-explicit leading 1
      bits = sign | (exp << 23) | (mant << 13);
    }
  } else if (exp == 0x1Fu) {
    // inf (mant==0) or NaN (mant!=0): float exponent all-ones, mantissa left-shifted to keep the
    // quiet/signalling bit and payload nonzero for NaN.
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    // Normalized: rebias exponent (15 -> 127) and left-align the 10-bit mantissa into 23 bits.
    bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

// One-time regression check of HalfToFloat against known values (0.0, 1.0, 0.5, max normal 65504,
// signed zero/one, smallest subnormal 2^-24). Runs the first time an HDR frame is decoded; logs on
// mismatch so a bit-shuffle bug surfaces loudly instead of producing a silently-wrong image.
bool HalfFloatSelfTest() {
  struct Case { uint16_t h; float f; };
  const Case cases[] = {
      {0x0000, 0.0f},      {0x3C00, 1.0f},  {0x3800, 0.5f}, {0x7BFF, 65504.0f},
      {0x8000, -0.0f},     {0xBC00, -1.0f},
  };
  bool ok = true;
  for (const Case& c : cases) {
    const float got = HalfToFloat(c.h);
    if (std::fabs(got - c.f) > 1e-4f * (std::fabs(c.f) + 1.0f)) ok = false;
  }
  // Smallest positive subnormal 0x0001 == 2^-24, exactly representable in float.
  if (HalfToFloat(0x0001) != std::ldexp(1.0f, -24)) ok = false;
  // inf / NaN classification.
  if (!std::isinf(HalfToFloat(0x7C00))) ok = false;   // +inf
  if (!std::isnan(HalfToFloat(0x7E00))) ok = false;   // NaN
  Log(std::string("HalfFloatSelfTest: ") + (ok ? "pass" : "FAIL"));
  return ok;
}

// Linear (scene-referred) -> sRGB electro-optical transfer function, clamped to [0,1]. inf clamps to
// 1.0 (>=1 branch); callers pre-map NaN to 0. This is the whole "tone mapping": no operator, no
// exposure knob -- HDR values above 1.0 saturate to white. Adjustable operators are a post-core
// extension (CLAUDE.md: keep the core loop minimal and honest).
float LinearToSrgb(float c) {
  if (!(c > 0.0f)) return 0.0f;   // handles <=0 and NaN (NaN>0 is false)
  if (c >= 1.0f) return 1.0f;     // handles +inf too
  if (c <= 0.0031308f) return c * 12.92f;
  return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
unsigned char QuantizeSrgb(float linear) {
  const float s = LinearToSrgb(linear);
  long q = std::lroundf(s * 255.0f);
  if (q < 0) q = 0;
  if (q > 255) q = 255;
  return static_cast<unsigned char>(q);
}
// Alpha is linear straight alpha (not sRGB-encoded); clamp + quantize directly.
unsigned char QuantizeLinearUnit(float a) {
  if (std::isnan(a)) a = 0.0f;
  if (a < 0.0f) a = 0.0f;
  if (a > 1.0f) a = 1.0f;
  long q = std::lroundf(a * 255.0f);
  if (q < 0) q = 0;
  if (q > 255) q = 255;
  return static_cast<unsigned char>(q);
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

}  // namespace

void VulkanSetBinding(void* vkInstance, void* vkPhysicalDevice, void* vkDevice,
                      uint32_t queueFamilyIndex, uint32_t queueIndex) {
  g_vk_instance = reinterpret_cast<VkInstance>(vkInstance);
  g_vk_phys = reinterpret_cast<VkPhysicalDevice>(vkPhysicalDevice);
  g_vk_device = reinterpret_cast<VkDevice>(vkDevice);
  g_vk_queue_family = queueFamilyIndex;
  g_vk_queue_index = queueIndex;
}

void VulkanFree() {
  FreeVulkanResources();
  g_vk_instance = VK_NULL_HANDLE;
  g_vk_phys = VK_NULL_HANDLE;
  g_vk_device = VK_NULL_HANDLE;
}

// Runs on the app (xrEndFrame) thread. Reads back `image` (already COLOR_ATTACHMENT_OPTIMAL, since we
// run before forwarding to the runtime) and writes a PNG. Returns the result JSON (path on success).
//
// Two paths:
//  * sampleCount==1: barrier source COLOR_ATTACHMENT_OPTIMAL->TRANSFER_SRC, copy the subimage rect
//    (imageOffset=view.x/y, baseArrayLayer=view.arrayIndex) straight into the staging buffer.
//  * sampleCount>1 (MSAA): a multisample image can't be copied to a buffer, so vkCmdResolveImage into
//    a single-sample device-local intermediate (EnsureResolveImage) first, then copy FROM that. The
//    intermediate is exactly w x h and single-layer, so its copy uses origin {0,0,0} / baseArrayLayer
//    0 -- the source's view.x/y/arrayIndex are used ONLY on the resolve's SRC side, never on the copy
//    from the intermediate (mixing them up reads out of bounds -> a silently-broken image, CLAUDE.md).
//
// Color decode: RGBA8 copies straight; BGRA8 gets a B<->R swizzle; R16G16B16A16_SFLOAT (HDR) is
// half-float-decoded, sRGB-encoded and quantized to 8-bit. Anything else is an explicit error.
json VulkanReadbackToPng(uint64_t imageHandle, int64_t format, uint32_t sampleCount,
                         int32_t x, int32_t y, int32_t viewW, int32_t viewH, uint32_t arrayIndex,
                         const std::string& eye, int idx) {
  VkImage image = reinterpret_cast<VkImage>(imageHandle);
  const bool rgba = VkFormatIsRGBA8(format);
  const bool bgra = VkFormatIsBGRA8(format);
  const bool hdr = VkFormatIsHDR(format);
  const uint32_t texelBytes = VkColorTexelBytes(format);
  if (texelBytes == 0 || (!rgba && !bgra && !hdr)) {
    return {{"ok", false},
            {"error", "unsupported Vulkan color format " + std::to_string(format) +
                          " (RGBA8/BGRA8/R16G16B16A16_SFLOAT implemented)"}};
  }
  if (image == VK_NULL_HANDLE) {
    return {{"ok", false}, {"error", "no released swapchain image to read (index out of range?)"}};
  }
  if (viewW <= 0 || viewH <= 0) {
    return {{"ok", false}, {"error", "invalid subimage rect"}};
  }
  if (!EnsureVulkanResources()) {
    return {{"ok", false}, {"error", "failed to create Vulkan capture resources"}};
  }
  const uint32_t w = static_cast<uint32_t>(viewW);
  const uint32_t h = static_cast<uint32_t>(viewH);
  const bool msaa = sampleCount > 1;
  if (msaa && !EnsureResolveImage(w, h, format)) {
    return {{"ok", false}, {"error", "failed to create MSAA resolve intermediate image"}};
  }
  // Staging holds one single-sample texel per output pixel; the resolve intermediate shares the source
  // format, so texelBytes is identical either way.
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * texelBytes;
  if (!EnsureStaging(bytes)) {
    return {{"ok", false}, {"error", "failed to allocate staging buffer"}};
  }

  // Record: barrier source COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC, (optionally resolve into the
  // intermediate), copy to buffer, barrier source back.
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
  toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, arrayIndex, 1};
  g_vk.cmdPipelineBarrier(g_vk_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

  // Copy source defaults to the swapchain image itself (single-sample path). For MSAA we redirect to
  // the resolve intermediate and ZERO the offset/layer, because that image is w x h origin-based.
  VkImage copySrc = image;
  int32_t copyOffX = x, copyOffY = y;
  uint32_t copyArrayLayer = arrayIndex;

  if (msaa) {
    // Intermediate UNDEFINED -> TRANSFER_DST (contents discarded; we overwrite it fully).
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = g_vk_resolve_image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vk.cmdPipelineBarrier(g_vk_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

    // Resolve. NOTE the asymmetry: SRC picks the requested subimage (view.x/y + arrayIndex) out of the
    // multisample swapchain image; DST always writes the intermediate's origin, layer 0.
    VkImageResolve rz{};
    rz.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, arrayIndex, 1};
    rz.srcOffset = {x, y, 0};
    rz.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rz.dstOffset = {0, 0, 0};
    rz.extent = {w, h, 1};
    g_vk.cmdResolveImage(g_vk_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk_resolve_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rz);

    // Intermediate TRANSFER_DST -> TRANSFER_SRC. Access masks (WRITE->READ, both TRANSFER stage) are
    // required, not just the layout transition, or the copy races the resolve (RAW hazard).
    VkImageMemoryBarrier resToSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    resToSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    resToSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    resToSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    resToSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    resToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resToSrc.image = g_vk_resolve_image;
    resToSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    g_vk.cmdPipelineBarrier(g_vk_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &resToSrc);

    copySrc = g_vk_resolve_image;
    copyOffX = 0;
    copyOffY = 0;
    copyArrayLayer = 0;
  }

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;    // tightly packed to imageExtent.width
  region.bufferImageHeight = 0;  // tightly packed to imageExtent.height
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, copyArrayLayer, 1};
  region.imageOffset = {copyOffX, copyOffY, 0};
  region.imageExtent = {w, h, 1};
  g_vk.cmdCopyImageToBuffer(g_vk_cmd, copySrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vk_staging, 1,
                            &region);

  // Restore the swapchain image to COLOR_ATTACHMENT_OPTIMAL (both paths left it in TRANSFER_SRC).
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
  g_vk_capture_inflight = true;  // GPU may now be reading g_vk_cmd/staging; cleared once the fence signals
  const uint64_t kTimeoutNs = 5000000000ULL;  // 5s
  if (g_vk.waitForFences(g_vk_device, 1, &g_vk_fence, VK_TRUE, kTimeoutNs) != VK_SUCCESS) {
    return {{"ok", false}, {"error", "timed out waiting for GPU copy fence"}};  // leave inflight=true
  }
  g_vk_capture_inflight = false;

  // Map the staging buffer and produce an 8-bit RGBA pixel buffer.
  //  * RGBA8: memcpy straight.  * BGRA8: memcpy + B<->R swizzle.
  //  * HDR (R16G16B16A16_SFLOAT): decode each 8-byte texel to 4 floats, sRGB-encode RGB, quantize.
  void* mapped = nullptr;
  if (g_vk.mapMemory(g_vk_device, g_vk_staging_mem, 0, bytes, 0, &mapped) != VK_SUCCESS || !mapped) {
    return {{"ok", false}, {"error", "vkMapMemory failed"}};
  }
  const size_t count = static_cast<size_t>(w) * h;
  std::vector<unsigned char> pixels(count * 4);  // always 8-bit RGBA output
  if (hdr) {
    static const bool kHalfTested = HalfFloatSelfTest();  // one-time regression check
    (void)kHalfTested;
    const unsigned char* src = static_cast<const unsigned char*>(mapped);
    for (size_t i = 0; i < count; ++i) {
      uint16_t half[4];
      std::memcpy(half, src + i * 8, 8);  // R,G,B,A half-floats
      pixels[i * 4 + 0] = QuantizeSrgb(HalfToFloat(half[0]));
      pixels[i * 4 + 1] = QuantizeSrgb(HalfToFloat(half[1]));
      pixels[i * 4 + 2] = QuantizeSrgb(HalfToFloat(half[2]));
      pixels[i * 4 + 3] = QuantizeLinearUnit(HalfToFloat(half[3]));
    }
  } else {
    pixels = RepackRows(static_cast<const unsigned char*>(mapped), static_cast<size_t>(w) * 4, w, h,
                        bgra);
  }
  g_vk.unmapMemory(g_vk_device, g_vk_staging_mem);

  const std::string path = NextColorCapturePath();
  unsigned err = EncodeRgbaPng(path, pixels, w, h);
  if (err) {
    return {{"ok", false},
            {"error", std::string("lodepng encode failed: ") + lodepng_error_text(err)}};
  }

  Log("readback " + std::to_string(bytes) + " bytes -> " + path +
      (msaa ? " (MSAA resolved)" : "") + (hdr ? " (HDR tonemapped)" : ""));
  // Observation-side honesty: the PNG is always 8-bit RGBA. When the source was HDR we say so and
  // describe the (fixed) conversion, so the observer never mistakes it for the raw HDR buffer.
  json result = BuildCaptureSuccessJson(path, eye, idx, "Vulkan", w, h, arrayIndex, format);
  result["sampleCount"] = sampleCount;
  result["msaaResolved"] = msaa;
  result["tonemapped"] = hdr;
  if (hdr) {
    result["sourceHdrFormat"] = format;
    result["colorConversion"] =
        "half-float linear -> sRGB OETF -> 8-bit (fixed operator, no exposure; values >1 clamp to white)";
  } else {
    result["colorConversion"] = "direct 8-bit (no tonemap)";
  }
  return result;
}

// Runs on the app (xrEndFrame) thread, after the color readback. `image` is the depth swapchain's
// last-released image, assumed left in DEPTH_STENCIL_ATTACHMENT_OPTIMAL by the app (mirrors the
// color path's COLOR_ATTACHMENT_OPTIMAL assumption). Returns {available:true, depthPath, depthMeta}
// or {available:false, note:...} -- never a silently-wrong depth image.
json VulkanReadbackDepthToPng(uint64_t imageHandle, int64_t format, uint32_t sampleCount,
                              int32_t depthX, int32_t depthY, int32_t depthW, int32_t depthH,
                              uint32_t depthArrayIndex, float minDepth, float maxDepth,
                              float nearZ, float farZ) {
  VkImage image = reinterpret_cast<VkImage>(imageHandle);
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
  if (depthW <= 0 || depthH <= 0) {
    return {{"available", false}, {"note", "invalid depth subimage rect"}};
  }
  if (!EnsureVulkanResources()) {
    return {{"available", false}, {"note", "failed to create Vulkan capture resources for depth"}};
  }
  const uint32_t w = static_cast<uint32_t>(depthW);
  const uint32_t h = static_cast<uint32_t>(depthH);
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
  toSrc.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, depthArrayIndex, 1};
  g_vk.cmdPipelineBarrier(g_vk_cmd, depthStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &toSrc);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;    // tightly packed to imageExtent.width
  region.bufferImageHeight = 0;  // tightly packed to imageExtent.height
  region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, depthArrayIndex, 1};
  region.imageOffset = {depthX, depthY, 0};
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
  g_vk_capture_inflight = true;  // see color path: cleared once the fence signals
  const uint64_t kTimeoutNs = 5000000000ULL;  // 5s
  if (g_vk.waitForFences(g_vk_device, 1, &g_vk_fence, VK_TRUE, kTimeoutNs) != VK_SUCCESS) {
    return {{"available", false}, {"note", "timed out waiting for GPU depth copy fence"}};  // inflight
  }
  g_vk_capture_inflight = false;

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
  const float dspan = maxDepth - minDepth;
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
    float z = (invSpan != 0.0f) ? (stored - minDepth) * invSpan : stored;
    if (z < 0.0f) z = 0.0f;
    if (z > 1.0f) z = 1.0f;
    const float zv = LinearizeViewDepth(z, nearZ, farZ);
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

  const std::string path = NextDepthCapturePath();
  unsigned err = lodepng::encode(path, png, w, h, LCT_GREY, 16);
  if (err) {
    return {{"available", false},
            {"note", std::string("lodepng depth encode failed: ") + lodepng_error_text(err)}};
  }

  const bool reversedZ = std::isinf(farZ) ? true : (nearZ > farZ);
  Log("depth readback " + std::to_string(bytes) + " bytes -> " + path);
  json meta = {
      {"nearZ", nearZ},
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
  if (std::isinf(farZ)) meta["farZ"] = "inf";
  else meta["farZ"] = farZ;
  return {{"available", true}, {"depthPath", path}, {"depthMeta", meta}};
}

}  // namespace vr_agent
