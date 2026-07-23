// Head/VIEW-space override + controller grip/aim pose override implementation. Moved verbatim from
// layer_entry.cpp (refactor phase 5); behaviour is unchanged (same math, same VIEW-space
// tracking, same LOCAL reference space, same lock discipline -- see pose_override.h for the invariants).
#include "pose_override.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <openxr/openxr.h>

#include "action_registry.h"   // ActionMutex() + the grip/aim registry containers (cluster F, shared)
#include "layer_state.h"       // HeadPose / StickyPose + LayerStateGetStickyPoses
#include "xr_math.h"           // QMul / QConj / QRot / VAdd / VSub
#include "layer_dispatch.h"    // Dispatch() (raw next xrLocateSpace / xrCreateReferenceSpace)
#include "layer_log.h"         // LayerLog
#include "pose_animator.h"     // AnimatorEvalController (durationMs glide evaluation)

namespace playspectra {

namespace {

void Log(const char* msg, const char* detail = nullptr) { LayerLog(msg, detail); }

// LOCAL reference space the layer creates itself, to express injected controller poses in (the same
// space hello_xr and typical apps use for their app space). Session-scoped: created lazily from the
// live session (only touched on the app thread) and cleared on xrDestroySession.
XrSpace g_local_space = XR_NULL_HANDLE;

// ---------------------------------------------------------------------------------------------
// Head / viewpoint override (WU1). The head is not an input device, so conformance_automation
// can't set it -- the layer overrides xrLocateViews (what the app renders from) and
// xrLocateSpace(VIEW) directly. We re-base the runtime's real views onto the injected head pose,
// preserving each eye's offset (IPD) and FOV so only the head *moves*.
// ---------------------------------------------------------------------------------------------
std::mutex g_view_spaces_mutex;
std::set<XrSpace> g_view_spaces;  // reference spaces of type VIEW (from xrCreateReferenceSpace)
// All reference spaces -> their type, so the `view` command can name the app's view-locate space
// (LOCAL / STAGE / ...). Guarded by g_view_spaces_mutex; populated/erased alongside g_view_spaces.
std::map<XrSpace, XrReferenceSpaceType> g_ref_space_types;

// Injected poses (head, controller grip) are defined in LOCAL (layer_state.h). An app may
// locate in a different world space (e.g. STAGE), so re-express `poseLocal` in `targetSpace` by
// composing it with LOCAL's pose in that space (via the layer's own LOCAL reference space). If the
// transform can't be obtained (no LOCAL space / locate fails / untracked), returns poseLocal as-is
// (correct for LOCAL-space apps, where the transform is identity) and sets *okOut=false.
XrPosef TransformLocalPoseToSpace(XrSession session, const XrPosef& poseLocal, XrSpace targetSpace,
                                  XrTime time, bool* okOut) {
  if (okOut) *okOut = false;
  XrSpace local = EnsureLocalSpace(session);
  if (local == XR_NULL_HANDLE || targetSpace == XR_NULL_HANDLE) return poseLocal;
  PFN_xrLocateSpace locSpace = Dispatch().locateSpace;
  if (!locSpace) return poseLocal;
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  XrResult r = locSpace(local, targetSpace, time, &loc);  // pose of LOCAL origin, in targetSpace
  const XrSpaceLocationFlags need =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if (XR_FAILED(r) || (loc.locationFlags & need) != need) return poseLocal;
  XrPosef out;
  out.orientation = QMul(loc.pose.orientation, poseLocal.orientation);
  out.position = VAdd(loc.pose.position, QRot(loc.pose.orientation, poseLocal.position));
  if (okOut) *okOut = true;
  return out;
}

// GAP-07: log-once guards promoted to file scope so xrDestroyInstance can reset them -- a fresh
// instance (Play-mode repeat) should be able to re-warn instead of staying silent forever.
bool g_warned_head_space = false;
bool g_warned_aim_offset = false;
bool g_warned_pose_null_subaction = false;
bool g_warned_pose_both_hands = false;
bool g_warned_pose_transform = false;

// GAP-04: whether a tracked action space serves `handTop`. PRECONDITION: caller holds g_action_mutex.
bool ActionSpaceServesHand(const ActionSpaceInfo& asi, const std::string& handTop) {
  if (!asi.handTop.empty()) return asi.handTop == handTop;
  for (const std::string& t : InferHandTops(asi.action)) if (t == handTop) return true;
  return false;  // null-subactionPath space with no matching /user/hand/* binding
}

// GAP-04: resolve the static grip->aim rigid offset (aim-in-grip frame) for `handTop`. The offset is
// O = locate(space=aimSpace, base=gripSpace): aim's pose expressed in grip's frame, so aim = grip * O.
// Because grip and aim are the same physical controller, this offset is constant and can be cached.
// Lock discipline (existing 2-phase rule): scan g_action_spaces under g_action_mutex to find the hand's
// grip/aim spaces, then RELEASE the lock before calling next xrLocateSpace (never call the runtime while
// holding g_action_mutex). Re-entry: uses the RAW resolved next pointer, never Hook_xrLocateSpace, so it
// cannot recurse into our own override. Sets *validOut=true and caches only on a VALID offset; on an
// untracked/failed locate it returns identity WITHOUT caching, so a later frame retries (no poisoning).
XrPosef ResolveGripToAimOffset(const std::string& handTop, XrTime time, bool* validOut) {
  XrPosef identity{};
  identity.orientation.w = 1.0f;
  XrSpace gripSpace = XR_NULL_HANDLE;
  XrSpace aimSpace = XR_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lock(ActionMutex());
    auto cached = RegistryGripToAimValid().find(handTop);
    if (cached != RegistryGripToAimValid().end()) {
      if (validOut) *validOut = true;
      return RegistryGripToAim()[handTop];
    }
    for (const auto& kv : RegistryActionSpaces()) {
      const ActionSpaceInfo& asi = kv.second;
      if (!ActionSpaceServesHand(asi, handTop)) continue;
      if (RegistryGripPoseActions().count(asi.action)) gripSpace = kv.first;
      else if (RegistryAimPoseActions().count(asi.action)) aimSpace = kv.first;
    }
  }  // g_action_mutex released before touching the runtime
  if (gripSpace == XR_NULL_HANDLE || aimSpace == XR_NULL_HANDLE) {
    if (validOut) *validOut = false;
    return identity;  // both spaces not yet created; don't cache -> retry next locate
  }
  // RAW next (not our hook) to avoid self-re-entry / double override.
  PFN_xrLocateSpace rawLocate = Dispatch().locateSpace;
  if (!rawLocate) {
    if (validOut) *validOut = false;
    return identity;
  }
  XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
  XrResult r = rawLocate(aimSpace, gripSpace, time, &loc);
  const XrSpaceLocationFlags need =
      XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if (XR_FAILED(r) || (loc.locationFlags & need) != need) {
    if (!g_warned_aim_offset) {
      g_warned_aim_offset = true;
      Log("aim override: runtime grip->aim offset untracked; falling back to identity (aim=grip) "
          "until it reports a valid offset (headless degrade-graceful; not cached, retried each locate)");
    }
    if (validOut) *validOut = false;
    return identity;  // degrade gracefully; do NOT cache identity (avoids permanent aim=grip collapse)
  }
  {
    std::lock_guard<std::mutex> lock(ActionMutex());
    RegistryGripToAim()[handTop] = loc.pose;
    RegistryGripToAimValid().insert(handTop);
  }
  if (validOut) *validOut = true;
  return loc.pose;
}

}  // namespace

