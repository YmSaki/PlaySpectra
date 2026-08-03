// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Locate/reference-space hook cluster implementation: head/VIEW + controller-pose override,
// velocity zeroing, and view publication. See hooks_locate.h.
#include "hooks_locate.h"

#include <string>
#include <vector>

#include "layer_state.h"       // LayerStateGetHead / LayerStateSetViews / HeadPose / ViewInfo
#include "layer_dispatch.h"    // Dispatch() / CurrentSession()
#include "pose_animator.h"     // AnimatorEvalHead / AnimatorNoteDisplayTime (durationMs glide)
#include "pose_override.h"     // pose math + VIEW tracking + velocity/next-chain helpers

using playspectra::ApplyHeadToLocation;
using playspectra::ApplyPoseOverride;
using playspectra::CurrentSession;
using playspectra::DescribeRefSpace;
using playspectra::Dispatch;
using playspectra::FindInNextChain;
using playspectra::IsViewSpace;
using playspectra::RebaseViewsToHead;
using playspectra::RecordRefSpace;
using playspectra::TransformHeadToSpace;
using playspectra::ZeroVelocity;

namespace {
// Evaluate the injected head target through the pose_animator (durationMs glide) at `now`, still in
// LOCAL space -- callers then transform to their locate space as before. Duration 0 returns the
// target unchanged.
playspectra::HeadPose EvalHead(const playspectra::HeadPose& target, XrTime now) {
  const XrPosef p = playspectra::AnimatorEvalHead(target, now).pose;
  playspectra::HeadPose out = target;
  out.px = p.position.x; out.py = p.position.y; out.pz = p.position.z;
  out.qx = p.orientation.x; out.qy = p.orientation.y; out.qz = p.orientation.z;
  out.qw = p.orientation.w;
  return out;
}
}  // namespace

