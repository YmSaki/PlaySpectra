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

#include "capture.h"
#include "control_channel.h"

// ---------------------------------------------------------------------------------------------
// Logging (exposed to control_channel.cpp via vr_agent::LayerLog).
// ---------------------------------------------------------------------------------------------
namespace vr_agent {

static std::mutex g_log_mutex;

static std::ofstream& LogStream() {
  static std::ofstream stream = [] {
    const char* env = std::getenv("VR_AGENT_LOG");
    const char* tmp = std::getenv("TEMP");
    std::string path = env   ? env
                       : tmp ? std::string(tmp) + "\\vr_agent_layer.log"
                             : "vr_agent_layer.log";
    return std::ofstream(path, std::ios::app);
  }();
  return stream;
}

void LayerLog(const char* msg, const char* detail) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  std::ofstream& out = LogStream();
  if (!out) return;
  out << "[vr_agent] " << msg;
  if (detail) out << ": " << detail;
  out << "\n";
  out.flush();
}

}  // namespace vr_agent

namespace {

void Log(const char* msg, const char* detail = nullptr) { vr_agent::LayerLog(msg, detail); }

// ---------------------------------------------------------------------------------------------
// Layer state. A single global instance/session is adequate for the headless single-app use case;
// task #7-full replaces this with per-instance dispatch state.
// ---------------------------------------------------------------------------------------------
std::mutex g_mutex;
PFN_xrGetInstanceProcAddr g_next_get_instance_proc_addr = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
bool g_ca_enabled = false;  // XR_EXT_conformance_automation successfully enabled on the instance

// Conformance-automation entry points, resolved lazily from the runtime (below the layer).
PFN_xrStringToPath g_xrStringToPath = nullptr;
PFN_xrSetInputDeviceActiveEXT g_set_active = nullptr;
PFN_xrSetInputDeviceStateBoolEXT g_set_bool = nullptr;
PFN_xrSetInputDeviceStateFloatEXT g_set_float = nullptr;
PFN_xrSetInputDeviceStateVector2fEXT g_set_vec2 = nullptr;
PFN_xrSetInputDeviceLocationEXT g_set_location = nullptr;  // controller pose injection

// LOCAL reference space the layer creates itself, to express injected controller poses in (the
// same space hello_xr and typical apps use for their app space). Created lazily from the session.
PFN_xrCreateReferenceSpace g_create_ref_space = nullptr;
PFN_xrDestroySpace g_destroy_space = nullptr;
XrSpace g_local_space = XR_NULL_HANDLE;

template <typename T>
T ResolveNext(const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  if (g_next_get_instance_proc_addr && g_instance != XR_NULL_HANDLE) {
    g_next_get_instance_proc_addr(g_instance, name, &fn);
  }
  return reinterpret_cast<T>(fn);
}

XrPath ToPath(const std::string& s) {
  XrPath p = XR_NULL_PATH;
  if (g_xrStringToPath && g_instance != XR_NULL_HANDLE) {
    g_xrStringToPath(g_instance, s.c_str(), &p);
  }
  return p;
}

void EnsureConformanceAutomationResolved() {
  if (!g_ca_enabled) return;
  if (!g_xrStringToPath) g_xrStringToPath = ResolveNext<PFN_xrStringToPath>("xrStringToPath");
  if (!g_set_active)
    g_set_active = ResolveNext<PFN_xrSetInputDeviceActiveEXT>("xrSetInputDeviceActiveEXT");
  if (!g_set_bool)
    g_set_bool = ResolveNext<PFN_xrSetInputDeviceStateBoolEXT>("xrSetInputDeviceStateBoolEXT");
  if (!g_set_float)
    g_set_float = ResolveNext<PFN_xrSetInputDeviceStateFloatEXT>("xrSetInputDeviceStateFloatEXT");
  if (!g_set_vec2)
    g_set_vec2 = ResolveNext<PFN_xrSetInputDeviceStateVector2fEXT>("xrSetInputDeviceStateVector2fEXT");
  if (!g_set_location)
    g_set_location = ResolveNext<PFN_xrSetInputDeviceLocationEXT>("xrSetInputDeviceLocationEXT");
}

// Create the layer's own LOCAL reference space to express injected controller poses in. Lazy:
// needs a live session. Returns XR_NULL_HANDLE if the runtime can't provide it.
XrSpace EnsureLocalSpace(XrSession session) {
  if (g_local_space != XR_NULL_HANDLE) return g_local_space;
  if (!g_create_ref_space)
    g_create_ref_space = ResolveNext<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
  if (!g_create_ref_space) return XR_NULL_HANDLE;
  XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  ci.poseInReferenceSpace.orientation.w = 1.0f;  // identity
  XrResult r = g_create_ref_space(session, &ci, &g_local_space);
  if (XR_FAILED(r)) {
    g_local_space = XR_NULL_HANDLE;
    Log("EnsureLocalSpace: xrCreateReferenceSpace(LOCAL) failed");
  } else {
    Log("EnsureLocalSpace: LOCAL reference space created");
  }
  return g_local_space;
}

// Drain queued MCP input commands and apply them via conformance automation. Runs on the app
// thread from inside the xrSyncActions hook, so the runtime latches the new state on this sync.
void ApplyPendingInputs(XrSession session) {
  std::vector<vr_agent::PendingInput> batch = vr_agent::ControlChannelDrainInputs();
  std::vector<vr_agent::StickyPose> poses = vr_agent::ControlChannelGetStickyPoses();
  if (batch.empty() && poses.empty()) return;
  if (!g_ca_enabled) {
    Log("input dropped: conformance_automation not enabled on this runtime");
    return;
  }
  EnsureConformanceAutomationResolved();

  for (const vr_agent::PendingInput& p : batch) {
    const XrPath top = ToPath(p.top_level);
    switch (p.type) {
      case vr_agent::InputType::Float:
        if (g_set_float) {
          XrResult r = g_set_float(session, top, ToPath(p.source), p.f);
          Log("setInputDeviceStateFloat", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case vr_agent::InputType::Bool:
        if (g_set_bool) {
          XrResult r = g_set_bool(session, top, ToPath(p.source), p.b ? XR_TRUE : XR_FALSE);
          Log("setInputDeviceStateBool", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case vr_agent::InputType::Vector2f:
        if (g_set_vec2) {
          XrVector2f v{p.x, p.y};
          XrResult r = g_set_vec2(session, top, ToPath(p.source), v);
          Log("setInputDeviceStateVector2f", (p.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
      case vr_agent::InputType::Active:
        if (g_set_active) {
          XrResult r = g_set_active(session, ToPath(p.profile), top, p.b ? XR_TRUE : XR_FALSE);
          Log("setInputDeviceActive", (p.top_level + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
        }
        break;
    }
  }

  // Re-apply sticky controller poses every sync (held until pose_clear). Injected via
  // xrSetInputDeviceLocationEXT in the layer's LOCAL space -- this is the controller-pose half of
  // task #7 (head/VIEW override is separate). If the runtime lacks the EXT_conformance_automation
  // location entry point or a LOCAL space, we log once per attempt and no-op gracefully.
  if (!poses.empty() && g_set_location) {
    XrSpace space = EnsureLocalSpace(session);
    if (space != XR_NULL_HANDLE) {
      for (const vr_agent::StickyPose& sp : poses) {
        XrPosef pose{};
        pose.orientation = XrQuaternionf{sp.qx, sp.qy, sp.qz, sp.qw};
        pose.position = XrVector3f{sp.px, sp.py, sp.pz};
        XrResult r = g_set_location(session, ToPath(sp.top_level), ToPath(sp.source), space, pose);
        Log("setInputDeviceLocation", (sp.source + (XR_SUCCEEDED(r) ? " ok" : " FAIL")).c_str());
      }
    }
  } else if (!poses.empty() && !g_set_location) {
    Log("pose dropped: xrSetInputDeviceLocationEXT unavailable on this runtime");
  }
}

// ---------------------------------------------------------------------------------------------
// Head / viewpoint override (WU1). The head is not an input device, so conformance_automation
// can't set it -- the layer overrides xrLocateViews (what the app renders from) and
// xrLocateSpace(VIEW) directly. We re-base the runtime's real views onto the injected head pose,
// preserving each eye's offset (IPD) and FOV so only the head *moves*.
// ---------------------------------------------------------------------------------------------
std::mutex g_view_spaces_mutex;
std::set<XrSpace> g_view_spaces;  // reference spaces of type VIEW (from xrCreateReferenceSpace)

XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b) {
  return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf QConj(const XrQuaternionf& q) { return XrQuaternionf{-q.x, -q.y, -q.z, q.w}; }
XrVector3f QRot(const XrQuaternionf& q, const XrVector3f& v) {
  XrQuaternionf p{v.x, v.y, v.z, 0.0f};
  XrQuaternionf r = QMul(QMul(q, p), QConj(q));  // unit-quat rotation: q * (v,0) * q*
  return XrVector3f{r.x, r.y, r.z};
}
XrVector3f VAdd(const XrVector3f& a, const XrVector3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f VSub(const XrVector3f& a, const XrVector3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// Re-base runtime-located views onto the injected head pose. `views`/`count` are the runtime's
// result (expressed in the caller's locate space). Each eye keeps its offset from the runtime head
// (so IPD is preserved) and its FOV; only the head position/orientation change.
void RebaseViewsToHead(const vr_agent::HeadPose& h, uint32_t count, XrView* views) {
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

bool IsViewSpace(XrSpace s) {
  std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
  return g_view_spaces.count(s) > 0;
}

// Injected poses (head, controller grip) are defined in LOCAL (control_channel.h). An app may
// locate in a different world space (e.g. STAGE), so re-express `poseLocal` in `targetSpace` by
// composing it with LOCAL's pose in that space (via the layer's own LOCAL reference space). If the
// transform can't be obtained (no LOCAL space / locate fails / untracked), returns poseLocal as-is
// (correct for LOCAL-space apps, where the transform is identity) and sets *okOut=false.
XrPosef TransformLocalPoseToSpace(XrSession session, const XrPosef& poseLocal, XrSpace targetSpace,
                                  XrTime time, bool* okOut) {
  if (okOut) *okOut = false;
  XrSpace local = EnsureLocalSpace(session);
  if (local == XR_NULL_HANDLE || targetSpace == XR_NULL_HANDLE) return poseLocal;
  static PFN_xrLocateSpace locSpace = ResolveNext<PFN_xrLocateSpace>("xrLocateSpace");
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

bool g_warned_head_space = false;
vr_agent::HeadPose TransformHeadToSpace(XrSession session, const vr_agent::HeadPose& h,
                                        XrSpace targetSpace, XrTime time) {
  XrPosef in{{h.qx, h.qy, h.qz, h.qw}, {h.px, h.py, h.pz}};
  bool ok = false;
  XrPosef out = TransformLocalPoseToSpace(session, in, targetSpace, time, &ok);
  if (!ok && !g_warned_head_space) {
    g_warned_head_space = true;
    Log("head override: could not resolve LOCAL->locate-space transform; assuming app space == "
        "LOCAL (correct for LOCAL-space apps, off for STAGE)");
  }
  vr_agent::HeadPose r = h;
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
// action spaces for which hand (xrCreateActionSpace).
PFN_xrPathToString g_xrPathToString = nullptr;
std::mutex g_action_mutex;
struct ActionSpaceInfo { XrAction action; std::string handTop; };  // handTop e.g. "/user/hand/left"
std::map<XrSpace, ActionSpaceInfo> g_action_spaces;
std::set<XrAction> g_grip_pose_actions;

// ---------------------------------------------------------------------------------------------
// Action discovery registry (WU5, `actions` command / vr_actions tool). Lets an agent enumerate the
// app's action sets + actions by NAME ("Grab", "Teleport") and see which interaction-profile paths
// each action is bound to, so it never has to guess OpenXR paths. Purely observational: we record
// what the app registers (xrCreateActionSet/xrCreateAction/xrSuggestInteractionProfileBindings/
// xrAttachSessionActionSets) and forward every call unchanged. All strings are captured at record
// time (on the app thread, where PathToStr is valid) so the socket-thread dump touches no OpenXR
// state. Guarded by the existing g_action_mutex; action sets/actions are instance-scoped and cleared
// at xrDestroyInstance (attachment is session-scoped and cleared at xrDestroySession).
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

std::string PathToStr(XrPath p) {
  if (p == XR_NULL_PATH || g_instance == XR_NULL_HANDLE) return "";
  if (!g_xrPathToString) g_xrPathToString = ResolveNext<PFN_xrPathToString>("xrPathToString");
  if (!g_xrPathToString) return "";
  uint32_t len = 0;
  if (XR_FAILED(g_xrPathToString(g_instance, p, 0, &len, nullptr)) || len == 0) return "";
  std::string s(len, '\0');
  uint32_t written = 0;
  if (XR_FAILED(g_xrPathToString(g_instance, p, len, &written, &s[0]))) return "";
  if (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

// If `space` is a tracked grip-pose action space for a hand with an injected sticky pose, write that
// pose (LOCAL -> baseSpace) into `outPose`/`outFlags` and return true. Takes pose+flags (not the
// struct) so it serves both xrLocateSpace (XrSpaceLocation) and xrLocateSpaces (XrSpaceLocationData).
// `session` may be g_session for the singular xrLocateSpace (which has no session parameter).
bool ApplyGripOverride(XrSession session, XrSpace space, XrSpace baseSpace, XrTime time,
                       XrPosef& outPose, XrSpaceLocationFlags& outFlags) {
  ActionSpaceInfo info;
  {
    std::lock_guard<std::mutex> lock(g_action_mutex);
    auto it = g_action_spaces.find(space);
    if (it == g_action_spaces.end()) return false;
    info = it->second;
    if (g_grip_pose_actions.count(info.action) == 0) return false;
  }
  if (info.handTop.empty()) {  // null subactionPath -> can't map to a hand
    static bool warned = false;
    if (!warned) {
      warned = true;
      Log("grip override: action space has null subactionPath; can't map to a hand "
          "(per-action subaction tracking is a WU5 follow-up)");
    }
    return false;
  }
  XrPosef poseLocal{};
  bool have = false;
  for (const vr_agent::StickyPose& sp : vr_agent::ControlChannelGetStickyPoses()) {
    if (sp.top_level == info.handTop) {
      poseLocal.orientation = XrQuaternionf{sp.qx, sp.qy, sp.qz, sp.qw};
      poseLocal.position = XrVector3f{sp.px, sp.py, sp.pz};
      have = true;
      break;
    }
  }
  if (!have) return false;
  bool ok = false;
  outPose = TransformLocalPoseToSpace(session, poseLocal, baseSpace, time, &ok);
  if (!ok) {  // parity with the head path: transform to base space unresolved
    static bool warned = false;
    if (!warned) {
      warned = true;
      Log("grip override: LOCAL->locate-space transform unresolved; emitting the LOCAL pose as-is "
          "(correct for LOCAL-space apps, off for STAGE)");
    }
  }
  outFlags |= XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
              XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Hooked functions.
// ---------------------------------------------------------------------------------------------
XrResult XRAPI_CALL Hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo,
                                          XrSession* session) {
  try {
    static PFN_xrCreateSession next = ResolveNext<PFN_xrCreateSession>("xrCreateSession");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(instance, createInfo, session);
    if (XR_SUCCEEDED(r) && session) {
      g_session = *session;
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
    static PFN_xrDestroySession next = ResolveNext<PFN_xrDestroySession>("xrDestroySession");
    if (session == g_session) {
      if (g_local_space != XR_NULL_HANDLE) {
        if (!g_destroy_space) g_destroy_space = ResolveNext<PFN_xrDestroySpace>("xrDestroySpace");
        if (g_destroy_space) g_destroy_space(g_local_space);
        g_local_space = XR_NULL_HANDLE;
      }
      {
        std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
        g_view_spaces.clear();  // VIEW spaces belong to this session
      }
      {
        std::lock_guard<std::mutex> lock(g_action_mutex);
        g_action_spaces.clear();  // action spaces belong to this session
        g_grip_pose_actions.clear();
        g_attached_action_sets.clear();  // attachment is per-session (re-attached on a new session)
      }
      g_session = XR_NULL_HANDLE;
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
    static PFN_xrCreateSwapchain next = ResolveNext<PFN_xrCreateSwapchain>("xrCreateSwapchain");
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
    static PFN_xrDestroySwapchain next = ResolveNext<PFN_xrDestroySwapchain>("xrDestroySwapchain");
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
    static PFN_xrEnumerateSwapchainImages next =
        ResolveNext<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
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
    static PFN_xrAcquireSwapchainImage next =
        ResolveNext<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
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
    static PFN_xrReleaseSwapchainImage next =
        ResolveNext<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
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
    static PFN_xrEndFrame next = ResolveNext<PFN_xrEndFrame>("xrEndFrame");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return next(session, frameEndInfo);
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
  try {
    ApplyPendingInputs(session);
    static PFN_xrSyncActions next = ResolveNext<PFN_xrSyncActions>("xrSyncActions");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    return next(session, syncInfo);
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
    static PFN_xrApplyHapticFeedback next =
        ResolveNext<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback");
    return next ? next(session, hapticActionInfo, hapticFeedback) : XR_SUCCESS;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

XrResult XRAPI_CALL Hook_xrDestroyInstance(XrInstance instance) {
  try {
    static PFN_xrDestroyInstance next = ResolveNext<PFN_xrDestroyInstance>("xrDestroyInstance");
    Log("xrDestroyInstance -- stopping control channel");
    {
      std::lock_guard<std::mutex> lock(g_action_mutex);
      g_action_sets.clear();  // action sets/actions are instance-scoped
      g_actions.clear();
      g_attached_action_sets.clear();
    }
    vr_agent::ControlChannelStop();
    vr_agent::ControlChannelSetInstance(false);
    vr_agent::ControlChannelSetSession(false);
    XrResult r = next ? next(instance) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (instance == g_instance) g_instance = XR_NULL_HANDLE;
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
    static PFN_xrLocateViews next = ResolveNext<PFN_xrLocateViews>("xrLocateViews");
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
    static PFN_xrCreateReferenceSpace next =
        ResolveNext<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, createInfo, space);
    if (XR_SUCCEEDED(r) && space && createInfo &&
        createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
      std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
      g_view_spaces.insert(*space);
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
    static PFN_xrDestroySpace next = ResolveNext<PFN_xrDestroySpace>("xrDestroySpace");
    {
      std::lock_guard<std::mutex> lock(g_view_spaces_mutex);
      g_view_spaces.erase(space);
    }
    {
      std::lock_guard<std::mutex> lock(g_action_mutex);
      g_action_spaces.erase(space);
    }
    return next ? next(space) : XR_ERROR_FUNCTION_UNSUPPORTED;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
}

// Fill a location's pose/flags from the injected head pose. Note: xrLocateSpace(VIEW) returns the
// *single* VIEW-space origin (= the head pose itself), so a direct copy is correct here -- unlike
// xrLocateViews, whose per-eye poses need RebaseViewsToHead to preserve each eye's IPD offset. `h`
// must already be expressed in the location's base space (callers pass a TransformHeadToSpace result).
void ApplyHeadToLocation(const vr_agent::HeadPose& h, XrPosef& pose, XrSpaceLocationFlags& flags) {
  pose.orientation = XrQuaternionf{h.qx, h.qy, h.qz, h.qw};
  pose.position = XrVector3f{h.px, h.py, h.pz};
  flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
           XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
}

// xrLocateSpace: override VIEW located in a world (non-VIEW) space to the injected head pose.
XrResult XRAPI_CALL Hook_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time,
                                       XrSpaceLocation* location) {
  try {
    static PFN_xrLocateSpace next = ResolveNext<PFN_xrLocateSpace>("xrLocateSpace");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(space, baseSpace, time, location);
    if (XR_SUCCEEDED(r) && location) {
      if (IsViewSpace(space)) {
        vr_agent::HeadPose h;
        if (!IsViewSpace(baseSpace) && vr_agent::ControlChannelGetHead(h)) {
          const vr_agent::HeadPose hInBase = TransformHeadToSpace(g_session, h, baseSpace, time);
          ApplyHeadToLocation(hInBase, location->pose, location->locationFlags);
        }
      } else {
        // Controller grip pose override (authoritative pose path; makes orientation work regardless
        // of the runtime's CA behaviour). No-op unless this is a tracked grip space with a pose set.
        ApplyGripOverride(g_session, space, baseSpace, time, location->pose, location->locationFlags);
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
    static PFN_xrLocateSpaces next = ResolveNext<PFN_xrLocateSpaces>("xrLocateSpaces");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, locateInfo, locations);
    if (XR_SUCCEEDED(r) && locateInfo && locateInfo->spaces && locations && locations->locations) {
      vr_agent::HeadPose h;
      const bool headActive = !IsViewSpace(locateInfo->baseSpace) && vr_agent::ControlChannelGetHead(h);
      const vr_agent::HeadPose hInBase =
          headActive ? TransformHeadToSpace(session, h, locateInfo->baseSpace, locateInfo->time) : h;
      const uint32_t n = locations->locationCount < locateInfo->spaceCount ? locations->locationCount
                                                                           : locateInfo->spaceCount;
      for (uint32_t i = 0; i < n; ++i) {
        XrSpace s = locateInfo->spaces[i];
        if (IsViewSpace(s)) {
          if (headActive)
            ApplyHeadToLocation(hInBase, locations->locations[i].pose,
                                locations->locations[i].locationFlags);
        } else {
          ApplyGripOverride(session, s, locateInfo->baseSpace, locateInfo->time,
                            locations->locations[i].pose, locations->locations[i].locationFlags);
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
    static PFN_xrCreateActionSpace next = ResolveNext<PFN_xrCreateActionSpace>("xrCreateActionSpace");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, ci, space);
    if (XR_SUCCEEDED(r) && space && ci) {
      const std::string hand = PathToStr(ci->subactionPath);  // "" if XR_NULL_PATH
      std::lock_guard<std::mutex> lock(g_action_mutex);
      g_action_spaces[*space] = ActionSpaceInfo{ci->action, hand};
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
    static PFN_xrSuggestInteractionProfileBindings next =
        ResolveNext<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (suggestedBindings && suggestedBindings->suggestedBindings) {
      const std::string profile = PathToStr(suggestedBindings->interactionProfile);
      std::lock_guard<std::mutex> lock(g_action_mutex);
      for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = suggestedBindings->suggestedBindings[i];
        const std::string bindingPath = PathToStr(b.binding);
        if (bindingPath.find("/input/grip/pose") != std::string::npos)
          g_grip_pose_actions.insert(b.action);
        auto it = g_actions.find(b.action);  // only actions the app created via the hooked path
        if (it != g_actions.end()) it->second.bindings.push_back(BindingReg{profile, bindingPath});
      }
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
    static PFN_xrCreateActionSet next = ResolveNext<PFN_xrCreateActionSet>("xrCreateActionSet");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(instance, ci, actionSet);
    if (XR_SUCCEEDED(r) && actionSet && ci) {
      std::lock_guard<std::mutex> lock(g_action_mutex);
      g_action_sets[*actionSet] = ActionSetReg{ci->actionSetName, ci->localizedActionSetName};
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
    static PFN_xrCreateAction next = ResolveNext<PFN_xrCreateAction>("xrCreateAction");
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
      std::lock_guard<std::mutex> lock(g_action_mutex);
      g_actions[*action] = std::move(reg);
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
    static PFN_xrDestroyActionSet next = ResolveNext<PFN_xrDestroyActionSet>("xrDestroyActionSet");
    {
      std::lock_guard<std::mutex> lock(g_action_mutex);
      g_action_sets.erase(actionSet);
      g_attached_action_sets.erase(actionSet);
      for (auto it = g_actions.begin(); it != g_actions.end();) {
        if (it->second.actionSet == actionSet) {
          g_grip_pose_actions.erase(it->first);
          it = g_actions.erase(it);
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
    static PFN_xrAttachSessionActionSets next =
        ResolveNext<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
    if (!next) return XR_ERROR_FUNCTION_UNSUPPORTED;
    XrResult r = next(session, attachInfo);
    if (XR_SUCCEEDED(r) && attachInfo && attachInfo->actionSets) {
      std::lock_guard<std::mutex> lock(g_action_mutex);
      for (uint32_t i = 0; i < attachInfo->countActionSets; ++i)
        g_attached_action_sets.insert(attachInfo->actionSets[i]);
    }
    return r;
  } catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
  }
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
      if (ResolveNext<PFN_xrLocateSpaces>("xrLocateSpaces") != nullptr) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(Hook_xrLocateSpaces);
        return XR_SUCCESS;
      }
      // fall through to passthrough (runtime lacks it -> report as the runtime does)
    }

    PFN_xrGetInstanceProcAddr next = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      next = g_next_get_instance_proc_addr;
    }
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
  auto get_props = ResolveNext<PFN_xrGetInstanceProperties>("xrGetInstanceProperties");
  if (!get_props) return;
  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  if (get_props(g_instance, &props) == XR_SUCCESS) {
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

    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_next_get_instance_proc_addr = next_gipa;
    }

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
      g_instance = *instance;
      g_ca_enabled = ca_supported;
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
// Bridge for control_channel.cpp's `actions` command (declared there). Defined here because the
// action registry and PathToStr-captured strings live in this translation unit.
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
