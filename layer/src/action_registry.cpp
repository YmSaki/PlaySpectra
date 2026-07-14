// Action discovery registry + shared action mutex implementation. Moved verbatim from
// openxr_agent_layer.cpp (refactor phase 4); behaviour is unchanged (same data, same algorithms,
// same lock discipline -- see action_registry.h for the shared-mutex invariants).
#include "action_registry.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <openxr/openxr.h>

#include "layer_dispatch.h"  // PathToStr (runtime-backed path stringification)

namespace vr_agent {

namespace {

// ---------------------------------------------------------------------------------------------
// Controller grip-pose in-layer override state (WU3b). To find the grip action spaces we track:
// which actions are bound to a .../input/grip/pose path (xrSuggestInteractionProfileBindings), and
// which XrSpaces are action spaces for which hand (xrCreateActionSpace).
// ---------------------------------------------------------------------------------------------
std::mutex g_action_mutex;
std::map<XrSpace, ActionSpaceInfo> g_action_spaces;
std::set<XrAction> g_grip_pose_actions;
// GAP-04: actions bound to a .../input/aim/pose path. aim and grip are the SAME rigid controller with
// a fixed offset, so we derive the aim pose as aim = grip * offset rather than tracking it separately.
std::set<XrAction> g_aim_pose_actions;
// Cached grip->aim static offset per hand (aim-in-grip frame). Only populated once the runtime returns
// a VALID offset for that hand; g_grip_to_aim_valid gates that so an initial untracked locate does NOT
// poison the cache with identity (which would collapse aim onto grip for the whole session). All three
// are guarded by g_action_mutex.
std::map<std::string, XrPosef> g_grip_to_aim;
std::set<std::string> g_grip_to_aim_valid;

// ---------------------------------------------------------------------------------------------
// Action discovery registry (WU5, `actions` command / vr_actions tool). Lets an agent enumerate the
// app's action sets + actions by NAME ("Grab", "Teleport") and see which interaction-profile paths
// each action is bound to, so it never has to guess OpenXR paths. Purely observational: we record
// what the app registers (xrCreateActionSet/xrCreateAction/xrSuggestInteractionProfileBindings/
// xrAttachSessionActionSets) and forward every call unchanged. All strings are captured at record
// time (on the app thread, where PathToStr is valid) so the socket-thread dump touches no OpenXR
// state. Guarded by the existing g_action_mutex; action sets/actions are instance-scoped and cleared
// at xrDestroyInstance (attachment is session-scoped and cleared at xrDestroySession).
std::map<XrActionSet, ActionSetReg> g_action_sets;
std::map<XrAction, ActionReg> g_actions;
std::set<XrActionSet> g_attached_action_sets;

const char* ActionTypeName(XrActionType t) {
  switch (t) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT: return "BOOLEAN_INPUT";
    case XR_ACTION_TYPE_FLOAT_INPUT: return "FLOAT_INPUT";
    case XR_ACTION_TYPE_VECTOR2F_INPUT: return "VECTOR2F_INPUT";
    case XR_ACTION_TYPE_POSE_INPUT: return "POSE_INPUT";
    case XR_ACTION_TYPE_VIBRATION_OUTPUT: return "VIBRATION_OUTPUT";
    default: return "UNKNOWN";
  }
}

}  // namespace

std::mutex& ActionMutex() { return g_action_mutex; }

std::map<XrSpace, ActionSpaceInfo>& RegistryActionSpaces() { return g_action_spaces; }
std::set<XrAction>& RegistryGripPoseActions() { return g_grip_pose_actions; }
std::set<XrAction>& RegistryAimPoseActions() { return g_aim_pose_actions; }
std::map<std::string, XrPosef>& RegistryGripToAim() { return g_grip_to_aim; }
std::set<std::string>& RegistryGripToAimValid() { return g_grip_to_aim_valid; }
std::map<XrActionSet, ActionSetReg>& RegistryActionSets() { return g_action_sets; }
std::map<XrAction, ActionReg>& RegistryActions() { return g_actions; }
std::set<XrActionSet>& RegistryAttachedActionSets() { return g_attached_action_sets; }

// GAP-06 helper: extract the top-level hand path ("/user/hand/left") from a full binding path
// ("/user/hand/left/input/grip/pose"). Returns "" if the path is not under /user/hand/*.
std::string HandTopFromBindingPath(const std::string& path) {
  if (path.compare(0, 11, "/user/hand/") != 0) return "";
  size_t slash = path.find('/', 11);  // end of the hand segment
  return slash == std::string::npos ? path : path.substr(0, slash);
}

