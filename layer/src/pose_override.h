// Head/VIEW-space override + controller grip/aim pose override. Extracted from openxr_agent_layer.cpp
// (refactor phase 5) so the layer's authoritative pose path -- the injected head pose (xrLocateViews /
// xrLocateSpace(VIEW)) and the injected controller grip/aim pose (xrLocateSpace / xrLocateSpaces on
// action spaces) -- lives in one translation unit. This is the "coordinate system of injected input":
// injected poses are defined in the layer's own LOCAL reference space and re-expressed into whatever
// space the app locates in. The quaternion/vector math (GAP-04 grip->aim offset, GAP-05 velocity
// zeroing, GAP-06 null-subactionPath hand inference, IPD-preserving head rebase), the VIEW-space
// tracking, the LOCAL reference space, and the log-once guards are all TU-private in pose_override.cpp;
// this header publishes only what the (thin, remaining) hooks in openxr_agent_layer.cpp call.
// Behaviour -- data, algorithms, lock discipline -- is unchanged; this is a move only.
#pragma once

#include <openxr/openxr.h>

#include <string>

#include "layer_state.h"  // HeadPose (injected head pose the override consumes)

namespace vr_agent {

// ---------------------------------------------------------------------------------------------
// LOCK / SAFETY DISCIPLINE (unchanged from the original; read before touching these functions).
//
// TWO mutexes are involved, and they are NEVER nested here:
//   * g_view_spaces_mutex (TU-private to pose_override.cpp) guards ONLY the VIEW-space tracking
//     (which reference spaces are VIEW-type, and every reference space's type for the `view`
//     command). IsViewSpace / DescribeRefSpace / RecordRefSpace / EraseRefSpace take it; nothing
//     else does. Because it is fully TU-private, no other translation unit can touch that state --
//     any missed call site is a compile error, not a silent aliasing bug.
//   * ActionMutex() (owned by action_registry, shared with cluster F/G -- see action_registry.h)
//     guards the grip/aim registry reads that ApplyPoseOverride / ResolveGripToAimOffset perform.
// The controller-pose functions take ActionMutex() only; the head/VIEW functions take
// g_view_spaces_mutex only. No function here holds both at once, so there is no cross-mutex
// ordering to preserve -- keep it that way.
//
// TWO-PHASE runtime rule (GAP-04): ResolveGripToAimOffset resolves the grip/aim action spaces under
// ActionMutex(), RELEASES the lock, and only THEN calls the RAW next xrLocateSpace (never our own
// hook -> no self-re-entry). ApplyPoseOverride likewise releases ActionMutex() before reading the
// sticky poses. NEVER call a runtime entry point or a ControlChannel* entry point while holding
// ActionMutex().
//
// GAP-05: velocity is zeroed ONLY on entries we actually overrode (ApplyHeadToLocation /
// ApplyPoseOverride return true), because we inject a static pose snapshot with no motion model.
// ---------------------------------------------------------------------------------------------

// Create (lazily, from a live session) the layer's own LOCAL reference space to express injected
// poses in -- the same space hello_xr and typical apps use as their app space. Session-scoped:
// cleared in PoseOverrideClearSessionScoped(). Returns XR_NULL_HANDLE if the runtime can't provide
// it. PUBLIC because cluster C's ApplyPendingInputs (still in openxr_agent_layer.cpp) injects sticky
// controller poses in this same LOCAL space via xrSetInputDeviceLocationEXT.
XrSpace EnsureLocalSpace(XrSession session);

// VIEW-space tracking (guarded by g_view_spaces_mutex, TU-private).
bool IsViewSpace(XrSpace s);                                    // is `s` a tracked VIEW reference space?
std::string DescribeRefSpace(XrSpace space);                    // human name for the `view` command
void RecordRefSpace(XrSpace space, XrReferenceSpaceType type);  // xrCreateReferenceSpace: track type/VIEW
void EraseRefSpace(XrSpace space);                              // xrDestroySpace: stop tracking (handle reuse)

// Head / viewpoint override. RebaseViewsToHead re-bases the runtime's per-eye views onto the injected
// head pose, preserving each eye's IPD offset and FOV (only the head moves). TransformHeadToSpace
// re-expresses an injected head pose (defined in LOCAL) into the app's locate space.
void RebaseViewsToHead(const HeadPose& h, uint32_t count, XrView* views);
HeadPose TransformHeadToSpace(XrSession session, const HeadPose& h, XrSpace targetSpace, XrTime time);

// Fill a location's pose/flags from an injected head pose already expressed in the location's base
// space (xrLocateSpace(VIEW) returns the single VIEW origin = the head pose, so a direct copy). Always
// overrides, so always returns true; the bool keeps the GAP-05 "zero velocity only on overridden
// entries" invariant explicit and parallel to ApplyPoseOverride.
bool ApplyHeadToLocation(const HeadPose& h, XrPosef& pose, XrSpaceLocationFlags& flags);

// Controller grip/aim override: if `space` is a tracked grip/aim action space for a hand with an
// injected sticky pose, write that pose (LOCAL -> baseSpace) into outPose/outFlags and return true.
// Serves both xrLocateSpace and xrLocateSpaces. GAP-04 aim = grip * offset; GAP-06 null-subactionPath
// hand inference. `session` may be the current session for the singular xrLocateSpace (no session arg).
bool ApplyPoseOverride(XrSession session, XrSpace space, XrSpace baseSpace, XrTime time,
                       XrPosef& outPose, XrSpaceLocationFlags& outFlags);

// GAP-05: zero a located space's velocity (static injected pose -> no motion) and set the VALID bits.
void ZeroVelocity(XrSpaceVelocityFlags& flags, XrVector3f& linear, XrVector3f& angular);
// GAP-05: walk a MUTABLE next-chain for a struct of `type`, returning a writable pointer (or null).
void* FindInNextChain(void* next, XrStructureType type);

// Cleanup helpers called from the (thin) lifecycle hooks. ClearSessionScoped destroys the LOCAL space
// and clears the VIEW-space tracking (xrDestroySession); ResetWarnings re-arms the log-once guards so a
// fresh instance can warn again (xrDestroyInstance).
void PoseOverrideClearSessionScoped();
void PoseOverrideResetWarnings();

}  // namespace vr_agent