// Create the layer's own LOCAL reference space to express injected controller poses in. Lazy:
// needs a live session. Returns XR_NULL_HANDLE if the runtime can't provide it.
XrSpace EnsureLocalSpace(XrSession session) {
  if (g_local_space != XR_NULL_HANDLE) return g_local_space;
  if (!Dispatch().createReferenceSpace) return XR_NULL_HANDLE;
  XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  ci.poseInReferenceSpace.orientation.w = 1.0f;  // identity
  XrResult r = Dispatch().createReferenceSpace(session, &ci, &g_local_space);
  if (XR_FAILED(r)) {
    g_local_space = XR_NULL_HANDLE;
    Log("EnsureLocalSpace: xrCreateReferenceSpace(LOCAL) failed");
  } else {
    Log("EnsureLocalSpace: LOCAL reference space created");
  }
  return g_local_space;
}

// Human-readable name of a reference space (for the `view` command's `space` field). Returns a
// descriptive fallback for handles we never saw created as a reference space, or types we don't name.
std::string DescribeRefSpace(XrSpace space) {
  XrReferenceSpaceType t;
  {
    std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
    auto it = g_ref_space_types.find(space);
    if (it == g_ref_space_types.end()) return "unknown (not a tracked reference space)";
    t = it->second;
  }
  switch (t) {
    case XR_REFERENCE_SPACE_TYPE_VIEW: return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL: return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE: return "STAGE";
    default: return "reference space type " + std::to_string(static_cast<int>(t));
  }
}

