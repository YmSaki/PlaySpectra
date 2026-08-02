// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Layer dispatch table + single-instance state. Extracted from layer_entry.cpp (refactor
// phase 3) so the hooks read the next-layer entry points and the current instance/session/CA flag
// through a small API instead of touching file-scope globals directly. The globals themselves live
// TU-private in layer_dispatch.cpp; behaviour (resolution, lock discipline, single-instance model)
// is unchanged.
#pragma once

#include <openxr/openxr.h>

#include <string>

namespace playspectra {

// All next-layer entry points, resolved once per instance from the current chain. Members are read
// on the app thread between a successful create and destroy (the window in which the runtime calls
// our hooks); they are (re)built and cleared under g_mutex. Reads in the hot hooks are unlocked --
// safe under the single-instance model because no hook runs concurrently with create/destroy of the
// same instance. CA/util entries are null unless the runtime provides them.
struct LayerDispatch {
  PFN_xrCreateSession createSession = nullptr;
  PFN_xrDestroySession destroySession = nullptr;
  PFN_xrCreateSwapchain createSwapchain = nullptr;
  PFN_xrDestroySwapchain destroySwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
  PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
  PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;
  PFN_xrEndFrame endFrame = nullptr;
  PFN_xrSyncActions syncActions = nullptr;
  PFN_xrApplyHapticFeedback applyHapticFeedback = nullptr;
  PFN_xrDestroyInstance destroyInstance = nullptr;
  PFN_xrLocateViews locateViews = nullptr;
  PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
  PFN_xrLocateSpace locateSpace = nullptr;
  PFN_xrDestroySpace destroySpace = nullptr;
  PFN_xrCreateActionSpace createActionSpace = nullptr;
  PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings = nullptr;
  PFN_xrCreateActionSet createActionSet = nullptr;
  PFN_xrCreateAction createAction = nullptr;
  PFN_xrDestroyActionSet destroyActionSet = nullptr;
  PFN_xrAttachSessionActionSets attachSessionActionSets = nullptr;
  PFN_xrLocateSpaces locateSpaces = nullptr;  // OpenXR 1.1 / XR_KHR_locate_spaces (may be null)
  // GAP-08: intercepted only for the non-CA input fallback (see the fallback section below).
  PFN_xrGetActionStateBoolean getActionStateBoolean = nullptr;
  PFN_xrGetActionStateFloat getActionStateFloat = nullptr;
  PFN_xrGetActionStateVector2f getActionStateVector2f = nullptr;
  PFN_xrGetCurrentInteractionProfile getCurrentInteractionProfile = nullptr;
  PFN_xrPollEvent pollEvent = nullptr;
  PFN_xrGetInstanceProperties getInstanceProperties = nullptr;
  PFN_xrPathToString pathToString = nullptr;
  PFN_xrStringToPath stringToPath = nullptr;
  // XR_EXT_conformance_automation input injection (null unless the extension is enabled).
  PFN_xrSetInputDeviceActiveEXT setInputDeviceActive = nullptr;
  PFN_xrSetInputDeviceStateBoolEXT setInputDeviceStateBool = nullptr;
  PFN_xrSetInputDeviceStateFloatEXT setInputDeviceStateFloat = nullptr;
  PFN_xrSetInputDeviceStateVector2fEXT setInputDeviceStateVector2f = nullptr;
  PFN_xrSetInputDeviceLocationEXT setInputDeviceLocation = nullptr;  // controller pose injection
};

// Read-only access to the published dispatch table. Reads in the hot hooks are unlocked (see the
// LayerDispatch comment above): the returned reference aliases the single global table.
const LayerDispatch& Dispatch();

// (Re)resolve every next-layer entry point from the current chain and publish it. PRECONDITION:
// the current instance and next-gipa are already set (call right after next_create). The
// next_gipa lookups run WITHOUT g_mutex (resolution must not re-enter our locked state); only the
// publish takes the lock. CA entries are resolved only when CA is enabled -- otherwise the loader
// wouldn't expose the EXT functions anyway (they'd resolve to null).
void RebuildLayerDispatch();

// Drop every next-layer pointer. Called from xrDestroyInstance so stale entry points can't outlive
// the runtime that owns them. Takes g_mutex; callers must not already hold it.
void ClearLayerDispatch();

// Current single-instance handles / CA flag. Read and written on the app thread WITHOUT g_mutex
// (single-instance model: no hook runs concurrently with create/destroy of the same instance).
XrInstance CurrentInstance();
void SetCurrentInstance(XrInstance instance);
XrSession CurrentSession();
void SetCurrentSession(XrSession session);
bool CaEnabled();
void SetCaEnabled(bool enabled);

// Next-layer xrGetInstanceProcAddr passthrough pointer. Read/written under g_mutex (the accessors
// take the lock internally), matching the rebuild/clear publish discipline.
PFN_xrGetInstanceProcAddr NextGetInstanceProcAddr();
void SetNextGetInstanceProcAddr(PFN_xrGetInstanceProcAddr pfn);

// Path helpers backed by the dispatch table's xrStringToPath / xrPathToString and the current
// instance. ToPath returns XR_NULL_PATH and PathToStr returns "" when unavailable.
XrPath ToPath(const std::string& s);
std::string PathToStr(XrPath p);

}  // namespace playspectra
