// Action discovery registry + the shared action mutex. Extracted from openxr_agent_layer.cpp
// (refactor phase 4) so the observational action-system registry (action sets / actions / bound
// interaction-profile paths / action spaces / grip+aim classification / GAP-06 inference /
// BuildActionsJson) lives in one translation unit. The registry globals and g_action_mutex are
// TU-private in action_registry.cpp; this header publishes the types, the mutex, the record/query
// helpers, and the raw-container accessors that the pose-override (cluster E) and non-CA input
// fallback (cluster G) still read directly. Behaviour -- data, algorithms, lock discipline -- is
// unchanged; this is a move only.
#pragma once

#include <openxr/openxr.h>

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vr_agent {

// ---------------------------------------------------------------------------------------------
// Registry record types (observational only; strings captured at record time on the app thread).
// ---------------------------------------------------------------------------------------------
struct ActionSpaceInfo { XrAction action; std::string handTop; };  // handTop e.g. "/user/hand/left"
struct ActionSetReg { std::string name; std::string localizedName; };
struct BindingReg { std::string profile; std::string path; };  // interaction profile + bound path
struct ActionReg {
  XrActionSet actionSet = XR_NULL_HANDLE;
  std::string name;
  std::string localizedName;
  XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
  std::vector<std::string> subactionPaths;
  std::vector<BindingReg> bindings;
};

// ---------------------------------------------------------------------------------------------
// THE SHARED ACTION MUTEX AND ITS DISCIPLINE (established GAP-08; corrected 71a9a16). READ THIS.
//
// A SINGLE mutex guards THREE clusters that were kept together on purpose:
//   [F] this registry (action sets / actions / bindings / action spaces / grip+aim sets /
//       grip->aim offset cache);
//   [E] the pose override (openxr_agent_layer.cpp: ResolveGripToAimOffset / ApplyPoseOverride) --
//       reads the registry containers to map an action space to a hand and classify grip vs aim;
//   [G] the non-CA input fallback (openxr_agent_layer.cpp: ApplyFallbackSync / AggregateFallback /
//       the GetActionState* readers) -- reverse-looks-up g_actions, so a SEPARATE lock would invert
//       the acquisition order relative to [F].
// Because [E] and [G] reach into [F]'s containers under this one lock, splitting it (finer grain,
// a second mutex, or a different acquisition order) is FORBIDDEN -- it was reviewed and the shared
// single lock is the correct design. Do not change lock granularity/order/scope.
//
// THREE INVARIANTS the callers must keep:
//   1. Ordering: [E]->[F]->[G] all take THIS mutex (ActionMutex()). Never introduce a second lock
//      for any of them (would reintroduce the GAP-08 inversion).
//   2. Never call a RUNTIME entry point (ToPath -> xrStringToPath, etc.) while holding this mutex.
//      Two-phase rule: resolve paths BEFORE taking the lock, then take the lock (see
//      openxr_agent_layer.cpp ApplyFallbackSync, and ResolveGripToAimOffset which releases the lock
//      before its raw xrLocateSpace). Historical exception preserved verbatim (move-only): the
//      binding-record loop stringifies binding paths (PathToStr) inside RegistryRecordBindings while
//      the caller holds the lock, exactly as the original xrSuggestInteractionProfileBindings hook
//      did -- not "fixed" here.
//   3. Never call a ControlChannel* entry point while holding this mutex (release first).
//
// PRECONDITION CONVENTION: every helper below EXCEPT BuildActionsJson assumes the caller ALREADY
// holds ActionMutex() and does NOT lock internally (so a caller can compose several registry
// operations, and its own [G] state mutations, inside one critical section without splitting scope).
// The raw-container accessors likewise return references to the guarded containers and must only be
// touched while holding ActionMutex(). BuildActionsJson is the sole self-locking entry (it is called
// from the control-channel socket thread and always managed its own lock).
// ---------------------------------------------------------------------------------------------
std::mutex& ActionMutex();

// Raw guarded containers. PRECONDITION: caller holds ActionMutex(). Exposed so cluster E (pose
// override) and cluster G (input fallback) keep reading/mutating them in place, unchanged, until
// they move in phases 5/6.
std::map<XrSpace, ActionSpaceInfo>& RegistryActionSpaces();
std::set<XrAction>& RegistryGripPoseActions();
std::set<XrAction>& RegistryAimPoseActions();
std::map<std::string, XrPosef>& RegistryGripToAim();
std::set<std::string>& RegistryGripToAimValid();
std::map<XrActionSet, ActionSetReg>& RegistryActionSets();
std::map<XrAction, ActionReg>& RegistryActions();
std::set<XrActionSet>& RegistryAttachedActionSets();

// Record helpers (called from the observing hooks). PRECONDITION: caller holds ActionMutex().
void RegistryRecordActionSet(XrActionSet actionSet, const XrActionSetCreateInfo* ci);
void RegistryRecordAction(XrAction action, ActionReg&& reg);
void RegistryRecordActionSpace(XrSpace space, XrAction action, const std::string& handTop);
// Stringifies each binding path (PathToStr) and records grip/aim classification + the action->binding
// map. See invariant 2: this DOES call the runtime under the lock, preserved from the original hook.
void RegistryRecordBindings(const XrInteractionProfileSuggestedBinding* suggestedBindings,
                            const std::string& profile);
void RegistryRecordAttach(const XrSessionActionSetsAttachInfo* attachInfo);

// Erase / scope-clear helpers. PRECONDITION: caller holds ActionMutex().
void RegistryEraseSpace(XrSpace space);          // xrDestroySpace: drop a recycled action-space handle
// xrDestroyActionSet: erase the action set + its actions and all handle-reuse-safety mirror state
// (grip/aim pose sets). eraseFallback is injected by the caller (so this TU needn't depend on
// input_inject) and is invoked per erased action to drop its GAP-08 fallback entries.
void RegistryEraseActionSet(XrActionSet actionSet,
                            const std::function<void(XrAction)>& eraseFallback);
void RegistryClearSessionScoped();               // xrDestroySession: action spaces + grip/aim + offset
                                                 //   cache + attachment (session-scoped registry state)
void RegistryClearInstanceScoped();              // xrDestroyInstance: action sets + actions + attachment

// GAP-06 hand inference. PRECONDITION for InferHandTops: caller holds ActionMutex() (pure registry
// read of g_actions). HandTopFromBindingPath is a pure string helper (no state / no lock).
std::string HandTopFromBindingPath(const std::string& path);
std::vector<std::string> InferHandTops(XrAction action);

// `actions` discovery dump for the control channel's socket thread. Locks ActionMutex() internally.
std::string BuildActionsJson();

}  // namespace vr_agent
