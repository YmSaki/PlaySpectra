// VR-MCP OpenXR API layer.
//
// Exports only xrNegotiateLoaderApiLayerInterface -- the loader chains us in via
// nextGetInstanceProcAddr, we never link against openxr_loader ourselves.
//
// Walking skeleton (tasks #6 + #7-lite): the layer runs a localhost TCP NDJSON control channel
// (control_channel.cpp) that the MCP server drives. Input mutations are applied on the app's own
// thread inside the xrSyncActions hook via XR_EXT_conformance_automation -- which the layer
// transparently enables at instance creation so the runtime handles isActive / interaction-profile
// binding / sync latching for us. xrApplyHapticFeedback is hooked purely to observe "the app buzzed
// the controller" (hello_xr vibrates when grab > 0.9), giving an end-to-end signal without capture.
//
// Implemented since the skeleton: frame capture (xrCreateSession graphics binding + swapchain +
// xrEndFrame, VULKAN readback -- capture.cpp; D3D11/D3D12 still return an explicit "not implemented"
// error, core-required follow-ups); head/VIEW override (xrLocateViews + xrLocateSpace(VIEW)); and
// controller pose override (xrLocateSpace on grip action spaces) since the Meta sim ignores CA
// orientation. The layer is the authoritative pose source at locate time; CA handles buttons/analog.
// TODO(#7-full): per-instance dispatch state -- this still keeps a single global instance/session and
//           each hook latches its next-pointer in a function static (see the 4-agent review notes).

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "action_registry.h"
#include "capture.h"
#include "control_channel.h"
#include "layer_dispatch.h"
#include "layer_log.h"
#include "pose_override.h"

