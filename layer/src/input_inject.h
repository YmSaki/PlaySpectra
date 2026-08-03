// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Input injection -- the CA and non-CA paths of the single "inject button/analog input" responsibility.
// Both halves live in one translation unit:
//   [C] ApplyPendingInputs -- on a runtime WITH XR_EXT_conformance_automation, drain queued MCP
//       injections and push them into the runtime (xrSetInputDeviceState* / xrSetInputDeviceLocationEXT)
//       from inside the xrSyncActions hook, so the runtime latches the new state on this sync;
//   [G] the non-CA fallback (ApplyFallbackSync / AggregateFallback + the accessors below) -- on a
//       runtime WITHOUT conformance_automation the layer emulates the OpenXR action system itself.
// Hook_xrSyncActions picks the path with `if (CaEnabled()) ApplyPendingInputs else ApplyFallbackSync`.
// The fallback state (the sticky (action, subactionPath) store, the active-set set, the emulated profile,
// the pending synthetic InteractionProfileChanged, the approximate sync counter) and the [G] helpers
// (ActionsBoundTo, FallbackActionState) are TU-private in input_inject.cpp; this header publishes only
// what the hooks in hooks_action.cpp call.
//
// ---------------------------------------------------------------------------------------------
// Non-CA input fallback, in full. On a runtime
// WITHOUT XR_EXT_conformance_automation we can't push button/analog state into the runtime, so the layer
// emulates the OpenXR action system itself:
//   - drained MCP injections are latched into a sticky (action, subactionPath) store on xrSyncActions;
//   - xrGetActionState{Boolean,Float,Vector2f} are intercepted to return those latched values with
//     isActive=TRUE (for actions whose set was active this sync) + changedSinceLastSync/lastChangeTime;
//   - xrGetCurrentInteractionProfile reports an emulated profile and one synthetic
//     XrEventDataInteractionProfileChanged is delivered via xrPollEvent, so the app treats the virtual
//     controller as connected and actually queries the actions.
// This is the "all keys, not just Enter" completeness for engine-independent input; controller POSE
// already works without CA via ApplyPoseOverride, so only buttons/analog live here. NOTE: our test
// runtime (Meta sim) HAS conformance_automation, so this path is build+review verified but not
// runtime-exercised -- it needs a non-CA runtime + app to drive end to end.
//
// LOCK DISCIPLINE: all [G] state is guarded by ActionMutex() (owned by action_registry -- see
// action_registry.h). The reverse lookup reads g_actions (cluster F's container), so a SEPARATE lock
// would invert the acquisition order relative to F/E; the single shared mutex is required.
// ApplyFallbackSync is TWO-PHASE: it resolves each injection's subactionPath (ToPath
// -> the runtime) BEFORE taking ActionMutex(), then takes the lock -- never call a runtime entry point
// while holding ActionMutex(). Each accessor below documents whether the caller must already hold it.
// ---------------------------------------------------------------------------------------------
#pragma once

#include <openxr/openxr.h>

namespace playspectra {

// [C] CA path: drain queued MCP input commands and apply them via conformance_automation. Runs on the
// app thread from inside the xrSyncActions hook. Takes no internal lock (the CA path touches no [G]
// state); reads the sticky controller poses and re-applies them via xrSetInputDeviceLocationEXT.
void ApplyPendingInputs(XrSession session);

// [G] non-CA path: xrSyncActions latch. Drains queued button/analog injections into the sticky store,
// refreshes the active-action-set set, and recomputes changedSinceLastSync/lastChangeTime. Controller
// poses are NOT handled here (ApplyPoseOverride is authoritative in both CA and non-CA). Takes
// ActionMutex() ITSELF (two-phase: paths resolved before the lock) -- caller must NOT hold it.
void ApplyFallbackSync(const XrActionsSyncInfo* syncInfo);

// Aggregated result of the fallback store for one (action, subactionPath) query. Returned to the
// GetActionState* hooks (hence public).
struct FallbackAgg {
  bool found = false;
  bool active = false;
  bool b = false;
  float f = 0.0f;
  XrVector2f v{0.0f, 0.0f};
  XrBool32 changed = XR_FALSE;
  XrTime lastChangeTime = 0;
};
// Aggregate the fallback store for a (action, subactionPath) query. NULL subactionPath aggregates all
// of the action's entries (bool OR, float absolute-max, vec2 greatest-magnitude), per the OpenXR
// combination rules. PRECONDITION: caller holds ActionMutex().
FallbackAgg AggregateFallback(XrAction action, XrPath subactionPath);

// [G] accessors for the emulated-profile / synthetic-event / per-action state. Each mutates or reads
// the shared fallback state. PRECONDITION: caller holds ActionMutex() (none locks internally).
XrPath FallbackCurrentProfile();          // xrGetCurrentInteractionProfile: the emulated profile (or NULL)
bool FallbackTakePendingIpEvent();        // xrPollEvent: consume the one-shot pending IP-changed flag
void FallbackArmIpEvent();                // xrAttachSessionActionSets: arm the synthetic IP-changed event
void FallbackNoteSuggestedProfile(XrPath profile);  // xrSuggestBindings: remember the FIRST profile only
void FallbackEraseForAction(XrAction action);       // xrDestroyActionSet: drop entries keyed by an action
void FallbackClearSessionScoped();        // xrDestroySession: sync state + pending IP event (per-session)
void FallbackClearInstanceScoped();       // xrDestroyInstance: all fallback state (keyed by dead actions)

}  // namespace playspectra
