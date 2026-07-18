// Layer dispatch table + single-instance state implementation. Moved verbatim from
// layer_entry.cpp (refactor phase 3); behaviour is unchanged (same resolution, same lock
// discipline, same single-instance model).
#include "layer_dispatch.h"

#include <mutex>
#include <string>

#include <openxr/openxr.h>

namespace playspectra {

namespace {

// ---------------------------------------------------------------------------------------------
// Layer state. A single global instance/session is adequate for the headless single-app use case
// (VR-Playwright's north star: one subject app under test, so at most one live XrInstance). We do
// NOT keep a handle->instance registry -- that would be a full multi-instance dispatch mechanism we
// don't need. What we DO fix (GAP-07): the next-layer entry points used to live in per-hook
// function-local statics that initialise exactly once per process, so a second instance (e.g. Unity
// Editor Play-mode repeat) kept the previous runtime's stale pointers. They now live in one
// LayerDispatch that is rebuilt on every xrCreateApiLayerInstance and cleared on xrDestroyInstance.
// ---------------------------------------------------------------------------------------------
std::mutex g_mutex;
PFN_xrGetInstanceProcAddr g_next_get_instance_proc_addr = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
bool g_ca_enabled = false;  // XR_EXT_conformance_automation successfully enabled on the instance

LayerDispatch g_dispatch;

template <typename T>
T ResolveNext(const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  if (g_next_get_instance_proc_addr && g_instance != XR_NULL_HANDLE) {
    g_next_get_instance_proc_addr(g_instance, name, &fn);
  }
  return reinterpret_cast<T>(fn);
}

}  // namespace

const LayerDispatch& Dispatch() { return g_dispatch; }

void RebuildLayerDispatch() {
  LayerDispatch d;
  d.createSession = ResolveNext<PFN_xrCreateSession>("xrCreateSession");
  d.destroySession = ResolveNext<PFN_xrDestroySession>("xrDestroySession");
  d.createSwapchain = ResolveNext<PFN_xrCreateSwapchain>("xrCreateSwapchain");
  d.destroySwapchain = ResolveNext<PFN_xrDestroySwapchain>("xrDestroySwapchain");
  d.enumerateSwapchainImages =
      ResolveNext<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
  d.acquireSwapchainImage = ResolveNext<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage");
  d.releaseSwapchainImage = ResolveNext<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
  d.endFrame = ResolveNext<PFN_xrEndFrame>("xrEndFrame");
  d.syncActions = ResolveNext<PFN_xrSyncActions>("xrSyncActions");
  d.applyHapticFeedback = ResolveNext<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback");
  d.destroyInstance = ResolveNext<PFN_xrDestroyInstance>("xrDestroyInstance");
  d.locateViews = ResolveNext<PFN_xrLocateViews>("xrLocateViews");
  d.createReferenceSpace = ResolveNext<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
  d.locateSpace = ResolveNext<PFN_xrLocateSpace>("xrLocateSpace");
  d.destroySpace = ResolveNext<PFN_xrDestroySpace>("xrDestroySpace");
  d.createActionSpace = ResolveNext<PFN_xrCreateActionSpace>("xrCreateActionSpace");
  d.suggestInteractionProfileBindings =
      ResolveNext<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
  d.createActionSet = ResolveNext<PFN_xrCreateActionSet>("xrCreateActionSet");
  d.createAction = ResolveNext<PFN_xrCreateAction>("xrCreateAction");
  d.destroyActionSet = ResolveNext<PFN_xrDestroyActionSet>("xrDestroyActionSet");
  d.attachSessionActionSets =
      ResolveNext<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
  d.locateSpaces = ResolveNext<PFN_xrLocateSpaces>("xrLocateSpaces");  // optional
  d.getActionStateBoolean = ResolveNext<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
  d.getActionStateFloat = ResolveNext<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
  d.getActionStateVector2f = ResolveNext<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
  d.getCurrentInteractionProfile =
      ResolveNext<PFN_xrGetCurrentInteractionProfile>("xrGetCurrentInteractionProfile");
  d.pollEvent = ResolveNext<PFN_xrPollEvent>("xrPollEvent");
  d.getInstanceProperties = ResolveNext<PFN_xrGetInstanceProperties>("xrGetInstanceProperties");
  d.pathToString = ResolveNext<PFN_xrPathToString>("xrPathToString");
  d.stringToPath = ResolveNext<PFN_xrStringToPath>("xrStringToPath");
  if (g_ca_enabled) {
    d.setInputDeviceActive = ResolveNext<PFN_xrSetInputDeviceActiveEXT>("xrSetInputDeviceActiveEXT");
    d.setInputDeviceStateBool =
        ResolveNext<PFN_xrSetInputDeviceStateBoolEXT>("xrSetInputDeviceStateBoolEXT");
    d.setInputDeviceStateFloat =
        ResolveNext<PFN_xrSetInputDeviceStateFloatEXT>("xrSetInputDeviceStateFloatEXT");
    d.setInputDeviceStateVector2f =
        ResolveNext<PFN_xrSetInputDeviceStateVector2fEXT>("xrSetInputDeviceStateVector2fEXT");
    d.setInputDeviceLocation =
        ResolveNext<PFN_xrSetInputDeviceLocationEXT>("xrSetInputDeviceLocationEXT");
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dispatch = d;
}

void ClearLayerDispatch() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_dispatch = LayerDispatch{};
}

XrInstance CurrentInstance() { return g_instance; }
void SetCurrentInstance(XrInstance instance) { g_instance = instance; }
XrSession CurrentSession() { return g_session; }
void SetCurrentSession(XrSession session) { g_session = session; }
bool CaEnabled() { return g_ca_enabled; }
void SetCaEnabled(bool enabled) { g_ca_enabled = enabled; }

PFN_xrGetInstanceProcAddr NextGetInstanceProcAddr() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_next_get_instance_proc_addr;
}
void SetNextGetInstanceProcAddr(PFN_xrGetInstanceProcAddr pfn) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_next_get_instance_proc_addr = pfn;
}

XrPath ToPath(const std::string& s) {
  XrPath p = XR_NULL_PATH;
  if (g_dispatch.stringToPath && g_instance != XR_NULL_HANDLE) {
    g_dispatch.stringToPath(g_instance, s.c_str(), &p);
  }
  return p;
}

std::string PathToStr(XrPath p) {
  if (p == XR_NULL_PATH || g_instance == XR_NULL_HANDLE) return "";
  PFN_xrPathToString pathToString = g_dispatch.pathToString;
  if (!pathToString) return "";
  uint32_t len = 0;
  if (XR_FAILED(pathToString(g_instance, p, 0, &len, nullptr)) || len == 0) return "";
  std::string s(len, '\0');
  uint32_t written = 0;
  if (XR_FAILED(pathToString(g_instance, p, len, &written, &s[0]))) return "";
  if (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}

}  // namespace playspectra