bool IsViewSpace(XrSpace s) {
  std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
  return g_view_spaces.count(s) > 0;
}

// Track a created reference space: remember its type (for the `view` space name) and, if it is a VIEW
// space, add it to the override set. Guarded by g_view_spaces_mutex.
void RecordRefSpace(XrSpace space, XrReferenceSpaceType type) {
  std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
  g_ref_space_types[space] = type;  // for the `view` space name
  if (type == XR_REFERENCE_SPACE_TYPE_VIEW)
    g_view_spaces.insert(space);
}

// Stop tracking a destroyed space. OpenXR runtimes may recycle a destroyed handle's value for a
// later space of a different type, so a VIEW handle left in g_view_spaces would cause a false
// positive (and clobber an unrelated world-space location). Mirrors the swapchain destroy tracking.
void EraseRefSpace(XrSpace space) {
  std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
  g_view_spaces.erase(space);
  g_ref_space_types.erase(space);
}

// Re-base runtime-located views onto the injected head pose. `views`/`count` are the runtime's
// result (expressed in the caller's locate space). Each eye keeps its offset from the runtime head
// (so IPD is preserved) and its FOV; only the head position/orientation change.
void RebaseViewsToHead(const HeadPose& h, uint32_t count, XrView* views) {
  if (count == 0 || !views) return;
  XrVector3f rtHeadPos{0.0f, 0.0f, 0.0f};
  for (uint32_t i = 0; i < count; ++i) rtHeadPos = VAdd(rtHeadPos, views[i].pose.position);
  rtHeadPos.x /= count; rtHeadPos.y /= count; rtHeadPos.z /= count;
  const XrQuaternionf rtHeadOri = views[0].pose.orientation;  // both eyes share head orientation
  const XrQuaternionf rtHeadOriInv = QConj(rtHeadOri);
  const XrQuaternionf injOri{h.qx, h.qy, h.qz, h.qw};
  const XrVector3f injPos{h.px, h.py, h.pz};
  for (uint32_t i = 0; i < count; ++i) {
    const XrVector3f localOffset = QRot(rtHeadOriInv, VSub(views[i].pose.position, rtHeadPos));
    const XrQuaternionf relOri = QMul(rtHeadOriInv, views[i].pose.orientation);
    views[i].pose.orientation = QMul(injOri, relOri);
    views[i].pose.position = VAdd(injPos, QRot(injOri, localOffset));
  }
}

HeadPose TransformHeadToSpace(XrSession session, const HeadPose& h, XrSpace targetSpace, XrTime time) {
  XrPosef in{{h.qx, h.qy, h.qz, h.qw}, {h.px, h.py, h.pz}};
  bool ok = false;
  XrPosef out = TransformLocalPoseToSpace(session, in, targetSpace, time, &ok);
  if (!ok && !g_warned_head_space) {
    g_warned_head_space = true;
    Log("head override: could not resolve LOCAL->locate-space transform; assuming app space == "
        "LOCAL (correct for LOCAL-space apps, off for STAGE)");
  }
  HeadPose r = h;
  r.px = out.position.x; r.py = out.position.y; r.pz = out.position.z;
  r.qx = out.orientation.x; r.qy = out.orientation.y; r.qz = out.orientation.z; r.qw = out.orientation.w;
  return r;
}

