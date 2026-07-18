// durationMs interpolation -- the evaluator that turns a sticky pose TARGET (with an optional
// durationMs) into the pose to inject THIS instant. This is the "glide" half of the design split
// agreed in the refactor plan (§5.1): the control channel's sticky store owns the TARGET (what the
// caller asked for), the animator owns the interpolation state (where the glide currently is:
// start pose, start time, last evaluated value). No new parallel system: every existing injection
// path (sticky re-apply on xrSyncActions, controller locate override, head/VIEW override) simply
// evaluates its target through here before using it, exactly where it used to read the raw target.
//
// Semantics (Playwright-like, linear):
//   - durationMs == 0 (the default)  -> snap to the target immediately (the pre-existing behaviour).
//   - durationMs  > 0                -> lerp position / slerp orientation from the PREVIOUS evaluated
//     value to the target over that wall-clock duration, then hold the target (sticky as before).
//   - no previous evaluated value (first ever set for that hand / the head) -> snap: there is
//     nothing meaningful to glide from (we do not know the runtime's own pose on these paths).
//   - retarget mid-glide -> a new glide starts from the CURRENT evaluated pose (no warp-back).
//
// Targets are identified by generation (StickyPose/HeadPose.seq, stamped by the control channel on
// every set command): a changed seq means "new glide"; an unchanged seq keeps evaluating the one in
// flight. Evaluation is keyed per hand top-level path for controllers plus one head slot -- the
// state is bounded (<= 3 entries), so there is no per-handle cleanup to get wrong.
//
// TIME SOURCE: the app's own display-time stream, which the layer already intercepts. The capture
// and locate hooks feed it in via AnimatorNoteDisplayTime (xrEndFrame's frameEndInfo->displayTime
// and xrLocateViews' viewLocateInfo->displayTime); locate-path evaluations pass their explicit
// XrTime instead. Until the first frame arrives the cached time is 0 and evaluation snaps (see
// AnimatorLastDisplayTime). No xrWaitFrame hook is needed.
//
// LOCK DISCIPLINE: all animator state lives behind its own TU-private leaf mutex. The animator
// never calls a runtime entry point, a ControlChannel* entry point, or anything that takes
// ActionMutex() -- so it is safe to call from any hook path regardless of what the caller holds
// (ApplyPoseOverride calls it AFTER releasing ActionMutex(), keep it that way for consistency).
#pragma once

#include <openxr/openxr.h>

#include "layer_state.h"  // StickyPose / HeadPose (the animation targets)

namespace playspectra {

// Result of evaluating an animated target at one instant. linVel/angVel are the glide's closed-form
// derivative (constant over a linear glide) -- the receptacle for the OPTIONAL §5.2 "real velocity"
// extension. Today's callers only consume `pose` (located velocities stay zeroed as before).
struct EvaluatedPose {
  XrPosef pose;                              // evaluated pose (LOCAL space, same as the target)
  XrVector3f linVel{0.0f, 0.0f, 0.0f};       // m/s while animating, zero otherwise
  XrVector3f angVel{0.0f, 0.0f, 0.0f};       // rad/s while animating, zero otherwise
  bool animating = false;                    // true while the glide is in flight (t < 1)
};

// Feed the app's display-time stream (called from Hook_xrEndFrame / Hook_xrLocateViews).
void AnimatorNoteDisplayTime(XrTime displayTime);
// Latest fed display time, 0 until the first frame. Used as `now` by the xrSyncActions path,
// which has no XrTime of its own.
XrTime AnimatorLastDisplayTime();

// Evaluate a sticky controller target (keyed by target.top_level) / the head target at time `now`.
// `now` == 0 (no display time seen yet) snaps to the target and latches it, so a glide can only
// start once a real time base exists -- no jump-back later.
EvaluatedPose AnimatorEvalController(const StickyPose& target, XrTime now);
EvaluatedPose AnimatorEvalHead(const HeadPose& target, XrTime now);

// Drop all glide state and the cached display time. Called on xrDestroyInstance: XrTime values are
// runtime-scoped, so a fresh instance must not compare its times against a dead runtime's.
void AnimatorReset();

}  // namespace playspectra
