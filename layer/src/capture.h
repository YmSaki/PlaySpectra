// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// PlaySpectra frame capture: track swapchains + graphics binding, and at xrEndFrame copy the dominant
// eye's projection subimage to a PNG. See capture.cpp.
//
// CORE COMPLETENESS (CLAUDE.md): color capture must cover ALL OpenXR graphics bindings a runtime
// can hand the app -- D3D11, D3D12, and Vulkan (XrGraphicsBinding{D3D11,D3D12,Vulkan}KHR). These
// are OpenXR-level, not engine-specific; none may be dropped. Vulkan lands first (the real target
// app is Godot/Vulkan and the Meta compositor is Vulkan-native), D3D11/D3D12 follow -- all
// first-class. Depth is a nice-to-have (task #8).
//
// Increment A (this pass): OpenXR-side tracking + xrEndFrame projection parsing + capture-request
// protocol, verifiable without any pixel copy. Increment B: the per-backend GPU readback + PNG.

#pragma once

#include <cstdint>
#include <string>

#include <openxr/openxr.h>

namespace playspectra {

enum class GfxApi { Unknown, Vulkan, D3D11, D3D12 };

const char* GfxApiName(GfxApi api);

// Called from the layer's hooks (app thread). These only record state; the actual GPU work happens
// at xrEndFrame when a capture is requested.
void CaptureOnCreateSession(const XrSessionCreateInfo* createInfo, XrSession session);
void CaptureOnDestroySession(XrSession session);
void CaptureOnCreateSwapchain(const XrSwapchainCreateInfo* createInfo, XrSwapchain swapchain);
void CaptureOnDestroySwapchain(XrSwapchain swapchain);
void CaptureOnEnumerateImages(XrSwapchain swapchain, uint32_t count,
                              const XrSwapchainImageBaseHeader* images);
void CaptureOnAcquireImage(XrSwapchain swapchain, uint32_t index);
void CaptureOnReleaseImage(XrSwapchain swapchain);
void CaptureOnEndFrame(const XrFrameEndInfo* frameEndInfo);

// Called from the control-channel (socket) thread. Requests a screenshot of the given eye
// ("left"|"right"|"dominant") and blocks until the next xrEndFrame fulfils it or timeoutMs elapses.
// When withDepth is true and the app submitted an XrCompositionLayerDepthInfoKHR for that view, the
// result also carries a "depth" object ({available:true, depthPath, depthMeta} or
// {available:false, note}); depth is a nice-to-have and never fabricated (CLAUDE.md).
// Returns a JSON string describing the result (path on success, or diagnostic state / error).
std::string CaptureRequestScreenshot(const std::string& eye, int timeoutMs, bool withDepth = false);

// Snapshot of tracked state for vr_status / diagnostics.
std::string CaptureStatusJson();

// Recording mode: periodic frame capture to a PNG sequence + manifest.
// start returns the session directory path; stop returns a JSON manifest of all captured frames.
std::string CaptureStartRecording(uint32_t intervalFrames, const std::string& eye);
std::string CaptureStopRecording();
bool CaptureIsRecording();

}  // namespace playspectra
