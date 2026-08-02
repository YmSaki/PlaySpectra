// Copyright (c) 2026 PlaySpectra contributors
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// SPDX-License-Identifier: MPL-2.0

// Input injection (CA + non-CA fallback) implementation. Moved verbatim from layer_entry.cpp
// (refactor phase 6); behaviour is unchanged (same data, same algorithms, same lock discipline -- see
// input_inject.h for the GAP-08 fallback description and the ActionMutex()/two-phase invariants).
#include "input_inject.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <openxr/openxr.h>

#include "action_registry.h"   // ActionMutex() + RegistryActions() (cluster F container, shared) + BindingReg
#include "layer_state.h"
#include "layer_dispatch.h"    // Dispatch() / CaEnabled() / ToPath() (runtime-backed)
#include "layer_log.h"         // LayerLog
#include "pose_animator.h"     // AnimatorEvalController (durationMs glide evaluation)
#include "pose_override.h"     // EnsureLocalSpace (inject sticky poses in the layer's LOCAL space)

namespace playspectra {

namespace {

void Log(const char* msg, const char* detail = nullptr) { LayerLog(msg, detail); }

}  // namespace

// Drain queued MCP input commands and apply them via conformance automation. Runs on the app
// thread from inside the xrSyncActions hook, so the runtime latches the new state on this sync.
void ApplyPendingInputs(XrSession session) {
  std::vector<playspectra::PendingInput> batch = playspectra::LayerStateDrainInputs();
  std::vector<playspectra::StickyPose> poses = playspectra::LayerStateGetStickyPoses();
  if (batch.empty() && poses.empty()) return;
  if (!CaEnabled()) {
    Log("input dropped: conformance_automation not enabled on this runtime");
    return;
  }

  for (const playspectra::PendingInput& p : batch) {
    const XrPath top = ToPath(p.top_level);
    switch (p.type) {
      case playspectra::InputType::Float:
        if (Dispatch().setInputDeviceStateFloat) {
          XrResult r = Dispatch().setInputDeviceStateFloat(session, top, ToPath(p.source), p.f);
          Log("setInputDeviceStateFloat", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case playspectra::InputType::Bool:
        if (Dispatch().setInputDeviceStateBool) {
          XrResult r =
              Dispatch().setInputDeviceStateBool(session, top, ToPath(p.source), p.b ? XR_TRUE : XR_FALSE);
          Log("setInputDeviceStateBool", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case playspectra::InputType::Vector2f:
        if (Dispatch().setInputDeviceStateVector2f) {
          XrVector2f v{p.x, p.y};
          XrResult r = Dispatch().setInputDeviceStateVector2f(session, top, ToPath(p.source), v);
          Log("setInputDeviceStateVector2f", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case playspectra::InputType::Active:
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
      // xrSyncActions carries no XrTime, so the glide evaluates against the latest intercepted
      // display time (0 before the first frame -> the animator snaps to the target).
      const XrTime now = AnimatorLastDisplayTime();
      for (const playspectra::StickyPose& sp : poses) {
        const XrPosef pose = AnimatorEvalController(sp, now).pose;
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
// GAP-08: non-CA input fallback. See input_inject.h for the full description and lock discipline.
// ---------------------------------------------------------------------------------------------
namespace {

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

}  // namespace

// xrSyncActions latch for the non-CA path: drain queued button/analog injections into the sticky store,
// refresh the active-action-set set, and recompute changedSinceLastSync/lastChangeTime. Controller
// poses are NOT handled here -- ApplyPoseOverride is the authoritative pose source in both CA and non-CA.
void ApplyFallbackSync(const XrActionsSyncInfo* syncInfo) {
  std::vector<playspectra::PendingInput> batch = playspectra::LayerStateDrainInputs();

  // Resolve each injection's subactionPath BEFORE taking g_action_mutex: ToPath calls the runtime
  // (xrStringToPath), and the established rule (GAP-06) is to never call the runtime while holding
  // g_action_mutex. Active commands carry no state value, so they are dropped here. `p` points into
  // `batch`, which outlives this vector.
  struct Resolved { const playspectra::PendingInput* p; XrPath sub; };
  std::vector<Resolved> resolved;
  resolved.reserve(batch.size());
  for (const playspectra::PendingInput& p : batch) {
    if (p.type == playspectra::InputType::Active) continue;
    resolved.push_back({&p, ToPath(p.top_level)});
  }

  std::lock_guard<std::mutex> lock(ActionMutex());

  for (const Resolved& r : resolved) {
    const playspectra::PendingInput& p = *r.p;
    for (XrAction action : ActionsBoundTo(p.source)) {  // g_actions read: fine under the lock
      FallbackActionState& st = g_fallback_states[{action, r.sub}];
      switch (p.type) {
        case playspectra::InputType::Float:  st.f = p.f; st.b = p.f > 0.5f; break;
        case playspectra::InputType::Bool:   st.b = p.b; st.f = p.b ? 1.0f : 0.0f; break;
        case playspectra::InputType::Vector2f: st.v = XrVector2f{p.x, p.y}; break;
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
// [G] accessors for the thin hooks in layer_entry.cpp. Each touches the shared fallback state
// in place, exactly as the original inline hook code did. PRECONDITION: caller holds ActionMutex().
// ---------------------------------------------------------------------------------------------

// xrGetCurrentInteractionProfile: the emulated interaction profile (XR_NULL_PATH if none suggested yet).
XrPath FallbackCurrentProfile() { return g_emulated_profile; }

// xrPollEvent: consume the one-shot synthetic InteractionProfileChanged flag (returns whether to deliver).
bool FallbackTakePendingIpEvent() {
  if (g_pending_ip_event) {
    g_pending_ip_event = false;
    return true;
  }
  return false;
}

// xrAttachSessionActionSets: arm the one synthetic InteractionProfileChanged for the next xrPollEvent.
void FallbackArmIpEvent() { g_pending_ip_event = true; }

// xrSuggestInteractionProfileBindings: remember the FIRST suggested profile as the one we emulate.
void FallbackNoteSuggestedProfile(XrPath profile) {
  if (g_emulated_profile == XR_NULL_PATH)
    g_emulated_profile = profile;
}

// xrDestroyActionSet: drop any fallback state keyed by this action (handle may be recycled).
void FallbackEraseForAction(XrAction action) {
  for (auto fit = g_fallback_states.begin(); fit != g_fallback_states.end();) {
    if (fit->first.first == action) fit = g_fallback_states.erase(fit);
    else ++fit;
  }
}

// xrDestroySession: sync state is per-session (the synthetic IP event is re-armed on the next attach).
void FallbackClearSessionScoped() {
  g_synced_active_sets.clear();
  g_pending_ip_event = false;
}

// xrDestroyInstance: fallback state is keyed by (now-invalid) actions; clear all of it.
void FallbackClearInstanceScoped() {
  g_fallback_states.clear();
  g_synced_active_sets.clear();
  g_emulated_profile = XR_NULL_PATH;
  g_pending_ip_event = false;
  g_fallback_sync_counter = 0;
}

}  // namespace playspectra
