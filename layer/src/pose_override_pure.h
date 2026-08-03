// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

#ifndef PLAYSPECTRA_POSE_OVERRIDE_PURE_H
#define PLAYSPECTRA_POSE_OVERRIDE_PURE_H

// Pure, I/O-free helpers of the pose-override path, split out (header-only inline) so they are
// unit-testable without linking pose_override.cpp's session/dispatch/state dependencies. Consumers
// (hooks_locate.cpp) reach them via pose_override.h, which includes this header.
#include <openxr/openxr.h>

#include "layer_state.h"  // HeadPose (injected head pose the override consumes)

namespace playspectra {

// Fill a location's pose/flags from the injected head pose. Note: xrLocateSpace(VIEW) returns the
// *single* VIEW-space origin (= the head pose itself), so a direct copy is correct here -- unlike
// xrLocateViews, whose per-eye poses need RebaseViewsToHead to preserve each eye's IPD offset. `h`
// must already be expressed in the location's base space (callers pass a TransformHeadToSpace result).
// Velocity zeroing: returns true (the pose WAS overridden) so callers know this entry is a static injected pose
// and may zero its velocity. It unconditionally overrides, so the return is always true, but the bool
// keeps the "velocity zeroed only on overridden entries" invariant explicit and parallel to
// ApplyPoseOverride's bool.
inline bool ApplyHeadToLocation(const HeadPose& h, XrPosef& pose, XrSpaceLocationFlags& flags) {
  pose.orientation = XrQuaternionf{h.qx, h.qy, h.qz, h.qw};
  pose.position = XrVector3f{h.px, h.py, h.pz};
  flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
           XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  return true;
}

// Velocity zeroing: zero a located space's velocity. We inject a STATIC pose snapshot with no motion model, so we
// assert velocity = 0 and set the VALID bits to suppress the runtime's/app's extrapolation between our
// discrete injections. This is deliberate: even when the agent moves the controller/head every frame,
// the true instantaneous velocity is unknown to us and we intentionally stop extrapolation rather than
// report a stale runtime velocity. There is no TRACKED-equivalent bit for velocity, so none is set.
inline void ZeroVelocity(XrSpaceVelocityFlags& flags, XrVector3f& linear, XrVector3f& angular) {
  linear = XrVector3f{0.0f, 0.0f, 0.0f};
  angular = XrVector3f{0.0f, 0.0f, 0.0f};
  flags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
}

// Velocity zeroing: walk a MUTABLE next-chain for a struct of `type`, returning a writable pointer (we zero into
// it). Non-const on purpose: XrSpaceLocation::next / XrSpaceLocations::next are both non-const void*.
inline void* FindInNextChain(void* next, XrStructureType type) {
  for (XrBaseOutStructure* p = reinterpret_cast<XrBaseOutStructure*>(next); p != nullptr; p = p->next) {
    if (p->type == type) return p;
  }
  return nullptr;
}

}  // namespace playspectra

#endif  // PLAYSPECTRA_POSE_OVERRIDE_PURE_H
