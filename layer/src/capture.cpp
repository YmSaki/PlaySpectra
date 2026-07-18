// PlaySpectra frame capture.
//
// OpenXR-side tracking + xrEndFrame projection parsing + capture-request handoff. At xrEndFrame,
// the resolved projection subimage's last-released image is dispatched to the graphics-API-specific
// backend (capture_vulkan.cpp / capture_d3d11.cpp / capture_d3d12.cpp) for GPU readback + PNG
// encode. All three OpenXR graphics bindings are supported.

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12

#include <windows.h>  // LoadLibraryA / GetProcAddress -- the layer loads Vulkan entry points at
                      // runtime and never links libvulkan (established design).
#include <direct.h>   // _mkdir (recording session directory)

#include <vulkan/vulkan.h>
#include <d3d11.h>
#include <d3d12.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "capture.h"
#include "capture_backends.h"
#include "capture_common.h"
#include "lodepng.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
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

#include "layer_log.h"

namespace playspectra {
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

std::map<XrSwapchain, SwapchainInfo> g_swapchains;

std::atomic<uint64_t> g_capture_counter{0};

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

// --- recording mode (periodic capture) ---
std::mutex g_rec_mutex;
bool g_recording = false;
uint32_t g_rec_interval = 30;
std::string g_rec_eye;
std::string g_rec_dir;
uint64_t g_rec_seq = 0;
struct RecEntry { std::string path; uint64_t frameNumber; std::string timestamp; };
std::vector<RecEntry> g_rec_entries;

std::string RecTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  struct tm tm_buf;
  localtime_s(&tm_buf, &t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, static_cast<int>(ms.count()));
  return std::string(buf);
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

std::string CaptureOutputDir() {
  if (const char* d = std::getenv("PLAYSPECTRA_CAPTURE_DIR")) {
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

// Resolve the depth swapchain's last-released image for `view` and read it back. Returns the
// {available:...} depth JSON. When the app chained no depth info, this is the honest "no depth
// submitted" answer (CLAUDE.md: never fabricate depth). Vulkan only (the only implemented backend).
json ResolveDepth(const EndFrameSnapshot::View& view) {
  if (!view.hasDepth) {
    return {{"available", false},
            {"note", "app submitted no XrCompositionLayerDepthInfoKHR; enable depth submission"}};
  }
  uint64_t depthImage = 0;
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
        depthImage = it->second.images[ri];
        haveDepthImage = true;
      }
    }
  }
  if (!haveDepthImage) {
    return {{"available", false}, {"note", "no tracked released image for the depth swapchain"}};
  }
  return VulkanReadbackDepthToPng(depthImage, depthFormat, depthSamples, view.depthX, view.depthY,
                                  view.depthW, view.depthH, view.depthArrayIndex, view.minDepth,
                                  view.maxDepth, view.nearZ, view.farZ);
}

// Dispatch table over the graphics-API backends. All three readback fns share the flat signature
// (capture_backends.h); only depth differs -- Vulkan reads real depth, D3D says N/A.
struct Backend {
  GfxApi api;
  json (*readback)(uint64_t, int64_t, uint32_t, int32_t, int32_t, int32_t, int32_t, uint32_t,
                   const std::string&, int);
  bool depthSupported;
};

static const Backend kBackends[] = {
    {GfxApi::Vulkan, VulkanReadbackToPng, true},
    {GfxApi::D3D11, D3D11ReadbackToPng, false},
    {GfxApi::D3D12, D3D12ReadbackToPng, false},
};

