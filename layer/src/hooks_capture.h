// Frame-capture hook cluster -- the swapchain/session/frame hooks that feed capture.cpp. Extracted
// from openxr_agent_layer.cpp (refactor R04) as a move only: the hook bodies are unchanged and the
// dispatch table (kHooks[] in openxr_agent_layer.cpp) still references these by name. There is no
// cross-cluster file-local state, so this header publishes only the hook prototypes (no shared-state
// header is needed); all state lives TU-private in the accessor TUs (layer_dispatch / action_registry /
// pose_override / input_inject). Prototypes are in the global namespace to match the linkage the
// dispatch table uses (the hooks were in an anonymous namespace before; kHooks references them
// unqualified). See openxr_agent_layer.cpp for the dispatch wiring.
#pragma once

#include <openxr/openxr.h>

XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                         XrSession* session);
XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                           XrSwapchain* swapchain);
XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain swapchain);
XrResult XRAPI_CALL Hook_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                    uint32_t* imageCountOutput,
                                                    XrSwapchainImageBaseHeader* images);
XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageAcquireInfo* acquireInfo,
                                                 uint32_t* index);
XrResult XRAPI_CALL Hook_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageReleaseInfo* releaseInfo);
XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo);