// GAP-06 helper: infer which hand(s) a pose action targets from its recorded binding paths. Used only
// for action spaces created with a null subactionPath (handTop == ""), where the hand must be resolved
// lazily at locate time (bindings are only guaranteed present by then). Returns 0, 1, or 2 distinct
// "/user/hand/*" tops. PRECONDITION: caller already holds g_action_mutex. Pure registry read: no
// control-channel calls, no locking inside (would deadlock the non-recursive mutex).
std::vector<std::string> InferHandTops(XrAction action) {
  std::vector<std::string> out;
  auto it = g_actions.find(action);
  if (it == g_actions.end()) return out;
  for (const BindingReg& b : it->second.bindings) {
    std::string top = HandTopFromBindingPath(b.path);
    if (top.empty()) continue;
    bool dup = false;
    for (const std::string& t : out) if (t == top) { dup = true; break; }
    if (!dup) out.push_back(top);
  }
  return out;
}

void RegistryRecordActionSet(XrActionSet actionSet, const XrActionSetCreateInfo* ci) {
  g_action_sets[actionSet] = ActionSetReg{ci->actionSetName, ci->localizedActionSetName};
}

void RegistryRecordAction(XrAction action, ActionReg&& reg) {
  g_actions[action] = std::move(reg);
}

void RegistryRecordActionSpace(XrSpace space, XrAction action, const std::string& handTop) {
  g_action_spaces[space] = ActionSpaceInfo{action, handTop};
}

void RegistryRecordBindings(const XrInteractionProfileSuggestedBinding* suggestedBindings,
                            const std::string& profile) {
  for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i) {
    const XrActionSuggestedBinding& b = suggestedBindings->suggestedBindings[i];
    const std::string bindingPath = PathToStr(b.binding);
    if (bindingPath.find("/input/grip/pose") != std::string::npos)
      g_grip_pose_actions.insert(b.action);
    // GAP-04: /input/aim/pose is defined by every OpenXR interaction profile (universal, core).
    if (bindingPath.find("/input/aim/pose") != std::string::npos)
      g_aim_pose_actions.insert(b.action);
    auto it = g_actions.find(b.action);  // only actions the app created via the hooked path
    if (it != g_actions.end()) it->second.bindings.push_back(BindingReg{profile, bindingPath});
  }
}

void RegistryRecordAttach(const XrSessionActionSetsAttachInfo* attachInfo) {
  for (uint32_t i = 0; i < attachInfo->countActionSets; ++i)
    g_attached_action_sets.insert(attachInfo->actionSets[i]);
}

void RegistryEraseSpace(XrSpace space) { g_action_spaces.erase(space); }

void RegistryClearSessionScoped() {
  g_action_spaces.clear();  // action spaces belong to this session
  g_grip_pose_actions.clear();
  g_aim_pose_actions.clear();  // GAP-04: mirror grip cleanup
  g_grip_to_aim.clear();
  g_grip_to_aim_valid.clear();
  g_attached_action_sets.clear();  // attachment is per-session (re-attached on a new session)
}

void RegistryClearInstanceScoped() {
  g_action_sets.clear();  // action sets/actions are instance-scoped
  g_actions.clear();
  g_attached_action_sets.clear();
}

// Build the `actions` discovery dump (called from the control-channel socket thread). Reads only the
// string-ified registry under g_action_mutex; touches no live OpenXR state. Live action values /
// isActive are intentionally NOT included -- xrGetActionState* must run on the app's session thread
// after a sync, which the socket thread must not do (same rule the whole control channel follows).
std::string BuildActionsJson() {
  using json = nlohmann::json;
  json sets = json::array();
  {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    for (const auto& setKv : g_action_sets) {
      const XrActionSet setHandle = setKv.first;
      json actions = json::array();
      for (const auto& actKv : g_actions) {
        const ActionReg& act = actKv.second;
        if (act.actionSet != setHandle) continue;
        json boundPaths = json::array();
        for (const BindingReg& b : act.bindings)
          boundPaths.push_back({{"profile", b.profile}, {"path", b.path}});
        json subs = json::array();
        for (const std::string& s : act.subactionPaths) subs.push_back(s);
        actions.push_back({{"name", act.name},
                           {"localizedName", act.localizedName},
                           {"type", static_cast<int>(act.type)},
                           {"typeName", ActionTypeName(act.type)},
                           {"boundPaths", boundPaths},
                           {"subactionPaths", subs}});
      }
      sets.push_back({{"name", setKv.second.name},
                      {"localizedName", setKv.second.localizedName},
                      {"attached", g_attached_action_sets.count(setHandle) > 0},
                      {"actions", actions}});
    }
  }
  json out = {
      {"ok", true},
      {"actionSets", sets},
      {"note",
       "Static registry only (action names / types / bound interaction-profile paths). Live action "
       "values and isActive are not included: reading xrGetActionState* requires the app's session "
       "thread with an attached, synced action set, which the control channel must not touch."},
  };
  return out.dump();
}

}  // namespace vr_agent