const Backend* FindBackend(GfxApi api) {
  for (const Backend& b : kBackends) {
    if (b.api == api) return &b;
  }
  return nullptr;
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

// Shared depth-capture output path (same numbering/dir as NextColorCapturePath). Exposed to the
// Vulkan backend TU via capture_backends.h so it doesn't re-derive it.
std::string NextDepthCapturePath() {
  return CaptureOutputDir() + "/vr_depth_" + std::to_string(g_capture_counter.fetch_add(1)) + ".png";
}

void CaptureOnCreateSession(const XrSessionCreateInfo* createInfo, XrSession /*session*/) {
  if (!createInfo) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_api = DetectGraphicsApi(createInfo->next);
  for (const auto* base = static_cast<const XrBaseInStructure*>(createInfo->next); base != nullptr;
       base = base->next) {
    if (base->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
      // Hand the app-owned handles to the Vulkan backend (capture_vulkan.cpp). Not ref-held here.
      const auto* b = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(base);
      VulkanSetBinding(b->instance, b->physicalDevice, b->device, b->queueFamilyIndex, b->queueIndex);
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
  VulkanFree();
  D3D11Free();
  D3D12Free();
  g_swapchains.clear();
  g_api = GfxApi::Unknown;
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

// Build the EndFrameSnapshot (projection views + any chained XrCompositionLayerDepthInfoKHR) from the
// app's xrEndFrame layers. The first projection layer wins. frameCount is assigned by the caller.
static EndFrameSnapshot ParseEndFrameSnapshot(const XrFrameEndInfo* frameEndInfo) {
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
  return snap;
}

void CaptureOnEndFrame(const XrFrameEndInfo* frameEndInfo) {
  EndFrameSnapshot snap = ParseEndFrameSnapshot(frameEndInfo);

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    snap.frameCount = g_last_frame.frameCount + 1;
    g_last_frame = snap;
  }

  // --- Recording path (periodic capture, independent of one-shot screenshots) ---
  {
    // Short lock: snapshot config (no seq claim yet — seq is claimed after successful readback to
    // keep rec_%04d.png numbering dense; gaps would truncate ffmpeg's image2 demuxer).
    std::string recEye, recDir;
    bool shouldCapture = false;
    {
      std::lock_guard<std::mutex> recLock(g_rec_mutex);
      if (g_recording && snap.hadProjection && (snap.frameCount % g_rec_interval == 0)) {
        recEye = g_rec_eye;
        recDir = g_rec_dir;
        shouldCapture = true;
      }
    }
    if (shouldCapture) {
      try {
        int idx = EyeToIndex(recEye, snap.viewCount);
        if (idx >= 0 && idx < static_cast<int>(snap.views.size())) {
          const auto& view = snap.views[idx];
          uint64_t rawHandle = 0;
          int64_t format = 0;
          uint32_t sampleCount = 1;
          GfxApi api = GfxApi::Unknown;
          bool haveImage = false;
          {
            std::lock_guard<std::mutex> mlock(g_mutex);
            auto it = g_swapchains.find(view.swapchain);
            if (it != g_swapchains.end() && it->second.lastReleasedIndex < it->second.images.size()) {
              rawHandle = it->second.images[it->second.lastReleasedIndex];
              format = it->second.format;
              sampleCount = it->second.sampleCount;
              api = g_api;
              haveImage = true;
            }
          }
          if (haveImage) {
            const Backend* be = FindBackend(api);
            if (be) {
              json r = be->readback(rawHandle, format, sampleCount, view.x, view.y, view.w, view.h,
                                    view.arrayIndex, recEye, idx);
              if (r.value("ok", false)) {
                std::string srcPath = r.value("path", "");
                if (!srcPath.empty()) {
                  // Short lock: claim seq + rename + append — only if same session (g_rec_dir match
                  // guards against a stop+start cycle that started a new session during readback).
                  std::lock_guard<std::mutex> recLock(g_rec_mutex);
                  if (g_recording && g_rec_dir == recDir) {
                    char fname[64];
                    std::snprintf(fname, sizeof(fname), "/rec_%04llu.png",
                                  static_cast<unsigned long long>(g_rec_seq));
                    std::string recPath = g_rec_dir + fname;
                    if (rename(srcPath.c_str(), recPath.c_str()) == 0) {
                      g_rec_entries.push_back({std::move(recPath), snap.frameCount, RecTimestamp()});
                      g_rec_seq++;
                    }
                  }
                }
              }
            }
          }
        }
      } catch (...) {
        Log("recording frame capture failed (exception swallowed)");
      }
    }
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
        // receive as uint64_t.
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
              haveImage = true;
            }
          }
        }

        // Depth answer for the non-Vulkan backends (depth readback is Vulkan-only, a nice-to-have).
        const json kDepthVulkanOnly = {{"available", false},
                                       {"note", "depth capture implemented for the Vulkan backend only"}};

        const Backend* be = FindBackend(g_api);
        if (be != nullptr) {
          if (!haveImage) {
            result = {{"ok", false}, {"error", "no tracked released image for this swapchain"}};
          } else {
            result = be->readback(rawHandle, format, sampleCount, view.x, view.y, view.w, view.h,
                                  view.arrayIndex, eye, idx);
          }
          if (withDepth) result["depth"] = be->depthSupported ? ResolveDepth(view) : kDepthVulkanOnly;
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

std::string CaptureStartRecording(uint32_t intervalFrames, const std::string& eye) {
  std::lock_guard<std::mutex> lock(g_rec_mutex);
  if (g_recording) {
    return json{{"ok", false}, {"error", "recording already in progress"}}.dump();
  }
  auto now = std::chrono::system_clock::now();
  auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  g_rec_dir = CaptureOutputDir() + "/recording_" + std::to_string(epoch_ms);
  _mkdir(g_rec_dir.c_str());
  g_rec_interval = intervalFrames > 0 ? intervalFrames : 30;
  g_rec_eye = eye.empty() ? "dominant" : eye;
  g_rec_seq = 0;
  g_rec_entries.clear();
  g_recording = true;
  Log("recording started: dir=" + g_rec_dir + " interval=" + std::to_string(g_rec_interval) +
      " eye=" + g_rec_eye);
  return json{{"ok", true}, {"recording", true}, {"dir", g_rec_dir},
              {"intervalFrames", g_rec_interval}, {"eye", g_rec_eye}}.dump();
}

std::string CaptureStopRecording() {
  std::lock_guard<std::mutex> lock(g_rec_mutex);
  if (!g_recording) {
    return json{{"ok", false}, {"error", "no recording in progress"}}.dump();
  }
  g_recording = false;
  json manifest = json::array();
  for (const RecEntry& e : g_rec_entries) {
    manifest.push_back({{"path", e.path}, {"frame", e.frameNumber}, {"timestamp", e.timestamp}});
  }
  std::string manifestPath = g_rec_dir + "/manifest.json";
  try {
    json manifestDoc = {{"frames", manifest}, {"count", g_rec_entries.size()},
                         {"dir", g_rec_dir}, {"intervalFrames", g_rec_interval}, {"eye", g_rec_eye}};
    FILE* f = fopen(manifestPath.c_str(), "w");
    if (f) {
      std::string s = manifestDoc.dump(2);
      fwrite(s.data(), 1, s.size(), f);
      fclose(f);
    }
  } catch (...) {}
  uint64_t count = g_rec_entries.size();
  g_rec_entries.clear();
  Log("recording stopped: " + std::to_string(count) + " frames captured");
  return json{{"ok", true}, {"recording", false}, {"dir", g_rec_dir},
              {"manifestPath", manifestPath}, {"framesCaptured", count},
              {"intervalFrames", g_rec_interval}}.dump();
}

bool CaptureIsRecording() {
  std::lock_guard<std::mutex> lock(g_rec_mutex);
  return g_recording;
}

}  // namespace playspectra