// ---------------------------------------------------------------------------------------------
// Controller grip-pose in-layer override (WU3b). The Meta sim's CA xrSetInputDeviceLocationEXT
// applies controller POSITION but IGNORES ORIENTATION (verified: byte-identical frames), so -- as
// with the head -- the layer becomes the authoritative source for the controller pose by overriding
// xrLocateSpace on the hand's grip action space (full position + orientation). CA location stays as
// a harmless position-only fallback. To find the grip action spaces we track: which actions are
// bound to a .../input/grip/pose path (xrSuggestInteractionProfileBindings), and which XrSpaces are
// action spaces for which hand (xrCreateActionSpace). (xrPathToString now lives in g_dispatch.)
// The action-discovery registry (action sets/actions/bindings), the grip/aim action-space tracking,
// the grip->aim offset cache, and the shared g_action_mutex all live in action_registry.cpp
// (refactor phase 4). Cluster E (here) reads/mutates those containers via the Registry* accessors
// while holding ActionMutex(). See action_registry.h for the shared-mutex invariants.

// If `space` is a tracked grip- OR aim-pose action space for a hand with an injected sticky pose, write
// that pose (LOCAL -> baseSpace) into `outPose`/`outFlags` and return true. Takes pose+flags (not the
// struct) so it serves both xrLocateSpace (XrSpaceLocation) and xrLocateSpaces (XrSpaceLocationData).
// `session` may be g_session for the singular xrLocateSpace (which has no session parameter).
// GAP-04: aim spaces get the rigid grip*offset composition. GAP-06: a null-subactionPath space resolves
// its hand lazily from the action's bindings (candidates), with the injected pose as the tiebreak.
bool ApplyPoseOverride(XrSession session, XrSpace space, XrSpace baseSpace, XrTime time,
                       XrPosef& outPose, XrSpaceLocationFlags& outFlags) {
  XrAction action = XR_NULL_HANDLE;
  bool isAim = false;
  bool handTopKnown = false;
  std::vector<std::string> candidates;  // possible hands for this space (1 for subactionPath'd spaces)
  {
    std::lock_guard<std::mutex> lock(ActionMutex());
    auto it = RegistryActionSpaces().find(space);
    if (it == RegistryActionSpaces().end()) return false;
    const ActionSpaceInfo& info = it->second;
    action = info.action;
    const bool isGrip = RegistryGripPoseActions().count(action) > 0;
    isAim = RegistryAimPoseActions().count(action) > 0;
    if (!isGrip && !isAim) return false;
    if (!info.handTop.empty()) {
      handTopKnown = true;
      candidates.push_back(info.handTop);  // subactionPath'd space: exact per-hand match (existing path)
    } else {
      // GAP-06: null subactionPath. Infer hand(s) from the bound paths. Doing this under the lock (the
      // registry read) is what prevents racing a concurrent xrSuggestInteractionProfileBindings mutation.
      candidates = InferHandTops(action);
    }
  }  // lock released: no ControlChannel* call is ever made while holding g_action_mutex.

  if (candidates.empty()) {  // null subactionPath and no /user/hand/* bindings recorded yet
    if (!g_warned_pose_null_subaction) {
      g_warned_pose_null_subaction = true;
      Log("pose override: action space has null subactionPath and no /user/hand/* bindings recorded "
          "yet; can't map to a hand (graceful no-op)");
    }
    return false;
  }

  // Widen the existing single sticky-pose read to accept the first injected pose whose hand is a
  // candidate. Single candidate -> exact per-hand match (unchanged). Ambiguous both-hands candidate ->
  // the injected pose is the natural tiebreak (whichever hand you injected wins).
  std::vector<playspectra::StickyPose> poses = playspectra::LayerStateGetStickyPoses();
  XrPosef gripLocal{};
  bool have = false;
  std::string matchedHand;
  for (const playspectra::StickyPose& sp : poses) {
    bool isCandidate = false;
    for (const std::string& c : candidates) if (c == sp.top_level) { isCandidate = true; break; }
    if (!isCandidate) continue;
    // durationMs glide: evaluate the target through the animator at the locate time. Both this
    // locate path and the xrSyncActions re-apply path share the per-hand glide state, so they agree
    // on where the glide currently is. Called AFTER ActionMutex() was released (leaf-mutex rule).
    gripLocal = AnimatorEvalController(sp, time).pose;
    matchedHand = sp.top_level;
    have = true;
    break;
  }
  if (!have) return false;

  if (candidates.size() > 1) {  // ambiguous both-hands: log once if both hands were injected
    int injected = 0;
    for (const std::string& c : candidates)
      for (const playspectra::StickyPose& sp : poses) if (sp.top_level == c) { ++injected; break; }
    if (injected > 1) {
      if (!g_warned_pose_both_hands) {
        g_warned_pose_both_hands = true;
        Log("pose override: null-subactionPath action bound to both hands with both injected; "
            "using the first match");
      }
    }
  } else if (!handTopKnown) {
    // Optimization: memoize an UNAMBIGUOUS single-hand resolution so later locates skip inference.
    // Never cache the ambiguous both-hands pick -- its tiebreak depends on live injected poses.
    std::lock_guard<std::mutex> lock(ActionMutex());
    auto it = RegistryActionSpaces().find(space);
    if (it != RegistryActionSpaces().end() && it->second.handTop.empty()) it->second.handTop = candidates[0];
  }

  XrPosef poseLocal = gripLocal;
  if (isAim) {
    // Rigid compose in LOCAL: aim = grip * offset (offset = aim-in-grip). Identity fallback -> aim=grip.
    bool offValid = false;
    XrPosef off = ResolveGripToAimOffset(matchedHand, time, &offValid);
    poseLocal.orientation = QMul(gripLocal.orientation, off.orientation);
    poseLocal.position = VAdd(gripLocal.position, QRot(gripLocal.orientation, off.position));
  }

  bool ok = false;
  outPose = TransformLocalPoseToSpace(session, poseLocal, baseSpace, time, &ok);
  if (!ok) {  // parity with the head path: transform to base space unresolved
    if (!g_warned_pose_transform) {
      g_warned_pose_transform = true;
      Log("pose override: LOCAL->locate-space transform unresolved; emitting the LOCAL pose as-is "
          "(correct for LOCAL-space apps, off for STAGE)");
    }
  }
  outFlags |= XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
              XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  return true;
}