// xrLocateViews: what the app renders (and submits) from. Override to the injected head pose.
XrResult XRAPI_CALL Hook_xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                       XrViewState* viewState, uint32_t viewCapacityInput,
                                       uint32_t* viewCountOutput, XrView* views) {
  try {
    PFN_xrLocateViews next = Dispatch().locateViews;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    // Feed the animator's time base (one of the two intercepted display-time streams; the other is
    // xrEndFrame) so the xrSyncActions path -- which has no XrTime -- can evaluate glides too.
    if (viewLocateInfo) playspectra::AnimatorNoteDisplayTime(viewLocateInfo->displayTime);
    XrResult r = next(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(r) && views && viewCountOutput && *viewCountOutput > 0) {
      playspectra::HeadPose h;
      // Head override is a PULL-model interception: unlike controller poses (which are pushed to the
      // runtime via xrSetInputDeviceLocationEXT on every xrSyncActions), the head is applied by
      // reading g_head here at locate time and rewriting the runtime's answer -- no per-sync re-apply.
      if (playspectra::LayerStateGetHead(h)) {
        h = EvalHead(h, viewLocateInfo ? viewLocateInfo->displayTime
                                       : playspectra::AnimatorLastDisplayTime());
        const XrViewStateFlags need =
            XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        // Rebase only off *valid* runtime views -- the per-eye IPD/offset decomposition is meaningless
        // if the runtime returned untracked/garbage poses.
        if (viewState && (viewState->viewStateFlags & need) == need) {
          const playspectra::HeadPose hInSpace =
              viewLocateInfo ? TransformHeadToSpace(session, h, viewLocateInfo->space,
                                                    viewLocateInfo->displayTime)
                             : h;
          RebaseViewsToHead(hInSpace, *viewCountOutput, views);
          viewState->viewStateFlags |=
              XR_VIEW_STATE_POSITION_TRACKED_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
        }
      }
      // Publish the FINAL located views (after any head override) so the `view` control command can
      // hand the agent the viewpoint pose + FOV -- the observe/act bridge for world<->pixel mapping.
      // Only when the runtime returned valid pose data; otherwise the poses are meaningless.
      if (viewState) {
        const XrViewStateFlags valid =
            XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        if ((viewState->viewStateFlags & valid) == valid) {
          std::vector<playspectra::ViewInfo> infos;
          infos.reserve(*viewCountOutput);
          for (uint32_t i = 0; i < *viewCountOutput; ++i) {
            playspectra::ViewInfo vi;
            vi.px = views[i].pose.position.x;
            vi.py = views[i].pose.position.y;
            vi.pz = views[i].pose.position.z;
            vi.qx = views[i].pose.orientation.x;
            vi.qy = views[i].pose.orientation.y;
            vi.qz = views[i].pose.orientation.z;
            vi.qw = views[i].pose.orientation.w;
            vi.angleLeft = views[i].fov.angleLeft;
            vi.angleRight = views[i].fov.angleRight;
            vi.angleUp = views[i].fov.angleUp;
            vi.angleDown = views[i].fov.angleDown;
            infos.push_back(vi);
          }
          const std::string space =
              viewLocateInfo ? DescribeRefSpace(viewLocateInfo->space) : std::string("unknown");
          playspectra::LayerStateSetViews(infos, space);
        }
      }
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Track which reference spaces are VIEW-type so xrLocateSpace(VIEW) can be overridden consistently.
XrResult XRAPI_CALL Hook_xrCreateReferenceSpace(XrSession session,
                                                const XrReferenceSpaceCreateInfo* createInfo,
                                                XrSpace* space) {
  try {
    PFN_xrCreateReferenceSpace next = Dispatch().createReferenceSpace;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, createInfo, space);
    if (XR_SUCCEEDED(r) && space && createInfo) {
      RecordRefSpace(*space, createInfo->referenceSpaceType);  // track type + VIEW membership
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// xrLocateSpace: override VIEW located in a world (non-VIEW) space to the injected head pose.
XrResult XRAPI_CALL Hook_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time,
                                       XrSpaceLocation* location) {
  try {
    PFN_xrLocateSpace next = Dispatch().locateSpace;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(space, baseSpace, time, location);
    if (XR_SUCCEEDED(r) && location) {
      bool overrode = false;
      if (IsViewSpace(space)) {
        playspectra::HeadPose h;
        if (!IsViewSpace(baseSpace) && playspectra::LayerStateGetHead(h)) {
          h = EvalHead(h, time);
          const playspectra::HeadPose hInBase = TransformHeadToSpace(CurrentSession(), h, baseSpace, time);
          overrode = ApplyHeadToLocation(hInBase, location->pose, location->locationFlags);
        }
      } else {
        // Controller grip/aim pose override (authoritative pose path; makes orientation work regardless
        // of the runtime's CA behaviour). No-op unless this is a tracked pose space with a pose set.
        overrode =
            ApplyPoseOverride(CurrentSession(), space, baseSpace, time, location->pose, location->locationFlags);
      }
      // Velocity zeroing: only zero velocity on entries we actually overrode (static injected pose -> no motion).
      if (overrode) {
        void* v = FindInNextChain(location->next, XR_TYPE_SPACE_VELOCITY);
        if (v) {
          XrSpaceVelocity* vel = reinterpret_cast<XrSpaceVelocity*>(v);
          ZeroVelocity(vel->velocityFlags, vel->linearVelocity, vel->angularVelocity);
        }
      }
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// xrLocateSpaces (OpenXR 1.1 batch locate): same VIEW override, per entry. Only intercepted when
// the runtime actually provides it (guarded in the dispatch table so we never falsely advertise it).
XrResult XRAPI_CALL Hook_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* locateInfo,
                                        XrSpaceLocations* locations) {
  try {
    PFN_xrLocateSpaces next = Dispatch().locateSpaces;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, locateInfo, locations);
    if (XR_SUCCEEDED(r) && locateInfo && locateInfo->spaces && locations && locations->locations) {
      playspectra::HeadPose h;
      const bool headActive = !IsViewSpace(locateInfo->baseSpace) && playspectra::LayerStateGetHead(h);
      if (headActive) h = EvalHead(h, locateInfo->time);
      const playspectra::HeadPose hInBase =
          headActive ? TransformHeadToSpace(session, h, locateInfo->baseSpace, locateInfo->time) : h;
      // Velocity zeroing: optional parallel XrSpaceVelocities in the output chain (fetched once). Per-entry
      // velocities[i] is zeroed only for entries we actually override, with null + range guards.
      XrSpaceVelocities* vels = reinterpret_cast<XrSpaceVelocities*>(
          FindInNextChain(locations->next, XR_TYPE_SPACE_VELOCITIES));
      const uint32_t n = locations->locationCount < locateInfo->spaceCount ? locations->locationCount
                                                                           : locateInfo->spaceCount;
      for (uint32_t i = 0; i < n; ++i) {
        XrSpace s = locateInfo->spaces[i];
        bool overrode = false;
        if (IsViewSpace(s)) {
          if (headActive)
            overrode = ApplyHeadToLocation(hInBase, locations->locations[i].pose,
                                           locations->locations[i].locationFlags);
        } else {
          overrode = ApplyPoseOverride(session, s, locateInfo->baseSpace, locateInfo->time,
                                       locations->locations[i].pose, locations->locations[i].locationFlags);
        }
        if (overrode && vels && vels->velocities && i < vels->velocityCount) {
          ZeroVelocity(vels->velocities[i].velocityFlags, vels->velocities[i].linearVelocity,
                       vels->velocities[i].angularVelocity);
        }
      }
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
