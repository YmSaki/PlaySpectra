// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Frame-capture hook cluster -- the swapchain/session/frame hooks that feed capture.cpp. The
// dispatch table (kHooks[] in layer_entry.cpp) references these by name. There is no cross-cluster
// file-local state, so this header publishes only the hook prototypes (no shared-state header is
// needed); all state lives TU-private in the accessor TUs (layer_dispatch / action_registry /
// pose_override / input_inject). Prototypes are in the global namespace to match the linkage the
// dispatch table uses (kHooks references them unqualified). See layer_entry.cpp for the dispatch
// wiring.
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