// ApplyHeadToLocation / ZeroVelocity / FindInNextChain are pure (I/O-free), so they now live in
// pose_override_pure.h as header-only inline functions and are unit-tested standalone
// (test_pose_override_pure.cpp). They reach this TU and hooks_locate.cpp via pose_override.h.

// xrDestroySession cleanup: destroy the layer's LOCAL reference space and clear the VIEW-space
// tracking (both are session-scoped). The LOCAL space is destroyed via the raw next xrDestroySpace,
// then the VIEW-space containers are cleared under g_view_spaces_mutex -- same order as the original
// hook (no cross-mutex nesting: ActionMutex()-guarded session state is cleared separately by the hook).
void PoseOverrideClearSessionScoped() {
  if (g_local_space != XR_NULL_HANDLE) {
    if (Dispatch().destroySpace) Dispatch().destroySpace(g_local_space);
    g_local_space = XR_NULL_HANDLE;
  }
  {
    std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
    g_view_spaces.clear();  // VIEW spaces belong to this session
    g_ref_space_types.clear();
  }
}

// xrDestroyInstance cleanup: re-arm the log-once guards so a fresh instance (Play-mode repeat) can
// warn again instead of staying silent forever (GAP-07).
void PoseOverrideResetWarnings() {
  g_warned_head_space = false;
  g_warned_aim_offset = false;
  g_warned_pose_null_subaction = false;
  g_warned_pose_both_hands = false;
  g_warned_pose_transform = false;
}

}  // namespace playspectra
