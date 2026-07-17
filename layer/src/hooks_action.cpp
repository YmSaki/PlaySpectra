// Action/input + teardown hook cluster implementation. Moved verbatim from openxr_agent_layer.cpp
// (refactor R04); behaviour is unchanged (same action-registry observation, same GAP-08 non-CA
// fallback, same teardown fan-out + lock discipline). See hooks_action.h.
#include "hooks_action.h"

#include <mutex>
#include <string>
#include <utility>

#include "action_registry.h"   // ActionMutex() / Registry* / ActionReg
#include "capture.h"           // CaptureOnDestroySession
#include "control_channel.h"   // ControlChannelStop
#include "layer_state.h"       // LayerState* (session/instance flags, haptic record)
#include "input_inject.h"      // CA + GAP-08 non-CA fallback entry points
#include "layer_dispatch.h"    // Dispatch() / instance-session state / CaEnabled() / PathToStr()
#include "layer_log.h"         // LayerLog
#include "pose_animator.h"     // AnimatorReset (glide state is dead-runtime-scoped, like XrTime)
#include "pose_override.h"     // PoseOverride* session/warning reset + EraseRefSpace

using vr_agent::ActionMutex;
using vr_agent::ActionReg;
using vr_agent::AggregateFallback;
using vr_agent::ApplyFallbackSync;
using vr_agent::ApplyPendingInputs;
using vr_agent::CaEnabled;
using vr_agent::ClearLayerDispatch;
using vr_agent::CurrentInstance;
using vr_agent::CurrentSession;
using vr_agent::Dispatch;
using vr_agent::EraseRefSpace;
using vr_agent::FallbackAgg;
using vr_agent::FallbackArmIpEvent;
using vr_agent::FallbackClearInstanceScoped;
using vr_agent::FallbackClearSessionScoped;
using vr_agent::FallbackCurrentProfile;
using vr_agent::FallbackEraseForAction;
using vr_agent::FallbackNoteSuggestedProfile;
using vr_agent::FallbackTakePendingIpEvent;
using vr_agent::PathToStr;
using vr_agent::PoseOverrideClearSessionScoped;
using vr_agent::PoseOverrideResetWarnings;
using vr_agent::RegistryClearInstanceScoped;
using vr_agent::RegistryClearSessionScoped;
using vr_agent::RegistryEraseActionSet;
using vr_agent::RegistryEraseSpace;
using vr_agent::RegistryRecordAction;
using vr_agent::RegistryRecordActionSet;
using vr_agent::RegistryRecordActionSpace;
using vr_agent::RegistryRecordAttach;
using vr_agent::RegistryRecordBindings;
using vr_agent::SetCaEnabled;
using vr_agent::SetCurrentInstance;
using vr_agent::SetCurrentSession;

namespace {

void Log(const char* msg, const char* detail = nullptr) { vr_agent::LayerLog(msg, detail); }

}  // namespace

XrResult XRAPI_CALL Hook_xrDestroySession(XrSession session) {
  try {
    PFN_xrDestroySession next = Dispatch().destroySession;
    if (session == CurrentSession()) {
      PoseOverrideClearSessionScoped();  // [D+E] LOCAL space + VIEW-space tracking (session-scoped)
      {
        std::lock_guard<std::mutex> lock(ActionMutex());
        RegistryClearSessionScoped();    // [F] action spaces + grip/aim + offset cache + attachment
        FallbackClearSessionScoped();    // [G] GAP-08: sync state + pending IP event (per-session)
      }
      SetCurrentSession(XR_NULL_HANDLE);
      vr_agent::LayerStateSetSession(false);
      vr_agent::CaptureOnDestroySession(session);
    }
    return next ? next(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
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
    const XrPath emulated = FallbackCurrentProfile();
    if (emulated != XR_NULL_PATH &&
        (XR_FAILED(r) || profileState->interactionProfile == XR_NULL_PATH)) {
      profileState->interactionProfile = emulated;
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
        deliver = FallbackTakePendingIpEvent();
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
    vr_agent::LayerStateRecordHaptic(hand, amplitude);
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
      FallbackClearInstanceScoped();  // [G] GAP-08: fallback state keyed by (now-invalid) actions
    }
    vr_agent::ControlChannelStop();
    vr_agent::LayerStateSetInstance(false);
    vr_agent::LayerStateSetSession(false);
    // XrTime values are runtime-scoped: drop the cached display time + glide state so a fresh
    // instance never compares its times against this dead runtime's. Leaf mutex; no lock held here.
    vr_agent::AnimatorReset();
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
      FallbackNoteSuggestedProfile(suggestedBindings->interactionProfile);
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
      // Registry-internal erase (action set + actions + grip/aim mirror + GAP-08 fallback) lives in
      // action_registry now; FallbackEraseForAction is injected so that TU needn't know input_inject.
      RegistryEraseActionSet(actionSet, FallbackEraseForAction);
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
      if (!CaEnabled()) FallbackArmIpEvent();
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}