namespace {

void Log(const char* msg, const char* detail = nullptr) { vr_agent::LayerLog(msg, detail); }

// The layer's dispatch table and single-instance state (instance / session / CA flag / next-gipa)
// now live TU-private in layer_dispatch.cpp; these using-declarations pull the accessors into the
// anonymous namespace so the hooks below keep calling them unqualified.
using vr_agent::CaEnabled;
using vr_agent::ClearLayerDispatch;
using vr_agent::CurrentInstance;
using vr_agent::CurrentSession;
using vr_agent::Dispatch;
using vr_agent::NextGetInstanceProcAddr;
using vr_agent::PathToStr;
using vr_agent::RebuildLayerDispatch;
using vr_agent::SetCaEnabled;
using vr_agent::SetCurrentInstance;
using vr_agent::SetCurrentSession;
using vr_agent::SetNextGetInstanceProcAddr;
using vr_agent::ToPath;

// The action-discovery registry, the grip/aim tracking, and the shared action mutex now live
// TU-private in action_registry.cpp; these using-declarations pull the accessors/record helpers into
// the anonymous namespace so the pose (cluster E) / fallback (cluster G) code and the observing hooks
// keep calling them unqualified. Registry types moved to action_registry.h too.
using vr_agent::ActionMutex;
using vr_agent::ActionReg;
using vr_agent::ActionSpaceInfo;
using vr_agent::BindingReg;
using vr_agent::InferHandTops;
using vr_agent::RegistryActions;
using vr_agent::RegistryActionSets;
using vr_agent::RegistryActionSpaces;
using vr_agent::RegistryAimPoseActions;
using vr_agent::RegistryAttachedActionSets;
using vr_agent::RegistryClearInstanceScoped;
using vr_agent::RegistryClearSessionScoped;
using vr_agent::RegistryEraseSpace;
using vr_agent::RegistryGripPoseActions;
using vr_agent::RegistryGripToAim;
using vr_agent::RegistryGripToAimValid;
using vr_agent::RegistryRecordAction;
using vr_agent::RegistryRecordActionSet;
using vr_agent::RegistryRecordActionSpace;
using vr_agent::RegistryRecordAttach;
using vr_agent::RegistryRecordBindings;

// The head/VIEW override and controller grip/aim pose override (cluster D+E) -- the pose math, the
// VIEW-space tracking, the layer's own LOCAL reference space, and the log-once guards -- now live
// TU-private in pose_override.cpp; these using-declarations pull the public entry points into the
// anonymous namespace so cluster C (ApplyPendingInputs) and the thin locate/lifecycle hooks below keep
// calling them unqualified. See pose_override.h for the lock/velocity invariants.
using vr_agent::ApplyHeadToLocation;
using vr_agent::ApplyPoseOverride;
using vr_agent::DescribeRefSpace;
using vr_agent::EnsureLocalSpace;
using vr_agent::EraseRefSpace;
using vr_agent::FindInNextChain;
using vr_agent::IsViewSpace;
using vr_agent::PoseOverrideClearSessionScoped;
using vr_agent::PoseOverrideResetWarnings;
using vr_agent::RebaseViewsToHead;
using vr_agent::RecordRefSpace;
using vr_agent::TransformHeadToSpace;
using vr_agent::ZeroVelocity;

// Drain queued MCP input commands and apply them via conformance automation. Runs on the app
// thread from inside the xrSyncActions hook, so the runtime latches the new state on this sync.
void ApplyPendingInputs(XrSession session) {
  std::vector<vr_agent::PendingInput> batch = vr_agent::ControlChannelDrainInputs();
  std::vector<vr_agent::StickyPose> poses = vr_agent::ControlChannelGetStickyPoses();
  if (batch.empty() && poses.empty()) return;
  if (!CaEnabled()) {
    Log("input dropped: conformance_automation not enabled on this runtime");
    return;
  }

  for (const vr_agent::PendingInput& p : batch) {
    const XrPath top = ToPath(p.top_level);
    switch (p.type) {
      case vr_agent::InputType::Float:
        if (Dispatch().setInputDeviceStateFloat) {
          XrResult r = Dispatch().setInputDeviceStateFloat(session, top, ToPath(p.source), p.f);
          Log("setInputDeviceStateFloat", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case vr_agent::InputType::Bool:
        if (Dispatch().setInputDeviceStateBool) {
          XrResult r =
              Dispatch().setInputDeviceStateBool(session, top, ToPath(p.source), p.b ? XR_TRUE : XR_FALSE);
          Log("setInputDeviceStateBool", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case vr_agent::InputType::Vector2f:
        if (Dispatch().setInputDeviceStateVector2f) {
          XrVector2f v{p.x, p.y};
          XrResult r = Dispatch().setInputDeviceStateVector2f(session, top, ToPath(p.source), v);
          Log("setInputDeviceStateVector2f", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case vr_agent::InputType::Active:
        if (Dispatch().setInputDeviceActive) {
          XrResult r =
              Dispatch().setInputDeviceActive(session, ToPath(p.profile), top, p.b ? XR_TRUE : XR_FALSE);
          Log("setInputDeviceActive", (p.top_level + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
    }
  }

  // Re-apply sticky controller poses every sync (held until pose_clear). Injected via
  // xrSetInputDeviceLocationEXT in the layer's LOCAL space -- this is the controller-pose half of
  // task #7 (head/VIEW override is separate). If the runtime lacks the EXT_conformance_automation
  // location entry point or a LOCAL space, we log once per attempt and no-op gracefully.
  if (!poses.empty() && Dispatch().setInputDeviceLocation) {
    XrSpace space = EnsureLocalSpace(session);
    if (space != XR_NULL_HANDLE) {
      for (const vr_agent::StickyPose& sp : poses) {
        XrPosef pose{};
        pose.orientation = XrQuaternionf{sp.qx, sp.qy, sp.qz, sp.qw};
        pose.position = XrVector3f{sp.px, sp.py, sp.pz};
        XrResult r =
            Dispatch().setInputDeviceLocation(session, ToPath(sp.top_level), ToPath(sp.source), space, pose);
        Log("setInputDeviceLocation", (sp.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
      }
    }
  } else if (!poses.empty() && !Dispatch().setInputDeviceLocation) {
    Log("pose dropped: xrSetInputDeviceLocationEXT unavailable on this runtime");
  }
}

// ---------------------------------------------------------------------------------------------
// GAP-08: non-CA input fallback. On a runtime WITHOUT XR_EXT_conformance_automation we can't push
// button/analog state into the runtime, so the layer emulates the OpenXR action system itself:
//   - drained MCP injections are latched into a sticky (action, subactionPath) store on xrSyncActions;
//   - xrGetActionState{Boolean,Float,Vector2f} are intercepted to return those latched values with
//     isActive=TRUE (for actions whose set was active this sync) + changedSinceLastSync/lastChangeTime;
//   - xrGetCurrentInteractionProfile reports an emulated profile and one synthetic
//     XrEventDataInteractionProfileChanged is delivered via xrPollEvent, so the app treats the virtual
//     controller as connected and actually queries the actions.
// This is the "all keys, not just Enter" completeness for engine-independent input; controller POSE
// already works without CA via ApplyPoseOverride, so only buttons/analog live here. All state is under
// the existing g_action_mutex (the reverse lookup reads g_actions, so a separate lock would invert the
// order). NOTE: our test runtime (Meta sim) HAS conformance_automation, so this path is build+review
// verified but not runtime-exercised -- it needs a non-CA runtime + app to drive end to end.
struct FallbackActionState {
  bool b = false;
  float f = 0.0f;
  XrVector2f v{0.0f, 0.0f};
  // Snapshot at the previous sync, to compute changedSinceLastSync.
  bool syncedB = false;
  float syncedF = 0.0f;
  XrVector2f syncedV{0.0f, 0.0f};
  bool everSynced = false;
  XrBool32 changedSinceLastSync = XR_FALSE;
  XrTime lastChangeTime = 0;
};
// key = (action, subactionPath). subactionPath is the injected top-level hand path (e.g.
// /user/hand/right); a NULL-path query aggregates all a given action's entries. Guarded by g_action_mutex.
std::map<std::pair<XrAction, XrPath>, FallbackActionState> g_fallback_states;
std::set<XrActionSet> g_synced_active_sets;  // action sets active as of the most recent xrSyncActions
XrPath g_emulated_profile = XR_NULL_PATH;     // first interaction profile the app suggested bindings for
bool g_pending_ip_event = false;              // one synthetic InteractionProfileChanged to deliver
XrTime g_fallback_sync_counter = 0;           // monotonic best-effort lastChangeTime source (approx)

float AbsF(float x) { return x < 0.0f ? -x : x; }

// Actions bound to `sourceBindingPath` (e.g. /user/hand/right/input/trigger/value) in the recorded
// suggestion registry. PRECONDITION: caller holds g_action_mutex. Binding paths are profile-agnostic
// in practice (same path across profiles), so we match on path alone.
std::vector<XrAction> ActionsBoundTo(const std::string& sourceBindingPath) {
  std::vector<XrAction> out;
  for (const auto& kv : RegistryActions()) {
    for (const BindingReg& bnd : kv.second.bindings) {
      if (bnd.path == sourceBindingPath) {
        out.push_back(kv.first);
        break;
      }
    }
  }
  return out;
}

// xrSyncActions latch for the non-CA path: drain queued button/analog injections into the sticky store,
// refresh the active-action-set set, and recompute changedSinceLastSync/lastChangeTime. Controller
// poses are NOT handled here -- ApplyPoseOverride is the authoritative pose source in both CA and non-CA.
void ApplyFallbackSync(const XrActionsSyncInfo* syncInfo) {
  std::vector<vr_agent::PendingInput> batch = vr_agent::ControlChannelDrainInputs();

  // Resolve each injection's subactionPath BEFORE taking g_action_mutex: ToPath calls the runtime
  // (xrStringToPath), and the established rule (GAP-06) is to never call the runtime while holding
  // g_action_mutex. Active commands carry no state value, so they are dropped here. `p` points into
  // `batch`, which outlives this vector.
  struct Resolved { const vr_agent::PendingInput* p; XrPath sub; };
  std::vector<Resolved> resolved;
  resolved.reserve(batch.size());
  for (const vr_agent::PendingInput& p : batch) {
    if (p.type == vr_agent::InputType::Active) continue;
    resolved.push_back({&p, ToPath(p.top_level)});
  }

  std::lock_guard<std::mutex> lock(ActionMutex());

  for (const Resolved& r : resolved) {
    const vr_agent::PendingInput& p = *r.p;
    for (XrAction action : ActionsBoundTo(p.source)) {  // g_actions read: fine under the lock
      FallbackActionState& st = g_fallback_states[{action, r.sub}];
      switch (p.type) {
        case vr_agent::InputType::Float:  st.f = p.f; st.b = p.f > 0.5f; break;
        case vr_agent::InputType::Bool:   st.b = p.b; st.f = p.b ? 1.0f : 0.0f; break;
        case vr_agent::InputType::Vector2f: st.v = XrVector2f{p.x, p.y}; break;
        default: break;
      }
    }
  }

  g_synced_active_sets.clear();
  if (syncInfo && syncInfo->activeActionSets) {
    for (uint32_t i = 0; i < syncInfo->countActiveActionSets; ++i)
      g_synced_active_sets.insert(syncInfo->activeActionSets[i].actionSet);
  }

  const XrTime now = ++g_fallback_sync_counter;  // approximate: OpenXR gives no time to xrSyncActions
  for (auto& kv : g_fallback_states) {
    FallbackActionState& st = kv.second;
    XrActionSet set = XR_NULL_HANDLE;
    auto ai = RegistryActions().find(kv.first.first);
    if (ai != RegistryActions().end()) set = ai->second.actionSet;
    if (!g_synced_active_sets.count(set)) {  // inactive set -> not latched, reported inactive
      st.changedSinceLastSync = XR_FALSE;
      continue;
    }
    const bool changed = !st.everSynced || st.syncedB != st.b || st.syncedF != st.f ||
                         st.syncedV.x != st.v.x || st.syncedV.y != st.v.y;
    st.changedSinceLastSync = changed ? XR_TRUE : XR_FALSE;
    if (changed) st.lastChangeTime = now;
    st.syncedB = st.b; st.syncedF = st.f; st.syncedV = st.v; st.everSynced = true;
  }
}

// Aggregate the fallback store for a (action, subactionPath) query. NULL subactionPath aggregates all
// of the action's entries (bool OR, float/vec2 absolute-max), per the OpenXR combination rules.
// PRECONDITION: caller holds g_action_mutex.
struct FallbackAgg {
  bool found = false;
  bool active = false;
  bool b = false;
  float f = 0.0f;
  XrVector2f v{0.0f, 0.0f};
  XrBool32 changed = XR_FALSE;
  XrTime lastChangeTime = 0;
};
FallbackAgg AggregateFallback(XrAction action, XrPath subactionPath) {
  FallbackAgg a;
  XrActionSet set = XR_NULL_HANDLE;
  auto ai = RegistryActions().find(action);
  if (ai != RegistryActions().end()) set = ai->second.actionSet;
  const bool setActive = g_synced_active_sets.count(set) > 0;
  float bestVecMag = -1.0f;  // for the Vector2f combination rule below
  for (const auto& kv : g_fallback_states) {
    if (kv.first.first != action) continue;
    if (subactionPath != XR_NULL_PATH && kv.first.second != subactionPath) continue;
    const FallbackActionState& st = kv.second;
    a.found = true;
    if (setActive) a.active = true;
    a.b = a.b || st.b;                          // bool: logical OR
    if (AbsF(st.f) > AbsF(a.f)) a.f = st.f;     // float: greatest absolute value
    // vec2: the whole vector with the greatest magnitude wins (OpenXR combines by length, not per axis).
    const float mag = st.v.x * st.v.x + st.v.y * st.v.y;
    if (mag > bestVecMag) { a.v = st.v; bestVecMag = mag; }
    if (st.changedSinceLastSync) a.changed = XR_TRUE;
    if (st.lastChangeTime > a.lastChangeTime) a.lastChangeTime = st.lastChangeTime;
  }
  return a;
}

// ---------------------------------------------------------------------------------------------
// Hooked functions.
// ---------------------------------------------------------------------------------------------
XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                          XrSession* session) {
  try {
    PFN_xrCreateSession next = Dispatch().createSession;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(instance, createInfo, session);
    if (XR_SUCCEEDED(r) && session) {
      SetCurrentSession(*session);
      vr_agent::ControlChannelSetSession(true);
      vr_agent::CaptureOnCreateSession(createInfo, *session);
      Log("session created");
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
  try {
    PFN_xrDestroySession next = Dispatch().destroySession;
    if (session == CurrentSession()) {
      PoseOverrideClearSessionScoped();  // [D+E] LOCAL space + VIEW-space tracking (session-scoped)
      {
        std::lock_guard<std::mutex> lock(ActionMutex());
        RegistryClearSessionScoped();    // [F] action spaces + grip/aim + offset cache + attachment
        g_synced_active_sets.clear();    // GAP-08: sync state is per-session
        g_pending_ip_event = false;
      }
      SetCurrentSession(XR_NULL_HANDLE);
      vr_agent::ControlChannelSetSession(false);
      vr_agent::CaptureOnDestroySession(session);
    }
    return next ? next(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo,
                                            XrSwapchain* swapchain) {
  try {
    PFN_xrCreateSwapchain next = Dispatch().createSwapchain;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, createInfo, swapchain);
    if (XR_SUCCEEDED(r) && swapchain) vr_agent::CaptureOnCreateSwapchain(createInfo, *swapchain);
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrDestroySwapchain(XrSwapchain swapchain) {
  try {
    PFN_xrDestroySwapchain next = Dispatch().destroySwapchain;
    vr_agent::CaptureOnDestroySwapchain(swapchain);
    return next ? next(swapchain) : XR_ERROR_FUNCTION_UNSUPPORTED;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput,
                                                    uint32_t* imageCountOutput,
                                                    XrSwapchainImageBaseHeader* images) {
  try {
    PFN_xrEnumerateSwapchainImages next = Dispatch().enumerateSwapchainImages;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(swapchain, imageCapacityInput, imageCountOutput, images);
    // Record only on the populating call (images non-null and something returned).
    if (XR_SUCCEEDED(r) && images && imageCountOutput && *imageCountOutput > 0)
      vr_agent::CaptureOnEnumerateImages(swapchain, *imageCountOutput, images);
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrAcquireSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageAcquireInfo* acquireInfo,
                                                 uint32_t* index) {
  try {
    PFN_xrAcquireSwapchainImage next = Dispatch().acquireSwapchainImage;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(swapchain, acquireInfo, index);
    if (XR_SUCCEEDED(r) && index) vr_agent::CaptureOnAcquireImage(swapchain, *index);
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrReleaseSwapchainImage(XrSwapchain swapchain,
                                                 const XrSwapchainImageReleaseInfo* releaseInfo) {
  try {
    PFN_xrReleaseSwapchainImage next = Dispatch().releaseSwapchainImage;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    vr_agent::CaptureOnReleaseImage(swapchain);
    return next(swapchain, releaseInfo);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
  try {
    vr_agent::CaptureOnEndFrame(frameEndInfo);
    PFN_xrEndFrame next = Dispatch().endFrame;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return next(session, frameEndInfo);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
  try {
    // CA runtimes push state into the runtime; non-CA runtimes get the in-layer action-system emulation.
    if (CaEnabled()) ApplyPendingInputs(session);
    else ApplyFallbackSync(syncInfo);
    PFN_xrSyncActions next = Dispatch().syncActions;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return next(session, syncInfo);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// GAP-08: non-CA fallback readers. Each forwards to the runtime first; only when CA is OFF and we hold a
// latched injection for this action do we overwrite the answer (so uninjected actions pass through).
XrResult XRAPI_CALL Hook_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                 XrActionStateBoolean* state) {
  try {
    PFN_xrGetActionStateBoolean next = Dispatch().getActionStateBoolean;
    XrResult r = next ? next(session, getInfo, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (CaEnabled() || XR_FAILED(r) || !getInfo || !state) return r;
    std::lock_guard<std::mutex> lock(ActionMutex());
    FallbackAgg a = AggregateFallback(getInfo->action, getInfo->subactionPath);
    if (!a.found) return r;  // no injected value: leave the runtime's answer untouched
    state->isActive = a.active ? XR_TRUE : XR_FALSE;
    state->currentState = (a.active && a.b) ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = a.active ? a.changed : XR_FALSE;
    state->lastChangeTime = a.lastChangeTime;
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                               XrActionStateFloat* state) {
  try {
    PFN_xrGetActionStateFloat next = Dispatch().getActionStateFloat;
    XrResult r = next ? next(session, getInfo, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (CaEnabled() || XR_FAILED(r) || !getInfo || !state) return r;
    std::lock_guard<std::mutex> lock(ActionMutex());
    FallbackAgg a = AggregateFallback(getInfo->action, getInfo->subactionPath);
    if (!a.found) return r;
    state->isActive = a.active ? XR_TRUE : XR_FALSE;
    state->currentState = a.active ? a.f : 0.0f;
    state->changedSinceLastSync = a.active ? a.changed : XR_FALSE;
    state->lastChangeTime = a.lastChangeTime;
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                                  XrActionStateVector2f* state) {
  try {
    PFN_xrGetActionStateVector2f next = Dispatch().getActionStateVector2f;
    XrResult r = next ? next(session, getInfo, state) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (CaEnabled() || XR_FAILED(r) || !getInfo || !state) return r;
    std::lock_guard<std::mutex> lock(ActionMutex());
    FallbackAgg a = AggregateFallback(getInfo->action, getInfo->subactionPath);
    if (!a.found) return r;
    state->isActive = a.active ? XR_TRUE : XR_FALSE;
    state->currentState = a.active ? a.v : XrVector2f{0.0f, 0.0f};
    state->changedSinceLastSync = a.active ? a.changed : XR_FALSE;
    state->lastChangeTime = a.lastChangeTime;
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// GAP-08: report the emulated interaction profile when a non-CA/headless runtime says no controller is
// connected, so the app enables and queries the actions we're feeding.
XrResult XRAPI_CALL Hook_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                        XrInteractionProfileState* profileState) {
  try {
    PFN_xrGetCurrentInteractionProfile next = Dispatch().getCurrentInteractionProfile;
    XrResult r =
        next ? next(session, topLevelUserPath, profileState) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (CaEnabled() || !profileState) return r;
    std::lock_guard<std::mutex> lock(ActionMutex());
    if (g_emulated_profile != XR_NULL_PATH &&
        (XR_FAILED(r) || profileState->interactionProfile == XR_NULL_PATH)) {
      profileState->interactionProfile = g_emulated_profile;
      r = XR_SUCCESS;
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// GAP-08: deliver one synthetic InteractionProfileChanged before falling through to the runtime's own
// events, so a non-CA app learns the virtual controller connected. Only ever fires in fallback mode.
XrResult XRAPI_CALL Hook_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
  try {
    if (!CaEnabled() && eventData) {
      bool deliver = false;
      {
        std::lock_guard<std::mutex> lock(ActionMutex());
        if (g_pending_ip_event) {
          g_pending_ip_event = false;
          deliver = true;
        }
      }
      if (deliver) {
        auto* ev = reinterpret_cast<XrEventDataInteractionProfileChanged*>(eventData);
        ev->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        ev->next = nullptr;
        ev->session = CurrentSession();
        Log("synthesized InteractionProfileChanged (non-CA fallback: virtual controller connected)");
        return XR_SUCCESS;
      }
    }
    PFN_xrPollEvent next = Dispatch().pollEvent;
    return next ? next(instance, eventData) : XR_EVENT_UNAVAILABLE;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                                const XrHapticBaseHeader* hapticFeedback) {
  try {
    float amplitude = 0.0f;
    if (hapticFeedback && hapticFeedback->type == XR_TYPE_HAPTIC_VIBRATION) {
      amplitude = reinterpret_cast<const XrHapticVibration*>(hapticFeedback)->amplitude;
    }
    // Report which hand buzzed (the haptic action's subactionPath), not a hardcoded "unknown".
    std::string hand = "unknown";
    if (hapticActionInfo) {
      std::string sp = PathToStr(hapticActionInfo->subactionPath);
      if (!sp.empty()) hand = sp;
    }
    vr_agent::ControlChannelRecordHaptic(hand, amplitude);
    Log("xrApplyHapticFeedback (app buzzed the controller -- grab detected)");
    PFN_xrApplyHapticFeedback next = Dispatch().applyHapticFeedback;
    return next ? next(session, hapticActionInfo, hapticFeedback) : XR_SUCCESS;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
  try {
    PFN_xrDestroyInstance next = Dispatch().destroyInstance;  // capture before clearing the table
    Log("xrDestroyInstance -- stopping control channel");
    {
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryClearInstanceScoped();  // [F] action sets + actions + attachment (instance-scoped)
      g_fallback_states.clear();  // GAP-08: fallback state is keyed by (now-invalid) actions
      g_synced_active_sets.clear();
      g_emulated_profile = XR_NULL_PATH;
      g_pending_ip_event = false;
      g_fallback_sync_counter = 0;
    }
    vr_agent::ControlChannelStop();
    vr_agent::ControlChannelSetInstance(false);
    vr_agent::ControlChannelSetSession(false);
    XrResult r = next ? next(instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (instance == CurrentInstance()) {
      // GAP-07: drop all next-layer pointers (they belong to the runtime we just tore down) and reset
      // instance-scoped flags, so a fresh xrCreateApiLayerInstance re-resolves against the new chain
      // instead of holding this runtime's stale entry points / CA availability.
      SetCurrentInstance(XR_NULL_HANDLE);
      SetCaEnabled(false);
      PoseOverrideResetWarnings();  // [D+E] re-arm the pose log-once guards for a fresh instance
      ClearLayerDispatch();  // takes g_mutex; not held here
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// xrLocateViews: what the app renders (and submits) from. Override to the injected head pose.
XrResult XRAPI_CALL Hook_xrLocateViews(XrSession session, const XrViewLocateInfo* viewLocateInfo,
                                       XrViewState* viewState, uint32_t viewCapacityInput,
                                       uint32_t* viewCountOutput, XrView* views) {
  try {
    PFN_xrLocateViews next = Dispatch().locateViews;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(r) && views && viewCountOutput && *viewCountOutput > 0) {
      vr_agent::HeadPose h;
      // Head override is a PULL-model interception: unlike controller poses (which are pushed to the
      // runtime via xrSetInputDeviceLocationEXT on every xrSyncActions), the head is applied by
      // reading g_head here at locate time and rewriting the runtime's answer -- no per-sync re-apply.
      if (vr_agent::ControlChannelGetHead(h)) {
        const XrViewStateFlags need =
            XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
        // Rebase only off *valid* runtime views -- the per-eye IPD/offset decomposition is meaningless
        // if the runtime returned untracked/garbage poses.
        if (viewState && (viewState->viewStateFlags & need) == need) {
          const vr_agent::HeadPose hInSpace =
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
          std::vector<vr_agent::ViewInfo> infos;
          infos.reserve(*viewCountOutput);
          for (uint32_t i = 0; i < *viewCountOutput; ++i) {
            vr_agent::ViewInfo vi;
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
          vr_agent::ControlChannelSetViews(infos, space);
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

// Stop tracking a destroyed space. OpenXR runtimes may recycle a destroyed handle's value for a
// later space of a different type, so a VIEW handle left in g_view_spaces would cause a false
// positive (and clobber an unrelated world-space location). Mirrors the swapchain destroy tracking.
XrResult XRAPI_CALL Hook_xrDestroySpace(XrSpace space) {
  try {
    PFN_xrDestroySpace next = Dispatch().destroySpace;
    EraseRefSpace(space);  // [D+E] stop tracking a destroyed reference space (handle reuse safety)
    {
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryEraseSpace(space);
    }
    return next ? next(space) : XR_ERROR_FUNCTION_UNSUPPORTED;
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
        vr_agent::HeadPose h;
        if (!IsViewSpace(baseSpace) && vr_agent::ControlChannelGetHead(h)) {
          const vr_agent::HeadPose hInBase = TransformHeadToSpace(CurrentSession(), h, baseSpace, time);
          overrode = ApplyHeadToLocation(hInBase, location->pose, location->locationFlags);
        }
      } else {
        // Controller grip/aim pose override (authoritative pose path; makes orientation work regardless
        // of the runtime's CA behaviour). No-op unless this is a tracked pose space with a pose set.
        overrode =
            ApplyPoseOverride(CurrentSession(), space, baseSpace, time, location->pose, location->locationFlags);
      }
      // GAP-05: only zero velocity on entries we actually overrode (static injected pose -> no motion).
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
      vr_agent::HeadPose h;
      const bool headActive = !IsViewSpace(locateInfo->baseSpace) && vr_agent::ControlChannelGetHead(h);
      const vr_agent::HeadPose hInBase =
          headActive ? TransformHeadToSpace(session, h, locateInfo->baseSpace, locateInfo->time) : h;
      // GAP-05: optional parallel XrSpaceVelocities in the output chain (fetched once). Per-entry
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

// Track action spaces: XrSpace -> (action, hand). Needed to know which spaces are grip pose spaces.
XrResult XRAPI_CALL Hook_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* ci,
                                             XrSpace* space) {
  try {
    PFN_xrCreateActionSpace next = Dispatch().createActionSpace;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, ci, space);
    if (XR_SUCCEEDED(r) && space && ci) {
      const std::string hand = PathToStr(ci->subactionPath);  // "" if XR_NULL_PATH
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryRecordActionSpace(*space, ci->action, hand);
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Track which actions are bound to a .../input/grip/pose path (so their action spaces are the grip
// pose spaces to override), AND record the full action->(interaction profile, binding path) map for
// the `actions` discovery dump. Records then forwards unchanged.
XrResult XRAPI_CALL Hook_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) {
  try {
    PFN_xrSuggestInteractionProfileBindings next = Dispatch().suggestInteractionProfileBindings;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (suggestedBindings && suggestedBindings->suggestedBindings) {
      const std::string profile = PathToStr(suggestedBindings->interactionProfile);
      std::lock_guard<std::mutex> lock(ActionMutex());
      // GAP-08: remember the first suggested profile as the one we emulate on a non-CA runtime.
      if (g_emulated_profile == XR_NULL_PATH)
        g_emulated_profile = suggestedBindings->interactionProfile;
      RegistryRecordBindings(suggestedBindings, profile);
    }
    return next(instance, suggestedBindings);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Record an action set: XrActionSet -> (name, localizedName). Forwards unchanged.
XrResult XRAPI_CALL Hook_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* ci,
                                           XrActionSet* actionSet) {
  try {
    PFN_xrCreateActionSet next = Dispatch().createActionSet;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(instance, ci, actionSet);
    if (XR_SUCCEEDED(r) && actionSet && ci) {
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryRecordActionSet(*actionSet, ci);
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Record an action: XrAction -> (owning set, name, localizedName, type, subactionPaths). Forwards
// unchanged. Subaction paths are stringified here (app thread) so the socket-thread dump is string-only.
XrResult XRAPI_CALL Hook_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* ci,
                                        XrAction* action) {
  try {
    PFN_xrCreateAction next = Dispatch().createAction;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(actionSet, ci, action);
    if (XR_SUCCEEDED(r) && action && ci) {
      ActionReg reg;
      reg.actionSet = actionSet;
      reg.name = ci->actionName;
      reg.localizedName = ci->localizedActionName;
      reg.type = ci->actionType;
      for (uint32_t i = 0; i < ci->countSubactionPaths; ++i)
        reg.subactionPaths.push_back(PathToStr(ci->subactionPaths[i]));
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryRecordAction(*action, std::move(reg));
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Drop a destroyed action set (and its actions) from the registry. xrDestroyActionSet destroys the
// set and all actions it owns; a runtime may later recycle those handle values, so -- as with
// xrDestroySpace -- we erase our tracking to avoid a stale/false entry. Erases then forwards.
XrResult XRAPI_CALL Hook_xrDestroyActionSet(XrActionSet actionSet) {
  try {
    PFN_xrDestroyActionSet next = Dispatch().destroyActionSet;
    {
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryActionSets().erase(actionSet);
      RegistryAttachedActionSets().erase(actionSet);
      auto& actions = RegistryActions();
      for (auto it = actions.begin(); it != actions.end();) {
        if (it->second.actionSet == actionSet) {
          RegistryGripPoseActions().erase(it->first);
          RegistryAimPoseActions().erase(it->first);  // GAP-04: mirror grip erase (handle-reuse safety)
          // GAP-08: drop any fallback state keyed by this action (handle may be recycled).
          for (auto fit = g_fallback_states.begin(); fit != g_fallback_states.end();) {
            if (fit->first.first == it->first) fit = g_fallback_states.erase(fit);
            else ++fit;
          }
          it = actions.erase(it);
        } else {
          ++it;
        }
      }
    }
    return next ? next(actionSet) : XR_ERROR_FUNCTION_UNSUPPORTED;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Record which action sets the app attached to the session (reported as "attached" in the dump).
XrResult XRAPI_CALL Hook_xrAttachSessionActionSets(
    XrSession session, const XrSessionActionSetsAttachInfo* attachInfo) {
  try {
    PFN_xrAttachSessionActionSets next = Dispatch().attachSessionActionSets;
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, attachInfo);
    if (XR_SUCCEEDED(r) && attachInfo && attachInfo->actionSets) {
      std::lock_guard<std::mutex> lock(ActionMutex());
      RegistryRecordAttach(attachInfo);
      // GAP-08: the app has finalized its action sets. On a non-CA runtime, arm the one synthetic
      // InteractionProfileChanged so the next xrPollEvent tells the app the virtual controller connected.
      if (!CaEnabled()) g_pending_ip_event = true;
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// ---------------------------------------------------------------------------------------------
// Dispatch.
// ---------------------------------------------------------------------------------------------
XrResult XRAPI_CALL VrAgentGetInstanceProcAddr(XrInstance instance, const char* name,
                                                PFN_xrVoidFunction* function) {
  try {
    if (name == nullptr || function == nullptr) return XR_ERROR_VALIDATION_FAILURE;

    // Return our own hooks for the intercepted functions.
    struct HookEntry {
      const char* name;
      PFN_xrVoidFunction fn;
    };
    static const HookEntry kHooks[] = {
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSession)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySession)},
        {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrSyncActions)},
        {"xrApplyHapticFeedback", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrApplyHapticFeedback)},
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroyInstance)},
        {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateSwapchain)},
        {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySwapchain)},
        {"xrEnumerateSwapchainImages",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEnumerateSwapchainImages)},
        {"xrAcquireSwapchainImage",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrAcquireSwapchainImage)},
        {"xrReleaseSwapchainImage",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrReleaseSwapchainImage)},
        {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrEndFrame)},
        {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateViews)},
        {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateReferenceSpace)},
        {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateSpace)},
        {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroySpace)},
        {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateActionSpace)},
        {"xrSuggestInteractionProfileBindings",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrSuggestInteractionProfileBindings)},
        {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateActionSet)},
        {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrCreateAction)},
        {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrDestroyActionSet)},
        {"xrAttachSessionActionSets",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrAttachSessionActionSets)},
        // GAP-08: non-CA input fallback (harmless passthrough when CA is enabled).
        {"xrGetActionStateBoolean",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetActionStateBoolean)},
        {"xrGetActionStateFloat", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetActionStateFloat)},
        {"xrGetActionStateVector2f",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetActionStateVector2f)},
        {"xrGetCurrentInteractionProfile",
         reinterpret_cast<PFN_xrVoidFunction>(Hook_xrGetCurrentInteractionProfile)},
        {"xrPollEvent", reinterpret_cast<PFN_xrVoidFunction>(Hook_xrPollEvent)},
    };
    for (const HookEntry& h : kHooks) {
      if (std::strcmp(name, h.name) == 0) {
        *function = h.fn;
        return XR_SUCCESS;
      }
    }

    // xrLocateSpaces (OpenXR 1.1) is optional: only advertise our hook if the runtime provides it,
    // so an app probing support via xrGetInstanceProcAddr isn't misled by a non-null pointer that
    // would then return UNSUPPORTED at call time.
    if (std::strcmp(name, "xrLocateSpaces") == 0) {
      if (Dispatch().locateSpaces != nullptr) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateSpaces);
        return XR_SUCCESS;
      }
      // fall through to passthrough (runtime lacks it -> report as the runtime does)
    }

    PFN_xrGetInstanceProcAddr next = NextGetInstanceProcAddr();
    if (next == nullptr) {
      *function = nullptr;
      return XR_ERROR_HANDLE_INVALID;
    }
    return next(instance, name, function);
  } catch (...) {
    if (function) *function = nullptr;
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Query whether the runtime (below us) advertises an extension, before the instance exists.
bool RuntimeSupportsExtension(PFN_xrGetInstanceProcAddr gipa, const char* ext_name) {
  PFN_xrVoidFunction fn = nullptr;
  if (gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &fn) != XR_SUCCESS || !fn) {
    return false;
  }
  auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(fn);
  uint32_t count = 0;
  if (enumerate(nullptr, 0, &count, nullptr) != XR_SUCCESS || count == 0) return false;
  std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
  if (enumerate(nullptr, count, &count, props.data()) != XR_SUCCESS) return false;
  for (const XrExtensionProperties& p : props) {
    if (std::strcmp(p.extensionName, ext_name) == 0) return true;
  }
  return false;
}

void PublishRuntimeName() {
  PFN_xrGetInstanceProperties get_props = Dispatch().getInstanceProperties;
  if (!get_props) return;
  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (get_props(CurrentInstance(), &props) == XR_SUCCESS) {
    vr_agent::ControlChannelSetRuntimeName(props.runtimeName);
    Log("runtime", props.runtimeName);
  }
}

XrResult XRAPI_CALL VrAgentCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                   const XrApiLayerCreateInfo* apiLayerInfo,
                                                   XrInstance* instance) {
  try {
    if (apiLayerInfo == nullptr ||
        apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        apiLayerInfo->nextInfo == nullptr ||
        apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        apiLayerInfo->nextInfo->nextGetInstanceProcAddr == nullptr ||
        apiLayerInfo->nextInfo->nextCreateApiLayerInstance == nullptr) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }

    PFN_xrGetInstanceProcAddr next_gipa = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance next_create = apiLayerInfo->nextInfo->nextCreateApiLayerInstance;

    // Advance the chain: the next layer must see its own nextInfo, not ours.
    XrApiLayerCreateInfo next_layer_info = *apiLayerInfo;
    next_layer_info.nextInfo = apiLayerInfo->nextInfo->next;

    SetNextGetInstanceProcAddr(next_gipa);

    // Transparently enable XR_EXT_conformance_automation so we can inject input through the
    // runtime. Only do so if the runtime actually supports it -- enabling an unsupported
    // extension would make xrCreateInstance fail and break the app.
    // Escape hatch: VR_AGENT_NO_CA=1 disables the injection (diagnostic / runtimes where enabling
    // CA changes session lifecycle behaviour).
    const bool ca_disabled = std::getenv("VR_AGENT_NO_CA") != nullptr;
    const bool ca_supported =
        !ca_disabled &&
        RuntimeSupportsExtension(next_gipa, XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME);

    std::vector<const char*> exts(info->enabledExtensionNames,
                                  info->enabledExtensionNames + info->enabledExtensionCount);
    bool already_enabled = false;
    for (const char* e : exts) {
      if (std::strcmp(e, XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME) == 0) already_enabled = true;
    }
    if (ca_supported && !already_enabled) {
      exts.push_back(XR_EXT_CONFORMANCE_AUTOMATION_EXTENSION_NAME);
      Log("injecting XR_EXT_conformance_automation into instance extensions");
    } else if (!ca_supported) {
      Log("runtime does NOT support XR_EXT_conformance_automation -- input injection disabled");
    }

    XrInstanceCreateInfo new_info = *info;
    new_info.enabledExtensionNames = exts.data();
    new_info.enabledExtensionCount = static_cast<uint32_t>(exts.size());

    Log("xrCreateApiLayerInstance -> forwarding to next layer");
    XrResult result = next_create(&new_info, &next_layer_info, instance);
    if (XR_SUCCEEDED(result) && instance) {
      SetCurrentInstance(*instance);
      SetCaEnabled(ca_supported);
      // GAP-07: resolve the whole next-layer table now, against THIS instance's chain. Rebuilt on
      // every create so a second instance never inherits the previous runtime's stale pointers.
      RebuildLayerDispatch();
      vr_agent::ControlChannelSetInstance(true);
      vr_agent::ControlChannelSetSession(false);
      vr_agent::ControlChannelSetConformanceAutomation(ca_supported);
      PublishRuntimeName();
      vr_agent::ControlChannelStart();
      Log("instance created; control channel started");
    } else {
      Log("instance creation failed");
    }
    return result;
  } catch (...) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
}

}  // namespace

namespace vr_agent {
// Bridge for control_channel.cpp's `actions` command (declared in layer_log.h). The registry dump
// itself now lives in action_registry.cpp (refactor phase 4); this thin forwarder is kept so
// control_channel.cpp keeps calling the same LayerBuildActionsJson symbol without a new include.
std::string LayerBuildActionsJson() { return BuildActionsJson(); }
}  // namespace vr_agent

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* apiLayerName,
    XrNegotiateApiLayerRequest* apiLayerRequest) {
  try {
    if (loaderInfo == nullptr || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (apiLayerName != nullptr && std::strcmp(apiLayerName, "XR_APILAYER_vr_agent") != 0) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (loaderInfo->minApiVersion > XR_CURRENT_API_VERSION ||
        loaderInfo->maxApiVersion < XR_CURRENT_API_VERSION) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    if (apiLayerRequest == nullptr ||
        apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest)) {
      return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = VrAgentGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = VrAgentCreateApiLayerInstance;

    Log("xrNegotiateLoaderApiLayerInterface OK");
    return XR_SUCCESS;
  } catch (...) {
    // Exceptions must never cross this ABI boundary.
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
