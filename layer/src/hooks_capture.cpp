// Frame-capture hook cluster implementation. Moved verbatim from openxr_agent_layer.cpp (refactor
// R04); behaviour is unchanged (same passthrough + capture.cpp notifications). See hooks_capture.h.
#include "hooks_capture.h"

#include "capture.h"           // CaptureOn* frame/swapchain/session notifications
#include "control_channel.h"   // ControlChannelSetSession
#include "layer_dispatch.h"    // Dispatch() / SetCurrentSession()
#include "layer_log.h"         // LayerLog

using vr_agent::Dispatch;
using vr_agent::SetCurrentSession;

namespace {

void Log(const char* msg, const char* detail = nullptr) { vr_agent::LayerLog(msg, detail); }

}  // namespace

XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                          XrSession* session) {
  try {
    PFN_xrCreateSession next = Dispatch().createSession;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(instance, createInfo, session);
    if (XR_SUCCEEDED(r) && session) {
      SetCurrentSession(*session);
      vr_agent::ControlChannelSetSession(true);
      vr_agent::CaptureOnCreateSession(createInfo, *session);
      Log("session created");
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                            XrSwapchain* swapchain) {
  try {
    PFN_xrCreateSwapchain next = Dispatch().createSwapchain;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, createInfo, swapchain);
    if (XR_SUCCEEDED(r) && swapchain) vr_agent::CaptureOnCreateSwapchain(createInfo, *swapchain);
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain swapchain) {
  try {
    PFN_xrDestroySwapchain next = Dispatch().destroySwapchain;
    vr_agent::CaptureOnDestroySwapchain(swapchain);
    return next ? next(swapchain) : XR_ERROR_FUNCTION_UNSUPPORTED;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                    uint32_t* imageCountOutput,
                                                    XrSwapchainImageBaseHeader* images) {
  try {
    PFN_xrEnumerateSwapchainImages next = Dispatch().enumerateSwapchainImages;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(swapchain, imageCapacityInput, imageCountOutput, images);
    // Record only on the populating call (images non-null and something returned).
    if (XR_SUCCEEDED(r) && images && imageCountOutput && *imageCountOutput > 0)
      vr_agent::CaptureOnEnumerateImages(swapchain, *imageCountOutput, images);
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageAcquireInfo* acquireInfo,
                                                 uint32_t* index) {
  try {
    PFN_xrAcquireSwapchainImage next = Dispatch().acquireSwapchainImage;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(swapchain, acquireInfo, index);
    if (XR_SUCCEEDED(r) && index) vr_agent::CaptureOnAcquireImage(swapchain, *index);
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageReleaseInfo* releaseInfo) {
  try {
    PFN_xrReleaseSwapchainImage next = Dispatch().releaseSwapchainImage;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    vr_agent::CaptureOnReleaseImage(swapchain);
    return next(swapchain, releaseInfo);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
  try {
    vr_agent::CaptureOnEndFrame(frameEndInfo);
    PFN_xrEndFrame next = Dispatch().endFrame;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return next(session, frameEndInfo);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
