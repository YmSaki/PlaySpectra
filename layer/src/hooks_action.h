// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Action/input + teardown hook cluster -- the action-system observers (create/attach/state/haptic),
// the non-CA fallback readers, and the three teardown hooks (xrDestroySession/xrDestroyInstance/
// xrDestroySpace, which fan out across the pose/registry/fallback state). The dispatch table
// (kHooks[] in layer_entry.cpp) references these by name. No cross-cluster file-local state, so this
// header publishes only the hook prototypes. Prototypes are in the global namespace to match the
// linkage the dispatch table uses (kHooks references them unqualified). See layer_entry.cpp for the
// dispatch wiring.
#pragma once

#include <openxr/openxr.h>

XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session);
XrResult XRAPI_CALL Hook_xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo);
XrResult XRAPI_CALL Hook_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                 XrActionStateBoolean* state);
XrResult XRAPI_CALL Hook_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                               XrActionStateFloat* state);
XrResult XRAPI_CALL Hook_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                                  XrActionStateVector2f* state);
XrResult XRAPI_CALL Hook_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                        XrInteractionProfileState* profileState);
XrResult XRAPI_CALL Hook_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData);
XrResult XRAPI_CALL Hook_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                               const XrHapticBaseHeader* hapticFeedback);
XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance);
XrResult XRAPI_CALL Hook_xrDestroySpace(XrSpace space);
XrResult XRAPI_CALL Hook_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* ci,
                                             XrSpace* space);
XrResult XRAPI_CALL Hook_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings);
XrResult XRAPI_CALL Hook_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* ci,
                                           XrActionSet* actionSet);
XrResult XRAPI_CALL Hook_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* ci,
                                        XrAction* action);
XrResult XRAPI_CALL Hook_xrDestroyActionSet(XrActionSet actionSet);
XrResult XRAPI_CALL Hook_xrAttachSessionActionSets(XrSession session,
                                                   const XrSessionActionSetsAttachInfo* attachInfo);
