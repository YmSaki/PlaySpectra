// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Locate/reference-space hook cluster -- the head/VIEW + controller-pose override hooks that drive
// pose_override.cpp. The dispatch table (kHooks[] in layer_entry.cpp) references these by name. No
// cross-cluster file-local state, so this header publishes only the hook prototypes. Prototypes are
// in the global namespace to match the linkage the dispatch table uses (kHooks references them
// unqualified). See layer_entry.cpp for the dispatch wiring; note xrDestroySpace stays with the
// teardown cluster in hooks_action.cpp, not here.
#pragma once

#include <openxr/openxr.h>

XrResult XRAPI_CALL Hook_xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                       XrViewState* viewState, uint32_t viewCapacityInput,
                                       uint32_t* viewCountOutput, XrView* views);
XrResult XRAPI_CALL Hook_xrCreateReferenceSpace(XrSession session,
                                                const XrReferenceSpaceCreateInfo* createInfo,
                                                XrSpace* space);
XrResult XRAPI_CALL Hook_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time,
                                       XrSpaceLocation* location);
XrResult XRAPI_CALL Hook_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* locateInfo,
                                        XrSpaceLocations* locations);
